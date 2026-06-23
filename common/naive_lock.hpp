#pragma once

#include "common/page_aligned_mutex.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>

template <size_t kLockCnt, typename Mutex = std::mutex> class NaiveLock {
public:
  explicit NaiveLock(size_t array_size)
      : array_size_(array_size),
        block_size_(array_size == 0 ? 1
                                    : (array_size + kLockCnt - 1) / kLockCnt),
        total_lock_time_(std::chrono::nanoseconds(0)) {}

  template <typename Func>
  void WriteQuery(size_t left, size_t right, Func &&func) {
    if (array_size_ == 0) {
      return;
    }

    left = std::min(left, array_size_ - 1);
    right = std::min(right, array_size_ - 1);
    if (left > right) {
      std::swap(left, right);
    }

    const auto start = std::chrono::steady_clock::now();
    const size_t first_lock = std::min(left / block_size_, kLockCnt - 1);
    const size_t last_lock = std::min(right / block_size_, kLockCnt - 1);

    if (first_lock == last_lock) {
      std::lock_guard guard(locks_[first_lock]);
      AddDuration(total_lock_time_, std::chrono::steady_clock::now() - start);
      ++operation_count_;
      func(left, right);
      return;
    }

    RangeLockGuard guard(locks_, first_lock, last_lock);

    AddDuration(total_lock_time_, std::chrono::steady_clock::now() - start);
    ++operation_count_;
    func(left, right);
  }

  void StartRebuilder(std::chrono::milliseconds, double) {}

  void StopRebuilder() {}

  void ForceSaveStats() {}

  double GetAvgLockTimeMs() const {
    const size_t operations = operation_count_.load();
    if (operations == 0) {
      return 0.0;
    }

    const auto total = total_lock_time_.load();
    return std::chrono::duration<double, std::milli>(total).count() /
           operations;
  }

  double GetTotalLockTimeMs() const {
    const auto total = total_lock_time_.load();
    return std::chrono::duration<double, std::milli>(total).count();
  }

  void ResetRuntimeStats() {
    operation_count_.store(0, std::memory_order_relaxed);
    total_lock_time_.store(std::chrono::nanoseconds(0),
                           std::memory_order_relaxed);
  }

  size_t GetRebuildCount() const { return 0; }
  size_t GetOperationCount() const { return operation_count_.load(); }

  size_t CountLocksForRange(size_t left, size_t right) const {
    if (array_size_ == 0) {
      return 0;
    }

    left = std::min(left, array_size_ - 1);
    right = std::min(right, array_size_ - 1);
    if (left > right) {
      std::swap(left, right);
    }

    const size_t first_lock = std::min(left / block_size_, kLockCnt - 1);
    const size_t last_lock = std::min(right / block_size_, kLockCnt - 1);
    return last_lock - first_lock + 1;
  }

private:
  using LockSlot = PageAlignedMutex<Mutex>;

  const size_t array_size_;
  const size_t block_size_;

  std::array<LockSlot, kLockCnt> locks_;
  std::atomic<size_t> operation_count_{0};
  std::atomic<std::chrono::nanoseconds> total_lock_time_;

  class RangeLockGuard {
  public:
    RangeLockGuard(std::array<LockSlot, kLockCnt> &locks, size_t first,
                   size_t last)
        : locks_(locks), first_(first) {
      try {
        for (size_t index = first; index <= last; ++index) {
          locks_[index].lock();
          ++locked_count_;
        }
      } catch (...) {
        UnlockAll();
        throw;
      }
    }

    RangeLockGuard(const RangeLockGuard &) = delete;
    RangeLockGuard &operator=(const RangeLockGuard &) = delete;

    ~RangeLockGuard() { UnlockAll(); }

  private:
    std::array<LockSlot, kLockCnt> &locks_;
    size_t first_;
    size_t locked_count_ = 0;

    void UnlockAll() {
      while (locked_count_ > 0) {
        --locked_count_;
        locks_[first_ + locked_count_].unlock();
      }
    }
  };

  static void AddDuration(std::atomic<std::chrono::nanoseconds> &target,
                          std::chrono::nanoseconds delta) {
    auto current = target.load(std::memory_order_relaxed);
    while (!target.compare_exchange_weak(current, current + delta,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
  }
};
