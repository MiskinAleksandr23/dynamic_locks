#pragma once

#include "genetic_partitioner.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

template <size_t kLockCnt, size_t kBlocks = 1024, typename Mutex = std::mutex>
class GeneticLock {
public:
  explicit GeneticLock(size_t array_size, uint64_t seed = 123,
                       size_t training_batch_size = 2048,
                       size_t history_limit = 12000,
                       size_t population_size = 24, size_t elite_count = 4,
                       size_t generation_count = 18)
      : array_size_(array_size),
        block_size_(array_size == 0 ? 1 : 1 + ((array_size - 1) / kBlocks)),
        training_batch_size_(std::max(training_batch_size, size_t{1})),
        partitioner_(array_size, seed, history_limit, population_size,
                     elite_count, generation_count),
        current_cuts_(partitioner_.PartitionCuts()),
        total_lock_time_(std::chrono::nanoseconds(0)),
        total_training_time_(std::chrono::nanoseconds(0)) {}

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

    size_t locked_mutexes = 0;
    {
      const auto start = std::chrono::steady_clock::now();
      const bool guard_reconfiguration =
          training_enabled_.load(std::memory_order_acquire);
      if (guard_reconfiguration) {
        EnterPartitionRead();
      }
      const auto [first_lock, last_lock] =
          FindLocksSegmentUnlocked(left, right);
      locked_mutexes = last_lock - first_lock + 1;

      std::vector<std::unique_lock<Mutex>> locks;
      locks.reserve(locked_mutexes);
      for (size_t lock_index = first_lock; lock_index <= last_lock;
           ++lock_index) {
        locks.emplace_back(locks_[lock_index]);
      }

      AddDuration(total_lock_time_, std::chrono::steady_clock::now() - start);
      operation_count_.fetch_add(1, std::memory_order_relaxed);

      func(left, right);
      if (guard_reconfiguration) {
        LeavePartitionRead();
      }
    }

    if (training_enabled_.load(std::memory_order_relaxed)) {
      MaybeTrain({static_cast<uint32_t>(left), static_cast<uint32_t>(right),
                  static_cast<uint32_t>(left), 0});
    }
  }

  void ObserveBatch(const std::vector<Query> &batch) {
    if (batch.empty()) {
      return;
    }

    std::lock_guard train_lock(training_mutex_);
    TrainOnBatch(batch);
  }

  void StartRebuilder(std::chrono::milliseconds = std::chrono::seconds(60),
                      double = 0.0) {
    training_enabled_.store(true, std::memory_order_release);
  }

  void StopRebuilder() {
    training_enabled_.store(false, std::memory_order_release);
  }

  void ForceSaveStats() {}

  void SetTrainingEnabled(bool enabled) {
    training_enabled_.store(enabled, std::memory_order_release);
  }

  void FlushTraining() {
    std::vector<Query> batch;
    {
      std::lock_guard pending_lock(pending_mutex_);
      batch.swap(pending_queries_);
    }
    ObserveBatch(batch);
  }

  void ResetRuntimeStats() {
    operation_count_.store(0, std::memory_order_relaxed);
    training_count_.store(0, std::memory_order_relaxed);
    total_lock_time_.store(std::chrono::nanoseconds(0),
                           std::memory_order_relaxed);
    total_training_time_.store(std::chrono::nanoseconds(0),
                               std::memory_order_relaxed);
  }

  double GetAvgLockTimeMs() const {
    const size_t operations = operation_count_.load(std::memory_order_relaxed);
    if (operations == 0) {
      return 0.0;
    }

    const auto total = total_lock_time_.load(std::memory_order_relaxed);
    return std::chrono::duration<double, std::milli>(total).count() /
           static_cast<double>(operations);
  }

  double GetTotalLockTimeMs() const {
    const auto total = total_lock_time_.load(std::memory_order_relaxed);
    return std::chrono::duration<double, std::milli>(total).count();
  }

  double GetAvgTrainingTimeMs() const {
    const size_t trainings = training_count_.load(std::memory_order_relaxed);
    if (trainings == 0) {
      return 0.0;
    }

    const auto total = total_training_time_.load(std::memory_order_relaxed);
    return std::chrono::duration<double, std::milli>(total).count() /
           static_cast<double>(trainings);
  }

  size_t GetRebuildCount() const {
    return training_count_.load(std::memory_order_relaxed);
  }

  size_t GetTrainingCount() const {
    return training_count_.load(std::memory_order_relaxed);
  }

  size_t GetOperationCount() const {
    return operation_count_.load(std::memory_order_relaxed);
  }

  size_t CountLocksForRange(size_t left, size_t right) const {
    if (array_size_ == 0) {
      return 0;
    }

    left = std::min(left, array_size_ - 1);
    right = std::min(right, array_size_ - 1);
    if (left > right) {
      std::swap(left, right);
    }

    const bool guard_reconfiguration =
        training_enabled_.load(std::memory_order_acquire);
    if (guard_reconfiguration) {
      EnterPartitionRead();
    }
    const auto [first_lock, last_lock] = FindLocksSegmentUnlocked(left, right);
    const size_t result = last_lock - first_lock + 1;
    if (guard_reconfiguration) {
      LeavePartitionRead();
    }
    return result;
  }

  std::vector<size_t> PartitionCuts() const {
    const bool guard_reconfiguration =
        training_enabled_.load(std::memory_order_acquire);
    if (guard_reconfiguration) {
      EnterPartitionRead();
    }
    std::vector<size_t> result = current_cuts_;
    if (guard_reconfiguration) {
      LeavePartitionRead();
    }
    return result;
  }

private:
  const size_t array_size_;
  const size_t block_size_;
  const size_t training_batch_size_;

  std::array<Mutex, kLockCnt> locks_;
  GeneticPartitioner<kLockCnt, kBlocks> partitioner_;
  std::vector<size_t> current_cuts_;

  std::mutex pending_mutex_;
  std::vector<Query> pending_queries_;
  std::mutex training_mutex_;
  std::atomic<bool> training_enabled_{false};
  mutable std::atomic<bool> pause_queries_{false};
  mutable std::atomic<size_t> active_queries_{0};

  std::atomic<size_t> operation_count_{0};
  std::atomic<size_t> training_count_{0};
  std::atomic<std::chrono::nanoseconds> total_lock_time_;
  std::atomic<std::chrono::nanoseconds> total_training_time_;

  size_t PositionToBlock(size_t position) const {
    return std::min(position / block_size_, kBlocks - 1);
  }

  size_t FindLockForBlockUnlocked(size_t block) const {
    for (size_t i = 0; i < kLockCnt; ++i) {
      if (current_cuts_[i] <= block && block < current_cuts_[i + 1]) {
        return i;
      }
    }
    return kLockCnt - 1;
  }

  std::pair<size_t, size_t> FindLocksSegmentUnlocked(size_t left,
                                                     size_t right) const {
    const size_t left_block = PositionToBlock(left);
    const size_t right_block = PositionToBlock(right);
    const size_t first_lock = FindLockForBlockUnlocked(left_block);
    const size_t last_lock = FindLockForBlockUnlocked(right_block);
    return {first_lock, last_lock};
  }

  static void AddDuration(std::atomic<std::chrono::nanoseconds> &target,
                          std::chrono::nanoseconds delta) {
    auto current = target.load(std::memory_order_relaxed);
    while (!target.compare_exchange_weak(current, current + delta,
                                         std::memory_order_relaxed)) {
    }
  }

  void EnterPartitionRead() const {
    while (true) {
      while (pause_queries_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      active_queries_.fetch_add(1, std::memory_order_acq_rel);
      if (!pause_queries_.load(std::memory_order_acquire)) {
        return;
      }

      active_queries_.fetch_sub(1, std::memory_order_acq_rel);
    }
  }

  void LeavePartitionRead() const {
    active_queries_.fetch_sub(1, std::memory_order_acq_rel);
  }

  void MaybeTrain(const Query &query) {
    std::vector<Query> batch;
    bool should_train = false;
    {
      std::lock_guard pending_lock(pending_mutex_);
      pending_queries_.push_back(query);
      if (pending_queries_.size() >= training_batch_size_ &&
          training_mutex_.try_lock()) {
        batch.swap(pending_queries_);
        should_train = true;
      }
    }

    if (!should_train) {
      return;
    }

    TrainOnBatch(batch);
    training_mutex_.unlock();
  }

  void TrainOnBatch(const std::vector<Query> &batch) {
    const auto start = std::chrono::steady_clock::now();
    partitioner_.ObserveBatch(batch);
    std::vector<size_t> next_cuts = partitioner_.PartitionCuts();

    if (training_enabled_.load(std::memory_order_acquire)) {
      pause_queries_.store(true, std::memory_order_release);
      while (active_queries_.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
      }
    }
    current_cuts_ = std::move(next_cuts);
    pause_queries_.store(false, std::memory_order_release);

    AddDuration(total_training_time_, std::chrono::steady_clock::now() - start);
    training_count_.fetch_add(1, std::memory_order_relaxed);
  }
};
