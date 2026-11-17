// Exercise the actual generator without introducing a second implementation.
#define main BenchmarkProgramMain
#include "../benchmarks/adaptive_lock_benchmark.cpp"
#undef main

namespace {
void Check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

const Scenario &FindScenario(const std::string &name) {
  return *std::find_if(kScenarios.begin(), kScenarios.end(),
                       [&](const auto &scenario) { return scenario.name == name; });
}

Options ParseForTest(std::vector<std::string> arguments) {
  arguments.insert(arguments.begin(), "benchmark");
  std::vector<char *> argv;
  for (auto &argument : arguments) argv.push_back(argument.data());
  return ParseOptions(static_cast<int>(argv.size()), argv.data());
}

void CheckGeometry(const std::string &name, size_t phase, const Options &options) {
  Random random{123456};
  const auto &scenario = FindScenario(name);
  std::array<size_t, 4> counts{};
  size_t central = 0;
  constexpr size_t samples = 5000;
  const size_t h = options.hot_width;
  for (size_t i = 0; i < samples; ++i) {
    const auto query = MakeQuery(random, scenario, options, phase);
    Check(query.left <= query.right && query.right < options.array_size,
          "query bounds outside array");
    Check(query.right - query.left + 1 == options.range_length, "range length changed");
    Check(query.left <= query.target && query.target <= query.right, "target outside range");
    Check(query.delta >= 1 && query.delta <= 7, "invalid update delta");
    if (name == "gradient_range") {
      Check(query.right < h, "gradient query outside hot window");
      const size_t starts = h - options.range_length + 1;
      central += query.left >= starts / 4 && query.left < 3 * starts / 4;
      continue;
    }
    bool found = false;
    for (size_t cluster = 0; cluster < scenario.clusters; ++cluster) {
      size_t begin = 0, width = 0;
      if (name == "islands_range") {
        // Independent expression of the four disjoint physical intervals.
        const std::array<size_t, 4> starts = {0, h / 8, h / 2, 5 * h / 8};
        begin = starts[cluster];
        width = h / 16;
      } else if (name == "unequal_widths_range") {
        const std::array<size_t, 4> widths = {h / 8, h / 4, h / 2, h};
        begin = cluster * (options.array_size - h) / 3;
        width = widths[cluster];
      } else {
        begin = cluster == 0 ? 0 : options.array_size - h;
        width = h;
      }
      if (query.left >= begin && query.right < begin + width) {
        ++counts[cluster];
        found = true;
        break;
      }
    }
    Check(found, "query crosses a window or falls in a cold gap");
  }
  if (name == "gradient_range") {
    // A triangular density puts 75% in the central half; uniform puts 50%.
    Check(central > 3500 && central < 4000, "expected broad triangular density");
    return;
  }
  std::array<size_t, 4> expected{};
  if (name == "islands_range") expected = {1250, 1250, 1250, 1250};
  else if (name == "unequal_widths_range") expected = {2000, 1500, 1000, 500};
  else expected = phase == 0 ? std::array<size_t, 4>{4500, 500, 0, 0}
                            : std::array<size_t, 4>{500, 4500, 0, 0};
  for (size_t cluster = 0; cluster < scenario.clusters; ++cluster)
    Check(counts[cluster] + 150 > expected[cluster] && counts[cluster] < expected[cluster] + 150,
          "unexpected traffic shares");
}

void CheckMovingVariable(Options options) {
  const auto &scenario = FindScenario("moving_variable_range");
  Check(scenario.phases == 4, "moving workload needs two subphases per residence");
  for (const size_t maximum : {options.range_length, size_t{8}}) {
    options.range_length = maximum;
    for (size_t phase = 0; phase < scenario.phases; ++phase) {
      Random random{123456 + phase};
      const size_t begin = phase < 2 ? 0 : options.array_size - options.hot_width;
      size_t min_length = maximum, max_length = 0, sum_length = 0;
      for (size_t i = 0; i < 5000; ++i) {
        const auto query = MakeQuery(random, scenario, options, phase);
        const size_t length = query.right - query.left + 1;
        Check(length >= 1 && length <= maximum, "variable length outside configured limits");
        Check(query.left >= begin && query.right < begin + options.hot_width,
              "moving variable query outside its current window");
        Check(query.target >= query.left && query.target <= query.right, "variable target outside query");
        min_length = std::min(min_length, length);
        max_length = std::max(max_length, length);
        sum_length += length;
      }
      Check(sum_length > 5000 * (maximum + 1) * 45 / 100 &&
            sum_length < 5000 * (maximum + 1) * 55 / 100,
            "variable lengths should be uniform, not fixed or shortened");
      if (maximum == 8)
        Check(min_length == 1 && max_length == maximum, "variable length endpoints must be reachable");
    }
  }
}

void CheckSlidingVariable(const Options &options) {
  const auto &scenario = FindScenario("sliding_variable_range");
  Check(scenario.phases == 9, "eight shifts need nine residences");
  // The measured geometry: four fixed stripes per window, shifting one stripe.
  const size_t stripe = options.array_size / 64;
  const std::array<size_t, 9> starts = {
      0, 1'048'576, 2'097'152, 3'145'728, 4'194'304,
      5'242'880, 6'291'456, 7'340'032, 8'388'608};
  for (size_t phase = 0; phase < starts.size(); ++phase) {
    Random random{123456 + phase};
    size_t sum_length = 0, leading = 0, trailing = 0;
    for (size_t i = 0; i < 5000; ++i) {
      const auto query = MakeQuery(random, scenario, options, phase);
      const size_t length = query.right - query.left + 1;
      Check(length >= 1 && length <= options.range_length, "sliding length outside limits");
      Check(query.left >= starts[phase] && query.right < starts[phase] + 4 * stripe,
            "sliding query outside the expected physical window");
      Check(query.target >= query.left && query.target <= query.right, "sliding target outside query");
      sum_length += length;
      leading += query.left < starts[phase] + stripe;
      trailing += query.left >= starts[phase] + 3 * stripe;
    }
    Check(leading > 1000 && trailing > 1000, "both edge stripes must receive traffic");
    Check(sum_length > 5000 * (options.range_length + 1) * 45 / 100 &&
          sum_length < 5000 * (options.range_length + 1) * 55 / 100,
          "sliding lengths should be uniform");
  }
  for (const auto &arguments : std::vector<std::vector<std::string>>{
           {"--threads", "3", "--operations", "26"},
           {"--array-size", "4096", "--hot-width", "1368"},
           {"--hot-width", "3", "--range-length", "1"}}) {
    auto args = arguments;
    args.insert(args.end(), {"--scenario", "sliding_variable_range"});
    bool rejected = false;
    try { ParseForTest(args); }
    catch (const std::invalid_argument &) { rejected = true; }
    Check(rejected, "invalid sliding geometry or empty phase must be rejected");
  }
  ParseForTest({"--scenario", "sliding_variable_range", "--threads", "3", "--operations", "27"});
  ParseForTest({"--scenario", "sliding_variable_range", "--array-size", "4096", "--hot-width", "1367"});
}

void CheckRangeUpdateOracle() {
  auto options = ParseForTest({"--scenario", "steady_range", "--array-size", "4096",
      "--hot-width", "256", "--threads", "3", "--operations", "8191", "--trace-size", "257",
      "--range-length", "17", "--min-samples", "64", "--rebuild-ms", "1"});
  Check(options.model == "paper" && options.write_body == "range", "paper/range must be the default");
  for (const std::string body : {"range", "single"}) {
    options.write_body = body;
    for (size_t point_percent : {size_t{0}, size_t{50}, size_t{100}}) {
      options.point_percent = point_percent;
      for (size_t read_percent : {size_t{0}, size_t{90}, size_t{100}}) {
        options.read_percent = read_percent;
        const auto &scenario = FindScenario("steady_range");
        const auto input = MakeInput(scenario, options, 456);
        std::vector<uint64_t> expected(options.array_size);
        size_t reads = 0, points = 0;
        for (const auto &worker : input.traces) {
          for (const auto &phase : worker) {
            for (size_t i = 0; i < phase.operations; ++i) {
              const auto &query = phase.queries[i % phase.queries.size()];
              points += query.left == query.right;
              if (query.delta == 0) { ++reads; continue; }
              if (body == "range") {
                Check(query.delta == 1, "paper writes increment by exactly one");
                for (size_t element = query.left; element <= query.right; ++element) ++expected[element];
              } else {
                expected[query.target] += query.delta;
              }
            }
          }
        }
        Check(expected == input.expected && reads == input.read_operations && points == input.point_operations,
              "mixed cyclic oracle or actual operation counts mismatch");
        if (point_percent == 0) Check(points == 0, "pure range trace contains points");
        if (point_percent == 100) Check(points == options.operations, "pure point trace contains ranges");
        if (point_percent == 50) Check(points > 3300 && points < 4900, "point-range mixture is not balanced");
        PaperLockConfig config;
        config.bins = options.bins;
        config.sample_rate = 1;
        config.lambda = 0;
        config.minimum_samples = options.minimum_samples;
        Run(input, scenario, options, [&] {
          return PaperRangeLock<64, std::shared_mutex>(options.array_size, config, PaperStrategy::Balanced);
        });
      }
    }
  }
  // Full-array requests exercise the difference-array edge without a sentinel allocation.
  options.write_body = "range";
  options.point_percent = 0;
  options.read_percent = 0;
  options.operations = 117;
  options.trace_size = 17;
  options.range_length = options.array_size;
  const Scenario full{"full_array", 999, 0, 0, 3};
  const auto input = MakeInput(full, options, 999);
  Check(input.expected == std::vector<uint64_t>(options.array_size, 117), "full-array oracle boundary incorrect");
  Run(input, full, options, [&] { return NaiveLock<64>(options.array_size); });

  ParseForTest({"--lambda", "0", "--min-improvement", "0", "--point-percent", "100"});
  ParseForTest({"--model", "legacy", "--write-body", "single", "--implementations", "genetic"});
  for (const auto &arguments : std::vector<std::vector<std::string>>{
      {"--model", "unknown"}, {"--write-body", "unknown"}, {"--bins", "63"},
      {"--bins", "5000", "--array-size", "4096"}, {"--lambda", "-1"},
      {"--min-improvement", "nan"}, {"--min-improvement", "inf"}, {"--min-improvement", "1"},
      {"--min-improvement", "-0.1"}, {"--point-percent", "101"}, {"--read-percent", "101"},
      {"--implementations", "fixed,genetic"}}) {
    bool rejected = false;
    try { ParseForTest(arguments); }
    catch (const std::invalid_argument &) { rejected = true; }
    Check(rejected, "invalid paper settings must be rejected");
  }
}

} // namespace

int main() {
  try {
    CheckRangeUpdateOracle();
    // Independently replay cyclic mixed traces and verify that reads never update
    // the reference. Changing the read ratio must preserve physical query geometry.
    auto mixed_options = ParseForTest({"--scenario", "steady_range", "--array-size", "4096",
        "--hot-width", "256", "--threads", "3", "--operations", "8191", "--trace-size", "257",
        "--model", "legacy", "--write-body", "single"});
    const auto &mixed_scenario = FindScenario("steady_range");
    const auto writes = MakeInput(mixed_scenario, mixed_options, 123);
    for (size_t percent : {size_t{0}, size_t{90}, size_t{100}}) {
      mixed_options.read_percent = percent;
      const auto input = MakeInput(mixed_scenario, mixed_options, 123);
      std::vector<uint64_t> expected(4096);
      size_t reads = 0;
      for (size_t worker = 0; worker < 3; ++worker) {
        for (size_t phase = 0; phase < 4; ++phase) {
          const auto &trace = input.traces[worker][phase];
          for (size_t q = 0; q < trace.queries.size(); ++q) {
            const auto &a = trace.queries[q], &b = writes.traces[worker][phase].queries[q];
            Check(a.left == b.left && a.right == b.right && a.target == b.target, "read ratio changed geometry");
          }
          for (size_t q = 0; q < trace.operations; ++q) {
            const auto &query = trace.queries[q % trace.queries.size()];
            if (query.delta == 0) ++reads;
            else expected[query.target] += query.delta;
          }
        }
      }
      Check(expected == input.expected && reads == input.read_operations, "mixed reference or read count incorrect");
      if (percent == 0) Check(reads == 0, "write-only case contains reads");
      if (percent == 100) Check(reads == 8191, "read-only case contains writes");
      if (percent == 90) Check(reads > 6800 && reads < 8000, "unexpected read share");
      Run(input, mixed_scenario, mixed_options, [&] { return DynamicLock<64, std::shared_mutex>(4096, 1); });
    }
    Options options;
    options.array_size = 1 << 26;
    options.hot_width = 1 << 22;
    options.range_length = 10000;
    CheckMovingVariable(options);
    CheckSlidingVariable(options);
    for (const std::string name : {"islands_range", "gradient_range", "unequal_widths_range", "exchange_range"}) {
      const auto &scenario = FindScenario(name);
      Check(scenario.phases == (name == "exchange_range" ? 2 : 4), "unexpected phase count");
      for (size_t phase = 0; phase < scenario.phases; ++phase)
        CheckGeometry(name, phase, options);
    }
    for (const std::string name : {"islands_range", "unequal_widths_range"}) {
      bool rejected = false;
      try {
        ParseForTest({"--scenario", name, "--hot-width", "64", "--range-length", "9"});
      } catch (const std::invalid_argument &) { rejected = true; }
      Check(rejected, "oversized ranges must be rejected instead of shortened");
    }
    ParseForTest({"--scenario", "steady_range", "--hot-width", "64", "--range-length", "9"});
    ParseForTest({"--scenario", "islands_range", "--array-size", "4096", "--hot-width", "64", "--range-length", "4"});
    for (const std::string name : {"unequal_widths_range", "exchange_range"}) {
      bool rejected = false;
      try {
        ParseForTest({"--scenario", name, "--array-size", "4096", "--hot-width", "4096"});
      } catch (const std::invalid_argument &) { rejected = true; }
      Check(rejected, "overlapping scenario windows must be rejected");
    }
    std::cout << "New workload geometry, traffic shares, phase counts and range validation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
