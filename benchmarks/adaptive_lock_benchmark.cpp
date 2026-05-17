#include "common/naive_lock.hpp"
#include "common/print_compat.hpp"
#include "common/request_workload.hpp"
#include "common/spinlock.hpp"
#include "dynamic/dynamic_lock.hpp"
#include "genetic/genetic_lock.hpp"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr size_t kLockCount = 64;
constexpr size_t kBlocks = 8192;
constexpr size_t kBlockSize = 512;
constexpr size_t kArraySize = kBlocks * kBlockSize;
constexpr size_t kHotWindowSize = kArraySize / kLockCount;
constexpr size_t kPointQueryLength = 1;
constexpr size_t kRandomRangeMaxLength = 65'536;
constexpr size_t kClusteredHotWindowSize = kHotWindowSize;
constexpr size_t kSmallRangeMaxLength = 8;
constexpr size_t kMovingWindowSize = kHotWindowSize;
constexpr size_t kMovingWindowStops = 8;

size_t BenchmarkThreadCount() {
  const size_t hardware_threads =
      std::max(1u, std::thread::hardware_concurrency());
  const char *value = std::getenv("DYNAMIC_LOCK_THREAD_COUNT");
  if (value == nullptr) {
    return std::min(size_t{32}, hardware_threads);
  }

  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (end == value || parsed == 0) {
    return std::min(size_t{32}, hardware_threads);
  }
  return std::max(size_t{1}, std::min(static_cast<size_t>(parsed),
                                      hardware_threads));
}

const size_t kThreadCount = BenchmarkThreadCount();

constexpr size_t kWarmupQueries = 2'400'000;
constexpr size_t kAdaptQueries = 7'200'000;
constexpr size_t kMeasuredQueries = 40'000'000;
constexpr size_t kRandomRangeWarmupQueries = 800'000;
constexpr size_t kRandomRangeAdaptQueries = 2'400'000;
constexpr size_t kRandomRangeMeasuredQueries = 2'000'000;
constexpr size_t kMovingWindowWarmupQueries = 4'000'000;
constexpr size_t kMovingWindowAdaptQueriesPerStop = 6'000'000;
constexpr size_t kMovingWindowMeasuredQueriesPerStop = 20'000'000;
constexpr size_t kChurnChanges = 5;
constexpr size_t kChurnAdaptQueries = 3'200'000;
constexpr size_t kChurnMeasuredQueries = 8'000'000;
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

struct TimedPhaseInput {
  std::string name;
  std::vector<Query> adapt_queries;
  std::vector<Query> measure_queries;
  double adapt_seconds = 0.0;
};

struct ScenarioInput {
  std::string name;
  std::string description;
  std::vector<PhaseInput> setup_phases;
  PhaseInput measured_phase;
  std::vector<TimedPhaseInput> timed_phases;
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

std::vector<Query>
BuildMultiWindowQueries(size_t count, const std::vector<size_t> &window_begins,
                        size_t window_size, size_t max_query_length,
                        uint64_t seed) {
  if (window_begins.empty()) {
    throw std::invalid_argument("window_begins must be non-empty");
  }

  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<size_t> window_dist(0,
                                                    window_begins.size() - 1);
  std::vector<Query> queries;
  queries.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    const size_t window_begin = window_begins[window_dist(rng)];
    queries.push_back(MakeWindowRandomRangeQuery(rng, window_begin, window_size,
                                                 max_query_length));
  }

  return queries;
}

std::vector<size_t> MakeClusteredWindowBegins(size_t cluster_count) {
  if (cluster_count == 0 || cluster_count > 16) {
    throw std::invalid_argument("cluster_count must be in [1, 16]");
  }
  const size_t gap = kClusteredHotWindowSize;
  std::vector<size_t> begins;
  begins.reserve(cluster_count);
  for (size_t i = 0; i < cluster_count; ++i) {
    begins.push_back((2 * i + 1) * gap);
  }
  return begins;
}

std::vector<size_t> MakeClusteredWindowPool() {
  std::vector<size_t> begins;
  const size_t step = 2 * kClusteredHotWindowSize;
  for (size_t begin = kClusteredHotWindowSize;
       begin + kClusteredHotWindowSize <= kArraySize; begin += step) {
    begins.push_back(begin);
  }
  return begins;
}

std::vector<size_t> MakeChurnedClusteredWindowBegins(size_t cluster_count,
                                                     size_t change_index) {
  std::vector<size_t> active = MakeClusteredWindowBegins(cluster_count);
  const std::vector<size_t> pool = MakeClusteredWindowPool();
  const size_t replacements = std::max(size_t{1}, cluster_count / 2);
  size_t cursor = cluster_count;

  for (size_t change = 0; change < change_index; ++change) {
    for (size_t index = 0; index < replacements; ++index) {
      const size_t slot = (change * replacements + index) % cluster_count;
      while (std::find(active.begin(), active.end(), pool[cursor]) !=
             active.end()) {
        cursor = (cursor + 1) % pool.size();
      }
      active[slot] = pool[cursor];
      cursor = (cursor + 1) % pool.size();
    }
  }

  std::sort(active.begin(), active.end());
  return active;
}

std::vector<size_t> MakeMovingWindowBegins() {
  std::vector<size_t> begins;
  begins.reserve(2 * kMovingWindowStops - 2);

  const size_t max_begin = kArraySize - kMovingWindowSize;
  for (size_t stop = 0; stop < kMovingWindowStops; ++stop) {
    begins.push_back(max_begin * stop / (kMovingWindowStops - 1));
  }
  for (size_t stop = kMovingWindowStops - 1; stop-- > 1;) {
    begins.push_back(max_begin * stop / (kMovingWindowStops - 1));
  }

  return begins;
}

double ReadEnvDouble(const char *name, double default_value) {
  const char *value = std::getenv(name);
  if (value == nullptr) {
    return default_value;
  }

  char *end = nullptr;
  const double parsed = std::strtod(value, &end);
  if (end == value || parsed < 0.0) {
    return default_value;
  }
  return parsed;
}

size_t ReadEnvSize(const char *name, size_t default_value) {
  const char *value = std::getenv(name);
  if (value == nullptr) {
    return default_value;
  }

  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (end == value || parsed == 0) {
    return default_value;
  }
  return static_cast<size_t>(parsed);
}

size_t QueryDivisor() {
  static const size_t divisor = ReadEnvSize("DYNAMIC_LOCK_QUERY_DIVISOR", 1);
  return divisor;
}

size_t ScaleQueryCount(size_t count) {
  return std::max(size_t{1}, count / QueryDivisor());
}

double MovingWindowAdaptSecondsPerStop() {
  static const double seconds_per_direction =
      ReadEnvDouble("DYNAMIC_LOCK_MOVING_SECONDS_PER_DIRECTION", 0.0);
  if (seconds_per_direction <= 0.0) {
    return 0.0;
  }
  return seconds_per_direction / static_cast<double>(kMovingWindowStops);
}

std::vector<TimedPhaseInput> BuildMovingWindowPhases(uint64_t seed) {
  const std::vector<size_t> begins = MakeMovingWindowBegins();
  const double adapt_seconds_per_stop = MovingWindowAdaptSecondsPerStop();
  std::vector<TimedPhaseInput> phases;
  phases.reserve(begins.size());

  for (size_t i = 0; i < begins.size(); ++i) {
    phases.push_back(
        {"window stop " + std::to_string(i),
         BuildMultiWindowQueries(
             ScaleQueryCount(kMovingWindowAdaptQueriesPerStop), {begins[i]},
             kMovingWindowSize, kSmallRangeMaxLength, seed + 2 * i),
         BuildMultiWindowQueries(
             ScaleQueryCount(kMovingWindowMeasuredQueriesPerStop), {begins[i]},
             kMovingWindowSize, kSmallRangeMaxLength, seed + 2 * i + 1),
         adapt_seconds_per_stop});
  }

  return phases;
}

ScenarioInput MakeShiftScenario() {
  return {
      "shift_hotspot_point",
      "right warmup, then left hotspot; final queries are point updates",
      {{"warmup right hotspot",
        BuildQueries(ScaleQueryCount(kWarmupQueries), QueryPattern::kHotWindow,
                     TrafficSide::kRight, kPointQueryLength, 11)},
       {"adapt left hotspot",
        BuildQueries(ScaleQueryCount(kAdaptQueries), QueryPattern::kHotWindow,
                     TrafficSide::kLeft, kPointQueryLength, 12)}},
      {"measure left hotspot",
       BuildQueries(ScaleQueryCount(kMeasuredQueries), QueryPattern::kHotWindow,
                    TrafficSide::kLeft, kPointQueryLength, 13)},
      {},
      true};
}

ScenarioInput MakeClusteredWindowsScenario(size_t cluster_count,
                                           uint64_t seed_base) {
  const std::vector<size_t> window_begins =
      MakeClusteredWindowBegins(cluster_count);
  return {"clustered_" + std::to_string(cluster_count) + "_hot_windows",
          std::to_string(cluster_count) +
              " " + std::to_string(kClusteredHotWindowSize) +
              "-element hot windows; final requests are point updates",
          {{"warmup clustered small windows",
            BuildMultiWindowQueries(ScaleQueryCount(kWarmupQueries),
                                    window_begins, kClusteredHotWindowSize,
                                    kPointQueryLength, seed_base)},
           {"adapt clustered small windows",
            BuildMultiWindowQueries(ScaleQueryCount(kAdaptQueries),
                                    window_begins, kClusteredHotWindowSize,
                                    kPointQueryLength, seed_base + 1)}},
          {"measure clustered small windows",
           BuildMultiWindowQueries(ScaleQueryCount(kMeasuredQueries),
                                   window_begins, kClusteredHotWindowSize,
                                   kPointQueryLength, seed_base + 2)},
          {},
          true};
}

ScenarioInput MakeClusteredChurnScenario(size_t cluster_count,
                                         uint64_t seed_base) {
  std::vector<TimedPhaseInput> phases;
  phases.reserve(kChurnChanges);
  for (size_t change = 1; change <= kChurnChanges; ++change) {
    const std::vector<size_t> window_begins =
        MakeChurnedClusteredWindowBegins(cluster_count, change);
    phases.push_back(
        {"churn change " + std::to_string(change),
         BuildMultiWindowQueries(ScaleQueryCount(kChurnAdaptQueries),
                                 window_begins, kClusteredHotWindowSize,
                                 kPointQueryLength, seed_base + 10 * change),
         BuildMultiWindowQueries(ScaleQueryCount(kChurnMeasuredQueries),
                                 window_begins, kClusteredHotWindowSize,
                                 kPointQueryLength,
                                 seed_base + 10 * change + 1),
         0.0});
  }

  return {"clustered_churn_" + std::to_string(cluster_count) + "_hot_windows",
          std::to_string(cluster_count) +
              " compact hot windows; five phases replace half of the active "
              "windows before measuring point requests",
          {{"warmup initial clustered windows",
            BuildMultiWindowQueries(
                ScaleQueryCount(kWarmupQueries),
                MakeChurnedClusteredWindowBegins(cluster_count, 0),
                kClusteredHotWindowSize, kPointQueryLength, seed_base)}},
          {},
          std::move(phases),
          true};
}

ScenarioInput MakeMovingWindowScenario() {
  return {
      "moving_small_window",
      std::to_string(kMovingWindowSize) +
          "-element hot window moves left-to-right and back; each stop "
      "runs long enough for online repartitioning attempts",
      {{"warmup first moving window",
        BuildMultiWindowQueries(ScaleQueryCount(kMovingWindowWarmupQueries),
                                {0}, kMovingWindowSize, kSmallRangeMaxLength,
                                61)}},
      {},
      BuildMovingWindowPhases(62),
      true,
  };
}

ScenarioInput MakeRandomScenario() {
  return {
      "random_uniform_point",
      "uniform random point updates over the whole array",
      {{"warmup uniform random",
        BuildQueries(ScaleQueryCount(kWarmupQueries), QueryPattern::kUniform,
                     TrafficSide::kLeft, kPointQueryLength, 21)}},
      {"measure uniform random",
       BuildQueries(ScaleQueryCount(kMeasuredQueries), QueryPattern::kUniform,
                    TrafficSide::kLeft, kPointQueryLength, 22)},
      {},
      false};
}

ScenarioInput MakeShiftRandomRangeScenario() {
  return {"shift_hotspot_random_ranges",
          "right warmup, then left hotspot; final ranges have random length",
          {{"warmup right hotspot random ranges",
            BuildQueries(ScaleQueryCount(kRandomRangeWarmupQueries),
                         QueryPattern::kHotWindowRandomRange,
                         TrafficSide::kRight, kRandomRangeMaxLength, 31)},
           {"adapt left hotspot random ranges",
            BuildQueries(ScaleQueryCount(kRandomRangeAdaptQueries),
                         QueryPattern::kHotWindowRandomRange,
                         TrafficSide::kLeft, kRandomRangeMaxLength, 32)}},
          {"measure left hotspot random ranges",
           BuildQueries(ScaleQueryCount(kRandomRangeMeasuredQueries),
                        QueryPattern::kHotWindowRandomRange, TrafficSide::kLeft,
                        kRandomRangeMaxLength, 33)},
          {},
          true};
}

ScenarioInput MakeRandomRangeScenario() {
  return {"random_uniform_ranges",
          "uniform random ranges over the whole array; range length is random",
          {{"warmup uniform random ranges",
            BuildQueries(ScaleQueryCount(kRandomRangeWarmupQueries),
                         QueryPattern::kUniformRandomRange, TrafficSide::kLeft,
                         kRandomRangeMaxLength, 41)}},
          {"measure uniform random ranges",
           BuildQueries(ScaleQueryCount(kRandomRangeMeasuredQueries),
                        QueryPattern::kUniformRandomRange, TrafficSide::kLeft,
                        kRandomRangeMaxLength, 42)},
          {},
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

template <typename Mutex>
void WaitForBackgroundRebuild(DynamicLock<kLockCount, Mutex> &,
                              double &seconds) {
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
    const auto &[_, queries] = scenario.setup_phases[phase_index];
    seconds += RunPhase(lock, data, queries, body).seconds;

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

template <typename Lock> double ForceImmediateAdaptation(Lock &lock) {
  const auto start = std::chrono::steady_clock::now();
  if constexpr (requires { lock.RebuildNow(); }) {
    lock.RebuildNow();
  }
  if constexpr (requires { lock.FlushTraining(); }) {
    lock.FlushTraining();
  }
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
      .count();
}

struct MeasurementStats {
  double seconds = 0.0;
  uint64_t queries = 0;
  double lock_total_ms = 0.0;
  double mutex_query_sum = 0.0;
};

void AddMeasurement(MeasurementStats &total, const MeasurementStats &part) {
  total.seconds += part.seconds;
  total.queries += part.queries;
  total.lock_total_ms += part.lock_total_ms;
  total.mutex_query_sum += part.mutex_query_sum;
}

template <typename Lock>
MeasurementStats RunMeasuredQueries(Lock &lock, std::vector<int> &data,
                                    const std::vector<Query> &queries,
                                    QueryBody body) {
  lock.ResetRuntimeStats();
  const PhaseRunStats measured = RunPhase(lock, data, queries, body);

  MeasurementStats stats;
  stats.seconds = measured.seconds;
  stats.queries = measured.queries;
  stats.lock_total_ms = lock.GetTotalLockTimeMs();
  stats.mutex_query_sum = AverageMutexesForQueries(lock, queries) *
                          static_cast<double>(measured.queries);
  return stats;
}

template <typename Lock>
double RunAdaptQueries(Lock &lock, std::vector<int> &data,
                       const TimedPhaseInput &phase, QueryBody body) {
  if (phase.adapt_seconds <= 0.0) {
    return RunPhase(lock, data, phase.adapt_queries, body).seconds;
  }

  const auto start = std::chrono::steady_clock::now();
  const auto deadline =
      start + std::chrono::duration<double>(phase.adapt_seconds);
  do {
    RunPhase(lock, data, phase.adapt_queries, body);
  } while (std::chrono::steady_clock::now() < deadline);

  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
      .count();
}

template <typename Lock>
MeasurementStats RunTimedMeasurements(Lock &lock, std::vector<int> &data,
                                      const ScenarioInput &scenario,
                                      QueryBody body, double &setup_seconds) {
  if (scenario.timed_phases.empty()) {
    return RunMeasuredQueries(lock, data, scenario.measured_phase.queries,
                              body);
  }

  MeasurementStats total;
  for (const TimedPhaseInput &phase : scenario.timed_phases) {
    lock.StartRebuilder(kDynamicRebuildInterval, kDynamicRebuildThreshold);
    setup_seconds += RunAdaptQueries(lock, data, phase, body);
    setup_seconds += ForceImmediateAdaptation(lock);
    lock.StopRebuilder();

    AddMeasurement(total,
                   RunMeasuredQueries(lock, data, phase.measure_queries, body));
  }

  return total;
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
    const size_t setup_reconfigurations = lock.GetRebuildCount();

    const size_t reconfigurations_after_reset = lock.GetRebuildCount();
    const MeasurementStats measured = RunTimedMeasurements(
        lock, data, scenario, QueryBody::kRealWork, result.setup_seconds);

    result.body_seconds = measured.seconds;
    result.body_queries = measured.queries;
    result.avg_lock_us = measured.queries == 0
                             ? 0.0
                             : measured.lock_total_ms * 1000.0 /
                                   static_cast<double>(measured.queries);
    result.measured_lock_total_seconds = measured.lock_total_ms / 1000.0;
    result.avg_mutexes_per_query =
        measured.queries == 0
            ? 0.0
            : measured.mutex_query_sum / static_cast<double>(measured.queries);
    result.reconfigurations =
        setup_reconfigurations +
        (lock.GetRebuildCount() - reconfigurations_after_reset);
    result.left_hot_locks = lock.CountLocksForRange(0, kHotWindowSize - 1);
    result.right_hot_locks =
        lock.CountLocksForRange(kArraySize - kHotWindowSize, kArraySize - 1);
  }
  {
    auto lock = factory();
    std::vector<int> data = MakeInitialData();

    RunSetup(lock, data, scenario, QueryBody::kLockOnly);
    double ignored_setup_seconds = 0.0;
    const MeasurementStats measured = RunTimedMeasurements(
        lock, data, scenario, QueryBody::kLockOnly, ignored_setup_seconds);

    result.lock_only_seconds = measured.seconds;
    result.lock_only_queries = measured.queries;
    result.lock_only_avg_lock_us =
        measured.queries == 0 ? 0.0
                              : measured.lock_total_ms * 1000.0 /
                                    static_cast<double>(measured.queries);
  }

  return result;
}

void PrintScenario(const ScenarioInput &scenario,
                   const std::vector<ImplementationResult> &results) {
  const ImplementationResult &baseline = results.front();
  uint64_t timed_adapt_queries = 0;
  uint64_t measured_queries = scenario.measured_phase.queries.size();
  double timed_adapt_seconds = 0.0;
  for (const TimedPhaseInput &phase : scenario.timed_phases) {
    timed_adapt_queries += phase.adapt_queries.size();
    measured_queries += phase.measure_queries.size();
    timed_adapt_seconds += phase.adapt_seconds;
  }

  std::println("scenario: {}", scenario.name);
  std::println("  {}", scenario.description);
  std::print("  setup queries: ");
  for (size_t i = 0; i < scenario.setup_phases.size(); ++i) {
    if (i != 0) {
      std::print(" + ");
    }
    std::print("{}", scenario.setup_phases[i].queries.size());
  }
  if (timed_adapt_queries != 0) {
    std::print(" + {} timed adapt", timed_adapt_queries);
    if (timed_adapt_seconds > 0.0) {
      std::print(" (time-capped to {:.1f}s total across timed phases)",
                 timed_adapt_seconds);
    }
  }
  std::print(", measured queries: {}\n\n", measured_queries);
  if (!scenario.timed_phases.empty()) {
    std::println("  timed scenario: each phase is adapted, rebuilt, then "
                 "measured with online adaptation disabled\n");
  }

  std::println("  impl       setup_s  body_s  body_x  lock_only_s  lock_x"
               "  avg_lock_us  lock_sum_s  avg_mutex  hot(L/R)  rebuild/train");

  for (const ImplementationResult &result : results) {
    const double body_speedup =
        result.body_seconds > 0.0 ? baseline.body_seconds / result.body_seconds
                                  : 0.0;
    const double lock_speedup =
        result.lock_only_seconds > 0.0
            ? baseline.lock_only_seconds / result.lock_only_seconds
            : 0.0;

    std::println("  {:<10}{:>8.3f}{:>8.3f}{:>8.3f}{:>13.3f}{:>8.3f}{:>13.2f}"
                 "{:>12.3f}{:>11.2f}{:>5}/{:>2}{:>10}",
                 result.name, result.setup_seconds, result.body_seconds,
                 body_speedup, result.lock_only_seconds, lock_speedup,
                 result.avg_lock_us, result.measured_lock_total_seconds,
                 result.avg_mutexes_per_query, result.left_hot_locks,
                 result.right_hot_locks, result.reconfigurations);
  }
  std::println("");
}

template <typename Lock = std::mutex>
std::vector<ImplementationResult>
RunScenarioResults(const ScenarioInput &scenario) {
  uint64_t genetic_seed = 500;

  auto naive_factory = [] { return NaiveLock<kLockCount, Lock>(kArraySize); };
  auto dynamic_factory = [] {
    return DynamicLock<kLockCount, Lock>(kArraySize);
  };
  auto genetic_factory = [&genetic_seed] {
    return GeneticLock<kLockCount, kBlocks, Lock>(
        kArraySize, genetic_seed++, kGeneticTrainingBatchSize,
        kGeneticHistoryLimit, kGeneticPopulationSize, kGeneticEliteCount,
        kGeneticGenerationCount);
  };

  std::vector<ImplementationResult> results;
  results.push_back(RunImplementation("naive", scenario, naive_factory));
  results.push_back(RunImplementation("dynamic", scenario, dynamic_factory));
  results.push_back(RunImplementation("genetic", scenario, genetic_factory));

  return results;
}

template <typename Lock = std::mutex>
std::vector<ImplementationResult> RunScenario(const ScenarioInput &scenario) {
  std::vector<ImplementationResult> results =
      RunScenarioResults<Lock>(scenario);
  PrintScenario(scenario, results);
  return results;
}

bool ShouldRunScenario(const ScenarioInput &scenario) {
  const char *filter = std::getenv("DYNAMIC_LOCK_SCENARIO");
  return filter == nullptr || scenario.name.find(filter) != std::string::npos;
}

const ImplementationResult &
FindResult(const std::vector<ImplementationResult> &results,
           const std::string &name) {
  const auto it = std::find_if(
      results.begin(), results.end(),
      [&](const ImplementationResult &result) { return result.name == name; });
  if (it == results.end()) {
    throw std::logic_error("missing benchmark result: " + name);
  }
  return *it;
}

double SafeSpeedup(double baseline_seconds, double measured_seconds) {
  return measured_seconds > 0.0 ? baseline_seconds / measured_seconds : 0.0;
}

void PrintPointSpinComparison(
    const ScenarioInput &scenario,
    const std::vector<ImplementationResult> &mutex_results,
    const std::vector<ImplementationResult> &spin_results) {
  const ImplementationResult &spin_naive = FindResult(spin_results, "naive");

  std::println("point lock primitive comparison: {}", scenario.name);
  std::print("  spin naive baseline: body_s={:.3f}, lock_only_s={:.3f}\n\n",
             spin_naive.body_seconds, spin_naive.lock_only_seconds);
  std::println("  impl       spin_body_s  spin_x  spin_lock_only_s  spin_lock_x"
               "  mutex_body_s  mutex_lock_only_s  spin_vs_mutex_body"
               "  spin_vs_mutex_lock");

  for (const std::string &name : {"dynamic", "genetic"}) {
    const ImplementationResult &spin_result = FindResult(spin_results, name);
    const ImplementationResult &mutex_result = FindResult(mutex_results, name);

    std::println(
        "  {:<10}{:>12.3f}{:>8.3f}{:>18.3f}{:>13.3f}{:>14.3f}{:>19.3f}"
        "{:>20.3f}{:>20.3f}",
        name, spin_result.body_seconds,
        SafeSpeedup(spin_naive.body_seconds, spin_result.body_seconds),
        spin_result.lock_only_seconds,
        SafeSpeedup(spin_naive.lock_only_seconds,
                    spin_result.lock_only_seconds),
        mutex_result.body_seconds, mutex_result.lock_only_seconds,
        SafeSpeedup(mutex_result.body_seconds, spin_result.body_seconds),
        SafeSpeedup(mutex_result.lock_only_seconds,
                    spin_result.lock_only_seconds));
  }
  std::println("");
}
} // namespace

int main() {
  std::println("dynamic lock comparison benchmark");
  std::println("  array size: {}", kArraySize);
  std::println("  mutexes: {}, fine blocks: {}, fine block size: {}",
               kLockCount, kBlocks, kBlockSize);
  std::println("  threads: {}", kThreadCount);
  std::println("  point query length: {}", kPointQueryLength);
  std::println("  random range max length: {}", kRandomRangeMaxLength);
  std::println("  hot window size: {}", kHotWindowSize);
  std::println("  clustered hot window size: {}", kClusteredHotWindowSize);
  std::println("  small range max length: {}", kSmallRangeMaxLength);
  std::println("  moving window size: {}", kMovingWindowSize);
  std::println("  moving window stops: {}", 2 * kMovingWindowStops - 2);
  std::println("  query divisor: {}", QueryDivisor());
  std::println("  body_s: final measured phase with sum+single update");
  std::println(
      "  lock_only_s: same final coordinates after same setup, empty body");
  std::print(
      "  body_x and lock_x are speedups over naive in the same scenario\n\n");

  const ScenarioInput shift_point = MakeShiftScenario();
  const ScenarioInput clustered_2_windows = MakeClusteredWindowsScenario(2, 51);
  const ScenarioInput clustered_4_windows = MakeClusteredWindowsScenario(4, 61);
  const ScenarioInput clustered_churn_2_windows =
      MakeClusteredChurnScenario(2, 71);
  const ScenarioInput clustered_churn_4_windows =
      MakeClusteredChurnScenario(4, 91);
  const ScenarioInput moving_window = MakeMovingWindowScenario();
  const ScenarioInput random_point = MakeRandomScenario();
  const bool has_scenario_filter =
      std::getenv("DYNAMIC_LOCK_SCENARIO") != nullptr;

  const auto run_if_selected =
      [](const ScenarioInput &scenario) -> std::vector<ImplementationResult> {
    if (ShouldRunScenario(scenario)) {
      return RunScenario(scenario);
    }
    return {};
  };

  const std::vector<ImplementationResult> shift_mutex_results =
      run_if_selected(shift_point);
  run_if_selected(clustered_2_windows);
  run_if_selected(clustered_4_windows);
  run_if_selected(clustered_churn_2_windows);
  run_if_selected(clustered_churn_4_windows);
  run_if_selected(moving_window);
  const std::vector<ImplementationResult> random_mutex_results =
      run_if_selected(random_point);
  run_if_selected(MakeShiftRandomRangeScenario());
  run_if_selected(MakeRandomRangeScenario());

  if (has_scenario_filter) {
    return 0;
  }

  const std::vector<ImplementationResult> shift_spin_results =
      RunScenarioResults<spinlock>(shift_point);
  const std::vector<ImplementationResult> random_spin_results =
      RunScenarioResults<spinlock>(random_point);

  PrintPointSpinComparison(shift_point, shift_mutex_results,
                           shift_spin_results);
  PrintPointSpinComparison(random_point, random_mutex_results,
                           random_spin_results);

  return 0;
}
