#include "common/srl_array_lock.hpp"

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
struct Operation { bool write; size_t left, right; };

void Check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

template <class Callback>
void Query(SrlArrayLock &lock, Operation operation, Callback &&callback) {
  if (operation.write) lock.WriteQuery(operation.left, operation.right, callback);
  else lock.ReadQuery(operation.left, operation.right, callback);
}

void BoundsAndConfiguration() {
  for (size_t kib : {size_t{4}, size_t{64}, size_t{256}}) {
    const size_t segment_elements = kib * 1024 / sizeof(uint64_t);
    for (size_t size : {size_t{0}, size_t{1}, segment_elements - 1,
                        segment_elements, segment_elements + 1, 2 * segment_elements + 3}) {
      SrlArrayLock lock(size, kib);
      Check(lock.GetSegmentKiB() == kib, "incorrect configured segment size");
      Check(lock.GetSegmentCount() == (size == 0 ? 0 : 1 + (size - 1) / segment_elements),
            "segment count must round up byte extent, including a partial final segment");
      Check(lock.GetRebuildCount() == 0, "static SRL unexpectedly rebuilds");
      std::vector<uint64_t> actual(size), expected(size);
      const auto maximum = std::numeric_limits<size_t>::max();
      for (const Range range : {Range{0, 0}, Range{0, maximum}, Range{maximum, 0},
                                Range{maximum, maximum}, Range{size / 2, 0},
                                Range{size, size + 7}}) {
        for (bool write : {true, false}) {
          size_t calls = 0;
          Query(lock, {write, range.first, range.second}, [&](size_t left, size_t right) {
            ++calls;
            Check(size != 0, "empty array invoked a callback");
            auto a = std::min(range.first, size - 1), b = std::min(range.second, size - 1);
            if (a > b) std::swap(a, b);
            Check(left == a && right == b, "endpoints must be clamped, sorted and inclusive");
            for (size_t index = left; index <= right; ++index) {
              if (write) ++actual[index];
              else Check(actual[index] == expected[index], "read disagrees with reference");
            }
          });
          Check(calls == (size ? 1 : 0), "callback must run exactly once for a nonempty array");
          if (write && size) {
            auto left = std::min(range.first, size - 1), right = std::min(range.second, size - 1);
            if (left > right) std::swap(left, right);
            for (size_t index = left; index <= right; ++index) ++expected[index];
          }
        }
      }
      Check(actual == expected, "normalized updates differ from independent reference");
    }
  }
  for (size_t kib : {size_t{0}, size_t{1}, size_t{6}}) {
    bool rejected = false;
    try { SrlArrayLock invalid(1024, kib); }
    catch (const std::invalid_argument &) { rejected = true; }
    Check(rejected, "invalid segment size accepted");
  }
  bool overflow_rejected = false;
  try { SrlArrayLock invalid(std::numeric_limits<size_t>::max(), 4); }
  catch (const std::length_error &) { overflow_rejected = true; }
  Check(overflow_rejected, "overflowing array byte extent accepted");
  overflow_rejected = false;
  try { SrlArrayLock invalid(1, std::numeric_limits<size_t>::max() - 3); }
  catch (const std::length_error &) { overflow_rejected = true; }
  Check(overflow_rejected, "overflowing segment byte extent accepted");
}

void CompatiblePair(Operation first, Operation second) {
  SrlArrayLock lock(2049, 4);
  std::barrier inside(2);
  auto run = [&](Operation operation) {
    Query(lock, operation, [&](size_t, size_t) {
      // Deterministically detects serialization of compatible requests.
      // CTest supplies a timeout if a broken implementation deadlocks here.
      inside.arrive_and_wait();
    });
  };
  std::thread a(run, first), b(run, second);
  a.join(); b.join();
}

void ExcludedPair(Operation first, Operation second) {
  SrlArrayLock lock(2049, 4);
  std::promise<void> attempting, completed;
  auto attempted = attempting.get_future(), done = completed.get_future();
  std::atomic<bool> owner_inside{false}, overlapped{false};
  std::thread contender;
  bool blocked = false;
  Query(lock, first, [&](size_t, size_t) {
    owner_inside = true;
    contender = std::thread([&] {
      attempting.set_value();
      try {
        Query(lock, second, [&](size_t, size_t) {
          if (owner_inside.load()) overlapped = true;
        });
        completed.set_value();
      } catch (...) { completed.set_exception(std::current_exception()); }
    });
    attempted.wait();
    // This bounded check supplements the atomic range and full-array oracle.
    blocked = done.wait_for(5ms) == std::future_status::timeout;
    owner_inside = false;
  });
  contender.join();
  done.get();
  Check(blocked && !overlapped, "conflicting ranges entered callbacks concurrently");
}

void Compatibility() {
  CompatiblePair({false, 0, 2048}, {false, 511, 1536});
  CompatiblePair({false, 511, 1024}, {false, 1024, 1536});
  CompatiblePair({true, 0, 511}, {true, 512, 1535});
  CompatiblePair({false, 0, 511}, {true, 512, 1535});
  CompatiblePair({true, 0, 1023}, {false, 1024, 2048});
  // Both ranges span multiple segments and share exactly endpoint1024.
  for (bool first_write : {false, true}) {
    for (bool second_write : {false, true}) {
      if (first_write || second_write)
        ExcludedPair({first_write, 511, 1024}, {second_write, 1024, 1536});
    }
  }
  ExcludedPair({false, 0, 2048}, {true, 1024, 1024});
  ExcludedPair({true, 1024, 1024}, {false, 0, 2048});
}

void ExceptionRelease() {
  SrlArrayLock lock(2049, 4);
  for (bool write : {false, true}) {
    for (Range range : {Range{0, 2048}, Range{511, 1536}, Range{2048, 2048}}) {
      bool caught = false;
      try {
        Query(lock, {write, range.first, range.second}, [](size_t, size_t) {
          throw std::runtime_error("callback");
        });
      } catch (const std::runtime_error &error) {
        caught = std::string(error.what()) == "callback";
      }
      Check(caught, "callback exception did not propagate");
      // Needs every segment; a leaked shared or exclusive lock deadlocks.
      lock.WriteQuery(0, 2048, [](size_t, size_t) {});
    }
  }
}

void OverlappingWrites(size_t kib) {
  constexpr size_t workers = 4, iterations = 600;
  const size_t size = kib * 1024 / sizeof(uint64_t) + 17;
  SrlArrayLock lock(size, kib);
  std::vector<uint64_t> actual(size), expected(size);
  std::array<std::vector<Range>, workers> traces;
  for (size_t worker = 0; worker < workers; ++worker) {
    for (size_t operation = 0; operation < iterations; ++operation) {
      size_t left = (operation * 37 + worker * 53) % size;
      size_t right = std::min(size - 1, left + 23);
      if (operation % 31 == 0) { left = 0; right = size - 1; }
      for (size_t index = left; index <= right; ++index) ++expected[index];
      traces[worker].emplace_back(operation % 2 ? left : right, operation % 2 ? right : left);
    }
  }
  std::barrier start(static_cast<std::ptrdiff_t>(workers));
  std::vector<std::thread> threads;
  for (size_t worker = 0; worker < workers; ++worker) {
    threads.emplace_back([&, worker] {
      start.arrive_and_wait();
      for (const auto [left, right] : traces[worker])
        lock.WriteQuery(left, right, [&](size_t a, size_t b) {
          for (size_t index = a; index <= b; ++index) ++actual[index];
        });
    });
  }
  for (auto &thread : threads) thread.join();
  Check(actual == expected, "overlapping writes differ from full independent array oracle");
}

void AtomicRangeReads() {
  constexpr size_t size = 1537, iterations = 600;
  SrlArrayLock lock(size, 4);
  std::array<std::atomic<uint64_t>, size> data{};
  std::atomic<bool> inconsistent{false};
  std::barrier start(6);
  std::vector<std::thread> threads;
  for (size_t worker = 0; worker < 6; ++worker) {
    threads.emplace_back([&, worker] {
      start.arrive_and_wait();
      for (size_t operation = 0; operation < iterations; ++operation) {
        if (worker < 2) {
          lock.WriteQuery(0, size - 1, [&](size_t left, size_t right) {
            for (size_t index = left; index <= right; ++index) {
              data[index].fetch_add(1, std::memory_order_relaxed);
              if (index == 512) std::this_thread::yield();
            }
          });
        } else {
          lock.ReadQuery(operation % 8, 1529 + operation % 8, [&](size_t left, size_t right) {
            const auto version = data[left].load(std::memory_order_relaxed);
            for (size_t index = left; index <= right; ++index)
              if (data[index].load(std::memory_order_relaxed) != version) inconsistent = true;
          });
        }
      }
    });
  }
  for (auto &thread : threads) thread.join();
  Check(!inconsistent, "reader observed an unfinished multi-segment write");
  for (const auto &value : data)
    Check(value.load() == 2 * iterations, "incorrect final whole-range update count");
}
} // namespace

int main() {
  try {
    BoundsAndConfiguration();
    Compatibility();
    ExceptionRelease();
    for (size_t kib : {size_t{4}, size_t{64}, size_t{256}}) OverlappingWrites(kib);
    AtomicRangeReads();
    std::cout << "PASS SRL segment sizing, bounds, reader sharing, exclusion, exceptions and full array oracle\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
