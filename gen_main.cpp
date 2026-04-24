/*
 *
 *
 *
 *
 *
*/

#include "genetic_lock.hpp"
#include "genetic_partitioner.hpp"
#include "naive_lock.hpp"
#include "request_workload.hpp"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
  constexpr size_t kLockCount = 64;
  constexpr size_t kBlocks = 1024;
  constexpr size_t kArraySize = 1024 * 1024;
  constexpr size_t kHotWindowSize = kArraySize / kLockCount;
  constexpr size_t kQueryLength = 64;
  constexpr size_t kBatchSize = 512;
  constexpr int kEpochsPerSide = 4;
  constexpr int kRecallWarmupEpochs = 3;
  constexpr int kRecallReturnEpochs = 3;
  constexpr size_t kPopulationSize = 24;
  constexpr size_t kEliteCount = 4;
  constexpr size_t kGenerationCount = 18;
  const size_t kThreadCount =
      std::max(1u, std::thread::hardware_concurrency());
  constexpr size_t kOnlineTrainingBatchSize = 2048;
  constexpr size_t kOnlinePhaseQueries = 48'000;
  constexpr size_t kPureLockQueries = 900'000;
  constexpr size_t kPretrainBatches = 8;

  struct Snapshot {
    std::string label;
    PartitionMetrics metrics;
    size_t left_hot_locks = 0;
    size_t right_hot_locks = 0;
  };

  struct ImprovementTestResult {
    PartitionMetrics initial_metrics;
    PartitionMetrics final_metrics;
    size_t left_hot_locks = 0;
    size_t right_hot_locks = 0;
    bool passed = false;
  };

  struct ShiftTestResult {
    std::vector<Snapshot> timeline;
    bool passed = false;
  };

  struct RecallTestResult {
    std::vector<Snapshot> timeline;
    bool passed = false;
  };

  struct LockBenchmarkPhase {
    std::string name;
    std::vector<Query> queries;
  };

  struct LockBenchmarkRun {
    std::string name;
    double seconds = 0.0;
    uint64_t queries = 0;
    double avg_lock_ms = 0.0;
    double avg_mutexes_per_query = 0.0;
    size_t trainings = 0;
    double avg_training_ms = 0.0;
    size_t left_hot_locks = 0;
    size_t right_hot_locks = 0;
  };

  struct LockBenchmarkComparison {
    std::string title;
    LockBenchmarkRun naive;
    LockBenchmarkRun genetic;
  };

  Snapshot CaptureSnapshot(
    const GeneticPartitioner<kLockCount, kBlocks> &optimizer,
    const std::vector<Query> &batch, const std::string &label) {
    return {
      label,
      optimizer.EvaluateCurrent(batch),
      optimizer.CountLocksForRange(0, kHotWindowSize - 1),
      optimizer.CountLocksForRange(kArraySize - kHotWindowSize,
                                   kArraySize - 1)
    };
  }

  std::vector<Query> BuildSideQueries(size_t count, TrafficSide side,
                                      uint64_t seed) {
    WorkloadGenerator generator(kArraySize, kHotWindowSize, kQueryLength, seed);
    return generator.BuildBatch(count, side);
  }

  template<typename Lock>
  uint64_t RunLockOnlyQueries(Lock &lock, const std::vector<Query> &queries) {
    std::vector<uint64_t> per_thread(kThreadCount, 0);
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    std::barrier start_barrier(static_cast<std::ptrdiff_t>(kThreadCount + 1));
    const auto worker = [&](size_t thread_index) {
      const size_t begin = queries.size() * thread_index / kThreadCount;
      const size_t end = queries.size() * (thread_index + 1) / kThreadCount;

      start_barrier.arrive_and_wait();
      for (size_t i = begin; i < end; ++i) {
        const Query &query = queries[i];
        lock.WriteQuery(query.left, query.right, [](size_t, size_t) {});
        ++per_thread[thread_index];
      }
    };

    for (size_t thread_index = 0; thread_index < kThreadCount; ++thread_index) {
      threads.emplace_back(worker, thread_index);
    }

    start_barrier.arrive_and_wait();
    for (std::thread &thread: threads) {
      thread.join();
    }

    uint64_t total = 0;
    for (uint64_t count: per_thread) {
      total += count;
    }
    return total;
  }

  template<typename Lock>
  double AverageLocksForQueries(const Lock &lock,
                                const std::vector<Query> &queries) {
    if (queries.empty()) {
      return 0.0;
    }

    uint64_t total = 0;
    for (const Query &query: queries) {
      total += lock.CountLocksForRange(query.left, query.right);
    }
    return static_cast<double>(total) / static_cast<double>(queries.size());
  }

  template<typename Lock>
  double GetAverageMutexes(const Lock &lock,
                           const std::vector<Query> &queries) {
    return AverageLocksForQueries(lock, queries);
  }

  template<typename Lock>
  size_t GetTrainingCount(const Lock &lock) {
    if constexpr (requires { lock.GetTrainingCount(); }) {
      return lock.GetTrainingCount();
    } else {
      return lock.GetRebuildCount();
    }
  }

  template<typename Lock>
  double GetAverageTrainingMs(const Lock &lock) {
    if constexpr (requires { lock.GetAvgTrainingTimeMs(); }) {
      return lock.GetAvgTrainingTimeMs();
    } else {
      return 0.0;
    }
  }

  template<typename Lock>
  LockBenchmarkRun RunPhasedLockBenchmark(
      const std::string &name, Lock &lock,
      const std::vector<LockBenchmarkPhase> &phases, bool enable_training) {
    LockBenchmarkRun result;
    result.name = name;

    if (enable_training) {
      lock.StartRebuilder(std::chrono::milliseconds(0), 0.0);
    } else {
      lock.StopRebuilder();
    }

    std::vector<Query> all_queries;
    for (const LockBenchmarkPhase &phase: phases) {
      all_queries.insert(all_queries.end(), phase.queries.begin(),
                         phase.queries.end());
    }

    const auto start = std::chrono::steady_clock::now();
    for (const LockBenchmarkPhase &phase: phases) {
      result.queries += RunLockOnlyQueries(lock, phase.queries);
    }
    result.seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - start).count();

    lock.StopRebuilder();
    result.avg_lock_ms = lock.GetAvgLockTimeMs();
    result.avg_mutexes_per_query = GetAverageMutexes(lock, all_queries);
    result.trainings = GetTrainingCount(lock);
    result.avg_training_ms = GetAverageTrainingMs(lock);
    result.left_hot_locks = lock.CountLocksForRange(0, kHotWindowSize - 1);
    result.right_hot_locks =
        lock.CountLocksForRange(kArraySize - kHotWindowSize, kArraySize - 1);
    return result;
  }

  ImprovementTestResult RunImprovementTest() {
    GeneticPartitioner<kLockCount, kBlocks> optimizer(kArraySize, 7, 5000,
                                                      kPopulationSize, kEliteCount,
                                                      kGenerationCount);
    WorkloadGenerator generator(kArraySize, kHotWindowSize, kQueryLength, 11);

    const std::vector<Query> probe = generator.BuildBatch(kBatchSize, TrafficSide::kLeft);
    const PartitionMetrics initial_metrics = optimizer.EvaluateCurrent(probe);

    for (int epoch = 0; epoch < kEpochsPerSide; ++epoch) {
      optimizer.ObserveBatch(generator.BuildBatch(kBatchSize, TrafficSide::kLeft));
    }

    const PartitionMetrics final_metrics = optimizer.EvaluateCurrent(probe);
    const size_t left_hot_locks = optimizer.CountLocksForRange(0, kHotWindowSize - 1);
    const size_t right_hot_locks =
        optimizer.CountLocksForRange(kArraySize - kHotWindowSize, kArraySize - 1);

    const bool passed = final_metrics.fitness < initial_metrics.fitness &&
                        left_hot_locks > right_hot_locks;

    return {initial_metrics, final_metrics, left_hot_locks, right_hot_locks, passed};
  }



  ShiftTestResult RunShiftTest() {
    GeneticPartitioner<kLockCount, kBlocks> optimizer(kArraySize, 17, 5000,
                                                      kPopulationSize, kEliteCount,
                                                      kGenerationCount);
    WorkloadGenerator generator(kArraySize, kHotWindowSize, kQueryLength, 23);

    ShiftTestResult result;

    for (int epoch = 0; epoch < kEpochsPerSide; ++epoch) {
      const std::vector<Query> batch =
          generator.BuildBatch(kBatchSize, TrafficSide::kLeft);
      optimizer.ObserveBatch(batch);
      result.timeline.push_back(
        CaptureSnapshot(optimizer, batch, "left epoch " + std::to_string(epoch + 1)));
    }

    const Snapshot after_left = result.timeline.back();

    for (int epoch = 0; epoch < kEpochsPerSide; ++epoch) {
      const std::vector<Query> batch =
          generator.BuildBatch(kBatchSize, TrafficSide::kRight);
      optimizer.ObserveBatch(batch);
      result.timeline.push_back(
        CaptureSnapshot(optimizer, batch, "right epoch " + std::to_string(epoch + 1)));
    }

    const Snapshot after_right = result.timeline.back();

    result.passed = after_left.left_hot_locks > after_left.right_hot_locks &&
                    after_right.right_hot_locks > after_right.left_hot_locks;
    return result;
  }

  RecallTestResult RunRecallTest() {
    GeneticPartitioner<kLockCount, kBlocks> optimizer(kArraySize, 31, 10000,
                                                      kPopulationSize, kEliteCount,
                                                      kGenerationCount);
    WorkloadGenerator generator(kArraySize, kHotWindowSize, kQueryLength, 37);

    RecallTestResult result;

    for (int epoch = 0; epoch < kRecallWarmupEpochs; ++epoch) {
      optimizer.ObserveBatch(generator.BuildBatch(kBatchSize, TrafficSide::kLeft));
    }
    const std::vector<Query> left_probe =
        generator.BuildBatch(kBatchSize, TrafficSide::kLeft);
    result.timeline.push_back(
      CaptureSnapshot(optimizer, left_probe, "after left warmup"));

    for (int epoch = 0; epoch < kRecallWarmupEpochs; ++epoch) {
      optimizer.ObserveBatch(generator.BuildBatch(kBatchSize, TrafficSide::kRight));
    }
    const std::vector<Query> right_probe =
        generator.BuildBatch(kBatchSize, TrafficSide::kRight);
    result.timeline.push_back(
      CaptureSnapshot(optimizer, right_probe, "after right warmup"));

    for (int epoch = 0; epoch < kRecallReturnEpochs; ++epoch) {
      const std::vector<Query> left_return =
          generator.BuildBatch(kBatchSize, TrafficSide::kLeft);
      optimizer.ObserveBatch(left_return);
      result.timeline.push_back(
        CaptureSnapshot(optimizer, left_return,
                        "left recall epoch " + std::to_string(epoch + 1)));
    }

    const Snapshot &after_left = result.timeline[0];
    const Snapshot &after_right = result.timeline[1];
    const Snapshot &after_recall = result.timeline.back();

    result.passed = after_left.left_hot_locks > after_left.right_hot_locks &&
                    after_right.right_hot_locks > after_right.left_hot_locks &&
                    after_recall.left_hot_locks > after_recall.right_hot_locks;
    return result;
  }

  using GeneticBenchmarkLock = GeneticLock<kLockCount, kBlocks>;

  void PretrainGeneticLock(GeneticBenchmarkLock &lock, TrafficSide side,
                           uint64_t seed) {
    WorkloadGenerator generator(kArraySize, kHotWindowSize, kQueryLength, seed);
    for (size_t batch = 0; batch < kPretrainBatches; ++batch) {
      lock.ObserveBatch(generator.BuildBatch(kOnlineTrainingBatchSize, side));
    }
    lock.ResetRuntimeStats();
  }

  LockBenchmarkComparison RunOnlineLockBenchmark() {
    std::vector<LockBenchmarkPhase> phases;
    phases.push_back({"online left mode",
                      BuildSideQueries(kOnlinePhaseQueries,
                                       TrafficSide::kLeft, 101)});
    phases.push_back({"online right mode",
                      BuildSideQueries(kOnlinePhaseQueries,
                                       TrafficSide::kRight, 102)});
    phases.push_back({"online left recall",
                      BuildSideQueries(kOnlinePhaseQueries,
                                       TrafficSide::kLeft, 103)});

    NaiveLock<kLockCount> naive(kArraySize);
    GeneticBenchmarkLock genetic(kArraySize, 201, kOnlineTrainingBatchSize,
                                 12000, kPopulationSize, kEliteCount,
                                 kGenerationCount);

    return {
      "online total time, including genetic training",
      RunPhasedLockBenchmark("naive fixed partitions", naive, phases, true),
      RunPhasedLockBenchmark("genetic adaptive partitions", genetic, phases, true)
    };
  }

  LockBenchmarkComparison RunPureLockBenchmark() {
    std::vector<LockBenchmarkPhase> phases;
    phases.push_back({"left hot lock/unlock probe",
                      BuildSideQueries(kPureLockQueries,
                                       TrafficSide::kLeft, 301)});

    NaiveLock<kLockCount> naive(kArraySize);
    GeneticBenchmarkLock genetic(kArraySize, 302, kOnlineTrainingBatchSize,
                                 12000, kPopulationSize, kEliteCount,
                                 kGenerationCount);
    PretrainGeneticLock(genetic, TrafficSide::kLeft, 303);

    return {
      "pure lock/unlock time after offline pretraining",
      RunPhasedLockBenchmark("naive fixed partitions", naive, phases, false),
      RunPhasedLockBenchmark("genetic adaptive partitions", genetic, phases, false)
    };
  }

  void PrintMetrics(const PartitionMetrics &metrics) {
    std::cout << "fitness " << std::fixed << std::setprecision(0)
        << metrics.fitness << ", contention " << metrics.contention_score
        << ", avg mutexes/query " << std::setprecision(2)
        << metrics.avg_mutexes_per_query;
  }

  void PrintImprovementTest(const ImprovementTestResult &result) {
    std::cout << "improvement test\n";
    std::cout << "  before: ";
    PrintMetrics(result.initial_metrics);
    std::cout << '\n';
    std::cout << "  after:  ";
    PrintMetrics(result.final_metrics);
    std::cout << '\n';
    std::cout << "  hot locks after training: left " << result.left_hot_locks
        << ", right " << result.right_hot_locks << '\n';
    std::cout << "  result: " << (result.passed ? "OK" : "FAILED") << "\n\n";
  }

  void PrintShiftTimeline(const ShiftTestResult &result) {
    std::cout << "shift test\n";
    for (const Snapshot &snapshot: result.timeline) {
      std::cout << "  " << snapshot.label << ": ";
      PrintMetrics(snapshot.metrics);
      std::cout << ", hot locks left " << snapshot.left_hot_locks << ", right "
          << snapshot.right_hot_locks << '\n';
    }
    std::cout << "  result: " << (result.passed ? "OK" : "FAILED") << "\n\n";
  }

  void PrintRecallTest(const RecallTestResult &result) {
    std::cout << "recall test\n";
    for (const Snapshot &snapshot: result.timeline) {
      std::cout << "  " << snapshot.label << ": ";
      PrintMetrics(snapshot.metrics);
      std::cout << ", hot locks left " << snapshot.left_hot_locks << ", right "
          << snapshot.right_hot_locks << '\n';
    }
    std::cout << "  result: " << (result.passed ? "OK" : "FAILED") << "\n\n";
  }

  void PrintLockBenchmarkRun(const LockBenchmarkRun &run) {
    const double qps = run.seconds > 0.0
                         ? static_cast<double>(run.queries) / run.seconds
                         : 0.0;
    std::cout << "  " << run.name << ": " << std::fixed << std::setprecision(3)
        << run.seconds << " s, " << std::setprecision(0) << qps
        << " qps, avg mutexes/query " << std::setprecision(2)
        << run.avg_mutexes_per_query << ", avg lock wait "
        << std::setprecision(4) << run.avg_lock_ms << " ms";

    if (run.trainings > 0) {
      std::cout << ", trainings " << run.trainings << ", avg train "
          << std::setprecision(2) << run.avg_training_ms << " ms";
    }

    std::cout << ", hot locks left " << run.left_hot_locks
        << ", right " << run.right_hot_locks << '\n';
  }

  void PrintLockBenchmark(const LockBenchmarkComparison &comparison) {
    std::cout << comparison.title << '\n';
    PrintLockBenchmarkRun(comparison.naive);
    PrintLockBenchmarkRun(comparison.genetic);

    const double naive_qps = comparison.naive.seconds > 0.0
                               ? static_cast<double>(comparison.naive.queries) /
                                 comparison.naive.seconds
                               : 0.0;
    const double genetic_qps = comparison.genetic.seconds > 0.0
                                 ? static_cast<double>(comparison.genetic.queries) /
                                   comparison.genetic.seconds
                                 : 0.0;
    const double speedup = naive_qps > 0.0 ? genetic_qps / naive_qps : 0.0;
    const double wall_time_ratio =
        comparison.genetic.seconds > 0.0
          ? comparison.naive.seconds / comparison.genetic.seconds
          : 0.0;

    std::cout << "  qps ratio genetic/naive: " << std::fixed
        << std::setprecision(3) << speedup << "x\n";
    std::cout << "  wall-time ratio naive/genetic: "
        << std::setprecision(3) << wall_time_ratio << "x\n\n";
  }
} // namespace

int main() {
  std::cout << "simple genetic partition demo\n";
  std::cout << "  requests: sum on range + apply f(int)->int to one element\n";
  std::cout << "  chromosome: partition boundaries between 1024 fine-grained blocks\n";
  std::cout << "  mutation: shift a few boundaries by several blocks\n";
  std::cout << "  fitness = contention_score + 5000 * avg_mutexes_per_query\n";
  std::cout << "  contention_score = sum(load_i^2), load_i = recent queries on mutex i\n";
  std::cout << "  online learning: batches are clustered into workload modes,\n";
  std::cout << "  each mode keeps its own history and remembered best partition\n\n";

  std::cout << "config\n";
  std::cout << "  array size: " << kArraySize << '\n';
  std::cout << "  mutexes: " << kLockCount << '\n';
  std::cout << "  fine blocks: " << kBlocks << '\n';
  std::cout << "  hot window size: " << kHotWindowSize << '\n';
  std::cout << "  query length: " << kQueryLength << '\n';
  std::cout << "  batch size: " << kBatchSize << '\n';
  std::cout << "  epochs per side: " << kEpochsPerSide << '\n';
  std::cout << "  recall return epochs: " << kRecallReturnEpochs << '\n';
  std::cout << "  population size: " << kPopulationSize << '\n';
  std::cout << "  generations: " << kGenerationCount << "\n\n";
  std::cout << "benchmark config\n";
  std::cout << "  threads: " << kThreadCount << '\n';
  std::cout << "  online training batch: " << kOnlineTrainingBatchSize << '\n';
  std::cout << "  online phase queries: " << kOnlinePhaseQueries << '\n';
  std::cout << "  pure lock queries: " << kPureLockQueries << "\n\n";

  const ImprovementTestResult improvement = RunImprovementTest();
  const ShiftTestResult shift = RunShiftTest();
  const RecallTestResult recall = RunRecallTest();
  const LockBenchmarkComparison online_benchmark = RunOnlineLockBenchmark();
  const LockBenchmarkComparison pure_lock_benchmark = RunPureLockBenchmark();

  PrintImprovementTest(improvement);
  PrintShiftTimeline(shift);
  PrintRecallTest(recall);
  PrintLockBenchmark(online_benchmark);
  PrintLockBenchmark(pure_lock_benchmark);

  const bool passed = improvement.passed && shift.passed && recall.passed;
  std::cout << "summary\n";
  std::cout << "  overall: " << (passed ? "OK" : "FAILED") << '\n';

  return passed ? 0 : 1;
}
