#include "dynamic/partitioner.hpp"

#include <iostream>
#include <cmath>
#include <random>
#include <stdexcept>

namespace {
using dynamic_lock_detail::BalancePartitions;
using dynamic_lock_detail::DynamicProgrammingPartitions;
using dynamic_lock_detail::MaxPartitionLoad;
using dynamic_lock_detail::PartitionLoadCost;

void Check(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Exception, typename Function> void CheckThrows(Function function) {
  try {
    function();
  } catch (const Exception &) {
    return;
  }
  throw std::runtime_error("malformed partitioner input was accepted");
}

void CheckAll(const std::vector<uint64_t> &weights) {
  const size_t bins = weights.size();
  std::vector<uint64_t> best(bins + 1, std::numeric_limits<uint64_t>::max());
  std::vector<uint64_t> best_squared(bins + 1,
                                     std::numeric_limits<uint64_t>::max());
  // Independent complete enumeration of every contiguous nonempty partition.
  for (size_t mask = 0; mask < (size_t{1} << (bins - 1)); ++mask) {
    size_t groups = 1;
    uint64_t load = weights[0], maximum = 0, squared = 0;
    for (size_t bin = 1; bin < bins; ++bin) {
      if (mask & (size_t{1} << (bin - 1))) {
        maximum = std::max(maximum, load);
        squared += load * load;
        load = 0;
        ++groups;
      }
      load += weights[bin];
    }
    best[groups] = std::min(best[groups], std::max(maximum, load));
    best_squared[groups] = std::min(best_squared[groups], squared + load * load);
  }
  for (size_t groups = 1; groups <= bins; ++groups) {
    const auto cuts = BalancePartitions(weights, groups);
    Check(cuts.size() == groups + 1, "wrong number of groups");
    Check(cuts.front() == 0 && cuts.back() == bins, "partition misses array ends");
    for (size_t cut = 1; cut < cuts.size(); ++cut) {
      Check(cuts[cut - 1] < cuts[cut], "partition contains an empty group");
    }
    Check(MaxPartitionLoad(weights, cuts) == best[groups],
          "balanced partition differs from brute-force optimum");
    const auto dp_cuts = DynamicProgrammingPartitions(weights, groups);
    Check(dp_cuts.size() == groups + 1, "DP uses wrong number of groups");
    Check(PartitionLoadCost(weights, dp_cuts) == best_squared[groups],
          "DP differs from brute-force squared-load optimum");

    auto scaled = weights;
    for (auto &weight : scaled) {
      weight *= 1'000'003;
    }
    Check(BalancePartitions(scaled, groups) == cuts,
          "sample scaling changes the partition");
    const auto scaled_dp = DynamicProgrammingPartitions(scaled, groups);
    const long double scaled_cost = PartitionLoadCost(scaled, scaled_dp) /
                                    (1'000'003.0L * 1'000'003.0L);
    Check(std::abs(scaled_cost - best_squared[groups]) < 1e-8L,
          "sample scaling changes the optimal DP cost");
  }
}

} // namespace

int main() {
  try {
    CheckAll({0, 0, 0, 0, 0, 0, 0, 0});
    CheckAll({10, 10, 0, 0, 0, 0, 10, 10});
    CheckAll({0, 0, 0, 100, 0, 0, 0, 0});
    CheckAll({1, 2, 3, 4, 5, 6, 7, 8});
    // A single indivisible heavy bin fixes the minimax optimum. The remaining
    // locks must still separate lighter traffic instead of covering cold zeros.
    std::vector<uint64_t> spike(4096);
    std::fill_n(spike.begin(), 64, 7);
    spike[32] += 192;
    const auto spike_cuts = BalancePartitions(spike, 64);
    const auto spike_loads = dynamic_lock_detail::PartitionLoads(spike, spike_cuts);
    Check(spike_loads.size() == 64 &&
              std::all_of(spike_loads.begin(), spike_loads.end(),
                          [](uint64_t load) { return load > 0; }),
          "heavy bin sends spare balanced locks to cold zeros");
    long double individual_cost = 0;
    for (const long double weight : spike) individual_cost += weight * weight;
    Check(PartitionLoadCost(spike, spike_cuts) == individual_cost,
          "balanced reconstruction fails to isolate independent positive bins");
    Check(BalancePartitions(std::vector<uint64_t>(8), 4) ==
              std::vector<size_t>({0, 2, 4, 6, 8}),
          "zero traffic should produce uniform groups");
    Check(DynamicProgrammingPartitions(std::vector<uint64_t>(8), 4) ==
              std::vector<size_t>({0, 2, 4, 6, 8}),
          "zero traffic should produce uniform DP groups");
    Check(BalancePartitions(std::vector<uint64_t>{4, 2}, 64) ==
              std::vector<size_t>({0, 1, 2}),
          "tiny arrays should use one group per bin");
    Check(BalancePartitions({}, 4) == std::vector<size_t>{0},
          "empty histogram should produce no groups");
    Check(DynamicProgrammingPartitions({}, 4) == std::vector<size_t>{0},
          "empty histogram should produce no DP groups");
    Check(MaxPartitionLoad({}, std::vector<size_t>{0}) == 0,
          "empty histogram has nonzero load");
    Check(PartitionLoadCost(std::vector<uint64_t>{2, 3, 5},
                            std::vector<size_t>{0, 2, 3}) == 50,
          "original squared-load objective changed");
    const uint64_t huge = std::numeric_limits<uint64_t>::max();
    Check(BalancePartitions(std::vector<uint64_t>{huge, 0}, 2) ==
              std::vector<size_t>({0, 1, 2}),
          "large weights overflow capacity search");
    CheckThrows<std::overflow_error>([&] {
      BalancePartitions(std::vector<uint64_t>{huge, 1}, 2);
    });
    Check(DynamicProgrammingPartitions(std::vector<uint64_t>{huge, 0}, 2) ==
              std::vector<size_t>({0, 1, 2}),
          "large weights overflow squared-load DP");
    CheckThrows<std::overflow_error>([&] {
      DynamicProgrammingPartitions(std::vector<uint64_t>{huge, 1}, 2);
    });
    CheckThrows<std::invalid_argument>([] {
      BalancePartitions(std::vector<uint64_t>{1}, 0);
    });
    CheckThrows<std::invalid_argument>([] {
      MaxPartitionLoad(std::vector<uint64_t>(8), std::vector<size_t>{0, 9, 8});
    });
    std::mt19937_64 random(1234567);
    for (size_t trial = 0; trial < 2000; ++trial) {
      std::vector<uint64_t> weights(1 + random() % 10);
      for (auto &weight : weights) {
        weight = random() % 20;
      }
      CheckAll(weights);
    }
    std::cout << "partitioner: exhaustive balance and original DP checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
