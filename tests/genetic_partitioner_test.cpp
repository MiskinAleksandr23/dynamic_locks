#include "genetic/histogram_partitioner.hpp"

#include <iostream>
#include <numeric>

using namespace dynamic_lock_detail;

void Check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  try {
    Check(GeneticPartitions({}, 4) == std::vector<size_t>{0}, "empty histogram");
    Check(GeneticPartitions(std::vector<uint64_t>(8), 4) ==
              std::vector<size_t>({0, 2, 4, 6, 8}), "zero histogram");
    Check(GeneticPartitions(std::vector<uint64_t>{4, 2}, 64) ==
              std::vector<size_t>({0, 1, 2}), "more locks than bins");
    bool invalid = false, overflow = false;
    try { GeneticPartitions(std::vector<uint64_t>{1}, 0); }
    catch (const std::invalid_argument &) { invalid = true; }
    try { GeneticPartitions(std::vector<uint64_t>{UINT64_MAX, 1}, 2); }
    catch (const std::overflow_error &) { overflow = true; }
    Check(invalid && overflow, "invalid inputs must be rejected");
    Check(GeneticPartitions(std::vector<uint64_t>{UINT64_MAX, 0}, 2) ==
              std::vector<size_t>({0, 1, 2}), "large weights");

    std::mt19937_64 random(1729);
    size_t evolutionary_improvements = 0;
    for (size_t trial = 0; trial < 300; ++trial) {
      const size_t bins = 2 + random() % 63, groups = 1 + random() % bins;
      std::vector<uint64_t> weights(bins);
      for (auto &weight : weights) weight = random() % 100;
      std::vector<size_t> uniform(groups + 1);
      for (size_t group = 0; group <= groups; ++group) uniform[group] = group * bins / groups;
      const auto cuts = GeneticPartitions(weights, groups, uniform, trial);
      const auto cost = PartitionLoadCost(weights, cuts); // Also validates coverage.
      Check(cuts.size() == groups + 1, "wrong number of genetic groups");
      Check(cost <= PartitionLoadCost(weights, uniform), "search discarded a better incumbent");
      Check(cost >= PartitionLoadCost(weights, DynamicProgrammingPartitions(weights, groups)),
            "genetic cost below exact DP optimum");
      Check(cuts == GeneticPartitions(weights, groups, uniform, trial), "seed is not reproducible");
      Check(PartitionLoadCost(weights, GeneticPartitions(weights, groups, cuts, trial + 1)) <= cost,
            "search made the current partition worse");

      // Verify that crossover/mutation can improve beyond both initial seeds.
      std::vector<uint64_t> prefix(bins + 1);
      std::partial_sum(weights.begin(), weights.end(), prefix.begin() + 1);
      auto quantiles = uniform;
      for (size_t group = 1; group < groups; ++group) {
        size_t cut = quantiles[group - 1] + 1;
        while (cut < bins - (groups - group) &&
               prefix[cut] < static_cast<long double>(prefix.back()) * group / groups) ++cut;
        quantiles[group] = cut;
      }
      evolutionary_improvements += cost < std::min(PartitionLoadCost(weights, uniform),
                                                    PartitionLoadCost(weights, quantiles));
    }
    Check(evolutionary_improvements > 0, "genetic search never improves its seeds");
    std::cout << "genetic: coverage, incumbent, DP lower bound, reproducibility passed; "
              << evolutionary_improvements << " evolutionary improvements\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
