#pragma once

#include "common/range_lock.hpp"
#include "dynamic/paper_partitioner.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

enum class PaperStrategy { Balanced, DynamicProgramming };

struct PaperLockConfig {
  size_t bins = 1024;
  size_t sample_rate = 64;
  uint64_t lambda = 10'000; // Tunable raw-count weight; article's 10^7 remains available.
  size_t minimum_samples = 640;
  double minimum_improvement = 0.10;
};

struct PaperStatistics {
  std::vector<uint64_t> hits, crossings;
  uint64_t samples = 0;
};

// Fixed mutex budget, configurable elementary blocks. Only publication stops
// admission. Sampled h_i and c_i are atomic; optimization uses relaxed snapshots.
template <size_t K, class Mutex = std::mutex> class PaperRangeLock {
  static_assert(K > 0);
  using Clock = std::chrono::steady_clock;
  struct Counters {
    std::atomic<uint64_t> hits{0}, crossings{0}, starts{0};
  };
  struct Snapshot {
    std::vector<uint64_t> hits, crossings, starts;
    explicit Snapshot(size_t n) : hits(n), crossings(n), starts(n) {}
  };

public:
  explicit PaperRangeLock(size_t size, PaperLockConfig config = {},
                          PaperStrategy strategy = PaperStrategy::DynamicProgramming)
      : size_(size), config_(Validate(config)), strategy_(strategy),
        bin_size_(size == 0 ? 1 : 1 + (size - 1) / std::min(size, config_.bins)),
        bins_(size == 0 ? 0 : 1 + (size - 1) / bin_size_),
        threshold_(std::numeric_limits<uint64_t>::max() / config_.sample_rate),
        map_(std::make_unique<std::atomic<size_t>[]>(bins_)),
        counters_(std::make_unique<Counters[]>(bins_)), previous_(bins_) {
    const size_t groups = std::min(K, bins_);
    cuts_.push_back(0);
    for (size_t j = 1; j <= groups; ++j) cuts_.push_back(j * bins_ / groups);
    for (size_t j = 1; j < cuts_.size(); ++j)
      for (size_t i = cuts_[j - 1]; i < cuts_[j]; ++i)
        map_[i].store(j - 1, std::memory_order_relaxed);
    for (size_t j = 0; j < K; ++j) locks_[j].sampling_state = j + 1;
  }
  ~PaperRangeLock() { StopRebuilder(); }
  PaperRangeLock(const PaperRangeLock &) = delete;
  PaperRangeLock &operator=(const PaperRangeLock &) = delete;

  template <class Func> void WriteQuery(size_t left, size_t right, Func &&func) {
    Query<false>(left, right, std::forward<Func>(func));
  }
  template <class Func> void ReadQuery(size_t left, size_t right, Func &&func) {
    Query<true>(left, right, std::forward<Func>(func));
  }

  void StartRebuilder(std::chrono::milliseconds interval = std::chrono::seconds(30)) {
    if (interval.count() <= 0) throw std::invalid_argument("rebuild interval must be positive");
    std::lock_guard lifecycle(lifecycle_mutex_);
    if (rebuilder_.joinable()) return;
    rebuilder_ = std::jthread([this, interval](std::stop_token stop) {
      std::unique_lock sleep(sleep_mutex_);
      while (!stop.stop_requested()) {
        wake_.wait_for(sleep, stop, interval, [] { return false; });
        if (stop.stop_requested()) break;
        sleep.unlock();
        try {
          RebuildNow();
        } catch (...) {
          std::lock_guard g(rebuild_mutex_);
          background_error_ = std::current_exception();
          return;
        }
        sleep.lock();
      }
    });
  }
  void StopRebuilder() {
    std::lock_guard lifecycle(lifecycle_mutex_);
    if (rebuilder_.joinable()) {
      rebuilder_.request_stop();
      wake_.notify_all();
      rebuilder_.join();
    }
  }
  void RethrowBackgroundError() const {
    std::lock_guard g(rebuild_mutex_);
    if (background_error_) std::rethrow_exception(background_error_);
  }

  bool RebuildNow() {
    std::lock_guard serialize(rebuild_mutex_);
    const auto start = Clock::now();
    if (bins_ < 2) return false;
    auto snapshot = ReadSnapshot();
    std::vector<uint64_t> hits(bins_), crossings(bins_);
    uint64_t samples = 0;
    for (size_t i = 0; i < bins_; ++i) {
      // Only this serialized rebuilder resets counters; otherwise each is
      // monotone. Different counters need not describe one atomic instant.
      hits[i] = snapshot.hits[i] - previous_.hits[i];
      crossings[i] = snapshot.crossings[i] - previous_.crossings[i];
      samples += snapshot.starts[i] - previous_.starts[i];
    }
    if (samples < config_.minimum_samples) {
      rebuild_ms_ += Milliseconds(Clock::now() - start);
      return false; // Accumulate sparse statistics until there is enough mass.
    }
    last_samples_ = samples;
    const auto optimization_start = Clock::now();
    const auto current = paper_lock_detail::Objective(hits, crossings, cuts_, config_.lambda);
    auto plan = strategy_ == PaperStrategy::Balanced
        ? paper_lock_detail::QuantilePartitions(hits, K)
        : paper_lock_detail::CHTPartitions(hits, crossings, K, config_.lambda);
    const auto proposed = paper_lock_detail::Objective(hits, crossings, plan, config_.lambda);
    ++optimizations_;
    optimize_ms_ += Milliseconds(Clock::now() - optimization_start);
    const bool improves = proposed < current &&
        static_cast<long double>(current - proposed) >
            static_cast<long double>(current) * config_.minimum_improvement;
    if (plan == cuts_ || !improves) {
      previous_ = std::move(snapshot);
      rebuild_ms_ += Milliseconds(Clock::now() - start);
      return false;
    }
    // Allocate/build everything before admission closes. Publishing below is
    // noexcept once all mutexes are acquired; RAII reopens on acquisition failure.
    std::vector<size_t> mapping(bins_);
    for (size_t j = 1; j < plan.size(); ++j)
      std::fill(mapping.begin() + plan[j - 1], mapping.begin() + plan[j], j - 1);
    const auto pause_start = Clock::now();
    Clock::time_point acquired, published;
    {
      Admission pause(admission_closed_);
      {
        dynamic_lock_detail::RangeLockGuard all(locks_, 0, K - 1);
        acquired = Clock::now();
        epoch_.fetch_add(1, std::memory_order_acq_rel); // Odd only after draining old bodies.
        cuts_.swap(plan);
        for (size_t i = 0; i < bins_; ++i) {
          map_[i].store(mapping[i], std::memory_order_relaxed);
          counters_[i].hits.store(0, std::memory_order_relaxed);
          counters_[i].crossings.store(0, std::memory_order_relaxed);
          counters_[i].starts.store(0, std::memory_order_relaxed);
          previous_.hits[i] = previous_.crossings[i] = previous_.starts[i] = 0;
        }
        epoch_.fetch_add(1, std::memory_order_release);
        published = Clock::now();
      }
    }
    const auto finish = Clock::now();
    if (rebuilds_ == 0) first_publication_ = finish;
    last_publication_ = finish;
    ++rebuilds_;
    publication_wait_ms_ += Milliseconds(acquired - pause_start);
    publication_hold_ms_ += Milliseconds(published - acquired);
    max_pause_ms_ = std::max(max_pause_ms_, Milliseconds(finish - pause_start));
    rebuild_ms_ += Milliseconds(finish - start);
    return true;
  }

  size_t GetBinCount() const { return bins_; }
  size_t GetRebuildCount() const { std::lock_guard g(rebuild_mutex_); return rebuilds_; }
  size_t GetOptimizationCount() const { std::lock_guard g(rebuild_mutex_); return optimizations_; }
  uint64_t GetLastWindowSampleCount() const { std::lock_guard g(rebuild_mutex_); return last_samples_; }
  double GetRebuildTimeMs() const { std::lock_guard g(rebuild_mutex_); return rebuild_ms_; }
  double GetOptimizeTimeMs() const { std::lock_guard g(rebuild_mutex_); return optimize_ms_; }
  double GetPublicationWaitTimeMs() const { std::lock_guard g(rebuild_mutex_); return publication_wait_ms_; }
  double GetPublicationHoldTimeMs() const { std::lock_guard g(rebuild_mutex_); return publication_hold_ms_; }
  double GetMaxPublicationPauseMs() const { std::lock_guard g(rebuild_mutex_); return max_pause_ms_; }
  std::pair<Clock::time_point, Clock::time_point> GetPublicationTimes() const {
    std::lock_guard g(rebuild_mutex_);
    return {first_publication_, last_publication_};
  }
  std::vector<size_t> PartitionCuts() const {
    std::lock_guard g(rebuild_mutex_);
    auto cuts = cuts_;
    for (auto &cut : cuts) cut = cut == bins_ ? size_ : cut * bin_size_;
    return cuts;
  }
  size_t CountLocksForRange(size_t left, size_t right) const {
    if (!size_) return 0;
    Normalize(left, right);
    std::lock_guard g(rebuild_mutex_);
    return map_[right / bin_size_].load(std::memory_order_relaxed) -
           map_[left / bin_size_].load(std::memory_order_relaxed) + 1;
  }
  // Coherent diagnostic accessor. Optimization deliberately uses the nonblocking
  // private snapshot instead. Do not call lock control/accessors from callbacks.
  PaperStatistics SnapshotStatistics() {
    std::lock_guard g(rebuild_mutex_);
    Admission pause(admission_closed_);
    dynamic_lock_detail::RangeLockGuard all(locks_, 0, K - 1);
    auto snapshot = ReadSnapshot();
    PaperStatistics result{std::move(snapshot.hits), std::move(snapshot.crossings), 0};
    for (auto n : snapshot.starts) result.samples += n;
    return result;
  }

private:
  class Admission {
  public:
    explicit Admission(std::atomic<bool> &closed) : closed_(closed) {
      closed_.store(true, std::memory_order_release);
    }
    ~Admission() { closed_.store(false, std::memory_order_release); }
    Admission(const Admission &) = delete;
  private:
    std::atomic<bool> &closed_;
  };
  static PaperLockConfig Validate(PaperLockConfig config) {
    if (!config.bins || !config.sample_rate || !config.minimum_samples ||
        !std::isfinite(config.minimum_improvement) || config.minimum_improvement < 0 ||
        config.minimum_improvement >= 1)
      throw std::invalid_argument("invalid paper lock configuration");
    return config;
  }
  template <class Duration> static double Milliseconds(Duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
  }
  void Normalize(size_t &left, size_t &right) const {
    left = std::min(left, size_ - 1);
    right = std::min(right, size_ - 1);
    if (left > right) std::swap(left, right);
  }
  bool Sample(uint64_t &state) const {
    if (config_.sample_rate == 1) return true;
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * UINT64_C(2685821657736338717) <= threshold_;
  }
  template <bool Read, class Func> void Query(size_t left, size_t right, Func &&func) {
    if (!size_) return;
    Normalize(left, right);
    const size_t a = left / bin_size_, b = right / bin_size_;
    for (;;) {
      if (admission_closed_.load(std::memory_order_acquire)) { std::this_thread::yield(); continue; }
      const size_t version = epoch_.load(std::memory_order_acquire);
      if (version & 1) continue;
      const size_t first = map_[a].load(std::memory_order_relaxed);
      const size_t last = a == b ? first : map_[b].load(std::memory_order_relaxed);
      if (first > last) continue;
      dynamic_lock_detail::RangeLockGuard<decltype(locks_), Read> guard(locks_, first, last);
      if (epoch_.load(std::memory_order_relaxed) != version) continue;
      bool sampled;
      if constexpr (Read && requires(Mutex &mutex) { mutex.lock_shared(); }) {
        // Shared readers cannot update a mutex-local PRNG concurrently.
        static std::atomic<uint64_t> seed{1};
        static thread_local uint64_t state = seed.fetch_add(UINT64_C(0x9e3779b97f4a7c15),
                                                           std::memory_order_relaxed);
        sampled = Sample(state);
      } else {
        // Exclusive queries already own this cache line. Avoid a platform TLS
        // lookup on every operation; no additional lock or global counter.
        sampled = Sample(locks_[first].sampling_state);
      }
      if (sampled) {
        counters_[a].starts.fetch_add(1, std::memory_order_relaxed);
        counters_[a].hits.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = a + 1; i <= b; ++i) {
          counters_[i].hits.fetch_add(1, std::memory_order_relaxed);
          counters_[i].crossings.fetch_add(1, std::memory_order_relaxed);
        }
      }
      func(left, right);
      return;
    }
  }
  Snapshot ReadSnapshot() const {
    Snapshot snapshot(bins_);
    for (size_t i = 0; i < bins_; ++i) {
      snapshot.hits[i] = counters_[i].hits.load(std::memory_order_relaxed);
      snapshot.crossings[i] = counters_[i].crossings.load(std::memory_order_relaxed);
      snapshot.starts[i] = counters_[i].starts.load(std::memory_order_relaxed);
    }
    return snapshot;
  }

  const size_t size_;
  const PaperLockConfig config_;
  const PaperStrategy strategy_;
  const size_t bin_size_, bins_;
  const uint64_t threshold_;
  std::array<dynamic_lock_detail::LockSlot<Mutex>, K> locks_;
  std::unique_ptr<std::atomic<size_t>[]> map_;
  std::unique_ptr<Counters[]> counters_;
  std::vector<size_t> cuts_;
  std::atomic<bool> admission_closed_{false};
  std::atomic<size_t> epoch_{0};
  mutable std::mutex rebuild_mutex_;
  std::exception_ptr background_error_;
  Snapshot previous_;
  size_t rebuilds_ = 0, optimizations_ = 0;
  Clock::time_point first_publication_{}, last_publication_{};
  uint64_t last_samples_ = 0;
  double rebuild_ms_ = 0, optimize_ms_ = 0, publication_wait_ms_ = 0,
         publication_hold_ms_ = 0, max_pause_ms_ = 0;
  std::mutex lifecycle_mutex_, sleep_mutex_;
  std::condition_variable_any wake_;
  std::jthread rebuilder_;
};
