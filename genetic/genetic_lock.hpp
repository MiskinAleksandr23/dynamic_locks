#pragma once

#include "common/page_aligned_mutex.hpp"
#include "genetic/genetic_partitioner.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
                       size_t generation_count = 18,
                       size_t training_sample_rate = 64,
                       double min_training_skew = 4.0,
                       size_t training_probe_gap = 5'000'000)
      : array_size_(array_size),
        block_size_(array_size == 0 ? 1 : 1 + ((array_size - 1) / kBlocks)),
        training_batch_size_(std::max(training_batch_size, size_t{1})),
        training_sample_rate_(std::max(training_sample_rate, size_t{1})),
        min_training_skew_(std::max(min_training_skew, 1.0)),
        training_probe_gap_(training_probe_gap),
        partitioner_(array_size, seed, history_limit, population_size,
                     elite_count, generation_count),
        current_cuts_(partitioner_.PartitionCuts()),
        total_lock_time_(std::chrono::nanoseconds(0)),
        total_training_time_(std::chrono::nanoseconds(0)) {
    RefreshBlockToLock();
    pending_queries_.reserve(training_batch_size_);
  }

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

    {
      const auto start = std::chrono::steady_clock::now();
      while (true) {
        while (pause_queries_.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }

        const size_t epoch = partition_epoch_.load(std::memory_order_acquire);
        if ((epoch & size_t{1}) != 0) {
          std::this_thread::yield();
          continue;
        }

        const auto [first_lock, last_lock] =
            FindLocksSegmentUnlocked(left, right);
        if (first_lock > last_lock) {
          continue;
        }

        if (first_lock == last_lock) {
          std::lock_guard guard(locks_[first_lock]);
          const size_t current_epoch =
              partition_epoch_.load(std::memory_order_acquire);
          if (epoch != current_epoch || (current_epoch & size_t{1}) != 0) {
            continue;
          }

          AddDuration(total_lock_time_,
                      std::chrono::steady_clock::now() - start);
          operation_count_.fetch_add(1, std::memory_order_relaxed);
          func(left, right);
          break;
        }

        RangeLockGuard guard(locks_, first_lock, last_lock);
        const size_t current_epoch =
            partition_epoch_.load(std::memory_order_acquire);
        if (epoch != current_epoch || (current_epoch & size_t{1}) != 0) {
          continue;
        }

        AddDuration(total_lock_time_, std::chrono::steady_clock::now() - start);
        operation_count_.fetch_add(1, std::memory_order_relaxed);
        func(left, right);
        break;
      }
    }

    if (ShouldSampleTraining()) {
      EnqueueTrainingSample(
          {static_cast<uint32_t>(left), static_cast<uint32_t>(right),
           static_cast<uint32_t>(left), 0});
    }
  }

  void ObserveBatch(const std::vector<Query> &batch) {
    if (batch.empty()) {
      return;
    }

    std::lock_guard train_lock(training_mutex_);
    if (HasEnoughSkew(batch)) {
      TrainOnBatch(batch);
    }
  }

  void StartRebuilder(std::chrono::milliseconds = std::chrono::seconds(60),
                      double = 0.0) {
    if (training_thread_.joinable()) {
      return;
    }
    stop_training_thread_.store(false, std::memory_order_release);
    next_training_probe_operation_.store(training_probe_gap_,
                                         std::memory_order_relaxed);
    training_enabled_.store(true, std::memory_order_release);
    training_thread_ = std::thread([this] { TrainingLoop(); });
  }

  void StopRebuilder() {
    training_enabled_.store(false, std::memory_order_release);
    stop_training_thread_.store(true, std::memory_order_release);
    training_cv_.notify_one();
    if (training_thread_.joinable()) {
      training_thread_.join();
    }
    {
      std::lock_guard pending_lock(pending_mutex_);
      pending_queries_.clear();
    }
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
    if (!batch.empty()) {
      ObserveBatch(batch);
    }
  }

  void ResetRuntimeStats() {
    operation_count_.store(0, std::memory_order_relaxed);
    next_training_probe_operation_.store(0, std::memory_order_relaxed);
    total_lock_time_.store(std::chrono::nanoseconds(0),
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

    while (true) {
      const size_t epoch = partition_epoch_.load(std::memory_order_acquire);
      if ((epoch & size_t{1}) != 0) {
        std::this_thread::yield();
        continue;
      }

      const auto [first_lock, last_lock] = FindLocksSegmentUnlocked(left, right);
      const size_t current_epoch =
          partition_epoch_.load(std::memory_order_acquire);
      if (epoch == current_epoch && (current_epoch & size_t{1}) == 0 &&
          first_lock <= last_lock) {
        return last_lock - first_lock + 1;
      }
    }
  }

  std::vector<size_t> PartitionCuts() const {
    std::lock_guard lock(cuts_mutex_);
    std::vector<size_t> result = current_cuts_;
    return result;
  }

private:
  using LockSlot = PageAlignedMutex<Mutex>;

  const size_t array_size_;
  const size_t block_size_;
  const size_t training_batch_size_;
  const size_t training_sample_rate_;
  const double min_training_skew_;
  const size_t training_probe_gap_;
  static constexpr size_t kLocalTrainingFlushSize = 64;

  std::array<LockSlot, kLockCnt> locks_;
  GeneticPartitioner<kLockCnt, kBlocks> partitioner_;
  std::vector<size_t> current_cuts_;
  std::array<std::atomic<size_t>, kBlocks> block_to_lock_{};

  std::mutex pending_mutex_;
  std::vector<Query> pending_queries_;
  std::condition_variable training_cv_;
  std::mutex training_mutex_;
  mutable std::mutex cuts_mutex_;
  std::atomic<bool> training_enabled_{false};
  std::atomic<bool> stop_training_thread_{false};
  std::atomic<bool> pause_queries_{false};
  std::atomic<size_t> partition_epoch_{0};
  std::thread training_thread_;

  std::atomic<size_t> operation_count_{0};
  std::atomic<size_t> training_count_{0};
  std::atomic<size_t> next_training_probe_operation_{0};
  std::atomic<std::chrono::nanoseconds> total_lock_time_;
  std::atomic<std::chrono::nanoseconds> total_training_time_;

  size_t PositionToBlock(size_t position) const {
    return std::min(position / block_size_, kBlocks - 1);
  }

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

  void RefreshBlockToLock() {
    size_t lock_index = 0;
    for (size_t block = 0; block < kBlocks; ++block) {
      while (lock_index + 1 < kLockCnt &&
             block >= current_cuts_[lock_index + 1]) {
        ++lock_index;
      }
      block_to_lock_[block].store(lock_index, std::memory_order_relaxed);
    }
  }

  size_t FindLockForBlockUnlocked(size_t block) const {
    return block_to_lock_[block].load(std::memory_order_relaxed);
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
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
  }

  bool ShouldSampleTraining() const {
    if (!training_enabled_.load(std::memory_order_relaxed)) {
      return false;
    }

    if (operation_count_.load(std::memory_order_relaxed) <
        next_training_probe_operation_.load(std::memory_order_relaxed)) {
      return false;
    }

    if (training_sample_rate_ <= 1) {
      return true;
    }

    static thread_local size_t counter = 0;
    ++counter;
    if ((training_sample_rate_ & (training_sample_rate_ - 1)) == 0) {
      return (counter & (training_sample_rate_ - 1)) == 0;
    }
    return counter % training_sample_rate_ == 0;
  }

  void EnqueueTrainingSample(const Query &query) {
    struct LocalTrainingBuffer {
      const GeneticLock *owner = nullptr;
      std::vector<Query> queries;
    };

    static thread_local LocalTrainingBuffer local_buffer;
    if (local_buffer.owner != this) {
      local_buffer.owner = this;
      local_buffer.queries.clear();
      local_buffer.queries.reserve(kLocalTrainingFlushSize);
    }

    local_buffer.queries.push_back(query);
    if (local_buffer.queries.size() >= kLocalTrainingFlushSize) {
      FlushTrainingSamples(local_buffer.queries);
      local_buffer.queries.clear();
    }
  }

  void FlushTrainingSamples(const std::vector<Query> &queries) {
    if (queries.empty()) {
      return;
    }

    bool should_notify = false;
    {
      std::lock_guard pending_lock(pending_mutex_);
      pending_queries_.insert(pending_queries_.end(), queries.begin(),
                              queries.end());
      should_notify = pending_queries_.size() >= training_batch_size_;
    }

    if (should_notify) {
      training_cv_.notify_one();
    }
  }

  void TrainingLoop() {
    while (true) {
      std::vector<Query> batch;
      {
        std::unique_lock pending_lock(pending_mutex_);
        training_cv_.wait(pending_lock, [this] {
          return stop_training_thread_.load(std::memory_order_acquire) ||
                 pending_queries_.size() >= training_batch_size_;
        });

        if (stop_training_thread_.load(std::memory_order_acquire)) {
          return;
        }

        batch.swap(pending_queries_);
        pending_queries_.reserve(training_batch_size_ +
                                 kLocalTrainingFlushSize);
      }

      if (!HasEnoughSkew(batch)) {
        ScheduleNextTrainingProbe();
        continue;
      }

      std::lock_guard train_lock(training_mutex_);
      TrainOnBatch(batch);
    }
  }

  bool HasEnoughSkew(const std::vector<Query> &batch) const {
    if (batch.empty()) {
      return false;
    }

    std::array<size_t, kBlocks> block_hits{};
    size_t total_hits = 0;
    for (const Query &query : batch) {
      const size_t left_block = PositionToBlock(query.left);
      const size_t right_block = PositionToBlock(query.right);
      for (size_t block = left_block; block <= right_block; ++block) {
        ++block_hits[block];
        ++total_hits;
      }
    }

    if (total_hits == 0) {
      return false;
    }

    const size_t max_block =
        *std::max_element(block_hits.begin(), block_hits.end());
    const double average_block =
        static_cast<double>(total_hits) / static_cast<double>(kBlocks);
    return average_block > 0.0 &&
           static_cast<double>(max_block) / average_block >=
               min_training_skew_;
  }

  void TrainOnBatch(const std::vector<Query> &batch) {
    const auto start = std::chrono::steady_clock::now();
    const std::vector<size_t> previous_cuts = PartitionCuts();
    partitioner_.ObserveBatch(batch);
    std::vector<size_t> next_cuts = partitioner_.PartitionCuts();
    if (next_cuts == previous_cuts) {
      AddDuration(total_training_time_,
                  std::chrono::steady_clock::now() - start);
      ScheduleNextTrainingProbe();
      return;
    }

    const bool concurrent_queries =
        training_enabled_.load(std::memory_order_acquire);
    if (concurrent_queries) {
      pause_queries_.store(true, std::memory_order_release);
    }

    const auto publish_partition = [&] {
      partition_epoch_.fetch_add(1, std::memory_order_acq_rel);
      {
        std::lock_guard cuts_lock(cuts_mutex_);
        current_cuts_ = std::move(next_cuts);
        RefreshBlockToLock();
      }
      partition_epoch_.fetch_add(1, std::memory_order_release);
    };

    if (concurrent_queries) {
      RangeLockGuard all_locks(locks_, 0, kLockCnt - 1);
      publish_partition();
    } else {
      publish_partition();
    }

    pause_queries_.store(false, std::memory_order_release);

    AddDuration(total_training_time_, std::chrono::steady_clock::now() - start);
    training_count_.fetch_add(1, std::memory_order_relaxed);
  }

  void ScheduleNextTrainingProbe() {
    if (training_probe_gap_ == 0) {
      return;
    }

    const size_t current =
        operation_count_.load(std::memory_order_relaxed);
    next_training_probe_operation_.store(current + training_probe_gap_,
                                         std::memory_order_relaxed);
  }
};
