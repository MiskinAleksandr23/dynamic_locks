#include "common/naive_lock.hpp"
#include "common/element_lock.hpp"
#include "dynamic/dynamic_lock.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Range = std::pair<size_t, size_t>;
using namespace std::chrono_literals;

template <size_t locks, class Mutex = std::mutex>
class BalancedLock : public DynamicLock<locks, Mutex> {
public:
  explicit BalancedLock(size_t size)
      : DynamicLock<locks, Mutex>(size, 64, PartitionStrategy::Balanced) {}
};

template <size_t locks, class Mutex = std::mutex>
class GeneticTestLock : public DynamicLock<locks, Mutex> {
public:
  explicit GeneticTestLock(size_t size)
      : DynamicLock<locks, Mutex>(size, 64, PartitionStrategy::Genetic) {}
};

template <size_t, class Mutex = std::mutex>
using ElementTestLock = ElementLock<Mutex>;

void Check(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

Range Normalize(Range range, size_t size) {
  range.first = std::min(range.first, size - 1);
  range.second = std::min(range.second, size - 1);
  if (range.first > range.second) {
    std::swap(range.first, range.second);
  }
  return range;
}

void Increment(std::vector<size_t> &data, size_t left, size_t right) {
  for (size_t index = left; index <= right; ++index) {
    ++data[index];
  }
}

void CheckData(const std::vector<size_t> &actual,
               const std::vector<size_t> &expected) {
  for (size_t index = 0; index < actual.size(); ++index) {
    Check(actual[index] == expected[index],
          "lost or duplicated update at index " + std::to_string(index) +
              ": expected " + std::to_string(expected[index]) + ", got " +
              std::to_string(actual[index]));
  }
}

template <class Lock> void CheckRangeSemantics(size_t size) {
  Lock lock(size);
  const size_t maximum = std::numeric_limits<size_t>::max();
  const std::vector<Range> ranges{
      {0, 0}, {0, maximum}, {maximum, 0}, {maximum, maximum},
      {size / 2, size / 2}, {size, size + 7}, {size / 2, 0}};
  std::vector<size_t> actual(size, 0), expected(size, 0);
  for (const auto range : ranges) {
    size_t calls = 0;
    lock.WriteQuery(range.first, range.second, [&](size_t left, size_t right) {
      ++calls;
      Check(size != 0, "empty array must not invoke callback");
      Check(Range{left, right} == Normalize(range, size),
            "callback must receive clamped, sorted, inclusive endpoints");
      Increment(actual, left, right);
    });
    Check(calls == (size == 0 ? 0 : 1), "callback invocation count");
    if (size == 0) {
      Check(lock.CountLocksForRange(range.first, range.second) == 0,
            "empty array must require no locks");
    } else {
      const auto [left, right] = Normalize(range, size);
      Increment(expected, left, right);
      Check(lock.CountLocksForRange(range.first, range.second) ==
                lock.CountLocksForRange(left, right),
            "lock counting must use the same endpoint normalization");
      Check(lock.CountLocksForRange(left, left) == 1,
            "a point must require exactly one lock");
    }
  }
  CheckData(actual, expected);
}

template <class Lock> void CheckExceptionUnlock() {
  Lock lock(257);
  std::vector<size_t> actual(257, 0);
  for (const Range range : {Range{128, 128}, Range{0, 256}}) {
    bool caught = false;
    try {
      lock.WriteQuery(range.first, range.second, [](size_t, size_t) {
        throw std::runtime_error("callback exception");
      });
    } catch (const std::runtime_error &error) {
      caught = std::string(error.what()) == "callback exception";
    }
    Check(caught, "callback exception must propagate");
    // A leaked point or range lock deadlocks here; CTest provides a timeout.
    lock.WriteQuery(0, 256, [&](size_t left, size_t right) {
      Increment(actual, left, right);
    });
  }
  CheckData(actual, std::vector<size_t>(257, 2));
}

class ThrowingMutex {
public:
  static inline thread_local int successful_locks_before_throw = -1;
  static inline thread_local size_t held = 0;

  void lock() {
    if (successful_locks_before_throw == 0) {
      throw std::runtime_error("mutex acquisition failure");
    }
    if (successful_locks_before_throw > 0) {
      --successful_locks_before_throw;
    }
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

template <class Lock> void CheckPartialAcquisitionUnlock() {
  Lock lock(257);
  bool called = false;
  bool caught = false;
  ThrowingMutex::successful_locks_before_throw = 2;
  try {
    lock.WriteQuery(0, 256, [&](size_t, size_t) { called = true; });
  } catch (const std::runtime_error &error) {
    caught = std::string(error.what()) == "mutex acquisition failure";
  }
  ThrowingMutex::successful_locks_before_throw = -1;
  Check(caught && !called, "partial lock failure must propagate without a callback");
  Check(ThrowingMutex::held == 0, "partial acquisition must release earlier locks");
  lock.WriteQuery(0, 256, [&](size_t, size_t) { called = true; });
  Check(called && ThrowingMutex::held == 0,
        "all locks must be reusable after an acquisition failure");
}

std::vector<Range> MakeQueries(size_t worker, size_t size, size_t operations) {
  std::vector<Range> queries;
  queries.reserve(operations);
  for (size_t operation = 0; operation < operations; ++operation) {
    const size_t point = (operation * 37 + worker * 53) % size;
    switch (operation % 8) {
    case 0:
      queries.emplace_back(point, point);
      break;
    case 1:
      queries.emplace_back(0, size - 1);
      break;
    case 2:
      queries.emplace_back(size / 2 - (size > 1), size / 2);
      break;
    case 3:
      queries.emplace_back(size - 1, point); // Reversed endpoints.
      break;
    case 4:
      queries.emplace_back(point, size + 9); // Clamped endpoint.
      break;
    default:
      queries.emplace_back(point, std::min(point + 7, size - 1));
      break;
    }
  }
  return queries;
}

template <class Lock> void CheckConcurrentUpdates(size_t size) {
  constexpr size_t workers = 6;
  Lock lock(size);
  std::vector<size_t> actual(size, 0), expected(size, 0);
  std::vector<std::vector<Range>> queries;
  for (size_t worker = 0; worker < workers; ++worker) {
    queries.push_back(MakeQueries(worker, size, 900));
    for (const auto range : queries.back()) {
      const auto [left, right] = Normalize(range, size);
      Increment(expected, left, right);
    }
  }
  std::barrier start(static_cast<std::ptrdiff_t>(workers));
  std::vector<std::thread> threads;
  for (size_t worker = 0; worker < workers; ++worker) {
    threads.emplace_back([&, worker] {
      start.arrive_and_wait();
      for (const auto [left, right] : queries[worker]) {
        lock.WriteQuery(left, right, [&](size_t first, size_t last) {
          Increment(actual, first, last);
        });
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  CheckData(actual, expected);
}

template <template <size_t, class> class Lock> void CheckBasics() {
  for (const size_t size : {size_t{0}, size_t{1}, size_t{3}, size_t{17},
                            size_t{257}}) {
    CheckRangeSemantics<Lock<1, std::mutex>>(size);
    CheckRangeSemantics<Lock<4, std::mutex>>(size);
    CheckRangeSemantics<Lock<32, std::mutex>>(size);
  }
  CheckExceptionUnlock<Lock<8, std::mutex>>();
  CheckPartialAcquisitionUnlock<Lock<8, ThrowingMutex>>();
  CheckConcurrentUpdates<Lock<1, std::mutex>>(17);
  CheckConcurrentUpdates<Lock<4, std::mutex>>(257);
  CheckConcurrentUpdates<Lock<32, std::mutex>>(3);
}

template <PartitionStrategy strategy> void CheckRebuilderLifecycle() {
  DynamicLock<8> lock(257, 1, strategy);
  lock.StopRebuilder();
  lock.RebuildNow(); // No observations yet.
  for (size_t cycle = 0; cycle < 4; ++cycle) {
    lock.StartRebuilder(1ms);
    lock.StartRebuilder(1ms); // Starting an active rebuilder is idempotent.
    for (size_t operation = 0; operation < 512; ++operation) {
      const size_t point = (cycle * 53 + operation % 16) % 257;
      lock.WriteQuery(point, point, [](size_t, size_t) {});
    }
    lock.RebuildNow();
    lock.StopRebuilder();
    lock.StopRebuilder();
  }
  // Destruction must also stop and join an active rebuilder.
  lock.StartRebuilder(1ms);
  DynamicLock<8> empty(0, 1, strategy);
  empty.StartRebuilder(1ms);
  empty.RebuildNow();
}

template <size_t locks>
void CheckPartitionCoverage(const DynamicLock<locks> &lock, size_t size) {
  const auto cuts = lock.PartitionCuts();
  Check(!cuts.empty(), "partition cuts must contain an origin");
  Check(cuts.front() == 0 && cuts.back() == size,
        "partition cuts must cover the complete array");
  Check(cuts.size() <= locks + 1, "partition count exceeds available locks");
  Check(std::adjacent_find(cuts.begin(), cuts.end(),
                           [](size_t left, size_t right) { return left >= right; }) ==
            cuts.end(),
        "partitions must have strictly increasing endpoints");
  for (size_t point = 0; point < size; ++point) {
    Check(lock.CountLocksForRange(point, point) == 1,
          "partitions must cover every point exactly once");
    const size_t right = std::min(point + 117, size - 1);
    const auto first = std::upper_bound(cuts.begin(), cuts.end(), point);
    const auto last = std::upper_bound(cuts.begin(), cuts.end(), right);
    Check(lock.CountLocksForRange(point, right) ==
              static_cast<size_t>(last - first) + 1,
          "range lock count disagrees with published partition cuts");
  }
}

template <PartitionStrategy strategy> void CheckManualRebuild() {
  for (const size_t size : {size_t{0}, size_t{1}, size_t{3}, size_t{257}}) {
    DynamicLock<16> lock(size, 1, strategy);
    CheckPartitionCoverage(lock, size);
    for (size_t operation = 0; operation < 512; ++operation) {
      lock.WriteQuery(0, 0, [](size_t, size_t) {});
    }
    lock.RebuildNow();
    CheckPartitionCoverage(lock, size);
  }

  DynamicLock<16> lock(4099, 1, strategy);
  const auto initial = lock.PartitionCuts();
  for (size_t operation = 0; operation < 4096; ++operation) {
    const size_t point = operation % 96;
    lock.WriteQuery(point, point, [](size_t, size_t) {});
  }
  lock.RebuildNow();
  Check(lock.GetRebuildCount() > 0 && lock.PartitionCuts() != initial,
        "manual rebuild must adapt to a sampled hotspot without a background thread");
  CheckPartitionCoverage(lock, 4099);
  const auto adapted = lock.PartitionCuts();
  const auto rebuilds = lock.GetRebuildCount();
  lock.RebuildNow();
  Check(lock.PartitionCuts() == adapted && lock.GetRebuildCount() == rebuilds,
        "rebuilding without new observations must leave the partition unchanged");
}

template <PartitionStrategy strategy> void CheckRebuildAcquisitionFailure() {
  DynamicLock<8, ThrowingMutex> lock(257, 1, strategy);
  for (size_t operation = 0; operation < 1024; ++operation) {
    lock.WriteQuery(operation % 16, operation % 16, [](size_t, size_t) {});
  }
  bool caught = false;
  ThrowingMutex::successful_locks_before_throw = 2;
  try {
    lock.RebuildNow();
  } catch (const std::runtime_error &error) {
    caught = std::string(error.what()) == "mutex acquisition failure";
  }
  ThrowingMutex::successful_locks_before_throw = -1;
  Check(caught && ThrowingMutex::held == 0,
        "failed snapshot must release all acquired mutexes");
  bool called = false;
  lock.WriteQuery(0, 256, [&](size_t, size_t) { called = true; });
  Check(called, "failed snapshot must resume query admission");
  lock.RebuildNow();
  Check(lock.GetRebuildCount() > 0,
        "rebuilder must remain usable after mutex acquisition failure");
}

template <PartitionStrategy strategy> void CheckRangeSampling() {
  DynamicLock<16> single(4099, 1, strategy);
  for (size_t operation = 0; operation < 1024; ++operation) {
    single.WriteQuery(100, 299, [](size_t, size_t) {});
  }
  single.RebuildNow();
  Check(single.GetRebuildCount() > 0,
        "a concentrated range workload must change the partition");
  Check(single.CountLocksForRange(120, 280) >= 8,
        "range statistics must count covered bins, not only range starts");
  CheckPartitionCoverage(single, 4099);

  DynamicLock<16> separated(4099, 1, strategy);
  for (size_t operation = 0; operation < 2048; ++operation) {
    const size_t left = operation % 2 == 0 ? 100 : 2700;
    separated.WriteQuery(left, left + 199, [](size_t, size_t) {});
  }
  separated.RebuildNow();
  Check(separated.GetRebuildCount() > 0,
        "separated range clusters must change the partition");
  Check(separated.CountLocksForRange(120, 280) >= 4 &&
            separated.CountLocksForRange(2720, 2880) >= 4,
        "both separated clusters must contribute covered-bin weights");
  CheckPartitionCoverage(separated, 4099);
}

template <PartitionStrategy strategy> void CheckConcurrentRebuilds() {
  constexpr size_t size = 4099; // Neither a power of two nor divisible by locks.
  constexpr size_t workers = 6;
  constexpr size_t rounds = 12;
  constexpr size_t operations = 600;
  DynamicLock<16> lock(size, 1, strategy);
  std::vector<size_t> actual(size, 0), expected(size, 0);
  std::vector<std::vector<Range>> queries(workers);
  for (size_t worker = 0; worker < workers; ++worker) {
    for (size_t round = 0; round < rounds; ++round) {
      const size_t base = (round % 3) * 1300;
      for (size_t operation = 0; operation < operations; ++operation) {
        size_t left = base + (operation * 13 + worker * 7) % 96;
        size_t right = left + operation % 5;
        if (operation % 23 == 0) {
          left = 250;
          right = 3100; // Overlap across both old and new partitions.
        }
        queries[worker].emplace_back(left, right);
        Increment(expected, left, right);
      }
    }
  }

  std::barrier phase(static_cast<std::ptrdiff_t>(workers + 2));
  std::vector<std::thread> threads;
  lock.StartRebuilder(1ms);
  for (size_t worker = 0; worker < workers; ++worker) {
    threads.emplace_back([&, worker] {
      for (size_t round = 0; round < rounds; ++round) {
        phase.arrive_and_wait();
        for (size_t operation = 0; operation < operations; ++operation) {
          const auto [left, right] = queries[worker][round * operations + operation];
          lock.WriteQuery(left, right, [&](size_t first, size_t last) {
            Increment(actual, first, last);
            if (operation % 31 == 0) {
              std::this_thread::yield();
            }
          });
        }
        phase.arrive_and_wait();
      }
    });
  }
  // Both manual callers compete with the periodic rebuilder and writers.
  threads.emplace_back([&] {
    for (size_t round = 0; round < rounds; ++round) {
      phase.arrive_and_wait();
      lock.RebuildNow();
      lock.RebuildNow();
      phase.arrive_and_wait();
    }
  });
  for (size_t round = 0; round < rounds; ++round) {
    // Seed a changing hotspot before releasing each concurrent phase. These
    // updates are included in the exact expected result, like worker updates.
    for (size_t operation = 0; operation < 1024; ++operation) {
      const size_t point = (round % 3) * 1300 + operation % 96;
      lock.WriteQuery(point, point, [&](size_t left, size_t right) {
        Increment(actual, left, right);
      });
      ++expected[point];
    }
    phase.arrive_and_wait();
    lock.RebuildNow();
    phase.arrive_and_wait();
  }
  for (auto &thread : threads) {
    thread.join();
  }
  lock.StopRebuilder();
  Check(lock.GetRebuildCount() > 0,
        "concurrent correctness test must perform actual partition changes");
  CheckData(actual, expected);
  CheckPartitionCoverage(lock, size);
}

void CheckGeneticSchedule() {
  DynamicLock<16> lock(4096, 1, PartitionStrategy::Genetic);
  const auto feed = [&](size_t base) {
    for (size_t operation = 0; operation < 4096; ++operation) {
      const size_t point = base + operation % 64;
      lock.WriteQuery(point, point, [](size_t, size_t) {});
    }
  };
  lock.StartRebuilder(); // 30 seconds; sampling must not trigger eager training.
  feed(0);
  lock.StopRebuilder(); // Must interrupt the wait rather than wait 30 seconds.
  Check(lock.GetOptimizationCount() == 0 && lock.GetRebuildCount() == 0,
        "genetic ignored the rebuild interval");
  Check(lock.RebuildNow(), "genetic did not learn the first hotspot");
  const auto searches = lock.GetOptimizationCount();
  const auto cuts = lock.PartitionCuts();
  feed(0);
  Check(!lock.RebuildNow() && lock.GetOptimizationCount() == searches &&
            lock.PartitionCuts() == cuts,
        "stable optimal traffic should skip genetic search and publication");
  feed(4032);
  Check(lock.RebuildNow() && lock.PartitionCuts() != cuts,
        "genetic failed to adapt to a shifted hotspot");
  feed(0);
  Check(lock.RebuildNow() && lock.PartitionCuts() == cuts,
        "genetic failed to adapt when the hotspot returned");
}

} // namespace

int main() {
  constexpr auto dp = PartitionStrategy::DynamicProgramming;
  constexpr auto balanced = PartitionStrategy::Balanced;
  constexpr auto genetic = PartitionStrategy::Genetic;
  const std::vector<std::pair<const char *, void (*)()>> tests{
      {"fixed: inclusive endpoints, edge sizes, exceptions and concurrency",
       &CheckBasics<NaiveLock>},
      {"per element: inclusive endpoints, exceptions, partial acquisition and concurrency",
       &CheckBasics<ElementTestLock>},
      {"dynamic DP: inclusive endpoints, edge sizes, exceptions and concurrency",
       &CheckBasics<DynamicLock>},
      {"dynamic balanced: inclusive endpoints, edge sizes, exceptions and concurrency",
       &CheckBasics<BalancedLock>},
      {"genetic: inclusive endpoints, edge sizes, exceptions and concurrency",
       &CheckBasics<GeneticTestLock>},
      {"genetic: interval, stable traffic, shift and return", &CheckGeneticSchedule},
      {"genetic: rebuilder start, stop, restart and destruction", &CheckRebuilderLifecycle<genetic>},
      {"genetic: manual rebuild and complete partition coverage", &CheckManualRebuild<genetic>},
      {"genetic: rebuild snapshot mutex acquisition failure", &CheckRebuildAcquisitionFailure<genetic>},
      {"genetic: complete range and separated cluster sampling", &CheckRangeSampling<genetic>},
      {"genetic: concurrent writes, manual rebuilds and periodic rebuilds",
       &CheckConcurrentRebuilds<genetic>},
      {"DP: rebuilder start, stop, restart and destruction", &CheckRebuilderLifecycle<dp>},
      {"balanced: rebuilder start, stop, restart and destruction", &CheckRebuilderLifecycle<balanced>},
      {"DP: manual rebuild and complete partition coverage", &CheckManualRebuild<dp>},
      {"balanced: manual rebuild and complete partition coverage", &CheckManualRebuild<balanced>},
      {"DP: rebuild snapshot mutex acquisition failure", &CheckRebuildAcquisitionFailure<dp>},
      {"balanced: rebuild snapshot mutex acquisition failure", &CheckRebuildAcquisitionFailure<balanced>},
      {"DP: complete range and separated cluster sampling", &CheckRangeSampling<dp>},
      {"balanced: complete range and separated cluster sampling", &CheckRangeSampling<balanced>},
      {"DP: concurrent writes, manual rebuilds and periodic rebuilds",
       &CheckConcurrentRebuilds<dp>},
      {"balanced: concurrent writes, manual rebuilds and periodic rebuilds",
       &CheckConcurrentRebuilds<balanced>}};
  size_t failures = 0;
  for (const auto &[name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}
