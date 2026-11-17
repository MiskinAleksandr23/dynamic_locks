#pragma once

#include "common/range_lock.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>

template <size_t kLockCnt, typename Mutex = std::mutex> class NaiveLock {
  static_assert(kLockCnt > 0);

public:
  explicit NaiveLock(size_t array_size)
      : array_size_(array_size),
        block_size_(array_size == 0 ? 1 : 1 + (array_size - 1) / kLockCnt) {}

  // Inclusive endpoints, clamped to the array and sorted. Empty arrays skip func.
  template <typename Func>
  void WriteQuery(size_t left, size_t right, Func &&func) {
    if (array_size_ == 0) {
      return;
    }
    Normalize(left, right);
    dynamic_lock_detail::RangeLockGuard guard(locks_, left / block_size_,
                                             right / block_size_);
    func(left, right);
  }

  size_t CountLocksForRange(size_t left, size_t right) const {
    if (array_size_ == 0) {
      return 0;
    }
    Normalize(left, right);
    return right / block_size_ - left / block_size_ + 1;
  }

  template <typename Func>
  void ReadQuery(size_t left, size_t right, Func &&func) {
    if (array_size_ == 0) return;
    Normalize(left, right);
    dynamic_lock_detail::RangeLockGuard<decltype(locks_), true> guard(
        locks_, left / block_size_, right / block_size_);
    func(left, right);
  }

  void StartRebuilder(std::chrono::milliseconds = std::chrono::milliseconds(30'000)) {}
  void StopRebuilder() {}
  size_t GetRebuildCount() const { return 0; }

private:
  void Normalize(size_t &left, size_t &right) const {
    left = std::min(left, array_size_ - 1);
    right = std::min(right, array_size_ - 1);
    if (left > right) {
      std::swap(left, right);
    }
  }

  const size_t array_size_;
  const size_t block_size_;
  std::array<dynamic_lock_detail::LockSlot<Mutex>, kLockCnt> locks_;
};
