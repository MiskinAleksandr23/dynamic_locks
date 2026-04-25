#pragma once

#include <atomic>

struct alignas(64) spinlock {
  void lock();

  void unlock();

private:
  std::atomic<bool> locked_ = false;
};
