#include "common/multigranularity_lock.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Range = std::pair<size_t, size_t>;
struct Operation { bool write; size_t left, right; };

void Check(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

template <class Func>
void Query(MultiGranularityRangeLock &lock, Operation operation, Func &&callback) {
  if (operation.write) lock.WriteQuery(operation.left, operation.right, callback);
  else lock.ReadQuery(operation.left, operation.right, callback);
}

void BoundsAndConfiguration() {
  for (const auto [size, expected_leaves] : std::array<Range, 6>{{
           {0, 0}, {1, 1}, {3, 2}, {5, 4}, {17, 8}, {257, 8}}}) {
    MultiGranularityRangeLock lock(size, 8);
    Check(lock.GetLeafCount() == expected_leaves, "incorrect effective leaf count");
    Check(lock.GetNodeCount() == (expected_leaves ? 2 * expected_leaves - 1 : 0),
          "incorrect hierarchy node count");
    Check(lock.GetRebuildCount() == 0, "static hierarchy unexpectedly rebuilds");
    const auto maximum = std::numeric_limits<size_t>::max();
    const std::array<Range, 6> ranges = {{{0, maximum}, {maximum, 0}, {maximum, maximum},
                                        {size / 2, 0}, {size, size + 7}, {0, 0}}};
    std::vector<uint64_t> data(size), expected(size);
    for (auto [left, right] : ranges) {
      size_t calls = 0;
      lock.WriteQuery(left, right, [&](size_t a, size_t b) {
        ++calls;
        Check(size != 0, "empty array invoked a write callback");
        auto x = std::min(left, size - 1), y = std::min(right, size - 1);
        if (x > y) std::swap(x, y);
        Check(a == x && b == y, "write endpoints not clamped/sorted/inclusive");
        for (size_t i = a; i <= b; ++i) ++data[i];
      });
      Check(calls == (size ? 1 : 0), "wrong write callback count");
      if (size) {
        auto a = std::min(left, size - 1), b = std::min(right, size - 1);
        if (a > b) std::swap(a, b);
        for (size_t i = a; i <= b; ++i) ++expected[i];
      }
      calls = 0;
      lock.ReadQuery(left, right, [&](size_t a, size_t b) {
        ++calls;
        Check(size != 0 && a <= b && b < size, "invalid read bounds");
        auto x = std::min(left, size - 1), y = std::min(right, size - 1);
        if (x > y) std::swap(x, y);
        Check(a == x && b == y, "read endpoints not normalized like writes");
        for (size_t i = a; i <= b; ++i) Check(data[i] == expected[i], "read returned wrong data");
      });
      Check(calls == (size ? 1 : 0), "wrong read callback count");
    }
    Check(data == expected, "boundary operations lost updates");
  }
  for (size_t leaves : {size_t{0}, size_t{3}, size_t{6}, size_t{1000}}) {
    bool rejected = false;
    try { MultiGranularityRangeLock invalid(64, leaves); }
    catch (const std::invalid_argument &) { rejected = true; }
    Check(rejected, "nonpositive/non-power-of-two leaf count accepted");
  }
  MultiGranularityRangeLock single_leaf(17, 1);
  Check(single_leaf.GetLeafCount() == 1 && single_leaf.GetNodeCount() == 1,
        "single-node hierarchy unsupported");
}

void CompatiblePair(Operation first, Operation second) {
  MultiGranularityRangeLock lock(64, 8);
  std::barrier both_inside(2);
  std::atomic<unsigned> arrivals{0};
  auto invoke = [&](Operation operation) {
    Query(lock, operation, [&](size_t, size_t) {
      ++arrivals;
      // Serializing compatible modes deadlocks here; CTest supplies a timeout.
      both_inside.arrive_and_wait();
    });
  };
  std::thread a(invoke, first), b(invoke, second);
  a.join(); b.join();
  Check(arrivals == 2, "compatible requests failed to enter together");
}

void ExcludedPair(Operation first, Operation second) {
  MultiGranularityRangeLock lock(64, 8);
  std::promise<void> attempting, completed;
  auto attempted = attempting.get_future();
  auto done = completed.get_future();
  std::atomic<bool> owner_inside{false}, overlap{false};
  bool blocked = false;
  std::thread contender;
  Query(lock, first, [&](size_t, size_t) {
    owner_inside = true;
    contender = std::thread([&] {
      attempting.set_value();
      try {
        Query(lock, second, [&](size_t, size_t) {
          if (owner_inside.load()) overlap = true;
        });
        completed.set_value();
      } catch (...) { completed.set_exception(std::current_exception()); }
    });
    attempted.wait();
    // This bounded observation supplements, rather than replaces, the stress
    // and data-oracle tests below. Do not join a conflicting request while held.
    blocked = done.wait_for(5ms) == std::future_status::timeout;
    owner_inside = false;
  });
  contender.join();
  done.get();
  Check(blocked && !overlap, "conflicting ancestor/descendant requests overlapped");
}

void ModeCompatibility() {
  // Ancestor S with descendant IS/S; unequal overlapping readers must coexist.
  CompatiblePair({false, 0, 63}, {false, 0, 63});
  CompatiblePair({false, 0, 63}, {false, 7, 19});
  CompatiblePair({false, 0, 31}, {false, 3, 7});
  CompatiblePair({false, 1, 39}, {false, 17, 61});
  // Root IX/IX and IS/IX remain compatible for disjoint protected regions.
  CompatiblePair({true, 0, 31}, {true, 32, 63});
  CompatiblePair({true, 0, 0}, {true, 63, 63});
  CompatiblePair({false, 0, 31}, {true, 32, 63});
  // S conflicts with IX, X conflicts with IS/IX, and inclusive shared endpoints conflict.
  for (const auto &[first, second] : std::array<std::pair<Operation, Operation>, 6>{{
           {{false, 0, 63}, {true, 7, 7}},
           {{true, 7, 7}, {false, 0, 63}},
           {{true, 0, 63}, {false, 3, 19}},
           {{false, 3, 19}, {true, 0, 63}},
           {{true, 0, 31}, {true, 31, 63}},
           {{true, 3, 27}, {true, 17, 61}}}}) {
    ExcludedPair(first, second);
  }
}

void ExceptionRelease() {
  MultiGranularityRangeLock lock(64, 8);
  for (bool write : {false, true}) {
    for (Range range : {Range{0, 63}, Range{7, 48}, Range{63, 63}}) {
      bool caught = false;
      try {
        Query(lock, {write, range.first, range.second}, [](size_t, size_t) {
          throw std::runtime_error("callback");
        });
      } catch (const std::runtime_error &error) { caught = std::string(error.what()) == "callback"; }
      Check(caught, "callback exception did not propagate");
      // Root X requires every prior S/X/IS/IX acquisition to have been released.
      lock.WriteQuery(0, 63, [](size_t, size_t) {});
    }
  }
}

void OverlappingWrites(size_t size, size_t leaves, size_t workers) {
  MultiGranularityRangeLock lock(size, leaves);
  constexpr size_t iterations = 1200;
  std::vector<uint64_t> data(size), expected(size);
  std::vector<std::vector<Range>> traces(workers);
  for (size_t worker = 0; worker < workers; ++worker) {
    for (size_t i = 0; i < iterations; ++i) {
      size_t left = (17 * i + 11 * worker) % size;
      size_t right = std::min(size - 1, left + (i % 23));
      if (i % 11 == 0) { left = 0; right = size - 1; }
      for (size_t j = left; j <= right; ++j) ++expected[j];
      // Reverse some user endpoints; the reference stays in physical index order.
      traces[worker].emplace_back(i % 3 ? left : right, i % 3 ? right : left);
    }
  }
  std::barrier start(static_cast<std::ptrdiff_t>(workers));
  std::vector<std::thread> threads;
  for (size_t worker = 0; worker < workers; ++worker) {
    threads.emplace_back([&, worker] {
      start.arrive_and_wait();
      for (const auto [left, right] : traces[worker])
        lock.WriteQuery(left, right, [&](size_t a, size_t b) {
          for (size_t i = a; i <= b; ++i) ++data[i];
        });
    });
  }
  for (auto &thread : threads) thread.join();
  Check(data == expected, "overlapping range writes differ from independent reference");
}

void AtomicRangeReads() {
  MultiGranularityRangeLock lock(64, 8);
  std::array<std::atomic<uint64_t>, 64> data{};
  std::atomic<bool> inconsistent{false};
  std::barrier start(6);
  std::vector<std::thread> threads;
  for (size_t worker = 0; worker < 6; ++worker) {
    threads.emplace_back([&, worker] {
      start.arrive_and_wait();
      for (size_t i = 0; i < 1500; ++i) {
        if (worker < 2) {
          lock.WriteQuery(0, 63, [&](size_t a, size_t b) {
            for (size_t j = a; j <= b; ++j) {
              data[j].fetch_add(1, std::memory_order_relaxed);
              if (j == 31) std::this_thread::yield();
            }
          });
        } else {
          const size_t left = (i + worker) % 8;
          const size_t right = 56 + (i + worker) % 8;
          lock.ReadQuery(left, right, [&](size_t a, size_t b) {
            const auto version = data[a].load(std::memory_order_relaxed);
            for (size_t j = a; j <= b; ++j)
              if (data[j].load(std::memory_order_relaxed) != version) inconsistent = true;
          });
        }
      }
    });
  }
  for (auto &thread : threads) thread.join();
  Check(!inconsistent, "reader observed a partially completed range update");
  for (const auto &value : data) Check(value == 3000, "incorrect final whole-range update count");
}
} // namespace

int main() {
  try {
    BoundsAndConfiguration();
    ModeCompatibility();
    ExceptionRelease();
    for (size_t workers : {size_t{1}, size_t{2}, size_t{4}}) {
      OverlappingWrites(1, 1, workers);
      OverlappingWrites(17, 8, workers);
      OverlappingWrites(257, 64, workers);
    }
    AtomicRangeReads();
    std::cout << "PASS MGL bounds, S/X/IS/IX compatibility, exceptions, overlap oracle and atomic range reads\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
