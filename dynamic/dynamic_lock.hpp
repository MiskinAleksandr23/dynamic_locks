#pragma once

#include "common/page_aligned_mutex.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

template <size_t kLockCnt, typename Mutex = std::mutex> class DynamicLock {
public:
  explicit DynamicLock(size_t array_size, size_t stats_sample_rate = 1,
                       double min_rebuild_gain = 1.0,
                       double min_rebuild_skew = 4.0)
      : instance_id_(NextInstanceId()), array_size_(array_size),
        block_size_(array_size == 0 ? 1 : 1 + ((array_size - 1) / kBlocks)),
        stats_sample_rate_(std::max(stats_sample_rate, size_t{1})),
        total_lock_time_(std::chrono::nanoseconds(0)),
        min_rebuild_gain_(std::max(min_rebuild_gain, 1.0)),
        min_rebuild_skew_(std::max(min_rebuild_skew, 1.0)) {
    partitions_.resize(kLockCnt + 1);
    for (size_t i = 0; i <= kLockCnt; ++i) {
      partitions_[i] = i * kBlocks / kLockCnt;
    }
    partitions_[kLockCnt] = kBlocks;
    RefreshBlockToLock();

    last_stats_.fill(0);
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
      const auto [lock_start, lock_end] = FindLocksSegmentUnlocked(left, right);
      if (lock_start > lock_end) {
        continue;
      }

      if (lock_start == lock_end) {
        std::lock_guard guard(locks_[lock_start]);
        const size_t current_epoch =
            partition_epoch_.load(std::memory_order_acquire);
        if (epoch != current_epoch || (current_epoch & size_t{1}) != 0) {
          continue;
        }
        AddDuration(total_lock_time_, std::chrono::steady_clock::now() - start);
        ++operation_count_;
        if (ShouldSampleStats()) {
          UpdateStats(left, right);
        }
        func(left, right);
        return;
      }

      RangeLockGuard guard(locks_, lock_start, lock_end);
      const size_t current_epoch =
          partition_epoch_.load(std::memory_order_acquire);
      if (epoch != current_epoch || (current_epoch & size_t{1}) != 0) {
        continue;
      }

      AddDuration(total_lock_time_, std::chrono::steady_clock::now() - start);
      ++operation_count_;
      if (ShouldSampleStats()) {
        UpdateStats(left, right);
      }
      func(left, right);
      return;
    }
  }

  void
  StartRebuilder(std::chrono::milliseconds interval = std::chrono::seconds(60),
                 double change_threshold = 0.3) {
    if (rebuild_thread_.joinable()) {
      return;
    }

    stop_rebuilder_ = false;
    collect_stats_.store(true, std::memory_order_release);
    rebuilder_running_.store(true, std::memory_order_release);
    rebuild_thread_ = std::thread([this, interval, change_threshold]() {
      while (!stop_rebuilder_) {
        auto slept = std::chrono::milliseconds(0);
        while (!stop_rebuilder_ && slept < interval) {
          const auto step =
              std::min(std::chrono::milliseconds(50), interval - slept);
          std::this_thread::sleep_for(step);
          slept += step;
        }
        if (stop_rebuilder_) {
          break;
        }

        if (ShouldRebuild(change_threshold) && RebuildPartitions()) {
          ++rebuild_count_;
        }
      }
    });
  }

  void StopRebuilder() {
    stop_rebuilder_ = true;
    if (rebuild_thread_.joinable()) {
      rebuild_thread_.join();
    }
    rebuilder_running_.store(false, std::memory_order_release);
    collect_stats_.store(false, std::memory_order_release);
  }

  void ForceSaveStats() {
    std::lock_guard lock(stats_mutex_);
    last_stats_ = SnapshotStats();
    ResetLocalStatsUnlocked();
  }

  void RebuildNow() {
    const auto current_stats = SnapshotStats();
    const size_t total_ops =
        std::accumulate(current_stats.begin(), current_stats.end(), size_t{0});
    if (total_ops == 0) {
      return;
    }

    if (RebuildPartitions()) {
      ++rebuild_count_;
    }
  }

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

  size_t GetRebuildCount() const { return rebuild_count_.load(); }
  size_t GetOperationCount() const { return operation_count_.load(); }

  void SetStatsSampleRate(size_t sample_rate) {
    stats_sample_rate_ = std::max(sample_rate, size_t{1});
  }

  std::vector<size_t> PartitionCuts() const {
    std::lock_guard lock(partitions_mutex_);
    return partitions_;
  }

  size_t CountLocksForRange(size_t left, size_t right) const {
    if (array_size_ == 0) [[unlikely]] {
      return 0;
    }

    left = std::min(left, array_size_ - 1);
    right = std::min(right, array_size_ - 1);
    if (left > right) {
      std::swap(left, right);
    }

    std::lock_guard lock(partitions_mutex_);
    const auto [lock_start, lock_end] = FindLocksSegmentUnlocked(left, right);
    return lock_end - lock_start + 1;
  }
  ~DynamicLock() { StopRebuilder(); }

private:
  using LockSlot = PageAlignedMutex<Mutex>;

  static constexpr size_t kBlocks = 1024;
  // Penalizes cuts that split observed range queries. Point queries do not
  // cross any boundary, so the original hotspot behavior remains unchanged.
  static constexpr size_t kBoundaryCrossingPenalty = 10'000'000;

  const size_t instance_id_;
  const size_t array_size_;
  const size_t block_size_;

  std::array<LockSlot, kLockCnt> locks_;
  std::array<std::atomic<size_t>, kBlocks> block_to_lock_{};
  std::array<size_t, kBlocks> last_stats_{};
  mutable Mutex stats_mutex_;

  struct ThreadLocalStats {
    std::array<std::atomic<size_t>, kBlocks> stats{};
    std::array<std::atomic<size_t>, kBlocks> boundary_crossings{};

    ThreadLocalStats() {
      for (auto &value : stats) {
        value.store(0, std::memory_order_relaxed);
      }
      for (auto &value : boundary_crossings) {
        value.store(0, std::memory_order_relaxed);
      }
    }
  };

  mutable Mutex local_stats_mutex_;
  std::vector<std::unique_ptr<ThreadLocalStats>> local_stats_;

  std::vector<size_t> partitions_;
  mutable Mutex partitions_mutex_;

  std::atomic<bool> rebuild_flag_{false};
  std::atomic<bool> collect_stats_{false};
  std::atomic<bool> pause_queries_{false};
  std::atomic<bool> rebuilder_running_{false};
  std::atomic<size_t> active_queries_{0};
  std::atomic<size_t> partition_epoch_{0};
  std::thread rebuild_thread_;
  std::atomic<bool> stop_rebuilder_{false};

  std::atomic<size_t> rebuild_count_{0};
  std::atomic<size_t> operation_count_{0};
  size_t stats_sample_rate_;
  std::atomic<std::chrono::nanoseconds> total_lock_time_;
  const double min_rebuild_gain_;
  const double min_rebuild_skew_;

  size_t PositionToBlock(size_t pos) const {
    return std::min(pos / block_size_, kBlocks - 1);
  }

  bool ShouldSampleStats() const {
    if (!collect_stats_.load(std::memory_order_relaxed)) {
      return false;
    }

    if (stats_sample_rate_ <= 1) {
      return true;
    }

    static thread_local size_t counter = 0;
    ++counter;
    if ((stats_sample_rate_ & (stats_sample_rate_ - 1)) == 0) {
      return (counter & (stats_sample_rate_ - 1)) == 0;
    }
    return counter % stats_sample_rate_ == 0;
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

  static void AddDuration(std::atomic<std::chrono::nanoseconds> &target,
                          std::chrono::nanoseconds delta) {
    auto current = target.load(std::memory_order_relaxed);
    while (!target.compare_exchange_weak(current, current + delta,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
  }

  static size_t NextInstanceId() {
    static std::atomic<size_t> next_id{1};
    return next_id.fetch_add(1, std::memory_order_relaxed);
  }

  void RefreshBlockToLock() {
    size_t lock_index = 0;
    for (size_t block = 0; block < kBlocks; ++block) {
      while (lock_index + 1 < kLockCnt &&
             block >= partitions_[lock_index + 1]) {
        ++lock_index;
      }
      block_to_lock_[block].store(lock_index, std::memory_order_relaxed);
    }
  }

  std::pair<size_t, size_t> FindLocksSegmentUnlocked(size_t left,
                                                     size_t right) const {
    const size_t left_block = PositionToBlock(left);
    const size_t right_block = PositionToBlock(right);
    return {block_to_lock_[left_block].load(std::memory_order_relaxed),
            block_to_lock_[right_block].load(std::memory_order_relaxed)};
  }

  ThreadLocalStats &GetThreadLocalStats() {
    struct ThreadLocalSlot {
      const DynamicLock *owner;
      size_t instance_id;
      ThreadLocalStats *stats;
    };

    static thread_local std::vector<ThreadLocalSlot> slots;

    for (const auto &slot : slots) {
      if (slot.owner == this && slot.instance_id == instance_id_) {
        return *slot.stats;
      }
    }

    std::lock_guard lock(local_stats_mutex_);
    local_stats_.push_back(std::make_unique<ThreadLocalStats>());
    slots.push_back({this, instance_id_, local_stats_.back().get()});
    return *slots.back().stats;
  }

  void UpdateStats(size_t left, size_t right) {
    const size_t left_block = PositionToBlock(left);
    const size_t right_block = PositionToBlock(right);
    auto &thread_stats = GetThreadLocalStats();

    for (size_t block = left_block; block <= right_block && block < kBlocks;
         ++block) {
      thread_stats.stats[block].fetch_add(1, std::memory_order_relaxed);
    }

    for (size_t boundary = left_block + 1;
         boundary <= right_block && boundary < kBlocks; ++boundary) {
      thread_stats.boundary_crossings[boundary].fetch_add(
          1, std::memory_order_relaxed);
    }
  }

  std::array<size_t, kBlocks> SnapshotStats() const {
    std::array<size_t, kBlocks> snapshot{};
    std::lock_guard lock(local_stats_mutex_);
    for (const auto &thread_stats : local_stats_) {
      for (size_t i = 0; i < kBlocks; ++i) {
        snapshot[i] += thread_stats->stats[i].load(std::memory_order_relaxed);
      }
    }
    return snapshot;
  }

  std::array<size_t, kBlocks> SnapshotBoundaryCrossings() const {
    std::array<size_t, kBlocks> snapshot{};
    std::lock_guard lock(local_stats_mutex_);
    for (const auto &thread_stats : local_stats_) {
      for (size_t i = 0; i < kBlocks; ++i) {
        snapshot[i] += thread_stats->boundary_crossings[i].load(
            std::memory_order_relaxed);
      }
    }
    return snapshot;
  }

  bool ShouldRebuild(double threshold) {
    std::lock_guard lock(stats_mutex_);
    const auto current_stats = SnapshotStats();

    const size_t total_ops =
        std::accumulate(current_stats.begin(), current_stats.end(), size_t{0});
    if (total_ops < 1000) {
      return false;
    }
    if (!HasEnoughSkew(current_stats, total_ops)) {
      ResetCollectedStatsUnlocked(current_stats);
      return false;
    }

    const size_t last_total =
        std::accumulate(last_stats_.begin(), last_stats_.end(), size_t{0});
    if (last_total == 0) {
      last_stats_ = current_stats;
      return true;
    }

    double diff = 0.0;
    size_t comparable_blocks = 0;
    for (size_t i = 0; i < kBlocks; ++i) {
      const auto old_value = static_cast<double>(last_stats_[i]);
      const auto new_value = static_cast<double>(current_stats[i]);
      if (old_value == 0.0 && new_value == 0.0) {
        continue;
      }

      diff += std::abs(new_value - old_value) / std::max(old_value, 1.0);
      comparable_blocks++;
    }

    if (comparable_blocks == 0) {
      return false;
    }

    return diff / static_cast<double>(comparable_blocks) > threshold;
  }

  bool HasEnoughSkew(const std::array<size_t, kBlocks> &stats,
                     size_t total_ops) const {
    const size_t max_block = *std::max_element(stats.begin(), stats.end());
    const double average_block =
        static_cast<double>(total_ops) / static_cast<double>(kBlocks);
    return average_block > 0.0 &&
           static_cast<double>(max_block) / average_block >=
               min_rebuild_skew_;
  }

  std::vector<size_t>
  OptimizePartitions(const std::vector<size_t> &prefix_sum,
                     const std::array<size_t, kBlocks> &boundary_crossings) {
    const size_t inf = std::numeric_limits<size_t>::max();

    std::vector dp(kBlocks + 1, std::vector(kLockCnt + 1, inf));
    std::vector split(kBlocks + 1, std::vector<size_t>(kLockCnt + 1, 0));

    dp[0][0] = 0;

    for (size_t right = 1; right <= kBlocks; ++right) {
      for (size_t locks = 1; locks <= kLockCnt; ++locks) {
        for (size_t left = 0; left < right; ++left) {
          if (dp[left][locks - 1] == inf) {
            continue;
          }

          const size_t weight = prefix_sum[right] - prefix_sum[left];
          const size_t cut_penalty =
              left == 0 ? 0
                        : kBoundaryCrossingPenalty * boundary_crossings[left];
          const size_t candidate =
              dp[left][locks - 1] + weight * weight + cut_penalty;
          if (candidate < dp[right][locks]) {
            dp[right][locks] = candidate;
            split[right][locks] = left;
          }
        }
      }
    }

    std::vector<size_t> new_partitions(kLockCnt + 1, 0);
    new_partitions[kLockCnt] = kBlocks;

    size_t current = kBlocks;
    for (size_t locks = kLockCnt; locks > 0; --locks) {
      new_partitions[locks - 1] = split[current][locks];
      current = split[current][locks];
    }

    return new_partitions;
  }

  size_t PartitionCost(const std::vector<size_t> &partitions,
                       const std::vector<size_t> &prefix_sum,
                       const std::array<size_t, kBlocks> &boundary_crossings)
      const {
    size_t total = 0;
    for (size_t lock_index = 0; lock_index < kLockCnt; ++lock_index) {
      const size_t left = partitions[lock_index];
      const size_t right = partitions[lock_index + 1];
      const size_t weight = prefix_sum[right] - prefix_sum[left];
      const size_t cut_penalty =
          left == 0 ? 0 : kBoundaryCrossingPenalty * boundary_crossings[left];
      total += weight * weight + cut_penalty;
    }
    return total;
  }

  bool HasEnoughPartitionGain(
      const std::vector<size_t> &current_partitions,
      const std::vector<size_t> &new_partitions,
      const std::vector<size_t> &prefix_sum,
      const std::array<size_t, kBlocks> &boundary_crossings) const {
    if (new_partitions == current_partitions) {
      return false;
    }

    const size_t current_cost =
        PartitionCost(current_partitions, prefix_sum, boundary_crossings);
    const size_t new_cost =
        PartitionCost(new_partitions, prefix_sum, boundary_crossings);
    if (new_cost == 0) {
      return current_cost != 0;
    }

    return static_cast<long double>(current_cost) >
           static_cast<long double>(new_cost) * min_rebuild_gain_;
  }

  void ResetCollectedStatsUnlocked(
      const std::array<size_t, kBlocks> &current_stats) {
    last_stats_ = current_stats;
    ResetLocalStatsUnlocked();
  }

  void ResetLocalStatsUnlocked() {
    std::lock_guard lock(local_stats_mutex_);
    for (auto &thread_stats : local_stats_) {
      for (size_t i = 0; i < kBlocks; ++i) {
        thread_stats->stats[i].store(0, std::memory_order_relaxed);
        thread_stats->boundary_crossings[i].store(0,
                                                  std::memory_order_relaxed);
      }
    }
  }

  bool RebuildPartitions() {
    if (rebuild_flag_.exchange(true)) {
      return false;
    }

    const auto current_stats = SnapshotStats();
    const auto current_boundary_crossings = SnapshotBoundaryCrossings();
    std::vector<size_t> prefix_sum(kBlocks + 1, 0);
    for (size_t i = 0; i < kBlocks; ++i) {
      prefix_sum[i + 1] = prefix_sum[i] + current_stats[i];
    }

    auto new_partitions =
        OptimizePartitions(prefix_sum, current_boundary_crossings);

    {
      std::lock_guard parts_lock(partitions_mutex_);
      if (!HasEnoughPartitionGain(partitions_, new_partitions, prefix_sum,
                                  current_boundary_crossings)) {
        std::lock_guard stats_lock(stats_mutex_);
        ResetCollectedStatsUnlocked(current_stats);
        rebuild_flag_ = false;
        return false;
      }
    }

    pause_queries_.store(true, std::memory_order_release);
    RangeLockGuard all_locks(locks_, 0, kLockCnt - 1);
    {
      std::lock_guard parts_lock(partitions_mutex_);
      std::lock_guard stats_lock(stats_mutex_);

      partitions_ = std::move(new_partitions);
      partition_epoch_.fetch_add(1, std::memory_order_acq_rel);
      RefreshBlockToLock();
      partition_epoch_.fetch_add(1, std::memory_order_release);
      last_stats_ = current_stats;
      ResetLocalStatsUnlocked();
    }

    rebuild_flag_ = false;
    pause_queries_.store(false, std::memory_order_release);
    return true;
  }
};
