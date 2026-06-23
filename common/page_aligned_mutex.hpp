#pragma once

#include <cstddef>

template <typename Mutex, size_t Alignment = 4096>
struct alignas(Alignment) PageAlignedMutex {
  Mutex mutex;

  void lock() { mutex.lock(); }
  void unlock() { mutex.unlock(); }
};
