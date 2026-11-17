#include "common/naive_lock.hpp"
#include "common/element_lock.hpp"
#include "common/spinlock.hpp"
#include <shared_mutex>
#include "dynamic/dynamic_lock.hpp"
#include "dynamic/paper_lock.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr size_t kLocks = 64;
using Clock = std::chrono::steady_clock;

struct Options {
  size_t threads = std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
  size_t operations = 4'000'000;
  size_t trace_size = 1 << 20;
  size_t repetitions = 3;
  size_t array_size = 1 << 20;
  size_t hot_width = 0;
  size_t range_length = 8;
  size_t sample_rate = 64;
  size_t rebuild_ms = 30'000;
  uint64_t seed = 1;
  std::string scenario = "all";
  std::string csv;
  bool partitions = false;
  size_t read_percent = 0;
  size_t point_percent = 0;
  std::string model = "paper";
  std::string write_body = "range";
  size_t bins = 1024;
  uint64_t lambda = 10'000'000;
  size_t minimum_samples = 640;
  double minimum_improvement = 0.10;
  std::string lock_kind = "mutex";
  std::vector<std::string> implementations{"fixed", "balanced", "dynamic_dp"};
};

struct Scenario {
  std::string name;
  size_t id; // Stable seed/order identity; retained when other scenarios are removed.
  size_t clusters = 0; // Zero means uniform.
  size_t max_length = 1; // 0: configured exact length; UINT32_MAX: variable length.
  size_t phases = 1;
};

const std::vector<Scenario> kScenarios = {
    {"uniform", 0},
    {"clusters_2", 3, 2},
    {"clusters_8", 5, 8},
    {"unequal", 6, 4},
    {"clusters_2_range", 11, 2, 0},
    {"clusters_4_range", 12, 4, 0},
    {"shift", 22, 1, 1, 2},
    {"steady_range", 25, 1, 0, 4},
    {"hot_with_spike", 26, 1, 1, 4},
    {"islands_range", 27, 4, 0, 4},
    {"gradient_range", 28, 1, 0, 4},
    {"unequal_widths_range", 29, 4, 0, 4},
    {"exchange_range", 30, 2, 0, 2},
    {"moving_variable_range", 31, 1, UINT32_MAX, 4},
    {"sliding_variable_range", 32, 1, UINT32_MAX, 9},
};

size_t ParseSize(const std::string &value) {
  if (value.empty() || value.front() == '-') {
    throw std::invalid_argument("expected a positive integer: " + value);
  }
  size_t used = 0;
  const auto parsed = std::stoull(value, &used);
  if (used != value.size() || parsed == 0 ||
      parsed > std::numeric_limits<size_t>::max()) {
    throw std::invalid_argument("expected a positive integer: " + value);
  }
  return static_cast<size_t>(parsed);
}

size_t ParsePercent(const std::string &value) {
  const size_t percent = value == "0" ? 0 : ParseSize(value);
  if (percent > 100) throw std::invalid_argument("percentage must be in [0,100]");
  return percent;
}

Options ParseOptions(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help") {
      std::cout << "Usage: adaptive_lock_benchmark [options]\n"
                   "  --threads N       workers (default min(hardware, 8))\n"
                   "  --operations N    total operations per scenario (default 4000000)\n"
                   "  --trace-size N    unique query budget per phase, all workers (default 1048576)\n"
                   "  --repetitions N   matched runs, rotating order (default 3)\n"
                   "  --array-size N    elements (default 1048576, minimum 4096)\n"
                   "  --hot-width N     elements per hot window (default array-size / 64)\n"
                   "  --range-length N  exact *_range length; variable range maximum (default 8)\n"
                   "  --seed N          deterministic trace seed (default 1)\n"
                   "  --scenario NAME   all (default) or one name below\n"
                   "  --model NAME      paper (default) or legacy\n"
                   "  --write-body NAME range: increment every element by 1 (default); single: legacy body\n"
                   "  --bins N          paper elementary blocks (default 1024)\n"
                   "  --lambda N        paper boundary-crossing penalty, integer >=0 (default 10000000)\n"
                   "  --min-samples N   paper minimum sampled queries (default 640)\n"
                   "  --min-improvement F paper required fractional objective decrease [0,1) (default 0.10)\n"
                   "  --point-percent N replace this percentage of scenario queries with points (default 0)\n"
                   "  --sample-rate N   adaptive sampling period (default 64)\n"
                   "  --rebuild-ms N    adaptive rebuild interval, including genetic (default 30000)\n"
                   "  --csv PATH        write every raw run\n"
                   "  --lock-kind NAME  mutex (default), spinlock, rwlock (std::shared_mutex)\n"
                   "  --read-percent N  read-only queries, 0..100 (default 0)\n"
                   "  --partitions      print final dynamic partition cuts\n"
                   "  --implementations LIST  comma-separated fixed,balanced,dynamic_dp,genetic,global,per_element\n"
                   "Scenarios:";
      for (const auto &scenario : kScenarios) std::cout << ' ' << scenario.name;
      std::cout << "\nTraces repeat cyclically when operations exceed their stored query count.\n";
      std::exit(0);
    }
    if (arg == "--partitions") {
      options.partitions = true;
      continue;
    }
    if (++i == argc) throw std::invalid_argument("missing value for " + arg);
    const std::string value = argv[i];
    if (arg == "--scenario") options.scenario = value;
    else if (arg == "--implementations") {
      options.implementations.clear();
      size_t start = 0;
      while (true) {
        const size_t end = value.find(',', start);
        const auto name = value.substr(start, end - start);
        if ((name != "fixed" && name != "balanced" && name != "dynamic_dp" &&
             name != "genetic" && name != "global" && name != "per_element") ||
            std::find(options.implementations.begin(), options.implementations.end(), name) !=
                options.implementations.end())
          throw std::invalid_argument("unknown or duplicate implementation: " + name);
        options.implementations.push_back(name);
        if (end == std::string::npos) break;
        start = end + 1;
      }
    }
    else if (arg == "--lock-kind") {
      if (value != "mutex" && value != "spinlock" && value != "rwlock")
        throw std::invalid_argument("unknown lock kind: " + value);
      options.lock_kind = value;
    }
    else if (arg == "--read-percent") options.read_percent = ParsePercent(value);
    else if (arg == "--point-percent") options.point_percent = ParsePercent(value);
    else if (arg == "--model") {
      if (value != "paper" && value != "legacy") throw std::invalid_argument("unknown model: " + value);
      options.model = value;
    }
    else if (arg == "--write-body") {
      if (value != "range" && value != "single") throw std::invalid_argument("unknown write body: " + value);
      options.write_body = value;
    }
    else if (arg == "--bins") options.bins = ParseSize(value);
    else if (arg == "--lambda") options.lambda = value == "0" ? 0 : ParseSize(value);
    else if (arg == "--min-samples") options.minimum_samples = ParseSize(value);
    else if (arg == "--min-improvement") {
      size_t used = 0;
      options.minimum_improvement = std::stod(value, &used);
      if (used != value.size() || !std::isfinite(options.minimum_improvement) ||
          options.minimum_improvement < 0 || options.minimum_improvement >= 1)
        throw std::invalid_argument("minimum improvement must be finite and in [0,1)");
    }
    else if (arg == "--csv") options.csv = value;
    else if (arg == "--threads") options.threads = ParseSize(value);
    else if (arg == "--operations") options.operations = ParseSize(value);
    else if (arg == "--trace-size") options.trace_size = ParseSize(value);
    else if (arg == "--repetitions") options.repetitions = ParseSize(value);
    else if (arg == "--array-size") options.array_size = ParseSize(value);
    else if (arg == "--hot-width") options.hot_width = ParseSize(value);
    else if (arg == "--range-length") options.range_length = ParseSize(value);
    else if (arg == "--seed") options.seed = ParseSize(value);
    else if (arg == "--sample-rate") options.sample_rate = ParseSize(value);
    else if (arg == "--rebuild-ms") options.rebuild_ms = ParseSize(value);
    else throw std::invalid_argument("unknown option " + arg);
  }
  if (options.array_size < 4096 || options.array_size > UINT32_MAX)
    throw std::invalid_argument("array size must be in [4096, UINT32_MAX]");
  if (options.model == "paper") {
    if (options.bins < kLocks || options.bins > options.array_size)
      throw std::invalid_argument("paper bins must be in [64,array_size]");
    if (std::find(options.implementations.begin(), options.implementations.end(), "genetic") !=
        options.implementations.end())
      throw std::invalid_argument("genetic is a legacy policy; use --model legacy explicitly");
  }
  if (options.hot_width == 0) options.hot_width = options.array_size / kLocks;
  if (options.hot_width > options.array_size || options.range_length > options.array_size)
    throw std::invalid_argument("hot width and range length must not exceed array size");
  if (options.trace_size < options.threads)
    throw std::invalid_argument("trace size must be at least the worker count");
  if (options.threads > 1024 || options.operations / options.threads < 4)
    throw std::invalid_argument("threads must be <= 1024 and operations >= threads * 4");
  if (options.scenario != "all" &&
      std::none_of(kScenarios.begin(), kScenarios.end(), [&](const auto &s) {
        return s.name == options.scenario;
      })) throw std::invalid_argument("unknown scenario " + options.scenario);
  for (const auto &scenario : kScenarios) {
    if (options.scenario != "all" && options.scenario != scenario.name) continue;
    if (options.operations / options.threads < scenario.phases)
      throw std::invalid_argument("each worker needs at least one operation per phase for " + scenario.name);
    if (scenario.clusters && scenario.max_length != 1 &&
        options.range_length > options.hot_width)
      throw std::invalid_argument("range length must fit inside hot width for " + scenario.name);
    if (scenario.name == "islands_range" && options.range_length > options.hot_width / 16)
      throw std::invalid_argument("range length must fit inside hot width / 16 for islands_range");
    if (scenario.name == "unequal_widths_range" &&
        (options.range_length > options.hot_width / 8 || options.hot_width > options.array_size / 4))
      throw std::invalid_argument("unequal_widths_range requires range length <= hot width / 8 and hot width <= array size / 4");
    if (scenario.name == "exchange_range" && options.hot_width > options.array_size / 2)
      throw std::invalid_argument("exchange_range requires hot width <= array size / 2");
    if (scenario.name == "sliding_variable_range" &&
        (options.hot_width < 4 || options.hot_width / 4 >
            (options.array_size - options.hot_width) / (scenario.phases - 1)))
      throw std::invalid_argument("sliding_variable_range requires width >= 4 and room for eight width/4 shifts");
  }
  return options;
}

// SplitMix64: independent per-worker streams; no synchronized walks or
// round-robin cluster selection. Traces are generated once for each matched set of runs.
struct Random {
  uint64_t state;
  uint64_t Next() {
    uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  size_t Below(size_t bound) { return Next() % bound; }
};

struct Query {
  uint32_t left, right, target, delta;
};
struct PhaseTrace {
  size_t operations = 0;
  std::vector<Query> queries;
};
struct Input {
  std::vector<std::vector<PhaseTrace>> traces; // Worker, then phase.
  std::vector<uint64_t> expected;
  size_t unique_queries = 0;
  size_t read_operations = 0;
  size_t point_operations = 0;
};

size_t Share(size_t total, size_t index, size_t parts) {
  return total / parts + (index < total % parts);
}

Query MakeQuery(Random &random, const Scenario &scenario, const Options &options,
                size_t phase) {
  const size_t size = options.array_size;
  size_t begin = 0;
  size_t width = size;
  if (scenario.clusters != 0) {
    width = options.hot_width;
    size_t cluster = random.Below(scenario.clusters);
    if (scenario.name == "unequal") {
      // 70%, 10%, 10%, 10%, selected independently for every operation.
      const size_t choice = random.Below(10);
      cluster = choice < 7 ? 0 : choice - 6;
    }
    if (scenario.name == "islands_range") {
      // Two nearby pairs, with cold gaps between all four equally busy islands.
      // For hot_width = array_size / 16, each pair shares one fixed stripe.
      constexpr std::array<size_t, 4> offsets = {0, 2, 8, 10};
      width = options.hot_width / 16;
      begin = offsets[cluster] * width;
    } else if (scenario.name == "unequal_widths_range") {
      // Distant tenants have different working-set sizes and request rates.
      const size_t choice = random.Below(10);
      cluster = choice < 4 ? 0 : choice < 7 ? 1 : choice < 9 ? 2 : 3;
      begin = cluster * (size - options.hot_width) / 3;
      width = options.hot_width / (size_t{8} >> cluster);
    } else if (scenario.name == "exchange_range") {
      // The windows stay put; their traffic shares swap once, between phases.
      const bool left = random.Below(10) < (phase == 0 ? 9 : 1);
      begin = left ? 0 : size - width;
    } else if (scenario.name == "shift" && phase == 1) {
      begin = size - width;
    } else if (scenario.name == "moving_variable_range") {
      begin = phase < 2 ? 0 : size - width;
    } else if (scenario.name == "sliding_variable_range") {
      // Nine residences, eight shifts; adjacent windows overlap by 75%
      // when width is divisible by four. No forced rebuild at the barriers.
      begin = phase * (width / 4);
    } else if (scenario.clusters > 1) {
      begin = cluster * (size - width) / (scenario.clusters - 1);
    }
  }
  if (scenario.name == "hot_with_spike" && random.Below(10) < 3) {
    // Mix a widely accessed key with independently uniform traffic in its
    // surrounding hot window. The distribution stays fixed across phases.
    begin += width / 2;
    width = 1;
  }
  const size_t length = scenario.max_length == 0 ? options.range_length
      : scenario.max_length == UINT32_MAX ? 1 + random.Below(options.range_length) : 1;
  const size_t positions = width - length + 1;
  size_t offset = random.Below(positions);
  if (scenario.name == "gradient_range") {
    // A broad triangular density: average two independent uniform starts.
    offset = (uint64_t{offset} + random.Below(positions)) / 2;
  }
  const size_t left = begin + offset;
  return {static_cast<uint32_t>(left), static_cast<uint32_t>(left + length - 1),
          static_cast<uint32_t>(left + random.Below(length)),
          static_cast<uint32_t>(1 + random.Below(7))};
}

Input MakeInput(const Scenario &scenario, const Options &options, uint64_t seed) {
  Input input;
  input.expected.resize(options.array_size, 0);
  input.traces.resize(options.threads);
  const bool range_updates = options.write_body == "range";
  Scenario point_scenario = scenario;
  point_scenario.max_length = 1;
  for (size_t thread = 0; thread < options.threads; ++thread) {
    auto &phases = input.traces[thread];
    phases.resize(scenario.phases);
    const size_t count = Share(options.operations, thread, options.threads);
    const size_t budget = Share(options.trace_size, thread, options.threads);
    Random random{seed ^ (0xd1b54a32d192ed03ULL * (thread + 1))};
    Random access_random{seed ^ (0x94d049bb133111ebULL * (thread + 1))};
    Random mixture_random{seed ^ (0xbf58476d1ce4e5b9ULL * (thread + 1))};
    for (size_t phase = 0; phase < scenario.phases; ++phase) {
      auto &trace = phases[phase];
      trace.operations = Share(count, phase, scenario.phases);
      const size_t unique = std::min(trace.operations, budget);
      trace.queries.reserve(unique);
      input.unique_queries += unique;
      const size_t cycles = trace.operations / unique;
      const size_t remainder = trace.operations % unique;
      for (size_t i = 0; i < unique; ++i) {
        const bool point = options.point_percent != 0 && mixture_random.Below(100) < options.point_percent;
        auto query = MakeQuery(random, point ? point_scenario : scenario, options, phase);
        if (range_updates) query.delta = 1;
        if (options.read_percent != 0 && access_random.Below(100) < options.read_percent)
          query.delta = 0; // Read-only; independent RNG preserves query geometry.
        trace.queries.push_back(query);
        // Addition is commutative. Account for all cycles without executing
        // billions of reference operations or storing billions of queries.
        const uint64_t multiplicity = cycles + (i < remainder);
        if (query.delta == 0) {
          input.read_operations += multiplicity;
        } else if (range_updates) {
          // Difference array modulo 2^64: subtraction wraps intentionally.
          // After the prefix sum each count is <= the total operation count.
          input.expected[query.left] += multiplicity;
          if (size_t{query.right} + 1 < options.array_size)
            input.expected[size_t{query.right} + 1] -= multiplicity;
        } else {
          input.expected[query.target] += multiplicity * query.delta;
        }
        if (query.left == query.right) input.point_operations += multiplicity;
      }
    }
  }
  if (range_updates) {
    uint64_t count = 0;
    for (auto &value : input.expected) {
      count += value;
      value = count;
    }
  }
  return input;
}

struct Result {
  double wall_seconds = 0, cpu_seconds = 0, rebuild_ms = 0;
  size_t rebuilds = 0, optimizations = 0;
  double optimize_ms = 0, publication_wait_ms = 0, publication_hold_ms = 0, max_publication_pause_ms = 0;
  uint64_t sampled_queries = 0;
  size_t actual_bins = 0;
  uint64_t read_checksum = 0;
  std::vector<size_t> cuts;
  std::vector<double> phase_seconds;
};

// std::clock is process CPU time: workers and the background rebuilder count.
// Input generation, zeroing the common data array and reference verification
// are outside the interval. The entire lock lifetime is inside it.
template <typename Factory>
Result Run(const Input &input, const Scenario &scenario, const Options &options,
           Factory factory) {
  std::vector<uint64_t> data(options.array_size, 0);
  const bool range_updates = options.write_body == "range";
  Result result;
  const auto cpu_start = std::clock();
  const auto start = Clock::now();
  {
    auto lock = factory();
    if constexpr (requires { lock.StartRebuilder(std::chrono::milliseconds(1)); })
      lock.StartRebuilder(std::chrono::milliseconds(options.rebuild_ms));
    std::vector<uint64_t> checksums(options.threads, 0);
    result.phase_seconds.resize(scenario.phases);
    auto phase_start = Clock::now();
    size_t boundary = 0;
    auto complete = [&]() noexcept {
      const auto now = Clock::now();
      if (boundary != 0)
        result.phase_seconds[boundary - 1] =
            std::chrono::duration<double>(now - phase_start).count();
      phase_start = now;
      ++boundary;
    };
    std::barrier gate(static_cast<std::ptrdiff_t>(options.threads), complete);
    std::vector<std::thread> workers;
    workers.reserve(options.threads);
    std::atomic<bool> cancelled{false};
    try {
      for (size_t thread = 0; thread < options.threads; ++thread) {
        workers.emplace_back([&, thread] {
          const auto &phases = input.traces[thread];
          uint64_t checksum = 0;
          gate.arrive_and_wait();
          if (cancelled.load(std::memory_order_acquire)) return;
          for (size_t phase = 0; phase < scenario.phases; ++phase) {
            const auto &trace = phases[phase];
            size_t cursor = 0;
            for (size_t i = 0; i < trace.operations; ++i) {
              const auto &query = trace.queries[cursor];
              if (query.delta == 0) {
                lock.ReadQuery(query.left, query.right, [&](size_t left, size_t right) {
                  for (size_t index = left; index <= right; ++index) checksum += data[index];
                });
              } else {
                lock.WriteQuery(query.left, query.right, [&](size_t left, size_t right) {
                  if (range_updates) {
                    for (size_t index = left; index <= right; ++index) ++data[index];
                  } else {
                    for (size_t index = left; index <= right; ++index) checksum += data[index];
                    data[query.target] += query.delta;
                  }
                });
              }
              if (++cursor == trace.queries.size()) cursor = 0;
            }
            gate.arrive_and_wait();
          }
          checksums[thread] = checksum;
        });
      }
    } catch (...) {
      // Workers already created are waiting at the initial barrier. Release
      // them without executing the workload if a later creation fails.
      cancelled.store(true, std::memory_order_release);
      for (size_t missing = workers.size(); missing < options.threads; ++missing)
        gate.arrive_and_drop();
      for (auto &worker : workers) worker.join();
      throw;
    }
    for (auto &worker : workers) worker.join();
    if constexpr (requires { lock.StopRebuilder(); }) lock.StopRebuilder();
    if constexpr (requires { lock.RethrowBackgroundError(); }) lock.RethrowBackgroundError();
    result.rebuilds = lock.GetRebuildCount();
    if constexpr (requires { lock.GetOptimizationCount(); })
      result.optimizations = lock.GetOptimizationCount();
    if constexpr (requires { lock.GetRebuildTimeMs(); })
      result.rebuild_ms = lock.GetRebuildTimeMs();
    if constexpr (requires { lock.PartitionCuts(); }) result.cuts = lock.PartitionCuts();
    if constexpr (requires { lock.GetOptimizeTimeMs(); }) result.optimize_ms = lock.GetOptimizeTimeMs();
    if constexpr (requires { lock.GetPublicationWaitTimeMs(); }) result.publication_wait_ms = lock.GetPublicationWaitTimeMs();
    if constexpr (requires { lock.GetPublicationHoldTimeMs(); }) result.publication_hold_ms = lock.GetPublicationHoldTimeMs();
    if constexpr (requires { lock.GetMaxPublicationPauseMs(); }) result.max_publication_pause_ms = lock.GetMaxPublicationPauseMs();
    if constexpr (requires { lock.GetSampleCount(); }) result.sampled_queries = lock.GetSampleCount();
    if constexpr (requires { lock.GetBinCount(); }) result.actual_bins = lock.GetBinCount();
    result.read_checksum = std::accumulate(checksums.begin(), checksums.end(), uint64_t{0});
  }
  result.wall_seconds = std::chrono::duration<double>(Clock::now() - start).count();
  const auto cpu_end = std::clock();
  if (cpu_start == std::clock_t(-1) || cpu_end == std::clock_t(-1))
    throw std::runtime_error("process CPU clock unavailable");
  result.cpu_seconds = double(cpu_end - cpu_start) / CLOCKS_PER_SEC;
  const auto mismatch = std::mismatch(data.begin(), data.end(), input.expected.begin());
  if (mismatch.first != data.end()) {
    throw std::runtime_error("incorrect final array at index " +
        std::to_string(mismatch.first - data.begin()) + ": got " +
        std::to_string(*mismatch.first) + ", expected " +
        std::to_string(*mismatch.second));
  }
  return result;
}

double Median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const size_t middle = values.size() / 2;
  return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

template <typename T>
void PrintSequence(std::ostream &stream, const std::vector<T> &values) {
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) stream << ';';
    stream << values[i];
  }
}

void WriteRow(std::ostream &csv, const Scenario &scenario, const Options &options,
              size_t repetition, uint64_t seed, size_t unique_queries, size_t read_operations,
              size_t point_operations,
              const std::string &implementation,
              const Result &result) {
  csv << scenario.name << ',' << repetition + 1 << ',' << implementation << ','
      << options.threads << ',' << options.operations << ',' << options.trace_size << ','
      << unique_queries << ',' << options.array_size << ','
      << options.hot_width << ',' << options.range_length << ','
      << seed << ',' << options.sample_rate << ',' << options.rebuild_ms << ','
      << std::setprecision(9) << result.wall_seconds << ',' << result.cpu_seconds << ','
      << result.rebuilds << ',' << result.rebuild_ms << ',' << result.read_checksum
      << ",true,";
  PrintSequence(csv, result.phase_seconds);
  csv << ',';
  PrintSequence(csv, result.cuts);
  csv << ',' << result.optimizations << ',' << options.lock_kind << ',' << options.read_percent
      << ',' << read_operations << ',' << options.operations - read_operations
      << ',' << options.model << ',';
  if (options.model == "paper")
    csv << options.bins << ',' << options.lambda << ',' << options.minimum_samples << ','
        << std::setprecision(17) << options.minimum_improvement << std::setprecision(9);
  else
    csv << ",,,"; // Paper parameters do not describe the legacy implementation.
  csv << ',' << options.write_body
      << ',' << options.point_percent << ',' << point_operations << ',' << options.operations - point_operations
      << ',' << result.optimize_ms << ',' << result.publication_wait_ms << ',' << result.publication_hold_ms
      << ',' << result.max_publication_pause_ms << ',' << result.sampled_queries << ',' << result.actual_bins << '\n';
  csv.flush();
  if (!csv) throw std::runtime_error("failed to write benchmark CSV");
}
} // namespace

int main(int argc, char **argv) {
  try {
    const auto options = ParseOptions(argc, argv);
    std::ofstream csv;
    if (!options.csv.empty()) {
      csv.open(options.csv);
      if (!csv) throw std::runtime_error("cannot write " + options.csv);
      csv << "scenario,repetition,implementation,threads,operations,trace_size,unique_queries,array_size,hot_width,range_length,seed,"
             "sample_rate,rebuild_interval_ms,wall_s,cpu_s,rebuilds,rebuild_ms,"
             "read_checksum,verified,phase_wall_s,partition_cuts,optimizations,lock_kind,read_percent,read_operations,write_operations,model,bins,lambda,min_samples,min_improvement,write_body,point_percent,point_operations,range_operations,optimize_ms,publication_wait_ms,publication_hold_ms,max_publication_pause_ms,sampled_queries,actual_bins\n";
    }
    std::cout << "fixed/adaptive: 64 mutexes; global: 1; per_element: array_size mutexes.\n"
              << "sizeof(std::mutex)=" << sizeof(std::mutex) << " bytes\n"
              << options.threads << " workers, " << options.operations
              << " operations/scenario, " << options.repetitions << " repetitions\n"
              << "trace_size=" << options.trace_size << " unique query budget per phase (all workers)\n"
              << "array_size=" << options.array_size << " hot_width=" << options.hot_width
              << " range_length=" << options.range_length << " sample_rate=" << options.sample_rate
              << " lock_kind=" << options.lock_kind << " read_percent=" << options.read_percent
              << " point_percent=" << options.point_percent << " model=" << options.model
              << " write_body=" << options.write_body
              << " rebuild_interval_ms=" << options.rebuild_ms << " seed=" << options.seed << '\n'
              << "All work from lock construction through destruction is timed; no warmup.\n"
              << "Final arrays verified against a deterministic reference after every run.\n"
              << "Read checksums depend on scheduling and are not equality checks.\n"
              << "Stored independent worker traces repeat cyclically; identical for all implementations.\n";
    if (options.model == "paper")
      std::cout << "paper bins_requested=" << options.bins << " lambda=" << options.lambda
                << " min_samples=" << options.minimum_samples
                << " min_improvement=" << options.minimum_improvement << '\n';
    for (const auto &scenario : kScenarios) {
      if (options.scenario != "all" && options.scenario != scenario.name) continue;
      const auto &names = options.implementations;
      const auto reference_it = std::find(names.begin(), names.end(), "fixed");
      const size_t reference = reference_it == names.end() ? 0 : reference_it - names.begin();
      std::vector<std::vector<double>> wall_times(names.size()), speedups(names.size()), cpu_ratios(names.size());
      std::cout << "\n" << scenario.name << '\n';
      for (size_t repetition = 0; repetition < options.repetitions; ++repetition) {
        Random random{options.seed + scenario.id * 0x9e3779b97f4a7c15ULL + repetition};
        const uint64_t seed = random.Next();
        const auto input = MakeInput(scenario, options, seed);
        std::cout << "  run=" << repetition + 1 << " unique_queries=" << input.unique_queries
                  << " phases=" << scenario.phases
                  << " read_operations=" << input.read_operations
                  << " write_operations=" << options.operations - input.read_operations
                  << " point_operations=" << input.point_operations
                  << " range_operations=" << options.operations - input.point_operations << '\n';
        std::vector<Result> results(names.size());
        for (size_t order = 0; order < names.size(); ++order) {
          const size_t implementation = (order + repetition + scenario.id) % names.size();
          const auto &name = names[implementation];
          const auto execute = [&]<class Mutex>() {
            if (name == "global")
              return Run(input, scenario, options, [&] { return NaiveLock<1, Mutex>(options.array_size); });
            if (name == "per_element")
              return Run(input, scenario, options, [&] { return ElementLock<Mutex>(options.array_size); });
            if (name == "fixed")
              return Run(input, scenario, options, [&] { return NaiveLock<kLocks, Mutex>(options.array_size); });
            if (options.model == "paper") {
              PaperLockConfig config;
              config.bins = options.bins;
              config.sample_rate = options.sample_rate;
              config.lambda = options.lambda;
              config.minimum_samples = options.minimum_samples;
              config.minimum_improvement = options.minimum_improvement;
              return Run(input, scenario, options, [&] {
                return PaperRangeLock<kLocks, Mutex>(options.array_size, config,
                    name == "balanced" ? PaperStrategy::Balanced : PaperStrategy::DynamicProgramming);
              });
            }
            return Run(input, scenario, options, [&] {
              return DynamicLock<kLocks, Mutex>(options.array_size, options.sample_rate,
                  name == "genetic" ? PartitionStrategy::Genetic :
                  name == "balanced" ? PartitionStrategy::Balanced : PartitionStrategy::DynamicProgramming,
                  options.seed);
            });
          };
          Result result = options.lock_kind == "spinlock" ? execute.template operator()<spinlock>()
              : options.lock_kind == "rwlock" ? execute.template operator()<std::shared_mutex>()
              : execute.template operator()<std::mutex>();
          std::cout << "  run=" << repetition + 1 << ' ' << name << std::fixed
                    << std::setprecision(4) << " wall_s=" << result.wall_seconds
                    << " cpu_s=" << result.cpu_seconds << " rebuilds=" << result.rebuilds
                    << " optimizations=" << result.optimizations
                    << " rebuild_ms=" << result.rebuild_ms
                    << " optimize_ms=" << result.optimize_ms
                    << " publication_wait_ms=" << result.publication_wait_ms
                    << " publication_hold_ms=" << result.publication_hold_ms
                    << " max_publication_pause_ms=" << result.max_publication_pause_ms
                    << " sampled_queries=" << result.sampled_queries
                    << " read_checksum=" << result.read_checksum << " verified=true\n";
          if (scenario.phases > 1) {
            std::cout << "    phase_wall_s=";
            PrintSequence(std::cout, result.phase_seconds);
            std::cout << '\n';
          }
          if (options.partitions && !result.cuts.empty()) {
            std::cout << "    partition_cuts=";
            PrintSequence(std::cout, result.cuts);
            std::cout << '\n';
          }
          if (csv.is_open()) WriteRow(csv, scenario, options, repetition, seed, input.unique_queries, input.read_operations, input.point_operations, name, result);
          results[implementation] = std::move(result);
          std::cout.flush();
        }
        for (size_t implementation = 0; implementation < names.size(); ++implementation) {
          wall_times[implementation].push_back(results[implementation].wall_seconds);
          speedups[implementation].push_back(
              results[reference].wall_seconds / results[implementation].wall_seconds);
          cpu_ratios[implementation].push_back(
              results[implementation].cpu_seconds / results[reference].cpu_seconds);
        }
      }
      std::cout << "  median " << names[reference] << "_s=" << Median(wall_times[reference]) << '\n';
      for (size_t implementation = 0; implementation < names.size(); ++implementation) {
        if (implementation == reference) continue;
        const auto &ratios = speedups[implementation];
        std::cout << "  median " << names[implementation] << "_s="
                  << Median(wall_times[implementation])
                  << " paired_speedup=" << Median(ratios)
                  << " range=[" << *std::min_element(ratios.begin(), ratios.end())
                  << ',' << *std::max_element(ratios.begin(), ratios.end())
                  << "] cpu/" << names[reference] << '=' << Median(cpu_ratios[implementation]) << '\n';
      }
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "benchmark error: " << error.what() << '\n';
    return 1;
  }
}
