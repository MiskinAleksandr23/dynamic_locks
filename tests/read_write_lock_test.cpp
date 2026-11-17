#include "common/naive_lock.hpp"
#include "common/element_lock.hpp"
#include "common/spinlock.hpp"
#include "dynamic/dynamic_lock.hpp"

#include <atomic>
#include <barrier>
#include <iostream>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <vector>

void Check(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

template <class Lock> void SharedReaders(Lock &lock) {
  // This deadlocks (CTest timeout) if a read acquires an exclusive lock.
  std::barrier together(2);
  std::vector<std::thread> readers;
  for (int i = 0; i < 2; ++i)
    readers.emplace_back([&] {
      lock.ReadQuery(0, 63, [&](size_t, size_t) { together.arrive_and_wait(); });
    });
  for (auto &reader : readers) reader.join();
  bool threw = false;
  try {
    lock.ReadQuery(0, 63, [](size_t, size_t) { throw std::runtime_error("callback"); });
  } catch (const std::runtime_error &) { threw = true; }
  Check(threw, "read exception not propagated");
  lock.WriteQuery(0, 63, [](size_t, size_t) {}); // No leaked shared locks.
}

template <class Lock> void Mixed(Lock &lock) {
  std::vector<uint64_t> data(4096);
  // Read traffic alone must contribute to the adaptive histogram.
  for (int i = 0; i < 4000; ++i) lock.ReadQuery(0, 63, [](size_t, size_t) {});
  if constexpr (requires { lock.RebuildNow(); }) {
    Check(lock.RebuildNow(), "read-only traffic must cause adaptation");
    lock.StartRebuilder(std::chrono::milliseconds(1));
  }
  std::atomic<bool> failed{false};
  std::barrier gate(6);
  std::vector<std::thread> threads;
  for (size_t worker = 0; worker < 6; ++worker) {
    threads.emplace_back([&, worker] {
      gate.arrive_and_wait();
      for (size_t iteration = 0; iteration < 2000; ++iteration) {
        const size_t begin = (iteration % 2) * 2048;
        if (worker < 2) {
          lock.WriteQuery(begin, begin + 63, [&](size_t left, size_t right) {
            for (size_t i = left; i <= right; ++i) {
              ++data[i];
              if (i == left + 32) std::this_thread::yield();
            }
          });
        } else {
          lock.ReadQuery(begin, begin + 63, [&](size_t left, size_t right) {
            for (size_t i = left; i <= right; ++i)
              if (data[i] != data[left]) failed.store(true);
          });
        }
      }
    });
  }
  for (auto &thread : threads) thread.join();
  if constexpr (requires { lock.StopRebuilder(); }) lock.StopRebuilder();
  Check(!failed, "reader observed a partially completed range write");
  for (size_t i = 0; i < data.size(); ++i)
    Check(data[i] == (i < 64 || (i >= 2048 && i < 2112) ? 2000u : 0u), "incorrect final data");
}

template <class Mutex> void CheckBackend() {
  NaiveLock<64, Mutex> fixed(4096);
  Mixed(fixed);
  for (auto strategy : {PartitionStrategy::Balanced, PartitionStrategy::DynamicProgramming}) {
    DynamicLock<64, Mutex> adaptive(4096, 1, strategy);
    Mixed(adaptive);
  }
}

struct ThrowingShared {
  static inline int held = 0;
  void lock_shared() { if (held == 2) throw std::runtime_error("acquire"); ++held; }
  void unlock_shared() { --held; }
};

int main() {
  try {
    NaiveLock<64, std::shared_mutex> fixed(4096);
    DynamicLock<64, std::shared_mutex> adaptive(4096, 1);
    ElementLock<std::shared_mutex> element(4096);
    SharedReaders(fixed);
    SharedReaders(adaptive);
    SharedReaders(element);
    struct Slot { ThrowingShared mutex; };
    std::array<Slot, 4> slots;
    bool threw = false;
    try { dynamic_lock_detail::RangeLockGuard<decltype(slots), true> guard(slots, 0, 3); }
    catch (const std::runtime_error &) { threw = true; }
    Check(threw && ThrowingShared::held == 0, "shared acquisition failure leaked locks");
    CheckBackend<std::mutex>();
    CheckBackend<spinlock>();
    CheckBackend<std::shared_mutex>();
    std::cout << "PASS shared reader overlap, read statistics, exceptions, mixed atomic ranges and concurrent rebuilds\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
