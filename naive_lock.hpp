#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

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

    std::vector<std::unique_lock<Mutex>> locks;
    locks.reserve(last_lock - first_lock + 1);
    for (size_t lock_index = first_lock; lock_index <= last_lock;
         ++lock_index) {
      locks.emplace_back(locks_[lock_index]);
    }

    const auto lock_time = std::chrono::steady_clock::now() - start;
    auto current = total_lock_time_.load();
    while (
        !total_lock_time_.compare_exchange_weak(current, current + lock_time)) {
    }

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
  const size_t array_size_;
  const size_t block_size_;

  std::array<Mutex, kLockCnt> locks_;
  std::atomic<size_t> operation_count_{0};
  std::atomic<std::chrono::nanoseconds> total_lock_time_;
};
