#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace dynamic_lock_detail {

inline uint64_t AddLoad(uint64_t left, uint64_t right) {
  if (right > std::numeric_limits<uint64_t>::max() - left) {
    throw std::overflow_error("partition load exceeds uint64_t");
  }
  return left + right;
}

// Cuts delimit nonempty half-open segments and include both zero and size.
inline std::vector<uint64_t> PartitionLoads(std::span<const uint64_t> weights,
                                           std::span<const size_t> cuts) {
  if (weights.empty() && cuts.size() == 1 && cuts.front() == 0) {
    return {};
  }
  if (cuts.size() < 2 || cuts.front() != 0 || cuts.back() != weights.size()) {
    throw std::invalid_argument("partition must cover the weight histogram");
  }
  std::vector<uint64_t> loads(cuts.size() - 1);
  for (size_t group = 1; group < cuts.size(); ++group) {
    if (cuts[group] <= cuts[group - 1] || cuts[group] > weights.size()) {
      throw std::invalid_argument("partition cuts must increase strictly");
    }
    for (size_t bin = cuts[group - 1]; bin < cuts[group]; ++bin) {
      loads[group - 1] = AddLoad(loads[group - 1], weights[bin]);
    }
  }
  return loads;
}

inline uint64_t MaxPartitionLoad(std::span<const uint64_t> weights,
                                std::span<const size_t> cuts) {
  const auto loads = PartitionLoads(weights, cuts);
  return loads.empty() ? 0 : *std::ranges::max_element(loads);
}

// Original objective: sum of squared summed block weights, without cut penalties.
inline long double PartitionLoadCost(std::span<const uint64_t> weights,
                                     std::span<const size_t> cuts) {
  long double cost = 0;
  for (const long double load : PartitionLoads(weights, cuts)) {
    cost += load * load;
  }
  return cost;
}

// Balance summed block weights across consecutive groups. The result minimizes
// the heaviest group's load, with exactly min(lock_count, weights.size()) groups.
// Greedily filling groups tests a capacity in O(B); binary search finds the
// minimum feasible capacity in O(B log(sum(weights))). Spare groups split the
// existing groups in O(K B), reducing the original squared-load cost while
// preserving the optimal maximum load.
inline std::vector<size_t> BalancePartitions(std::span<const uint64_t> weights,
                                            size_t lock_count) {
  if (lock_count == 0) {
    throw std::invalid_argument("lock count must be positive");
  }
  const size_t bins = weights.size();
  if (bins == 0) {
    return {0};
  }
  lock_count = std::min(lock_count, bins);
  uint64_t minimum = 0, total = 0;
  for (const uint64_t weight : weights) {
    minimum = std::max(minimum, weight);
    total = AddLoad(total, weight);
  }
  if (total == 0) {
    std::vector<size_t> cuts(lock_count + 1);
    for (size_t group = 0; group <= lock_count; ++group) {
      cuts[group] = group * bins / lock_count;
    }
    return cuts;
  }

  const auto fits = [&](uint64_t capacity) {
    size_t groups = 1;
    uint64_t load = 0;
    for (const uint64_t weight : weights) {
      if (weight > capacity - load) {
        if (++groups > lock_count) {
          return false;
        }
        load = 0;
      }
      load += weight;
    }
    return true;
  };
  uint64_t maximum = total;
  while (minimum < maximum) {
    const uint64_t middle = minimum + (maximum - minimum) / 2;
    if (fits(middle)) {
      maximum = middle;
    } else {
      minimum = middle + 1;
    }
  }

  std::vector<size_t> cuts;
  cuts.reserve(lock_count + 1);
  cuts.push_back(0);
  uint64_t load = 0;
  for (size_t bin = 0; bin < bins; ++bin) {
    if (weights[bin] > minimum - load) {
      cuts.push_back(bin);
      load = 0;
    }
    load += weights[bin];
  }
  cuts.push_back(bins);

  // A heavy indivisible bin can make the optimal capacity much larger than
  // other loads. Spend spare locks on useful splits, not on the zero suffix.
  std::vector<uint64_t> prefix(bins + 1);
  for (size_t bin = 0; bin < bins; ++bin) {
    prefix[bin + 1] = prefix[bin] + weights[bin]; // Total was checked above.
  }
  while (cuts.size() <= lock_count) {
    long double best_gain = -1;
    size_t best_group = 0, best_cut = 0, best_width = 0;
    for (size_t group = 1; group < cuts.size(); ++group) {
      const size_t first = cuts[group - 1], last = cuts[group];
      for (size_t cut = first + 1; cut < last; ++cut) {
        const long double left = prefix[cut] - prefix[first];
        const long double right = prefix[last] - prefix[cut];
        // Squared-load reduction is 2*left*right. Prefer even-sized pieces
        // on ties, so flat zero regions also receive stable boundaries.
        const long double gain = left * right;
        const size_t width = std::min(cut - first, last - cut);
        if (gain > best_gain || (gain == best_gain && width > best_width)) {
          best_gain = gain;
          best_group = group;
          best_cut = cut;
          best_width = width;
        }
      }
    }
    cuts.insert(cuts.begin() + best_group, best_cut);
  }
  return cuts;
}

// Original dynamic program: dp[k][r] = min_l(dp[k-1][l] + weight(l,r)^2).
// For nonnegative weights its segment cost is Monge, so optimal predecessors
// are monotone. Divide-and-conquer evaluates the same recurrence in O(K B log B)
// with two cost rows and one flat table of predecessors.
inline std::vector<size_t>
DynamicProgrammingPartitions(std::span<const uint64_t> weights,
                             size_t lock_count) {
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
  if (prefix.back() == 0) {
    return BalancePartitions(weights, lock_count);
  }
  const auto infinity = std::numeric_limits<long double>::infinity();
  std::vector<long double> previous(bins + 1, infinity), next(bins + 1);
  std::vector<size_t> split((lock_count + 1) * (bins + 1));
  previous[0] = 0;
  for (size_t groups = 1; groups <= lock_count; ++groups) {
    std::fill(next.begin(), next.end(), infinity);
    const auto solve = [&](auto &&self, size_t low, size_t high,
                           size_t opt_low, size_t opt_high) -> void {
      if (low > high) {
        return;
      }
      const size_t middle = low + (high - low) / 2;
      size_t best = opt_low;
      for (size_t cut = opt_low; cut <= std::min(opt_high, middle - 1); ++cut) {
        const long double load = prefix[middle] - prefix[cut];
        const long double cost = previous[cut] + load * load;
        if (cost < next[middle]) {
          next[middle] = cost;
          best = cut;
        }
      }
      split[groups * (bins + 1) + middle] = best;
      if (middle > low) {
        self(self, low, middle - 1, opt_low, best);
      }
      self(self, middle + 1, high, best, opt_high);
    };
    solve(solve, groups, bins, groups - 1, bins - 1);
    previous.swap(next);
  }
  std::vector<size_t> cuts(lock_count + 1);
  cuts[lock_count] = bins;
  for (size_t groups = lock_count; groups > 0; --groups) {
    cuts[groups - 1] = split[groups * (bins + 1) + cuts[groups]];
  }
  return cuts;
}

} // namespace dynamic_lock_detail
