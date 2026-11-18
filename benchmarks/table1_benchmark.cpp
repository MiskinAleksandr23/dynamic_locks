// Reuse the actual request generator, lock classes and CSV format.
#define main CountedBenchmarkMain
#include "adaptive_lock_benchmark.cpp"
#undef main
#include <set>

#ifdef __linux__
#include <sched.h>
#endif

namespace {
void PinWorker(size_t worker, const std::vector<size_t> &cpus) {
  if (cpus.empty()) return;
#ifdef __linux__
  if (cpus[worker] >= CPU_SETSIZE) throw std::runtime_error("CPU ID exceeds CPU_SETSIZE");
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(cpus[worker], &mask);
  if (sched_setaffinity(0, sizeof(mask), &mask) != 0)
    throw std::runtime_error("worker CPU affinity failed");
#else
  throw std::runtime_error("individual CPU pinning requires Linux");
#endif
}

template <class Factory>
Result RunTimed(Input &input, const Scenario &scenario, Options &options,
                double seconds, const std::vector<size_t> &cpus, Factory factory) {
  std::vector<uint64_t> data(options.array_size);
  std::vector<std::vector<size_t>> completed(options.threads, std::vector<size_t>(scenario.phases));
  std::vector<uint64_t> checksums(options.threads);
  std::vector<std::exception_ptr> errors(options.threads);
  std::atomic<bool> cancelled{false};
  Result result;
  result.phase_seconds.resize(scenario.phases);
  const auto cpu_start = std::clock();
  const auto start = Clock::now();
  {
    auto lock = factory();
    if constexpr (requires { lock.StartRebuilder(std::chrono::milliseconds(1)); })
      lock.StartRebuilder(std::chrono::milliseconds(options.rebuild_ms));
    Clock::time_point phase_start, deadline;
    size_t boundary = 0;
    const auto phase_duration = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(seconds / scenario.phases));
    auto advance = [&]() noexcept {
      const auto now = Clock::now();
      if (boundary == 0) result.work_start_s = std::chrono::duration<double>(now - start).count();
      else result.phase_seconds[boundary - 1] = std::chrono::duration<double>(now - phase_start).count();
      phase_start = now;
      deadline = now + phase_duration;
      ++boundary;
    };
    std::barrier gate(static_cast<std::ptrdiff_t>(options.threads), advance);
    std::vector<std::thread> workers;
    workers.reserve(options.threads);
    try {
      for (size_t t = 0; t < options.threads; ++t) workers.emplace_back([&, t] {
        try { PinWorker(t, cpus); }
        catch (...) { errors[t] = std::current_exception(); cancelled.store(true); }
        gate.arrive_and_wait();
        for (size_t phase = 0; phase < scenario.phases; ++phase) {
          const auto &trace = input.traces[t][phase].queries;
          size_t cursor = 0;
          try {
            // Finish requests atomically. Every policy checks the clock after
            // the same 64-request batch. Overshoot is included in wall time.
            while (!cancelled.load(std::memory_order_relaxed) && Clock::now() < deadline) {
              for (size_t batch = 0; batch < 64; ++batch) {
                const auto &q = trace[cursor];
                if (q.delta == 0)
                  lock.ReadQuery(q.left, q.right, [&](size_t lo, size_t hi) {
                    for (size_t i = lo; i <= hi; ++i) checksums[t] += data[i];
                  });
                else
                  lock.WriteQuery(q.left, q.right, [&](size_t lo, size_t hi) {
                    if (options.write_body == "range")
                      for (size_t i = lo; i <= hi; ++i) ++data[i];
                    else {
                      for (size_t i = lo; i <= hi; ++i) checksums[t] += data[i];
                      data[q.target] += q.delta;
                    }
                  });
                ++completed[t][phase];
                if (++cursor == trace.size()) cursor = 0;
              }
            }
          } catch (...) { errors[t] = std::current_exception(); cancelled.store(true); }
          gate.arrive_and_wait();
        }
      });
    } catch (...) {
      cancelled.store(true);
      for (size_t missing = workers.size(); missing < options.threads; ++missing) gate.arrive_and_drop();
      for (auto &worker : workers) worker.join();
      throw;
    }
    for (auto &worker : workers) worker.join();
    if constexpr (requires { lock.StopRebuilder(); }) lock.StopRebuilder();
    if constexpr (requires { lock.RethrowBackgroundError(); }) lock.RethrowBackgroundError();
    for (auto error : errors) if (error) std::rethrow_exception(error);
    result.rebuilds = lock.GetRebuildCount();
    if constexpr (requires { lock.GetPublicationTimes(); }) {
      result.optimizations = lock.GetOptimizationCount();
      result.rebuild_ms = lock.GetRebuildTimeMs();
      result.optimize_ms = lock.GetOptimizeTimeMs();
      result.publication_wait_ms = lock.GetPublicationWaitTimeMs();
      result.publication_hold_ms = lock.GetPublicationHoldTimeMs();
      result.max_publication_pause_ms = lock.GetMaxPublicationPauseMs();
      result.last_window_samples = lock.GetLastWindowSampleCount();
      result.actual_bins = lock.GetBinCount();
      result.cuts = lock.PartitionCuts();
      if (result.rebuilds) {
        const auto [first, last] = lock.GetPublicationTimes();
        result.first_publication_s = std::chrono::duration<double>(first - start).count();
        result.last_publication_s = std::chrono::duration<double>(last - start).count();
      }
    }
  }
  result.wall_seconds = std::chrono::duration<double>(Clock::now() - start).count();
  const auto cpu_end = std::clock();
  if (cpu_start == std::clock_t(-1) || cpu_end == std::clock_t(-1))
    throw std::runtime_error("process CPU clock unavailable");
  result.cpu_seconds = double(cpu_end - cpu_start) / CLOCKS_PER_SEC;
  result.read_checksum = std::accumulate(checksums.begin(), checksums.end(), uint64_t{0});

  // Independently replay the executed prefixes, accounting for complete cycles.
  std::fill(input.expected.begin(), input.expected.end(), 0);
  options.operations = input.read_operations = input.point_operations = 0;
  result.phase_operations.resize(scenario.phases);
  for (size_t t = 0; t < options.threads; ++t) {
    for (size_t phase = 0; phase < scenario.phases; ++phase) {
      const auto &trace = input.traces[t][phase].queries;
      const size_t count = completed[t][phase];
      options.operations += count;
      result.phase_operations[phase] += count;
      for (size_t i = 0; i < trace.size(); ++i) {
        const auto &q = trace[i];
        const uint64_t n = count / trace.size() + (i < count % trace.size());
        if (q.delta == 0) input.read_operations += n;
        else if (options.write_body == "range") {
          input.expected[q.left] += n;
          if (size_t(q.right) + 1 < data.size()) input.expected[size_t(q.right) + 1] -= n;
        } else input.expected[q.target] += n * q.delta;
        if (q.left == q.right) input.point_operations += n;
      }
    }
  }
  if (options.write_body == "range")
    std::partial_sum(input.expected.begin(), input.expected.end(), input.expected.begin());
  if (data != input.expected) throw std::runtime_error("timed benchmark final array mismatch");
  return result;
}
} // namespace

int main(int argc, char **argv) {
  try {
    double seconds = 20;
    std::vector<size_t> cpus;
    std::vector<char *> counted{argv[0]};
    for (int i = 1; i < argc; ++i) {
      const std::string flag = argv[i];
      if (flag == "--seconds") {
        if (++i == argc) throw std::invalid_argument("missing --seconds value");
        size_t used;
        seconds = std::stod(argv[i], &used);
        if (used != std::string(argv[i]).size() || !std::isfinite(seconds) || seconds <= 0)
          throw std::invalid_argument("seconds must be finite and positive");
      } else if (flag == "--cpu-list") {
        if (++i == argc) throw std::invalid_argument("missing --cpu-list value");
        std::string list = argv[i];
        size_t offset = 0;
        do {
          const auto end = list.find(',', offset);
          const auto id = list.substr(offset, end - offset);
          cpus.push_back(id == "0" ? 0 : ParseSize(id));
          if (end == std::string::npos) break;
          offset = end + 1;
        } while (true);
      } else counted.push_back(argv[i]);
    }
    auto options = ParseOptions(counted.size(), counted.data());
    if (options.model != "paper" || options.scenario == "all" || options.implementations.size() != 1 ||
        options.repetitions != 1 || options.csv.empty())
      throw std::invalid_argument("use one scenario, one implementation, paper model, one repetition and --csv");
    if (!cpus.empty() && (cpus.size() != options.threads ||
        std::set<size_t>(cpus.begin(), cpus.end()).size() != cpus.size()))
      throw std::invalid_argument("cpu-list needs one distinct logical CPU per worker");
    const auto &scenario = *std::find_if(kScenarios.begin(), kScenarios.end(),
        [&](const auto &s) { return s.name == options.scenario; });
    options.operations = options.trace_size * scenario.phases;
    Random random{options.seed + scenario.id * 0x9e3779b97f4a7c15ULL};
    const auto seed = random.Next();
    auto input = MakeInput(scenario, options, seed);
    const auto &name = options.implementations.front();
    std::ofstream csv(options.csv);
    if (!csv) throw std::runtime_error("cannot open result CSV");
    WriteHeader(csv);
    csv.flush();
    std::cout << "START " << name << " " << scenario.name << " duration_s=" << seconds << std::endl;
    Result result;
    if (name == "fixed")
      result = RunTimed(input, scenario, options, seconds, cpus, [&] { return NaiveLock<64>(options.array_size); });
    else if (name == "global")
      result = RunTimed(input, scenario, options, seconds, cpus, [&] { return NaiveLock<1>(options.array_size); });
    else if (name == "per_element")
      result = RunTimed(input, scenario, options, seconds, cpus, [&] { return ElementLock<>(options.array_size); });
    else if (name == "balanced" || name == "dynamic_dp") {
      PaperLockConfig config{options.bins, options.sample_rate, options.lambda,
                             options.minimum_samples, options.minimum_improvement};
      result = RunTimed(input, scenario, options, seconds, cpus, [&] {
        return PaperRangeLock<64>(options.array_size, config,
            name == "balanced" ? PaperStrategy::Balanced : PaperStrategy::DynamicProgramming);
      });
    } else throw std::invalid_argument("unsupported table implementation");
    WriteRow(csv, scenario, options, 0, seed, input.unique_queries,
             input.read_operations, input.point_operations, name, result);
    std::cout << "DONE " << name << " requests=" << options.operations << " wall_s=" << result.wall_seconds
              << " rebuilds=" << result.rebuilds << " verified=true" << std::endl;
  } catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
