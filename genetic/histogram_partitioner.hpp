#pragma once

#include "dynamic/partitioner.hpp"

#include <random>

namespace dynamic_lock_detail {

// A bounded genetic search over contiguous, nonempty partitions. The fitness
// is the same squared-load objective as DP. Prefix sums make each evaluation
// O(K), independent of the number of sampled requests.
inline std::vector<size_t>
GeneticPartitions(std::span<const uint64_t> weights, size_t lock_count,
                  std::span<const size_t> current = {}, uint64_t seed = 123) {
  if (lock_count == 0) {
    throw std::invalid_argument("lock count must be positive");
  }
  const size_t bins = weights.size();
  if (bins == 0) {
    return {0};
  }
  lock_count = std::min(lock_count, bins);
  std::vector<uint64_t> prefix(bins + 1);
  for (size_t bin = 0; bin < bins; ++bin) {
    prefix[bin + 1] = AddLoad(prefix[bin], weights[bin]);
  }
  std::vector<size_t> uniform(lock_count + 1);
  for (size_t group = 0; group <= lock_count; ++group) {
    uniform[group] = group * bins / lock_count;
  }
  if (prefix.back() == 0 || lock_count == 1 || lock_count == bins) {
    return uniform;
  }
  if (!current.empty()) {
    // Validate before indexing prefix with externally supplied boundaries.
    PartitionLoads(weights, current);
    if (current.size() != lock_count + 1) {
      throw std::invalid_argument("current partition has wrong lock count");
    }
  }

  struct Candidate {
    std::vector<size_t> cuts;
    long double cost;
  };
  const auto evaluate = [&](std::vector<size_t> cuts) {
    long double cost = 0;
    for (size_t group = 1; group < cuts.size(); ++group) {
      const long double load = prefix[cuts[group]] - prefix[cuts[group - 1]];
      cost += load * load;
    }
    return Candidate{std::move(cuts), cost};
  };
  const auto repair = [&](std::vector<size_t> &cuts) {
    std::sort(cuts.begin() + 1, cuts.end() - 1);
    for (size_t group = 1; group < lock_count; ++group) {
      cuts[group] = std::clamp(cuts[group], cuts[group - 1] + 1,
                               bins - (lock_count - group));
    }
  };

  // Cheap quantiles seed the search; no DP or minimax solver is hidden here.
  auto guided = uniform;
  for (size_t group = 1; group < lock_count; ++group) {
    const long double target = static_cast<long double>(prefix.back()) * group / lock_count;
    const size_t upper = bins - (lock_count - group);
    size_t cut = guided[group - 1] + 1;
    while (cut < upper && prefix[cut] < target) {
      ++cut;
    }
    guided[group] = cut;
  }

  constexpr size_t population_size = 24, elite_count = 4;
  constexpr size_t generations = 18, patience = 4;
  std::mt19937_64 random(seed);
  const auto mutate = [&](std::vector<size_t> &cuts) {
    const size_t mutations = 1 + random() % 4;
    for (size_t index = 0; index < mutations; ++index) {
      const size_t group = 1 + random() % (lock_count - 1);
      const size_t first = cuts[group - 1] + 1;
      const size_t count = cuts[group + 1] - first;
      cuts[group] = first + random() % count;
    }
  };
  std::vector<Candidate> population;
  population.reserve(population_size);
  population.push_back(evaluate(uniform));
  population.push_back(evaluate(guided));
  if (!current.empty()) {
    population.push_back(evaluate({current.begin(), current.end()}));
  }
  while (population.size() < population_size) {
    auto cuts = population.size() % 2 ? guided : uniform;
    mutate(cuts);
    population.push_back(evaluate(std::move(cuts)));
  }
  const auto rank = [](const Candidate &left, const Candidate &right) {
    return left.cost < right.cost || (left.cost == right.cost && left.cuts < right.cuts);
  };
  std::sort(population.begin(), population.end(), rank);
  size_t stale = 0;
  for (size_t generation = 0; generation < generations && stale < patience; ++generation) {
    std::vector<Candidate> next(population.begin(), population.begin() + elite_count);
    next.reserve(population_size);
    while (next.size() < population_size) {
      const auto &left = population[random() % elite_count].cuts;
      const auto &right = population[random() % elite_count].cuts;
      auto child = left;
      for (size_t group = 1; group < lock_count; ++group) {
        if (random() & 1) child[group] = right[group];
      }
      repair(child);
      mutate(child);
      next.push_back(evaluate(std::move(child)));
    }
    std::sort(next.begin(), next.end(), rank);
    stale = next.front().cost < population.front().cost ? 0 : stale + 1;
    population.swap(next);
  }
  return population.front().cuts;
}

} // namespace dynamic_lock_detail
