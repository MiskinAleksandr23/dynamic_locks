#include "dynamic/paper_lock.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;

void Check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

PaperLockConfig Config(size_t bins = 64) {
  PaperLockConfig config;
  config.bins = bins;
  config.sample_rate = 1;
  config.lambda = 0;
  config.minimum_samples = 1;
  config.minimum_improvement = 0.05;
  return config;
}

template <class Lock> void Coverage(const Lock &lock, size_t size) {
  const auto cuts = lock.PartitionCuts();
  Check(!cuts.empty() && cuts.front() == 0 && cuts.back() == size,
        "partition must cover the entire array");
  for (size_t i = 1; i < cuts.size(); ++i)
    Check(cuts[i - 1] < cuts[i], "partition groups must be nonempty");
  if (!size) {
    Check(cuts.size() == 1, "empty array must have only origin cut");
    return;
  }
  for (size_t left = 0; left < size; ++left) {
    const size_t right = std::min(size - 1, left + 37);
    const auto first = std::upper_bound(cuts.begin(), cuts.end(), left);
    const auto last = std::upper_bound(cuts.begin(), cuts.end(), right);
    Check(lock.CountLocksForRange(left, right) == size_t(last - first) + 1,
          "element-to-lock mapping disagrees with published cuts");
  }
}

void BasicSemantics(PaperStrategy strategy) {
  for (size_t size : {size_t{0}, size_t{1}, size_t{3}, size_t{17}, size_t{257}}) {
    PaperRangeLock<8> lock(size, Config(), strategy);
    const size_t maximum = std::numeric_limits<size_t>::max();
    std::vector<uint64_t> actual(size), expected(size);
    for (auto [left, right] : std::vector<std::pair<size_t, size_t>>{
             {0, 0}, {0, maximum}, {maximum, 0}, {maximum, maximum},
             {size / 2, 0}, {size / 2, size / 2}, {size, size + 7}}) {
      size_t first = size ? std::min(left, size - 1) : 0;
      size_t last = size ? std::min(right, size - 1) : 0;
      if (first > last) std::swap(first, last);
      size_t writes = 0, reads = 0;
      lock.WriteQuery(left, right, [&](size_t begin, size_t end) {
        ++writes;
        Check(begin == first && end == last, "write endpoints must be clamped, sorted and inclusive");
        for (size_t i = begin; i <= end; ++i) ++actual[i];
      });
      lock.ReadQuery(left, right, [&](size_t begin, size_t end) {
        ++reads;
        Check(begin == first && end == last, "read endpoints must be clamped, sorted and inclusive");
      });
      Check(writes == (size != 0) && reads == (size != 0), "callback count is incorrect");
      if (size) for (size_t i = first; i <= last; ++i) ++expected[i];
      else Check(lock.CountLocksForRange(left, right) == 0, "empty range needs no mutex");
    }
    Check(actual == expected, "range updates do not match normalized reference");
    lock.RebuildNow();
    Coverage(lock, size);
    if (size) {
      for (bool read : {false, true}) {
        bool threw = false;
        const auto fail = [](size_t, size_t) { throw std::runtime_error("callback"); };
        try {
          if (read) lock.ReadQuery(0, size - 1, fail);
          else lock.WriteQuery(0, size - 1, fail);
        } catch (const std::runtime_error &e) { threw = std::string(e.what()) == "callback"; }
        Check(threw, "callback exception must propagate");
        lock.WriteQuery(0, size - 1, [](size_t, size_t) {});
        lock.ReadQuery(0, size - 1, [](size_t, size_t) {});
      }
    }
    lock.StartRebuilder(1ms);
    lock.StartRebuilder(1ms);
    lock.StopRebuilder();
    lock.StopRebuilder();
    lock.StartRebuilder(1ms); // Destructor must stop/join the restarted thread.
  }
}

void HistogramSemantics(PaperStrategy strategy) {
  PaperRangeLock<4, std::shared_mutex> lock(160, Config(16), strategy);
  std::vector<uint64_t> hits(16), crossings(16);
  const std::vector<std::pair<size_t, size_t>> queries{
      {0, 0}, {9, 10}, {10, 10}, {19, 10}, {31, 8}, {0, 159}, {1000, 159}};
  for (size_t index = 0; index < queries.size(); ++index) {
    auto [left, right] = queries[index];
    if (index % 2) lock.ReadQuery(left, right, [](size_t, size_t) {});
    else lock.WriteQuery(left, right, [](size_t, size_t) {});
    left = std::min(left, size_t{159});
    right = std::min(right, size_t{159});
    if (left > right) std::swap(left, right);
    for (size_t bin = left / 10; bin <= right / 10; ++bin) ++hits[bin];
    for (size_t cut = left / 10 + 1; cut <= right / 10; ++cut) ++crossings[cut];
  }
  auto stats = lock.SnapshotStatistics();
  Check(stats.hits == hits && stats.crossings == crossings,
        "histogram does not count every touched block and crossed internal boundary");
  Check(stats.samples == queries.size(), "sample_rate=1 must record every read and write");

  // Concurrent shared readers cannot use non-atomic histogram increments.
  std::barrier start(4);
  std::vector<std::thread> readers;
  for (size_t t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      start.arrive_and_wait();
      for (size_t i = 0; i < 1000; ++i) lock.ReadQuery(10, 29, [](size_t, size_t) {});
    });
  }
  for (auto &reader : readers) reader.join();
  hits[1] += 4000;
  hits[2] += 4000;
  crossings[2] += 4000;
  stats = lock.SnapshotStatistics();
  Check(stats.hits == hits && stats.crossings == crossings && stats.samples == queries.size() + 4000,
        "concurrent shared reads lost statistics");
}

void SharedReadersAndReadOnlyAdaptation(PaperStrategy strategy) {
  PaperRangeLock<4, std::shared_mutex> lock(1024, Config(64), strategy);
  std::barrier together(2);
  std::thread first([&] {
    lock.ReadQuery(0, 255, [&](size_t, size_t) { together.arrive_and_wait(); });
  });
  std::thread second([&] {
    lock.ReadQuery(0, 255, [&](size_t, size_t) { together.arrive_and_wait(); });
  });
  first.join();
  second.join();
  for (size_t i = 0; i < 1024; ++i) lock.ReadQuery(i % 256, i % 256, [](size_t, size_t) {});
  const auto before = lock.PartitionCuts();
  Check(lock.RebuildNow(), "read-only statistics must support a useful repartition");
  Check(before != lock.PartitionCuts(), "successful publication must change cuts");
  const auto empty = lock.SnapshotStatistics();
  Check(empty.samples == 0 && std::ranges::all_of(empty.hits, [](auto n) { return n == 0; }) &&
        std::ranges::all_of(empty.crossings, [](auto n) { return n == 0; }),
        "publication must start a fresh statistics window");
  Coverage(lock, 1024);
}

void CrossingPenaltyAndMinimumMass(PaperStrategy strategy) {
  auto config = Config(16);
  config.minimum_samples = 1000;
  PaperRangeLock<4> low_penalty(160, config, strategy);
  config.lambda = 1'000'000'000;
  PaperRangeLock<4> high_penalty(160, config, strategy);
  const auto initial = low_penalty.PartitionCuts();
  for (size_t i = 0; i < 999; ++i) {
    low_penalty.WriteQuery(0, 39, [](size_t, size_t) {});
    high_penalty.WriteQuery(0, 39, [](size_t, size_t) {});
  }
  Check(!low_penalty.RebuildNow() && low_penalty.GetOptimizationCount() == 0,
        "optimization must wait for sufficient sampled mass");
  low_penalty.WriteQuery(0, 39, [](size_t, size_t) {});
  high_penalty.WriteQuery(0, 39, [](size_t, size_t) {});
  Check(low_penalty.RebuildNow(), "without crossing penalty the hot segment should be subdivided");
  Check(!high_penalty.RebuildNow() && high_penalty.PartitionCuts() == initial,
        "publication ignored the full crossing-penalized objective");
  Check(low_penalty.CountLocksForRange(0, 39) > high_penalty.CountLocksForRange(0, 39),
        "penalty integration test must distinguish acquisition counts");
}

class ThrowingMutex {
public:
  static inline thread_local int successful_before_throw = -1;
  static inline thread_local size_t held = 0;
  void lock() {
    if (successful_before_throw == 0) throw std::runtime_error("mutex acquisition");
    if (successful_before_throw > 0) --successful_before_throw;
    mutex_.lock();
    ++held;
  }
  void unlock() {
    --held;
    mutex_.unlock();
  }
private:
  std::mutex mutex_;
};

void AcquisitionFailures(PaperStrategy strategy) {
  PaperRangeLock<4, ThrowingMutex> lock(1024, Config(), strategy);
  for (size_t i = 0; i < 1024; ++i) lock.WriteQuery(i % 256, i % 256, [](size_t, size_t) {});
  bool callback = false, threw = false;
  ThrowingMutex::successful_before_throw = 2;
  try { lock.WriteQuery(0, 1023, [&](size_t, size_t) { callback = true; }); }
  catch (const std::runtime_error &) { threw = true; }
  ThrowingMutex::successful_before_throw = -1;
  Check(threw && !callback && ThrowingMutex::held == 0, "partial query acquisition leaked locks");

  const auto before = lock.PartitionCuts();
  threw = false;
  ThrowingMutex::successful_before_throw = 2;
  try { lock.RebuildNow(); }
  catch (const std::runtime_error &) { threw = true; }
  ThrowingMutex::successful_before_throw = -1;
  Check(threw && ThrowingMutex::held == 0 && before == lock.PartitionCuts(),
        "failed publication leaked locks or modified the mapping");
  // Deadlocks if failed publication left admission closed or epoch odd.
  lock.WriteQuery(0, 1023, [](size_t, size_t) {});
  Check(lock.RebuildNow(), "lock must recover after an acquisition failure");
  Coverage(lock, 1024);
}

class BackgroundFailMutex {
public:
  static inline std::thread::id foreground;
  static inline std::atomic<bool> failure_attempted{false};
  static inline std::atomic<size_t> background_held{0};
  void lock() {
    const bool background = std::this_thread::get_id() != foreground;
    if (background && background_held.load() == 2) {
      failure_attempted.store(true, std::memory_order_release);
      throw std::runtime_error("background publication acquisition");
    }
    mutex_.lock();
    if (background) ++background_held;
  }
  void unlock() {
    if (std::this_thread::get_id() != foreground) --background_held;
    mutex_.unlock();
  }
private:
  std::mutex mutex_;
};

void BackgroundFailureRecovery(PaperStrategy strategy) {
  BackgroundFailMutex::foreground = std::this_thread::get_id();
  BackgroundFailMutex::failure_attempted = false;
  BackgroundFailMutex::background_held = 0;
  PaperRangeLock<4, BackgroundFailMutex> lock(1024, Config(), strategy);
  lock.RethrowBackgroundError();
  for (size_t i = 0; i < 1024; ++i) lock.WriteQuery(i % 256, i % 256, [](size_t, size_t) {});
  const auto before = lock.PartitionCuts();
  lock.StartRebuilder(1ms);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!BackgroundFailMutex::failure_attempted.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  lock.StopRebuilder(); // Joins the catch path, so the stored exception is now observable.
  Check(BackgroundFailMutex::failure_attempted && BackgroundFailMutex::background_held == 0,
        "background publication did not fail as arranged or leaked partial locks");
  bool caught = false;
  try { lock.RethrowBackgroundError(); }
  catch (const std::runtime_error &error) {
    caught = std::string(error.what()) == "background publication acquisition";
  }
  Check(caught && lock.PartitionCuts() == before,
        "background failure must be retained and must not install a partial mapping");
  // Both calls deadlock if unwinding failed to reopen admission or unlock data.
  lock.ReadQuery(0, 1023, [](size_t, size_t) {});
  lock.WriteQuery(0, 1023, [](size_t, size_t) {});
  Check(lock.RebuildNow(), "foreground rebuilding must remain usable after background failure");
}

class PausingMutex {
public:
  static inline std::atomic<size_t> next_id{0};
  static inline std::atomic<bool> paused{false}, resume{false};
  static inline thread_local bool pause_once = false;
  static inline thread_local std::vector<size_t> held;
  PausingMutex() : id_(next_id.fetch_add(1)) {}
  void lock() {
    if (pause_once) {
      pause_once = false;
      paused.store(true, std::memory_order_release);
      while (!resume.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    mutex_.lock();
    held.push_back(id_);
  }
  void unlock() {
    held.erase(std::find(held.begin(), held.end(), id_));
    mutex_.unlock();
  }
private:
  const size_t id_;
  std::mutex mutex_;
};

void StaleMappingRetry(PaperStrategy strategy) {
  PausingMutex::next_id = 0;
  PausingMutex::paused = false;
  PausingMutex::resume = false;
  PaperRangeLock<4, PausingMutex> lock(1024, Config(), strategy);
  for (size_t i = 0; i < 1024; ++i) lock.WriteQuery(i % 256, i % 256, [](size_t, size_t) {});
  size_t expected = 0;
  bool correct = false;
  size_t callbacks = 0;
  std::thread delayed([&] {
    PausingMutex::pause_once = true;
    lock.WriteQuery(200, 200, [&](size_t, size_t) {
      ++callbacks;
      correct = PausingMutex::held == std::vector<size_t>{expected};
    });
  });
  while (!PausingMutex::paused.load(std::memory_order_acquire)) std::this_thread::yield();
  const bool changed = lock.RebuildNow();
  const auto cuts = lock.PartitionCuts();
  expected = std::upper_bound(cuts.begin(), cuts.end(), 200) - cuts.begin() - 1;
  PausingMutex::resume.store(true, std::memory_order_release);
  delayed.join();
  Check(changed && expected != 0, "stale-map test must move the query to a different mutex");
  Check(correct && callbacks == 1, "delayed admitted query failed to retry with current mapping");
}

template <class Mutex> void ConcurrentRebuilds(PaperStrategy strategy) {
  constexpr size_t rounds = 6, operations = 400, workers = 6;
  PaperRangeLock<8, Mutex> lock(1024, Config(64), strategy);
  std::vector<uint64_t> data(1024), expected(1024);
  std::atomic<bool> invalid{false};
  std::barrier phase(workers + 1);
  std::vector<std::thread> threads;
  lock.StartRebuilder(1ms);
  for (size_t worker = 0; worker < workers; ++worker) {
    threads.emplace_back([&, worker] {
      for (size_t round = 0; round < rounds; ++round) {
        const size_t begin = (round % 2) * 512;
        phase.arrive_and_wait();
        for (size_t i = 0; i < operations; ++i) {
          if (worker < 2) {
            lock.WriteQuery(begin, begin + 63, [&](size_t first, size_t last) {
              for (size_t p = first; p <= last; ++p) {
                ++data[p];
                if (p == first + 32) std::this_thread::yield();
              }
            });
          } else {
            lock.ReadQuery(begin, begin + 63, [&](size_t first, size_t last) {
              for (size_t p = first; p <= last; ++p)
                if (data[p] != data[first]) invalid.store(true, std::memory_order_relaxed);
            });
          }
        }
        phase.arrive_and_wait();
      }
    });
  }
  for (size_t round = 0; round < rounds; ++round) {
    const size_t begin = (round % 2) * 512;
    for (size_t i = 0; i < 2000; ++i) lock.ReadQuery(begin, begin + 63, [](size_t, size_t) {});
    phase.arrive_and_wait();
    lock.RebuildNow();
    lock.RebuildNow();
    phase.arrive_and_wait();
    for (size_t i = begin; i < begin + 64; ++i) expected[i] += 2 * operations;
  }
  for (auto &thread : threads) thread.join();
  lock.StopRebuilder();
  Check(!invalid && data == expected, "concurrent publication broke atomic range semantics or lost updates");
  Check(lock.GetRebuildCount() >= 2, "moving load must cause multiple actual publications");
  Coverage(lock, data.size());
}
} // namespace

int main() {
  try {
    for (auto strategy : {PaperStrategy::Balanced, PaperStrategy::DynamicProgramming}) {
      BasicSemantics(strategy);
      HistogramSemantics(strategy);
      SharedReadersAndReadOnlyAdaptation(strategy);
      CrossingPenaltyAndMinimumMass(strategy);
      AcquisitionFailures(strategy);
      BackgroundFailureRecovery(strategy);
      StaleMappingRetry(strategy);
      ConcurrentRebuilds<std::mutex>(strategy);
      ConcurrentRebuilds<std::shared_mutex>(strategy);
    }
    std::cout << "PASS paper range semantics, exact hit/crossing statistics, shared readers, read-only adaptation, "
                 "exception recovery, stale mapping retry and concurrent publications\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
