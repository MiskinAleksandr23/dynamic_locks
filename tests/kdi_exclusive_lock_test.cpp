#include "common/kdi_exclusive_lock.hpp"

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
using Range = std::pair<size_t, size_t>;
using namespace std::chrono_literals;

void Check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

template <class Function>
void Query(KdiExclusiveLock &lock, bool write, Range range, Function &&function) {
  if (write) lock.WriteQuery(range.first, range.second, function);
  else lock.ReadQuery(range.first, range.second, function);
}

void BoundsAndExceptions() {
  constexpr size_t maximum = std::numeric_limits<size_t>::max();
  for (size_t size : {size_t{0}, size_t{1}, size_t{3}, size_t{257}}) {
    KdiExclusiveLock lock(size, 1);
    for (Range range : {Range{0, maximum}, Range{maximum, 0}, Range{size / 2, 0},
                        Range{maximum, maximum}, Range{size, size + 7}}) {
      for (bool write : {false, true}) {
        size_t calls = 0;
        Query(lock, write, range, [&](size_t left, size_t right) {
          ++calls;
          Check(size != 0, "empty domain called callback");
          auto expected_left = std::min(range.first, size - 1);
          auto expected_right = std::min(range.second, size - 1);
          if (expected_left > expected_right) std::swap(expected_left, expected_right);
          Check(left == expected_left && right == expected_right,
                "inclusive bounds must be clamped then sorted");
        });
        Check(calls == (size != 0), "incorrect callback count");
      }
    }
    if (!size) continue;
    for (bool write : {false, true}) {
      bool caught = false;
      try {
        Query(lock, write, {0, size - 1}, [](size_t, size_t) { throw std::runtime_error("callback"); });
      } catch (const std::runtime_error &error) {
        caught = std::string(error.what()) == "callback";
      }
      Check(caught, "callback exception was not propagated");
      lock.WriteQuery(0, size - 1, [](size_t, size_t) {});
    }
  }
}

void DisjointRanges() {
  for (bool first_write : {false, true}) {
    for (bool second_write : {false, true}) {
      KdiExclusiveLock lock(64, 2);
      std::barrier inside(2);
      auto enter = [&](bool write, Range range) {
        Query(lock, write, range, [&](size_t, size_t) {
          // Adjacent inclusive ranges must enter simultaneously. CTest supplies
          // an overall timeout for an implementation that serializes them.
          inside.arrive_and_wait();
        });
      };
      std::thread left(enter, first_write, Range{0, 31});
      std::thread right(enter, second_write, Range{32, 63});
      left.join();
      right.join();
    }
  }
}

void ExclusiveOverlaps() {
  for (bool first_write : {false, true}) {
    for (bool second_write : {false, true}) {
      KdiExclusiveLock lock(64, 2);
      std::promise<void> attempting, completed;
      auto attempted = attempting.get_future(), done = completed.get_future();
      std::atomic<bool> owner_inside{false}, overlap{false};
      std::thread contender;
      bool blocked = false;
      Query(lock, first_write, {0, 31}, [&](size_t, size_t) {
        owner_inside = true;
        contender = std::thread([&] {
          attempting.set_value();
          try {
            // The single shared endpoint is a conflict. This exclusive variant
            // also serializes two overlapping calls to ReadQuery.
            Query(lock, second_write, {31, 63}, [&](size_t, size_t) {
              if (owner_inside.load()) overlap = true;
            });
            completed.set_value();
          } catch (...) { completed.set_exception(std::current_exception()); }
        });
        attempted.wait();
        blocked = done.wait_for(5ms) == std::future_status::timeout;
        owner_inside = false;
      });
      contender.join();
      done.get();
      Check(blocked && !overlap, "overlapping exclusive intervals entered together");
    }
  }
}

void OverlappingUpdateOracle() {
  constexpr size_t workers = 6, iterations = 1500, size = 257;
  KdiExclusiveLock lock(size, workers);
  std::array<std::vector<Range>, workers> traces;
  std::vector<uint64_t> actual(size), expected(size);
  for (size_t worker = 0; worker < workers; ++worker) {
    for (size_t operation = 0; operation < iterations; ++operation) {
      size_t left = (worker * 47 + operation * 13) % size;
      size_t right = std::min(size - 1, left + 31);
      if (operation % 17 == 0) { left = 0; right = size - 1; }
      for (size_t index = left; index <= right; ++index) ++expected[index];
      traces[worker].emplace_back(operation % 2 ? left : right,
                                  operation % 2 ? right : left);
    }
  }
  std::barrier start(static_cast<std::ptrdiff_t>(workers));
  std::vector<std::thread> threads;
  for (size_t worker = 0; worker < workers; ++worker) {
    threads.emplace_back([&, worker] {
      start.arrive_and_wait();
      for (auto [left, right] : traces[worker]) {
        lock.WriteQuery(left, right, [&](size_t a, size_t b) {
          for (size_t index = a; index <= b; ++index) ++actual[index];
        });
      }
    });
  }
  for (auto &thread : threads) thread.join();
  Check(actual == expected, "partial-overlap updates differ from independent full-array oracle");
}

void AtomicRangeReads() {
  constexpr size_t size = 97, workers = 4, iterations = 500;
  KdiExclusiveLock lock(size, workers);
  std::array<std::atomic<uint64_t>, size> data{};
  std::atomic<bool> inconsistent{false};
  std::barrier start(static_cast<std::ptrdiff_t>(workers));
  std::vector<std::thread> threads;
  for (size_t worker = 0; worker < workers; ++worker) {
    threads.emplace_back([&, worker] {
      start.arrive_and_wait();
      for (size_t operation = 0; operation < iterations; ++operation) {
        if (worker < 2) {
          lock.WriteQuery(0, size - 1, [&](size_t left, size_t right) {
            for (size_t index = left; index <= right; ++index) {
              data[index].fetch_add(1, std::memory_order_relaxed);
              if (index == size / 2) std::this_thread::yield();
            }
          });
        } else {
          lock.ReadQuery(10, 87, [&](size_t left, size_t right) {
            const auto version = data[left].load(std::memory_order_relaxed);
            for (size_t index = left; index <= right; ++index)
              if (data[index].load(std::memory_order_relaxed) != version) inconsistent = true;
          });
        }
      }
    });
  }
  for (auto &thread : threads) thread.join();
  Check(!inconsistent, "reader observed a partially applied range update");
  for (const auto &value : data) Check(value.load() == 2 * iterations, "incorrect final range value");
}
} // namespace

int main() {
  try {
    BoundsAndExceptions();
    DisjointRanges();
    ExclusiveOverlaps();
    OverlappingUpdateOracle();
    AtomicRangeReads();
    std::cout << "PASS KDI exclusive bounds, disjoint ranges, shared endpoint, exceptions and array oracle\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
