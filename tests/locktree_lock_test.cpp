#include "common/locktree_lock.hpp"

#include <atomic>
#include <barrier>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

static void Check(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

int main() {
  try {
    LockTreeRangeLock empty(0);
    empty.ReadQuery(0, 1, [](size_t, size_t) { throw std::runtime_error("empty callback"); });
    empty.WriteQuery(0, 1, [](size_t, size_t) { throw std::runtime_error("empty callback"); });
    LockTreeRangeLock lock(64);
    lock.WriteQuery(std::numeric_limits<size_t>::max(), 0, [](size_t l, size_t r) {
      Check(l == 0 && r == 63, "range normalization failed");
    });
    bool propagated = false;
    try { lock.WriteQuery(0, 63, [](size_t, size_t) { throw std::runtime_error("callback"); }); }
    catch (const std::runtime_error &) { propagated = true; }
    Check(propagated, "callback exception not propagated");
    lock.WriteQuery(0, 63, [](size_t, size_t) {});
    propagated = false;
    try { lock.ReadQuery(0, 63, [](size_t, size_t) { throw std::runtime_error("callback"); }); }
    catch (const std::runtime_error &) { propagated = true; }
    Check(propagated, "read callback exception not propagated");
    lock.WriteQuery(0, 63, [](size_t, size_t) {});

    // The native library shares identical read intervals. Do not falsely require
    // general partially overlapping readers to coexist: this version serializes them.
    std::barrier shared(2);
    std::thread reader1([&] { lock.ReadQuery(0, 31, [&](size_t, size_t) { shared.arrive_and_wait(); }); });
    std::thread reader2([&] { lock.ReadQuery(0, 31, [&](size_t, size_t) { shared.arrive_and_wait(); }); });
    reader1.join(); reader2.join();

    // Adjacent inclusive ranges [0,31] and [32,63] must not conflict.
    std::barrier disjoint(2);
    std::thread writer1([&] { lock.WriteQuery(0, 31, [&](size_t, size_t) { disjoint.arrive_and_wait(); }); });
    std::thread writer2([&] { lock.WriteQuery(32, 63, [&](size_t, size_t) { disjoint.arrive_and_wait(); }); });
    writer1.join(); writer2.join();

    // Sharing just one endpoint is still a conflict for inclusive ranges.
    std::atomic<int> inside{0};
    std::atomic<bool> endpoint_overlap{false};
    std::barrier endpoint_start(2);
    auto endpoint_writer = [&](size_t left, size_t right) {
      endpoint_start.arrive_and_wait();
      for (int i = 0; i < 500; ++i) {
        lock.WriteQuery(left, right, [&](size_t, size_t) {
          if (inside.fetch_add(1) != 0) endpoint_overlap = true;
          std::this_thread::yield();
          inside.fetch_sub(1);
        });
      }
    };
    std::thread endpoint1(endpoint_writer, 0, 31);
    std::thread endpoint2(endpoint_writer, 31, 63);
    endpoint1.join(); endpoint2.join();
    Check(!endpoint_overlap, "writers sharing one endpoint overlapped");

    std::vector<uint64_t> overlap_data(64);
    std::barrier overlap_start(3);
    std::vector<std::thread> overlap_workers;
    const std::pair<size_t, size_t> intervals[] = {{0, 31}, {31, 63}, {10, 52}};
    for (const auto [left, right] : intervals) {
      overlap_workers.emplace_back([&, left, right] {
        overlap_start.arrive_and_wait();
        for (int j = 0; j < 500; ++j)
          lock.WriteQuery(left, right, [&](size_t a, size_t b) {
            for (size_t i = a; i <= b; ++i) ++overlap_data[i];
          });
      });
    }
    for (auto &worker : overlap_workers) worker.join();
    for (size_t i = 0; i < 64; ++i) {
      uint64_t expected = 0;
      for (const auto [left, right] : intervals) if (left <= i && i <= right) expected += 500;
      Check(overlap_data[i] == expected, "incorrect partial-overlap update");
    }

    std::vector<uint64_t> data(64);
    std::atomic<bool> inconsistent{false};
    std::barrier start(6);
    std::vector<std::thread> workers;
    for (size_t w = 0; w < 6; ++w) {
      workers.emplace_back([&, w] {
        start.arrive_and_wait();
        for (int iteration = 0; iteration < 1000; ++iteration) {
          if (w < 2) lock.WriteQuery(0, 63, [&](size_t l, size_t r) {
            for (size_t i = l; i <= r; ++i) {
              ++data[i];
              if (i == 31) std::this_thread::yield();
            }
          });
          else lock.ReadQuery(0, 63, [&](size_t l, size_t r) {
            for (size_t i = l; i <= r; ++i)
              if (data[i] != data[l]) inconsistent = true;
          });
        }
      });
    }
    for (auto &worker : workers) worker.join();
    Check(!inconsistent, "read observed a partial range update");
    for (auto value : data) Check(value == 2000, "incorrect final array");
    std::cout << "PASS LockTree wrapper bounds, exceptions, shared readers, disjoint writers, atomic range updates\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
