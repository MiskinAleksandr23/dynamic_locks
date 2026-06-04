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
#include <stdexcept>
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
constexpr size_t kClusteredHotWindowSize = kHotWindowSize;
constexpr size_t kSmallRangeMaxLength = 8;
constexpr size_t kMovingWindowSize = kHotWindowSize;
constexpr size_t kMovingWindowStops = 8;

size_t BenchmarkThreadCount() {
  const size_t hardware_threads =
      std::max(1u, std::thread::hardware_concurrency());
  const char *value = std::getenv("DYNAMIC_LOCK_THREAD_COUNT");
  if (value == nullptr) {
    return hardware_threads;
  }

  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (end == value || parsed == 0) {
    return hardware_threads;
  }
  return std::max(size_t{1}, static_cast<size_t>(parsed));
}

const size_t kThreadCount = BenchmarkThreadCount();

constexpr size_t kWarmupQueries = 6'400'000;
constexpr size_t kAdaptQueries = 19'200'000;
constexpr size_t kMeasuredQueries = 120'000'000;
constexpr size_t kRandomRangeWarmupQueries = 1'600'000;
constexpr size_t kRandomRangeAdaptQueries = 4'800'000;
constexpr size_t kRandomRangeMeasuredQueries = 4'000'000;
constexpr size_t kMovingWindowWarmupQueries = 12'800'000;
constexpr size_t kMovingWindowAdaptQueriesPerStop = 19'200'000;
constexpr size_t kMovingWindowMeasuredQueriesPerStop = 70'000'000;
constexpr size_t kChurnChanges = 5;
constexpr size_t kChurnAdaptQueries = 9'600'000;
constexpr size_t kChurnMeasuredQueries = 20'000'000;
constexpr auto kDefaultDynamicRebuildInterval =
    std::chrono::milliseconds(10'000);
constexpr double kDefaultDynamicRebuildThreshold = 2.0;
constexpr double kDefaultDynamicRebuildMinGain = 1.0;
constexpr double kDefaultDynamicRebuildMinSkew = 4.0;

constexpr size_t kDefaultGeneticTrainingBatchSize = 100'000;
constexpr size_t kDefaultGeneticTrainingSampleRate = 64;
constexpr size_t kDefaultGeneticTrainingProbeGap = 1'000'000;
constexpr double kDefaultGeneticMinTrainingSkew = 4.0;
constexpr size_t kGeneticHistoryLimit = 12'000;
constexpr size_t kGeneticPopulationSize = 24;
constexpr size_t kGeneticEliteCount = 4;
constexpr size_t kGeneticGenerationCount = 0;

enum class QueryPattern {
  kHotWindow,
  kUniform,
  kHotWindowRandomRange,
  kUniformRandomRange
};

enum class QueryBody { kRealWork, kLockOnly };

struct QueryStreamSpec {
  size_t count = 0;
  QueryPattern pattern = QueryPattern::kUniform;
  TrafficSide side = TrafficSide::kLeft;
  size_t query_length = 1;
  uint64_t seed = 0;
  std::vector<size_t> window_begins;
  size_t window_size = 0;
  bool multi_window = false;
  bool thread_grouped_windows = false;
  bool thread_striped_windows = false;
};

struct PhaseInput {
  std::string name;
  QueryStreamSpec queries;
};

struct TimedPhaseInput {
  std::string name;
  QueryStreamSpec adapt_queries;
  QueryStreamSpec measure_queries;
  double adapt_seconds = 0.0;
};

struct ScenarioInput {
  std::string name;
  std::string description;
  std::vector<PhaseInput> setup_phases;
  PhaseInput measured_phase;
  std::vector<TimedPhaseInput> timed_phases;
  std::vector<size_t> debug_window_begins;
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
  double total_seconds = 0.0;
  double setup_seconds = 0.0;
  double measured_seconds = 0.0;
  double lock_only_total_seconds = 0.0;
  double avg_lock_us = 0.0;
  double lock_only_avg_lock_us = 0.0;
  double measured_lock_total_seconds = 0.0;
  double avg_mutexes_per_query = 0.0;
  size_t left_hot_locks = 0;
  size_t right_hot_locks = 0;
  size_t reconfigurations = 0;
  uint64_t total_queries = 0;
  uint64_t lock_only_queries = 0;
  std::vector<size_t> partition_cuts;
  std::vector<size_t> debug_window_locks;
};

std::vector<int> MakeInitialData() {
  std::vector<int> data(kArraySize);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<int>((i * 17 + 23) % 1000);
  }
  return data;
}

QueryStreamSpec BuildQueries(size_t count, QueryPattern pattern,
                             TrafficSide side, size_t query_length,
                             uint64_t seed) {
  return {count, pattern, side, query_length, seed, {}, 0, false, false, false};
}

QueryStreamSpec
BuildMultiWindowQueries(size_t count, const std::vector<size_t> &window_begins,
                        size_t window_size, size_t max_query_length,
                        uint64_t seed, bool thread_grouped_windows = false,
                        bool thread_striped_windows = false) {
  if (window_begins.empty()) {
    throw std::invalid_argument("window_begins must be non-empty");
  }

  return {count,
          QueryPattern::kHotWindowRandomRange,
          TrafficSide::kLeft,
          max_query_length,
          seed,
          window_begins,
          window_size,
          true,
          thread_grouped_windows,
          thread_striped_windows};
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
      {}};
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
          window_begins};
}

ScenarioInput MakeClusteredThreadGroupsScenario(size_t cluster_count,
                                                uint64_t seed_base) {
  const std::vector<size_t> window_begins =
      MakeClusteredWindowBegins(cluster_count);
  return {"clustered_" + std::to_string(cluster_count) + "_thread_groups",
          std::to_string(cluster_count) +
              " hot windows; threads are split evenly across windows and "
              "striped inside each window",
          {{"warmup grouped clustered windows",
            BuildMultiWindowQueries(ScaleQueryCount(kWarmupQueries),
                                    window_begins, kClusteredHotWindowSize,
                                    kPointQueryLength, seed_base, true, true)},
           {"adapt grouped clustered windows",
            BuildMultiWindowQueries(ScaleQueryCount(kAdaptQueries),
                                    window_begins, kClusteredHotWindowSize,
                                    kPointQueryLength, seed_base + 1, true,
                                    true)}},
          {"measure grouped clustered windows",
           BuildMultiWindowQueries(ScaleQueryCount(kMeasuredQueries),
                                   window_begins, kClusteredHotWindowSize,
                                   kPointQueryLength, seed_base + 2, true,
                                   true)},
          {},
          window_begins};
}

ScenarioInput MakeClusteredThreadGroupsContendedScenario(size_t cluster_count,
                                                         uint64_t seed_base) {
  const std::vector<size_t> window_begins =
      MakeClusteredWindowBegins(cluster_count);
  return {"clustered_" + std::to_string(cluster_count) +
              "_thread_groups_contended",
          std::to_string(cluster_count) +
              " hot windows; threads are split evenly across windows and "
              "randomize inside each assigned window",
          {{"warmup grouped contended windows",
            BuildMultiWindowQueries(ScaleQueryCount(kWarmupQueries),
                                    window_begins, kClusteredHotWindowSize,
                                    kPointQueryLength, seed_base, true, false)},
           {"adapt grouped contended windows",
            BuildMultiWindowQueries(ScaleQueryCount(kAdaptQueries),
                                    window_begins, kClusteredHotWindowSize,
                                    kPointQueryLength, seed_base + 1, true,
                                    false)}},
          {"measure grouped contended windows",
           BuildMultiWindowQueries(ScaleQueryCount(kMeasuredQueries),
                                   window_begins, kClusteredHotWindowSize,
                                   kPointQueryLength, seed_base + 2, true,
                                   false)},
          {},
          window_begins};
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
          std::move(phases)};
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
      {}};
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
          {}};
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
          {}};
}

struct FastRng {
  uint64_t state;

  explicit FastRng(uint64_t seed) : state(seed) {}

  uint64_t Next() {
    uint64_t value = (state += 0x9E3779B97F4A7C15ULL);
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
  }

  size_t Uniform(size_t bound) {
    return bound == 0 ? 0 : static_cast<size_t>(Next() % bound);
  }
};

uint64_t ThreadSeed(uint64_t seed, size_t thread_index) {
  return seed ^ (0xD1B54A32D192ED03ULL * (thread_index + 1));
}

size_t DeterministicOffset(size_t sequence_index, size_t thread_index,
                           size_t bound) {
  if (bound == 0) {
    return 0;
  }
  return (sequence_index + thread_index * 131) % bound;
}

Query MakeStreamQuery(FastRng &rng, const QueryStreamSpec &spec,
                      size_t thread_index, size_t sequence_index) {
  const size_t length_cap = std::max(size_t{1}, spec.query_length);
  const int delta = static_cast<int>(1 + ((sequence_index + thread_index) % 7));

  if (spec.multi_window) {
    const size_t window_count = spec.window_begins.size();
    const size_t window_index =
        spec.thread_grouped_windows
            ? std::min(thread_index * window_count / kThreadCount,
                       window_count - 1)
            : (sequence_index + thread_index) % window_count;
    size_t window_begin = spec.window_begins[window_index];
    size_t window_size = std::max(size_t{1}, spec.window_size);

    if (spec.thread_striped_windows) {
      const size_t first_thread = window_index * kThreadCount / window_count;
      const size_t end_thread = (window_index + 1) * kThreadCount / window_count;
      const size_t group_threads = std::max(size_t{1}, end_thread - first_thread);
      const size_t local_thread =
          std::min(thread_index - first_thread, group_threads - 1);
      const size_t window_blocks = std::max(size_t{1}, window_size / kBlockSize);
      const size_t stripes = std::min(group_threads, window_blocks);
      const size_t stripe_index = local_thread % stripes;
      const size_t first_block = stripe_index * window_blocks / stripes;
      const size_t end_block = (stripe_index + 1) * window_blocks / stripes;
      window_begin += first_block * kBlockSize;
      window_size = std::max(size_t{1}, (end_block - first_block) * kBlockSize);
    }

    const size_t length =
        1 + rng.Uniform(std::min(length_cap, window_size));
    const size_t left =
        window_begin +
        (length == 1
             ? DeterministicOffset(sequence_index, thread_index, window_size)
             : rng.Uniform(window_size - length + 1));
    const size_t target = left + (length == 1 ? 0 : rng.Uniform(length));
    return {static_cast<uint32_t>(left),
            static_cast<uint32_t>(left + length - 1),
            static_cast<uint32_t>(target), delta};
  }

  if (spec.pattern == QueryPattern::kUniform) {
    const size_t length = std::min(length_cap, kArraySize);
    const size_t left =
        length == 1
            ? DeterministicOffset(sequence_index, thread_index, kArraySize)
            : rng.Uniform(kArraySize - length + 1);
    const size_t target = left + (length == 1 ? 0 : rng.Uniform(length));
    return {static_cast<uint32_t>(left),
            static_cast<uint32_t>(left + length - 1),
            static_cast<uint32_t>(target), delta};
  }

  if (spec.pattern == QueryPattern::kUniformRandomRange) {
    const size_t length = 1 + rng.Uniform(std::min(length_cap, kArraySize));
    const size_t left = rng.Uniform(kArraySize - length + 1);
    const size_t target = left + rng.Uniform(length);
    return {static_cast<uint32_t>(left),
            static_cast<uint32_t>(left + length - 1),
            static_cast<uint32_t>(target), delta};
  }

  const size_t hot_begin =
      spec.side == TrafficSide::kLeft ? 0 : kArraySize - kHotWindowSize;
  const size_t length =
      spec.pattern == QueryPattern::kHotWindow
          ? std::min(length_cap, kHotWindowSize)
          : 1 + rng.Uniform(std::min(length_cap, kHotWindowSize));
  const size_t left =
      hot_begin +
      (length == 1
           ? DeterministicOffset(sequence_index, thread_index, kHotWindowSize)
           : rng.Uniform(kHotWindowSize - length + 1));
  const size_t target = left + (length == 1 ? 0 : rng.Uniform(length));
  return {static_cast<uint32_t>(left),
          static_cast<uint32_t>(left + length - 1),
          static_cast<uint32_t>(target), delta};
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
                       const QueryStreamSpec &queries, QueryBody body) {
  std::vector<WorkerStats> worker_stats(kThreadCount);
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  std::barrier start_barrier(static_cast<std::ptrdiff_t>(kThreadCount + 1));
  const auto worker = [&](size_t thread_index) {
    const size_t begin = queries.count * thread_index / kThreadCount;
    const size_t end = queries.count * (thread_index + 1) / kThreadCount;
    FastRng rng(ThreadSeed(queries.seed, thread_index));

    start_barrier.arrive_and_wait();
    WorkerStats &stats = worker_stats[thread_index];
    for (size_t i = begin; i < end; ++i) {
      ExecuteQuery(lock, data,
                   MakeStreamQuery(rng, queries, thread_index, i), body,
                   stats);
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

std::chrono::milliseconds DynamicRebuildInterval() {
  static const auto interval = std::chrono::milliseconds(ReadEnvSize(
      "DYNAMIC_LOCK_REBUILD_INTERVAL_MS",
      static_cast<size_t>(kDefaultDynamicRebuildInterval.count())));
  return interval;
}

double DynamicRebuildThreshold() {
  static const double threshold = ReadEnvDouble(
      "DYNAMIC_LOCK_REBUILD_THRESHOLD", kDefaultDynamicRebuildThreshold);
  return threshold;
}

double DynamicRebuildMinGain() {
  static const double min_gain = ReadEnvDouble(
      "DYNAMIC_LOCK_REBUILD_MIN_GAIN", kDefaultDynamicRebuildMinGain);
  return min_gain;
}

double DynamicRebuildMinSkew() {
  static const double min_skew = ReadEnvDouble(
      "DYNAMIC_LOCK_REBUILD_MIN_SKEW", kDefaultDynamicRebuildMinSkew);
  return min_skew;
}

size_t GeneticTrainingBatchSize() {
  static const size_t batch_size = ReadEnvSize(
      "DYNAMIC_LOCK_GENETIC_TRAINING_BATCH",
      kDefaultGeneticTrainingBatchSize);
  return batch_size;
}

size_t GeneticTrainingSampleRate() {
  static const size_t sample_rate = ReadEnvSize(
      "DYNAMIC_LOCK_GENETIC_TRAINING_SAMPLE_RATE",
      kDefaultGeneticTrainingSampleRate);
  return sample_rate;
}

size_t GeneticTrainingProbeGap() {
  static const size_t probe_gap = ReadEnvSize(
      "DYNAMIC_LOCK_GENETIC_PROBE_GAP", kDefaultGeneticTrainingProbeGap);
  return probe_gap;
}

double GeneticMinTrainingSkew() {
  static const double skew = ReadEnvDouble(
      "DYNAMIC_LOCK_GENETIC_MIN_SKEW", kDefaultGeneticMinTrainingSkew);
  return skew;
}

size_t DynamicStatsSampleRate() {
  static const size_t sample_rate =
      ReadEnvSize("DYNAMIC_LOCK_STATS_SAMPLE_RATE", 64);
  return sample_rate;
}

bool RunGeneticEnabled() {
  const char *value = std::getenv("DYNAMIC_LOCK_RUN_GENETIC");
  return value == nullptr || std::string(value) != "0";
}

template <typename Lock>
double AverageMutexesForQueries(const Lock &lock,
                                const QueryStreamSpec &queries) {
  if (queries.count == 0) {
    return 0.0;
  }

  constexpr size_t kMaxSamples = 100'000;
  const size_t samples = std::min(kMaxSamples, queries.count);
  uint64_t total_mutexes = 0;
  FastRng rng(ThreadSeed(queries.seed, 0xA5A5A5A5U));

  for (size_t i = 0; i < samples; ++i) {
    const Query query = MakeStreamQuery(rng, queries, 0, i);
    total_mutexes += lock.CountLocksForRange(query.left, query.right);
  }

  return static_cast<double>(total_mutexes) / static_cast<double>(samples);
}

struct MeasurementStats {
  double seconds = 0.0;
  uint64_t queries = 0;
  double mutex_query_sum = 0.0;
};

template <typename Lock>
MeasurementStats RunWorkloadPhase(Lock &lock, std::vector<int> &data,
                                  const QueryStreamSpec &queries,
                                  QueryBody body) {
  const PhaseRunStats measured = RunPhase(lock, data, queries, body);

  MeasurementStats stats;
  stats.seconds = measured.seconds;
  stats.queries = measured.queries;
  stats.mutex_query_sum = AverageMutexesForQueries(lock, queries) *
                          static_cast<double>(measured.queries);
  return stats;
}

struct WorkloadRunStats {
  double total_seconds = 0.0;
  double setup_seconds = 0.0;
  double measured_seconds = 0.0;
  uint64_t total_queries = 0;
  uint64_t measured_queries = 0;
  double lock_total_ms = 0.0;
  double mutex_query_sum = 0.0;
};

template <typename Lock> std::vector<size_t> GetPartitionCuts(const Lock &lock) {
  if constexpr (requires { lock.PartitionCuts(); }) {
    return lock.PartitionCuts();
  }
  return {};
}

template <typename Lock>
WorkloadRunStats RunWorkload(Lock &lock, std::vector<int> &data,
                             const ScenarioInput &scenario, QueryBody body) {
  WorkloadRunStats total;
  lock.ResetRuntimeStats();

  const auto start = std::chrono::steady_clock::now();
  lock.StartRebuilder(DynamicRebuildInterval(), DynamicRebuildThreshold());

  const auto run_phase = [&](const QueryStreamSpec &queries, bool measured) {
    const MeasurementStats stats = RunWorkloadPhase(lock, data, queries, body);
    total.total_queries += stats.queries;
    total.mutex_query_sum += stats.mutex_query_sum;
    if (measured) {
      total.measured_seconds += stats.seconds;
      total.measured_queries += stats.queries;
    } else {
      total.setup_seconds += stats.seconds;
    }
  };

  for (const PhaseInput &phase : scenario.setup_phases) {
    run_phase(phase.queries, false);
  }

  for (const TimedPhaseInput &phase : scenario.timed_phases) {
    if (phase.adapt_seconds <= 0.0) {
      run_phase(phase.adapt_queries, false);
    } else {
      const auto adapt_start = std::chrono::steady_clock::now();
      const auto deadline =
          adapt_start + std::chrono::duration<double>(phase.adapt_seconds);
      do {
        run_phase(phase.adapt_queries, false);
      } while (std::chrono::steady_clock::now() < deadline);
    }
    run_phase(phase.measure_queries, true);
  }

  if (scenario.measured_phase.queries.count != 0) {
    run_phase(scenario.measured_phase.queries, true);
  }

  lock.StopRebuilder();

  total.total_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  total.lock_total_ms = lock.GetTotalLockTimeMs();
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

    const WorkloadRunStats measured =
        RunWorkload(lock, data, scenario, QueryBody::kRealWork);

    result.total_seconds = measured.total_seconds;
    result.setup_seconds = measured.setup_seconds;
    result.measured_seconds = measured.measured_seconds;
    result.total_queries = measured.total_queries;
    result.avg_lock_us = measured.total_queries == 0
                             ? 0.0
                             : measured.lock_total_ms * 1000.0 /
                                   static_cast<double>(measured.total_queries);
    result.measured_lock_total_seconds = measured.lock_total_ms / 1000.0;
    result.avg_mutexes_per_query =
        measured.total_queries == 0
            ? 0.0
            : measured.mutex_query_sum /
                  static_cast<double>(measured.total_queries);
    result.reconfigurations = lock.GetRebuildCount();
    result.left_hot_locks = lock.CountLocksForRange(0, kHotWindowSize - 1);
    result.right_hot_locks =
        lock.CountLocksForRange(kArraySize - kHotWindowSize, kArraySize - 1);
    result.partition_cuts = GetPartitionCuts(lock);
    for (const size_t window_begin : scenario.debug_window_begins) {
      result.debug_window_locks.push_back(lock.CountLocksForRange(
          window_begin, window_begin + kClusteredHotWindowSize - 1));
    }
  }
  {
    auto lock = factory();
    std::vector<int> data = MakeInitialData();

    const WorkloadRunStats measured =
        RunWorkload(lock, data, scenario, QueryBody::kLockOnly);

    result.lock_only_total_seconds = measured.total_seconds;
    result.lock_only_queries = measured.total_queries;
    result.lock_only_avg_lock_us =
        measured.total_queries == 0 ? 0.0
                              : measured.lock_total_ms * 1000.0 /
                                    static_cast<double>(measured.total_queries);
  }

  return result;
}

bool DebugPartitionsEnabled();
void PrintPartitionDebug(const ScenarioInput &scenario,
                         const ImplementationResult &result);

void PrintScenario(const ScenarioInput &scenario,
                   const std::vector<ImplementationResult> &results) {
  const ImplementationResult &baseline = results.front();
  uint64_t timed_adapt_queries = 0;
  uint64_t measured_queries = scenario.measured_phase.queries.count;
  double timed_adapt_seconds = 0.0;
  for (const TimedPhaseInput &phase : scenario.timed_phases) {
    timed_adapt_queries += phase.adapt_queries.count;
    measured_queries += phase.measure_queries.count;
    timed_adapt_seconds += phase.adapt_seconds;
  }

  std::println("scenario: {}", scenario.name);
  std::println("  {}", scenario.description);
  std::print("  setup queries: ");
  for (size_t i = 0; i < scenario.setup_phases.size(); ++i) {
    if (i != 0) {
      std::print(" + ");
    }
    std::print("{}", scenario.setup_phases[i].queries.count);
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
    std::println("  timed scenario: adapt and measure phases run continuously; "
                 "online rebuild/training is included in total_s\n");
  }

  std::println("  impl       total_s total_x  setup_s measured_s  lock_only_s"
               "  lock_x  avg_lock_us  lock_sum_s  avg_mutex  hot(L/R)"
               "  rebuild/train");

  for (const ImplementationResult &result : results) {
    const double total_speedup = result.total_seconds > 0.0
                                     ? baseline.total_seconds /
                                           result.total_seconds
                                     : 0.0;
    const double lock_speedup =
        result.lock_only_total_seconds > 0.0
            ? baseline.lock_only_total_seconds / result.lock_only_total_seconds
            : 0.0;

    std::println("  {:<10}{:>8.3f}{:>8.3f}{:>8.3f}{:>11.3f}{:>13.3f}"
                 "{:>8.3f}{:>13.2f}{:>12.3f}{:>11.2f}{:>5}/{:>2}{:>10}",
                 result.name, result.total_seconds, total_speedup,
                 result.setup_seconds, result.measured_seconds,
                 result.lock_only_total_seconds, lock_speedup,
                 result.avg_lock_us, result.measured_lock_total_seconds,
                 result.avg_mutexes_per_query, result.left_hot_locks,
                 result.right_hot_locks, result.reconfigurations);
  }
  if (DebugPartitionsEnabled()) {
    std::println("");
    for (const ImplementationResult &result : results) {
      PrintPartitionDebug(scenario, result);
    }
  }
  std::println("");
}

template <typename Lock = std::mutex>
std::vector<ImplementationResult>
RunScenarioResults(const ScenarioInput &scenario) {
  uint64_t genetic_seed = 500;

  auto naive_factory = [] { return NaiveLock<kLockCount, Lock>(kArraySize); };
  auto dynamic_factory = [] {
    return DynamicLock<kLockCount, Lock>(kArraySize,
                                         DynamicStatsSampleRate(),
                                         DynamicRebuildMinGain(),
                                         DynamicRebuildMinSkew());
  };
  auto genetic_factory = [&genetic_seed] {
    return GeneticLock<kLockCount, kBlocks, Lock>(
        kArraySize, genetic_seed++, GeneticTrainingBatchSize(),
        kGeneticHistoryLimit, kGeneticPopulationSize, kGeneticEliteCount,
        kGeneticGenerationCount, GeneticTrainingSampleRate(),
        GeneticMinTrainingSkew(), GeneticTrainingProbeGap());
  };

  std::vector<ImplementationResult> results;
  results.push_back(RunImplementation("naive", scenario, naive_factory));
  results.push_back(RunImplementation("dynamic", scenario, dynamic_factory));
  if (RunGeneticEnabled()) {
    results.push_back(RunImplementation("genetic", scenario, genetic_factory));
  }

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

bool HasResult(const std::vector<ImplementationResult> &results,
               const std::string &name) {
  return std::any_of(results.begin(), results.end(),
                     [&](const ImplementationResult &result) {
                       return result.name == name;
                     });
}

double SafeSpeedup(double baseline_seconds, double measured_seconds) {
  return measured_seconds > 0.0 ? baseline_seconds / measured_seconds : 0.0;
}

bool DebugPartitionsEnabled() {
  const char *value = std::getenv("DYNAMIC_LOCK_DEBUG_PARTITIONS");
  return value != nullptr && std::string(value) != "0";
}

void PrintSizeVector(const std::vector<size_t> &values) {
  std::print("[");
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      std::print(", ");
    }
    std::print("{}", values[i]);
  }
  std::print("]");
}

std::vector<size_t> CutsToWidths(const std::vector<size_t> &cuts) {
  std::vector<size_t> widths;
  if (cuts.size() < 2) {
    return widths;
  }
  widths.reserve(cuts.size() - 1);
  for (size_t i = 1; i < cuts.size(); ++i) {
    widths.push_back(cuts[i] - cuts[i - 1]);
  }
  return widths;
}

void PrintPartitionDebug(const ScenarioInput &scenario,
                         const ImplementationResult &result) {
  if (result.partition_cuts.empty() && result.debug_window_locks.empty()) {
    return;
  }

  std::println("  {} partition debug:", result.name);
  if (!result.partition_cuts.empty()) {
    std::print("    cuts: ");
    PrintSizeVector(result.partition_cuts);
    std::println("");
    std::print("    widths: ");
    PrintSizeVector(CutsToWidths(result.partition_cuts));
    std::println("");
  }
  if (!scenario.debug_window_begins.empty()) {
    std::print("    hot window begins: ");
    PrintSizeVector(scenario.debug_window_begins);
    std::println("");
    std::print("    locks per hot window: ");
    PrintSizeVector(result.debug_window_locks);
    std::println("");
  }
}

void PrintPointSpinComparison(
    const ScenarioInput &scenario,
    const std::vector<ImplementationResult> &mutex_results,
    const std::vector<ImplementationResult> &spin_results) {
  const ImplementationResult &spin_naive = FindResult(spin_results, "naive");

  std::println("point lock primitive comparison: {}", scenario.name);
  std::print("  spin naive baseline: total_s={:.3f}, lock_only_s={:.3f}\n\n",
             spin_naive.total_seconds, spin_naive.lock_only_total_seconds);
  std::println("  impl       spin_total_s  spin_x  spin_lock_only_s  spin_lock_x"
               "  mutex_total_s  mutex_lock_only_s  spin_vs_mutex_total"
               "  spin_vs_mutex_lock");

  for (const std::string &name : {"dynamic", "genetic"}) {
    if (!HasResult(spin_results, name) || !HasResult(mutex_results, name)) {
      continue;
    }
    const ImplementationResult &spin_result = FindResult(spin_results, name);
    const ImplementationResult &mutex_result = FindResult(mutex_results, name);

    std::println(
        "  {:<10}{:>12.3f}{:>8.3f}{:>18.3f}{:>13.3f}{:>14.3f}{:>19.3f}"
        "{:>20.3f}{:>20.3f}",
        name, spin_result.total_seconds,
        SafeSpeedup(spin_naive.total_seconds, spin_result.total_seconds),
        spin_result.lock_only_total_seconds,
        SafeSpeedup(spin_naive.lock_only_total_seconds,
                    spin_result.lock_only_total_seconds),
        mutex_result.total_seconds, mutex_result.lock_only_total_seconds,
        SafeSpeedup(mutex_result.total_seconds, spin_result.total_seconds),
        SafeSpeedup(mutex_result.lock_only_total_seconds,
                    spin_result.lock_only_total_seconds));
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
  std::println("  rebuild interval, ms: {}", DynamicRebuildInterval().count());
  std::println("  rebuild threshold: {:.3f}", DynamicRebuildThreshold());
  std::println("  rebuild min gain: {:.3f}", DynamicRebuildMinGain());
  std::println("  rebuild min skew: {:.3f}", DynamicRebuildMinSkew());
  std::println("  dynamic stats sample rate: {}", DynamicStatsSampleRate());
  std::println("  genetic training batch: {}", GeneticTrainingBatchSize());
  std::println("  genetic training sample rate: {}",
               GeneticTrainingSampleRate());
  std::println("  genetic training probe gap: {}",
               GeneticTrainingProbeGap());
  std::println("  genetic min training skew: {:.3f}",
               GeneticMinTrainingSkew());
  std::println(
      "  total_s: full scenario wall time, including online rebuild/training");
  std::println("  measured_s: final measured phases inside the same run");
  std::println(
      "  lock_only_s: same full scenario with empty critical-section body");
  std::print(
      "  total_x and lock_x are speedups over naive in the same scenario\n\n");

  const ScenarioInput shift_point = MakeShiftScenario();
  const ScenarioInput clustered_2_windows = MakeClusteredWindowsScenario(2, 51);
  const ScenarioInput clustered_4_windows = MakeClusteredWindowsScenario(4, 61);
  const ScenarioInput clustered_4_thread_groups =
      MakeClusteredThreadGroupsScenario(4, 65);
  const ScenarioInput clustered_4_thread_groups_contended =
      MakeClusteredThreadGroupsContendedScenario(4, 66);
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
  run_if_selected(clustered_4_thread_groups);
  run_if_selected(clustered_4_thread_groups_contended);
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
