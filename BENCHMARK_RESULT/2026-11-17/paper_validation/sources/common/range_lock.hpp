#pragma once

#include <cstddef>
#include <cstdint>

namespace dynamic_lock_detail {

// Identical spacing for fixed and adaptive locks, including 128-byte cache lines.
template <class Mutex> struct alignas(128) LockSlot {
  // Keep this beside the mutex's control fields. Putting it AFTER a 64-byte
  // std::mutex made sampling a second contended write region on our ARM host.
  uint64_t sampling_state = 1; // DynamicLock only; protected by this mutex.
  Mutex mutex;
};

// Writers and the rebuilder always acquire in increasing order.
template <class Slots, bool Read = false> class RangeLockGuard {
public:
  RangeLockGuard(Slots &slots, size_t first, size_t last)
      : slots_(slots), first_(first), end_(first) {
    try {
      while (end_ <= last) {
        if constexpr (Read && requires { slots_[end_].mutex.lock_shared(); })
          slots_[end_].mutex.lock_shared();
        else
          slots_[end_].mutex.lock();
        ++end_;
      }
    } catch (...) {
      Unlock();
      throw;
    }
  }
  ~RangeLockGuard() { Unlock(); }
  RangeLockGuard(const RangeLockGuard &) = delete;
  RangeLockGuard &operator=(const RangeLockGuard &) = delete;

private:
  void Unlock() {
    while (end_ > first_) {
      --end_;
      if constexpr (Read && requires { slots_[end_].mutex.unlock_shared(); })
        slots_[end_].mutex.unlock_shared();
      else
        slots_[end_].mutex.unlock();
    }
  }
  Slots &slots_;
  size_t first_;
  size_t end_;
};

} // namespace dynamic_lock_detail
