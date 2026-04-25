#include "common/spinlock.hpp"

#include <thread>

void spinlock::lock() {
  bool expected = false;

  while (!locked_.compare_exchange_weak(
      expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
    while (locked_.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
    expected = false;
  }
}

void spinlock::unlock() { locked_.store(false, std::memory_order_release); }
