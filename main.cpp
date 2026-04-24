#include "dynamic_lock.hpp"
#include "genetic_lock.hpp"
#include "naive_lock.hpp"
#include "request_workload.hpp"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr size_t kLockCount = 64;
constexpr size_t kBlocks = 1024;
constexpr size_t kBlockSize = 1024;
constexpr size_t kArraySize = kBlocks * kBlockSize;
constexpr size_t kHotWindowSize = kArraySize / kLockCount;
constexpr size_t kPointQueryLength = 1;
constexpr size_t kRandomRangeMaxLength = 65'536;

const size_t kThreadCount = std::max(1u, std::thread::hardware_concurrency());

constexpr size_t kWarmupQueries = 64'000;
constexpr size_t kAdaptQueries = 192'000;
constexpr size_t kMeasuredQueries = 1'200'000;
constexpr size_t kRandomRangeWarmupQueries = 16'000;
constexpr size_t kRandomRangeAdaptQueries = 48'000;
constexpr size_t kRandomRangeMeasuredQueries = 40'000;
constexpr auto kDynamicRebuildInterval = std::chrono::milliseconds(100);
constexpr double kDynamicRebuildThreshold = 2.0;

constexpr size_t kGeneticTrainingBatchSize = 8'192;
constexpr size_t kGeneticHistoryLimit = 12'000;
constexpr size_t kGeneticPopulationSize = 24;
constexpr size_t kGeneticEliteCount = 4;
constexpr size_t kGeneticGenerationCount = 18;

enum class QueryPattern {
  kHotWindow,
  kUniform,
  kHotWindowRandomRange,
  kUniformRandomRange
};
enum class QueryBody { kRealWork, kLockOnly };

struct PhaseInput {
  std::string name;
  std::vector<Query> queries;
};

struct ScenarioInput {
  std::string name;
  std::string description;
  std::vector<PhaseInput> setup_phases;
  PhaseInput measured_phase;
  bool wait_for_background_rebuild = false;
};

struct WorkerStats {
  uint64_t queries = 0;
  uint64_t sum_checksum = 0;
  uint64_t update_checksum = 0;
};

struct PhaseRunStats {
  double seconds = 0.0;
  uint64_t queries = 0;
  uint64_t sum_checksum = 0;
  uint64_t update_checksum = 0;
};

struct ImplementationResult {
  std::string name;
  double setup_seconds = 0.0;
  double body_seconds = 0.0;
  double lock_only_seconds = 0.0;
  double avg_lock_us = 0.0;
  double lock_only_avg_lock_us = 0.0;
  double measured_lock_total_seconds = 0.0;
  double avg_mutexes_per_query = 0.0;
  size_t left_hot_locks = 0;
  size_t right_hot_locks = 0;
  size_t reconfigurations = 0;
  uint64_t body_queries = 0;
  uint64_t lock_only_queries = 0;
};

std::vector<int> MakeInitialData() {
  std::vector<int> data(kArraySize);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<int>((i * 17 + 23) % 1000);
  }
  return data;
}

Query MakeWindowQuery(std::mt19937_64 &rng, size_t window_begin,
                      size_t window_size, size_t query_length) {
  std::uniform_int_distribution<size_t> left_dist(
      window_begin, window_begin + window_size - query_length);
  std::uniform_int_distribution<size_t> target_offset_dist(0, query_length - 1);
  std::uniform_int_distribution<int> delta_dist(1, 7);

  const size_t left = left_dist(rng);
  const size_t target = left + target_offset_dist(rng);
  return {static_cast<uint32_t>(left),
          static_cast<uint32_t>(left + query_length - 1),
          static_cast<uint32_t>(target), delta_dist(rng)};
}

Query MakeUniformQuery(std::mt19937_64 &rng, size_t query_length) {
  std::uniform_int_distribution<size_t> left_dist(0, kArraySize - query_length);
  std::uniform_int_distribution<size_t> target_offset_dist(0, query_length - 1);
  std::uniform_int_distribution<int> delta_dist(1, 7);

  const size_t left = left_dist(rng);
  const size_t target = left + target_offset_dist(rng);
  return {static_cast<uint32_t>(left),
          static_cast<uint32_t>(left + query_length - 1),
          static_cast<uint32_t>(target), delta_dist(rng)};
}

Query MakeWindowRandomRangeQuery(std::mt19937_64 &rng, size_t window_begin,
                                 size_t window_size, size_t max_length) {
  const size_t capped_max_length = std::min(max_length, window_size);
  std::uniform_int_distribution<size_t> length_dist(1, capped_max_length);
  std::uniform_int_distribution<int> delta_dist(1, 7);

  const size_t length = length_dist(rng);
  std::uniform_int_distribution<size_t> left_dist(
      window_begin, window_begin + window_size - length);
  std::uniform_int_distribution<size_t> target_offset_dist(0, length - 1);

  const size_t left = left_dist(rng);
  const size_t target = left + target_offset_dist(rng);
  return {static_cast<uint32_t>(left), static_cast<uint32_t>(left + length - 1),
          static_cast<uint32_t>(target), delta_dist(rng)};
}

Query MakeUniformRandomRangeQuery(std::mt19937_64 &rng, size_t max_length) {
  const size_t capped_max_length = std::min(max_length, kArraySize);
  std::uniform_int_distribution<size_t> length_dist(1, capped_max_length);
  std::uniform_int_distribution<int> delta_dist(1, 7);

  const size_t length = length_dist(rng);
  std::uniform_int_distribution<size_t> left_dist(0, kArraySize - length);
  std::uniform_int_distribution<size_t> target_offset_dist(0, length - 1);

  const size_t left = left_dist(rng);
  const size_t target = left + target_offset_dist(rng);
  return {static_cast<uint32_t>(left), static_cast<uint32_t>(left + length - 1),
          static_cast<uint32_t>(target), delta_dist(rng)};
}

std::vector<Query> BuildQueries(size_t count, QueryPattern pattern,
                                TrafficSide side, size_t query_length,
                                uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<Query> queries;
  queries.reserve(count);

  const size_t hot_begin =
      side == TrafficSide::kLeft ? 0 : kArraySize - kHotWindowSize;
  for (size_t i = 0; i < count; ++i) {
    if (pattern == QueryPattern::kUniform) {
      queries.push_back(MakeUniformQuery(rng, query_length));
    } else if (pattern == QueryPattern::kUniformRandomRange) {
      queries.push_back(MakeUniformRandomRangeQuery(rng, query_length));
    } else if (pattern == QueryPattern::kHotWindowRandomRange) {
      queries.push_back(MakeWindowRandomRangeQuery(
          rng, hot_begin, kHotWindowSize, query_length));
    } else {
      queries.push_back(
          MakeWindowQuery(rng, hot_begin, kHotWindowSize, query_length));
    }
  }

  return queries;
}

ScenarioInput MakeShiftScenario() {
  return {"shift_hotspot_point",
          "right warmup, then left hotspot; final queries are point updates",
          {{"warmup right hotspot",
            BuildQueries(kWarmupQueries, QueryPattern::kHotWindow,
                         TrafficSide::kRight, kPointQueryLength, 11)},
           {"adapt left hotspot",
            BuildQueries(kAdaptQueries, QueryPattern::kHotWindow,
                         TrafficSide::kLeft, kPointQueryLength, 12)}},
          {"measure left hotspot",
           BuildQueries(kMeasuredQueries, QueryPattern::kHotWindow,
                        TrafficSide::kLeft, kPointQueryLength, 13)},
          true};
}

ScenarioInput MakeRandomScenario() {
  return {"random_uniform_point",
          "uniform random point updates over the whole array",
          {{"warmup uniform random",
            BuildQueries(kWarmupQueries, QueryPattern::kUniform,
                         TrafficSide::kLeft, kPointQueryLength, 21)}},
          {"measure uniform random",
           BuildQueries(kMeasuredQueries, QueryPattern::kUniform,
                        TrafficSide::kLeft, kPointQueryLength, 22)},
          false};
}

ScenarioInput MakeShiftRandomRangeScenario() {
  return {"shift_hotspot_random_ranges",
          "right warmup, then left hotspot; final ranges have random length",
          {{"warmup right hotspot random ranges",
            BuildQueries(kRandomRangeWarmupQueries,
                         QueryPattern::kHotWindowRandomRange,
                         TrafficSide::kRight, kRandomRangeMaxLength, 31)},
           {"adapt left hotspot random ranges",
            BuildQueries(kRandomRangeAdaptQueries,
                         QueryPattern::kHotWindowRandomRange,
                         TrafficSide::kLeft, kRandomRangeMaxLength, 32)}},
          {"measure left hotspot random ranges",
           BuildQueries(kRandomRangeMeasuredQueries,
                        QueryPattern::kHotWindowRandomRange, TrafficSide::kLeft,
                        kRandomRangeMaxLength, 33)},
          true};
}

ScenarioInput MakeRandomRangeScenario() {
  return {"random_uniform_ranges",
          "uniform random ranges over the whole array; range length is random",
          {{"warmup uniform random ranges",
            BuildQueries(kRandomRangeWarmupQueries,
                         QueryPattern::kUniformRandomRange, TrafficSide::kLeft,
                         kRandomRangeMaxLength, 41)}},
          {"measure uniform random ranges",
           BuildQueries(kRandomRangeMeasuredQueries,
                        QueryPattern::kUniformRandomRange, TrafficSide::kLeft,
                        kRandomRangeMaxLength, 42)},
          false};
}

template <typename Lock>
void ExecuteQuery(Lock &lock, std::vector<int> &data, const Query &query,
                  QueryBody body, WorkerStats &stats) {
  lock.WriteQuery(query.left, query.right, [&](size_t left, size_t right) {
    if (body == QueryBody::kLockOnly) {
      return;
    }

    uint64_t sum = 0;
    for (size_t i = left; i <= right; ++i) {
      sum += static_cast<uint64_t>(static_cast<uint32_t>(data[i]));
    }

    data[query.target] += query.delta;
    stats.sum_checksum += sum;
    stats.update_checksum +=
        static_cast<uint64_t>(static_cast<uint32_t>(data[query.target]));
  });
  ++stats.queries;
}

template <typename Lock>
PhaseRunStats RunPhase(Lock &lock, std::vector<int> &data,
                       const std::vector<Query> &queries, QueryBody body) {
  std::vector<WorkerStats> worker_stats(kThreadCount);
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  std::barrier start_barrier(static_cast<std::ptrdiff_t>(kThreadCount + 1));
  const auto worker = [&](size_t thread_index) {
    const size_t begin = queries.size() * thread_index / kThreadCount;
    const size_t end = queries.size() * (thread_index + 1) / kThreadCount;

    start_barrier.arrive_and_wait();
    WorkerStats &stats = worker_stats[thread_index];
    for (size_t i = begin; i < end; ++i) {
      ExecuteQuery(lock, data, queries[i], body, stats);
    }
  };

  for (size_t thread_index = 0; thread_index < kThreadCount; ++thread_index) {
    threads.emplace_back(worker, thread_index);
  }

  const auto start = std::chrono::steady_clock::now();
  start_barrier.arrive_and_wait();
  for (std::thread &thread : threads) {
    thread.join();
  }
  const auto finish = std::chrono::steady_clock::now();

  PhaseRunStats result;
  result.seconds = std::chrono::duration<double>(finish - start).count();
  for (const WorkerStats &stats : worker_stats) {
    result.queries += stats.queries;
    result.sum_checksum += stats.sum_checksum;
    result.update_checksum += stats.update_checksum;
  }
  return result;
}

template <typename Lock> void WaitForBackgroundRebuild(Lock &, double &) {}

void WaitForBackgroundRebuild(DynamicLock<kLockCount> &, double &seconds) {
  const auto start = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(kDynamicRebuildInterval * 3);
  seconds +=
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
}

template <typename Lock> double FlushPendingTraining(Lock &lock) {
  const auto start = std::chrono::steady_clock::now();
  if constexpr (requires { lock.FlushTraining(); }) {
    lock.FlushTraining();
  }
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
      .count();
}

template <typename Lock>
double RunSetup(Lock &lock, std::vector<int> &data,
                const ScenarioInput &scenario, QueryBody body) {
  double seconds = 0.0;
  lock.StartRebuilder(kDynamicRebuildInterval, kDynamicRebuildThreshold);

  for (size_t phase_index = 0; phase_index < scenario.setup_phases.size();
       ++phase_index) {
    const PhaseInput &phase = scenario.setup_phases[phase_index];
    seconds += RunPhase(lock, data, phase.queries, body).seconds;

    if (phase_index == 0) {
      lock.ForceSaveStats();
    }
  }

  if (scenario.wait_for_background_rebuild) {
    WaitForBackgroundRebuild(lock, seconds);
  }

  seconds += FlushPendingTraining(lock);
  lock.StopRebuilder();
  return seconds;
}

template <typename Lock>
double AverageMutexesForQueries(const Lock &lock,
                                const std::vector<Query> &queries) {
  if (queries.empty()) {
    return 0.0;
  }

  constexpr size_t kMaxSamples = 100'000;
  const size_t step = std::max(size_t{1}, queries.size() / kMaxSamples);
  uint64_t total_mutexes = 0;
  uint64_t samples = 0;

  for (size_t i = 0; i < queries.size(); i += step) {
    total_mutexes += lock.CountLocksForRange(queries[i].left, queries[i].right);
    ++samples;
  }

  return static_cast<double>(total_mutexes) / static_cast<double>(samples);
}

template <typename LockFactory>
ImplementationResult RunImplementation(const std::string &name,
                                       const ScenarioInput &scenario,
                                       LockFactory factory) {
  ImplementationResult result;
  result.name = name;

  {
    auto lock = factory();
    std::vector<int> data = MakeInitialData();

    result.setup_seconds = RunSetup(lock, data, scenario, QueryBody::kRealWork);
    result.reconfigurations = lock.GetRebuildCount();
    result.left_hot_locks = lock.CountLocksForRange(0, kHotWindowSize - 1);
    result.right_hot_locks =
        lock.CountLocksForRange(kArraySize - kHotWindowSize, kArraySize - 1);

    lock.ResetRuntimeStats();
    const PhaseRunStats measured = RunPhase(
        lock, data, scenario.measured_phase.queries, QueryBody::kRealWork);
    result.body_seconds = measured.seconds;
    result.body_queries = measured.queries;
    result.avg_lock_us = lock.GetAvgLockTimeMs() * 1000.0;
    result.measured_lock_total_seconds = lock.GetTotalLockTimeMs() / 1000.0;
    result.avg_mutexes_per_query =
        AverageMutexesForQueries(lock, scenario.measured_phase.queries);
  }

  {
    auto lock = factory();
    std::vector<int> data = MakeInitialData();

    RunSetup(lock, data, scenario, QueryBody::kLockOnly);
    lock.ResetRuntimeStats();
    const PhaseRunStats measured = RunPhase(
        lock, data, scenario.measured_phase.queries, QueryBody::kLockOnly);
    result.lock_only_seconds = measured.seconds;
    result.lock_only_queries = measured.queries;
    result.lock_only_avg_lock_us = lock.GetAvgLockTimeMs() * 1000.0;
  }

  return result;
}

void PrintScenario(const ScenarioInput &scenario,
                   const std::vector<ImplementationResult> &results) {
  const ImplementationResult &baseline = results.front();

  std::cout << "scenario: " << scenario.name << '\n';
  std::cout << "  " << scenario.description << '\n';
  std::cout << "  setup queries: ";
  for (size_t i = 0; i < scenario.setup_phases.size(); ++i) {
    if (i != 0) {
      std::cout << " + ";
    }
    std::cout << scenario.setup_phases[i].queries.size();
  }
  std::cout << ", measured queries: " << scenario.measured_phase.queries.size()
            << "\n\n";

  std::cout
      << "  impl       setup_s  body_s  body_x  lock_only_s  lock_x"
      << "  avg_lock_us  lock_sum_s  avg_mutex  hot(L/R)  rebuild/train\n";

  for (const ImplementationResult &result : results) {
    const double body_speedup =
        result.body_seconds > 0.0 ? baseline.body_seconds / result.body_seconds
                                  : 0.0;
    const double lock_speedup =
        result.lock_only_seconds > 0.0
            ? baseline.lock_only_seconds / result.lock_only_seconds
            : 0.0;

    std::cout << "  " << std::left << std::setw(10) << result.name << std::right
              << std::fixed << std::setprecision(3) << std::setw(8)
              << result.setup_seconds << std::setw(8) << result.body_seconds
              << std::setw(8) << body_speedup << std::setw(13)
              << result.lock_only_seconds << std::setw(8) << lock_speedup
              << std::setw(13) << std::setprecision(2) << result.avg_lock_us
              << std::setw(12) << std::setprecision(3)
              << result.measured_lock_total_seconds << std::setw(11)
              << std::setprecision(2) << result.avg_mutexes_per_query
              << std::setw(5) << result.left_hot_locks << '/' << std::setw(2)
              << result.right_hot_locks << std::setw(10)
              << result.reconfigurations << '\n';
  }
  std::cout << '\n';
}

void RunScenario(const ScenarioInput &scenario) {
  uint64_t genetic_seed = 500;

  auto naive_factory = [] { return NaiveLock<kLockCount>(kArraySize); };
  auto dynamic_factory = [] { return DynamicLock<kLockCount>(kArraySize); };
  auto genetic_factory = [&genetic_seed] {
    return GeneticLock<kLockCount, kBlocks>(
        kArraySize, genetic_seed++, kGeneticTrainingBatchSize,
        kGeneticHistoryLimit, kGeneticPopulationSize, kGeneticEliteCount,
        kGeneticGenerationCount);
  };

  std::vector<ImplementationResult> results;
  results.push_back(RunImplementation("naive", scenario, naive_factory));
  results.push_back(RunImplementation("dynamic", scenario, dynamic_factory));
  results.push_back(RunImplementation("genetic", scenario, genetic_factory));

  PrintScenario(scenario, results);
}
} // namespace

int main() {
  std::cout << "dynamic lock comparison benchmark\n";
  std::cout << "  array size: " << kArraySize << '\n';
  std::cout << "  mutexes: " << kLockCount << ", fine blocks: " << kBlocks
            << ", fine block size: " << kBlockSize << '\n';
  std::cout << "  threads: " << kThreadCount << '\n';
  std::cout << "  point query length: " << kPointQueryLength << '\n';
  std::cout << "  random range max length: " << kRandomRangeMaxLength << '\n';
  std::cout << "  hot window size: " << kHotWindowSize << '\n';
  std::cout << "  body_s: final measured phase with sum+single update\n";
  std::cout
      << "  lock_only_s: same final coordinates after same setup, empty body\n";
  std::cout
      << "  body_x and lock_x are speedups over naive in the same scenario\n\n";

  RunScenario(MakeShiftScenario());
  RunScenario(MakeRandomScenario());
  RunScenario(MakeShiftRandomRangeScenario());
  RunScenario(MakeRandomRangeScenario());

  return 0;
}
