#pragma once

#include "common/range_lock.hpp"
#include "dynamic/partitioner.hpp"
#include "genetic/histogram_partitioner.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <utility>

enum class PartitionStrategy { DynamicProgramming, Balanced, Genetic };

template <size_t kLockCnt, typename Mutex = std::mutex> class DynamicLock {
  static_assert(kLockCnt > 0);
  static_assert(kLockCnt <= std::numeric_limits<size_t>::max() / 64);
  static constexpr size_t kMaxBins = 64 * kLockCnt;
  static constexpr double kMinimumImprovement = 0.10;

public:
  explicit DynamicLock(size_t array_size, size_t stats_sample_rate = 64,
                       PartitionStrategy strategy = PartitionStrategy::DynamicProgramming,
                       uint64_t genetic_seed = 123)
      : array_size_(array_size),
        bin_size_(array_size == 0 ? 1 : 1 + (array_size - 1) / kMaxBins),
        bin_count_(array_size == 0 ? 0 : 1 + (array_size - 1) / bin_size_),
        sample_threshold_(std::numeric_limits<uint64_t>::max() /
                          std::max(stats_sample_rate, size_t{1})),
        strategy_(strategy),
        genetic_seed_(genetic_seed),
        starts_(bin_count_), ends_(bin_count_) {
    const size_t count = std::min(kLockCnt, bin_count_);
    cuts_.push_back(0);
    for (size_t index = 1; index <= count; ++index) {
      cuts_.push_back(index * bin_count_ / count);
    }
    for (size_t index = 0; index < kLockCnt; ++index) {
      locks_[index].sampling_state = index + 1;
    }
    RefreshMapping();
  }

  ~DynamicLock() { StopRebuilder(); }
  DynamicLock(const DynamicLock &) = delete;
  DynamicLock &operator=(const DynamicLock &) = delete;

  template <typename Func>
  void WriteQuery(size_t left, size_t right, Func &&func) {
    Query<false>(left, right, std::forward<Func>(func));
  }

  template <typename Func>
  void ReadQuery(size_t left, size_t right, Func &&func) {
    Query<true>(left, right, std::forward<Func>(func));
  }

private:
  template <bool Read, typename Func>
  void Query(size_t left, size_t right, Func &&func) {
    if (array_size_ == 0) {
      return;
    }
    Normalize(left, right);
    const size_t first_bin = left / bin_size_;
    const size_t last_bin = right / bin_size_;
    for (;;) {
      const size_t version = epoch_.load(std::memory_order_acquire);
      if (version & 1) {
        std::this_thread::yield();
        continue;
      }
      const size_t first = bin_to_lock_[first_bin].load(std::memory_order_relaxed);
      const size_t last = first_bin == last_bin ? first
          : bin_to_lock_[last_bin].load(std::memory_order_relaxed);
      if (first > last) {
        continue;
      }
      dynamic_lock_detail::RangeLockGuard<decltype(locks_), Read> guard(locks_, first, last);
      if (epoch_.load(std::memory_order_relaxed) != version) {
        continue;
      }
      if constexpr (Read) {
        // Readers can share a data lock, so neither RNG state nor histogram
        // updates may rely on exclusive ownership. Same read path for all mutex types.
        static thread_local uint64_t sampling_state = 1;
        if (Sample(sampling_state)) {
          std::atomic_ref<uint64_t>(starts_[first_bin]).fetch_add(1, std::memory_order_relaxed);
          std::atomic_ref<uint64_t>(ends_[last_bin]).fetch_add(1, std::memory_order_relaxed);
        }
      } else if (Sample(locks_[first].sampling_state)) {
        // Exclusive data locks exclude readers of these bins too. The rebuilder
        // holds every data lock exclusively before accessing/swapping counters.
        ++starts_[first_bin];
        ++ends_[last_bin];
      }
      func(left, right);
      return;
    }
  }

public:
  void StartRebuilder(std::chrono::milliseconds interval =
                          std::chrono::milliseconds(30'000)) {
    if (interval.count() <= 0) {
      throw std::invalid_argument("rebuild interval must be positive");
    }
    std::lock_guard lifecycle(lifecycle_mutex_);
    if (rebuilder_.joinable()) {
      return;
    }
    rebuilder_ = std::jthread([this, interval](std::stop_token stop) {
      std::unique_lock sleep(sleep_mutex_);
      while (!stop.stop_requested()) {
        wake_.wait_for(sleep, stop, interval, [] { return false; });
        if (stop.stop_requested()) {
          break;
        }
        sleep.unlock();
        RebuildNow();
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

  bool RebuildNow() {
    std::lock_guard rebuild(rebuild_mutex_);
    if (bin_count_ < 2) {
      return false;
    }
    const auto begin = std::chrono::steady_clock::now();
    std::vector<uint64_t> starts(bin_count_), ends(bin_count_);
    {
      Pause pause(epoch_);
      dynamic_lock_detail::RangeLockGuard all(locks_, 0, kLockCnt - 1);
      starts.swap(starts_);
      ends.swap(ends_);
    }
    bool changed = false;

    uint64_t samples = 0;
    for (const auto count : starts) {
      samples += count;
    }
    if (samples < kLockCnt / kMinimumImprovement) {
      Pause pause(epoch_);
      dynamic_lock_detail::RangeLockGuard all(locks_, 0, kLockCnt - 1);
      for (size_t bin = 0; bin < bin_count_; ++bin) {
        starts_[bin] += starts[bin];
        ends_[bin] += ends[bin];
      }
    } else {
      std::vector<uint64_t> weights(bin_count_);
      uint64_t active = 0;
      long double total_weight = 0, bin_cost = 0;
      for (size_t bin = 0; bin < bin_count_; ++bin) {
        active += starts[bin];
        weights[bin] = active;
        total_weight += active;
        bin_cost += static_cast<long double>(active) * active;
        active -= ends[bin];
      }
      const auto current_cost = dynamic_lock_detail::PartitionLoadCost(weights, cuts_);
      const auto lower_bound = std::max(bin_cost, total_weight * total_weight /
                                                   std::min(kLockCnt, bin_count_));
      std::vector<size_t> plan;
      if (lower_bound < current_cost * (1.0 - kMinimumImprovement)) {
        ++optimization_count_;
        switch (strategy_) {
        case PartitionStrategy::DynamicProgramming:
          plan = dynamic_lock_detail::DynamicProgrammingPartitions(weights, kLockCnt);
          break;
        case PartitionStrategy::Balanced:
          plan = dynamic_lock_detail::BalancePartitions(weights, kLockCnt);
          break;
        case PartitionStrategy::Genetic:
          plan = dynamic_lock_detail::GeneticPartitions(weights, kLockCnt, cuts_, genetic_seed_);
          break;
        }
      }
      if (!plan.empty() && plan != cuts_ &&
          dynamic_lock_detail::PartitionLoadCost(weights, plan) <
              current_cost * (1.0 - kMinimumImprovement)) {
        Pause pause(epoch_);
        dynamic_lock_detail::RangeLockGuard all(locks_, 0, kLockCnt - 1);
        cuts_ = plan;
        RefreshMapping();
        ++rebuild_count_;
        changed = true;
      }
    }
    rebuild_time_ += std::chrono::steady_clock::now() - begin;
    return changed;
  }

  size_t GetRebuildCount() const {
    std::lock_guard lock(rebuild_mutex_);
    return rebuild_count_;
  }

  double GetRebuildTimeMs() const {
    std::lock_guard lock(rebuild_mutex_);
    return std::chrono::duration<double, std::milli>(rebuild_time_).count();
  }

  size_t GetOptimizationCount() const {
    std::lock_guard lock(rebuild_mutex_);
    return optimization_count_;
  }

  std::vector<size_t> PartitionCuts() const {
    std::lock_guard lock(rebuild_mutex_);
    auto positions = cuts_;
    for (auto &cut : positions) {
      cut = cut == bin_count_ ? array_size_ : cut * bin_size_;
    }
    return positions;
  }

  size_t CountLocksForRange(size_t left, size_t right) const {
    if (array_size_ == 0) {
      return 0;
    }
    Normalize(left, right);
    std::lock_guard lock(rebuild_mutex_);
    return bin_to_lock_[right / bin_size_].load(std::memory_order_relaxed) -
               bin_to_lock_[left / bin_size_].load(std::memory_order_relaxed) + 1;
  }

private:
  class Pause {
  public:
    explicit Pause(std::atomic<size_t> &epoch) : epoch_(epoch) {
      epoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    ~Pause() { epoch_.fetch_add(1, std::memory_order_release); }
    Pause(const Pause &) = delete;
    Pause &operator=(const Pause &) = delete;
  private:
    std::atomic<size_t> &epoch_;
  };

  bool Sample(uint64_t &state) const {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * UINT64_C(2685821657736338717) <= sample_threshold_;
  }

  void Normalize(size_t &left, size_t &right) const {
    left = std::min(left, array_size_ - 1);
    right = std::min(right, array_size_ - 1);
    if (left > right) {
      std::swap(left, right);
    }
  }

  void RefreshMapping() {
    for (size_t index = 1; index < cuts_.size(); ++index) {
      for (size_t bin = cuts_[index - 1]; bin < cuts_[index]; ++bin) {
        bin_to_lock_[bin].store(index - 1, std::memory_order_relaxed);
      }
    }
  }

  const size_t array_size_, bin_size_, bin_count_;
  const uint64_t sample_threshold_;
  const PartitionStrategy strategy_;
  const uint64_t genetic_seed_;
  std::array<dynamic_lock_detail::LockSlot<Mutex>, kLockCnt> locks_;
  std::array<std::atomic<size_t>, kMaxBins> bin_to_lock_{};
  std::vector<uint64_t> starts_, ends_;
  std::vector<size_t> cuts_;
  std::atomic<size_t> epoch_{0};
  mutable std::mutex rebuild_mutex_;
  size_t rebuild_count_ = 0;
  size_t optimization_count_ = 0;
  std::chrono::steady_clock::duration rebuild_time_{};
  std::mutex lifecycle_mutex_, sleep_mutex_;
  std::condition_variable_any wake_;
  std::jthread rebuilder_;
};
