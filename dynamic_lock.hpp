#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

template <size_t kLockCnt, typename Mutex = std::mutex> class DynamicLock {
public:
  explicit DynamicLock(size_t array_size)
      : array_size_(array_size),
        block_size_(array_size == 0 ? 1 : 1 + ((array_size - 1) / kBlocks)),
        total_lock_time_(std::chrono::nanoseconds(0)) {
    partitions_.resize(kLockCnt + 1);
    for (size_t i = 0; i <= kLockCnt; ++i) {
      partitions_[i] = i * kBlocks / kLockCnt;
    }
    partitions_[kLockCnt] = kBlocks;

    for (auto &value : stats_) {
      value.store(0, std::memory_order_relaxed);
    }
    for (auto &value : boundary_crossings_) {
      value.store(0, std::memory_order_relaxed);
    }
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

    const bool rebuilder_running =
        rebuilder_running_.load(std::memory_order_acquire);

    if (rebuilder_running) {
      while (true) {
        while (pause_queries_.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }

        active_queries_.fetch_add(1, std::memory_order_acq_rel);
        if (!pause_queries_.load(std::memory_order_acquire)) {
          break;
        }

        active_queries_.fetch_sub(1, std::memory_order_acq_rel);
      }
    }

    const auto [lock_start, lock_end] = FindLocksSegmentUnlocked(left, right);

    std::vector<std::unique_lock<Mutex>> locks;
    locks.reserve(lock_end - lock_start + 1);
    for (size_t lock_index = lock_start; lock_index <= lock_end; ++lock_index) {
      locks.emplace_back(locks_[lock_index]);
    }

    const auto lock_time = std::chrono::steady_clock::now() - start;
    auto current = total_lock_time_.load(std::memory_order_relaxed);
    while (!total_lock_time_.compare_exchange_weak(
        current, current + lock_time, std::memory_order_relaxed,
        std::memory_order_relaxed)) {
    }

    ++operation_count_;
    if (collect_stats_.load(std::memory_order_relaxed)) {
      UpdateStats(left, right);
    }
    func(left, right);

    if (rebuilder_running) {
      active_queries_.fetch_sub(1, std::memory_order_acq_rel);
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
        std::this_thread::sleep_for(interval);
        if (stop_rebuilder_) {
          break;
        }

        if (ShouldRebuild(change_threshold)) {
          RebuildPartitions();
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
    for (size_t i = 0; i < kBlocks; ++i) {
      last_stats_[i] = stats_[i].exchange(0, std::memory_order_relaxed);
      boundary_crossings_[i].store(0, std::memory_order_relaxed);
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
  static constexpr size_t kBlocks = 1024;
  // Penalizes cuts that split observed range queries. Point queries do not
  // cross any boundary, so the original hotspot behavior remains unchanged.
  static constexpr size_t kBoundaryCrossingPenalty = 10'000'000;

  const size_t array_size_;
  const size_t block_size_;

  std::array<Mutex, kLockCnt> locks_;
  std::array<std::atomic<size_t>, kBlocks> stats_;
  std::array<std::atomic<size_t>, kBlocks> boundary_crossings_;
  std::array<size_t, kBlocks> last_stats_{};
  mutable Mutex stats_mutex_;

  std::vector<size_t> partitions_;
  mutable Mutex partitions_mutex_;

  std::atomic<bool> rebuild_flag_{false};
  std::atomic<bool> collect_stats_{false};
  std::atomic<bool> pause_queries_{false};
  std::atomic<bool> rebuilder_running_{false};
  std::atomic<size_t> active_queries_{0};
  std::thread rebuild_thread_;
  std::atomic<bool> stop_rebuilder_{false};

  std::atomic<size_t> rebuild_count_{0};
  std::atomic<size_t> operation_count_{0};
  std::atomic<std::chrono::nanoseconds> total_lock_time_;

  size_t PositionToBlock(size_t pos) const {
    return std::min(pos / block_size_, kBlocks - 1);
  }

  std::pair<size_t, size_t> FindLocksSegmentUnlocked(size_t left,
                                                     size_t right) const {
    const size_t left_block = PositionToBlock(left);
    const size_t right_block = PositionToBlock(right);

    size_t lock_start = 0;
    for (size_t i = 0; i < kLockCnt; ++i) {
      if (partitions_[i] <= left_block && left_block < partitions_[i + 1]) {
        lock_start = i;
        break;
      }
    }

    size_t lock_end = lock_start;
    for (size_t i = lock_start; i < kLockCnt; ++i) {
      if (partitions_[i] <= right_block && right_block < partitions_[i + 1]) {
        lock_end = i;
        break;
      }
    }

    return {lock_start, lock_end};
  }

  void UpdateStats(size_t left, size_t right) {
    const size_t left_block = PositionToBlock(left);
    const size_t right_block = PositionToBlock(right);

    for (size_t block = left_block; block <= right_block && block < kBlocks;
         ++block) {
      stats_[block].fetch_add(1, std::memory_order_relaxed);
    }

    for (size_t boundary = left_block + 1;
         boundary <= right_block && boundary < kBlocks; ++boundary) {
      boundary_crossings_[boundary].fetch_add(1, std::memory_order_relaxed);
    }
  }

  std::array<size_t, kBlocks> SnapshotStats() const {
    std::array<size_t, kBlocks> snapshot{};
    for (size_t i = 0; i < kBlocks; ++i) {
      snapshot[i] = stats_[i].load(std::memory_order_relaxed);
    }
    return snapshot;
  }

  std::array<size_t, kBlocks> SnapshotBoundaryCrossings() const {
    std::array<size_t, kBlocks> snapshot{};
    for (size_t i = 0; i < kBlocks; ++i) {
      snapshot[i] = boundary_crossings_[i].load(std::memory_order_relaxed);
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

    const size_t last_total =
        std::accumulate(last_stats_.begin(), last_stats_.end(), size_t{0});
    if (last_total == 0) {
      return false;
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

  void RebuildPartitions() {
    if (rebuild_flag_.exchange(true)) {
      return;
    }

    const auto current_stats = SnapshotStats();
    const auto current_boundary_crossings = SnapshotBoundaryCrossings();
    std::vector<size_t> prefix_sum(kBlocks + 1, 0);
    for (size_t i = 0; i < kBlocks; ++i) {
      prefix_sum[i + 1] = prefix_sum[i] + current_stats[i];
    }

    auto new_partitions =
        OptimizePartitions(prefix_sum, current_boundary_crossings);

    pause_queries_.store(true, std::memory_order_release);
    while (active_queries_.load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
    }
    {
      std::lock_guard parts_lock(partitions_mutex_);
      std::lock_guard stats_lock(stats_mutex_);

      partitions_ = std::move(new_partitions);
      last_stats_ = current_stats;
      for (size_t i = 0; i < kBlocks; ++i) {
        stats_[i].store(0, std::memory_order_relaxed);
        boundary_crossings_[i].store(0, std::memory_order_relaxed);
      }
    }

    rebuild_flag_ = false;
    pause_queries_.store(false, std::memory_order_release);
  }
};
