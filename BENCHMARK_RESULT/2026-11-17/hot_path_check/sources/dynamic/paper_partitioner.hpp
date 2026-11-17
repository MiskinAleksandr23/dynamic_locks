#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace paper_lock_detail {

using Cost = unsigned __int128;
using SignedCost = __int128;

// Exact fixed-width arithmetic, not a floating-point approximation to the model.
// This envelope leaves room for signed CHT intercepts and evaluations. Inputs
// exceeding it are rejected instead of silently wrapping. In particular,
// total_hits^2 + lambda * sum(crossings) must not exceed 2^124 - 1.
inline constexpr Cost kCostLimit = (Cost{1} << 124) - 1;

namespace partition_detail {

inline std::vector<uint64_t> Prefix(std::span<const uint64_t> hits) {
  if (hits.size() == std::numeric_limits<size_t>::max())
    throw std::length_error("histogram too large");
  std::vector<uint64_t> prefix(hits.size() + 1);
  for (size_t i = 0; i < hits.size(); ++i) {
    if (hits[i] > std::numeric_limits<uint64_t>::max() - prefix[i])
      throw std::overflow_error("total hits exceed uint64_t");
    prefix[i + 1] = prefix[i] + hits[i];
  }
  return prefix;
}

inline void ValidateCost(uint64_t total, std::span<const uint64_t> crossings,
                         uint64_t lambda, size_t bins) {
  if (crossings.size() != bins || (!crossings.empty() && crossings[0] != 0))
    throw std::invalid_argument("one crossing count per block boundary is required, with c[0]=0");
  Cost bound = Cost{total} * total;
  if (bound > kCostLimit) throw std::overflow_error("partition cost exceeds exact arithmetic envelope");
  for (size_t i = 1; i < crossings.size(); ++i) {
    const Cost term = Cost{lambda} * crossings[i];
    if (term > kCostLimit - bound)
      throw std::overflow_error("partition cost exceeds exact arithmetic envelope");
    bound += term;
  }
}

// Compare nonnegative fractions exactly, without overflowing their cross
// products. Ordinary-sized histograms take the two-multiplication fast path.
// The fallback is the Euclidean/continued-fraction comparison on fixed 128-bit
// integers; its bounded iteration count does not grow with B or K.
inline bool FractionAtLeast(Cost a, Cost b, Cost c, Cost d) {
  constexpr Cost maximum = ~Cost{0};
  if (a <= maximum / d && c <= maximum / b) return a * d >= c * b;
  bool reversed = false;
  for (;;) {
    const Cost qa = a / b, qc = c / d;
    if (qa != qc) return reversed ? qa < qc : qa > qc;
    a %= b;
    c %= d;
    if (a == 0 || c == 0) {
      if (a == c) return true;
      return reversed ? a == 0 : c == 0;
    }
    std::swap(a, b);
    std::swap(c, d);
    reversed = !reversed;
  }
}

inline bool FractionAtLeast(SignedCost a, Cost b, SignedCost c, Cost d) {
  if (a < 0 && c >= 0) return false;
  if (a >= 0 && c < 0) return true;
  if (a < 0) return FractionAtLeast(Cost(-c), d, Cost(-a), b);
  return FractionAtLeast(Cost(a), b, Cost(c), d);
}

struct Line {
  uint64_t prefix;
  SignedCost intercept;
  size_t cut;

  SignedCost At(uint64_t x) const {
    return intercept - SignedCost{2} * prefix * x;
  }
};

// Slopes are -2*prefix and strictly decrease after equal-slope elimination.
inline bool Redundant(const Line &a, const Line &b, const Line &c) {
  // Intersection(a,b) >= Intersection(b,c); omit common factor 2.
  return FractionAtLeast(b.intercept - a.intercept, Cost{b.prefix - a.prefix},
                         c.intercept - b.intercept, Cost{c.prefix - b.prefix});
}

} // namespace partition_detail

// Literal cumulative quantile rule from the paper. Internal cut j is the first
// legal r with K*S[r] >= j*S[B], capped to leave one block for each later group.
// A zero-load profile uses evenly sized groups. K>B is clamped to B.
inline std::vector<size_t> QuantilePartitions(std::span<const uint64_t> hits,
                                             size_t lock_count) {
  if (lock_count == 0) throw std::invalid_argument("lock count must be positive");
  const size_t bins = hits.size();
  if (bins == 0) return {0};
  lock_count = std::min(lock_count, bins);
  const auto prefix = partition_detail::Prefix(hits);
  std::vector<size_t> cuts(lock_count + 1);
  cuts.back() = bins;
  if (prefix.back() == 0) {
    for (size_t j = 1; j < lock_count; ++j)
      cuts[j] = static_cast<size_t>(Cost{j} * bins / lock_count);
    return cuts;
  }
  size_t r = 0;
  for (size_t j = 1; j < lock_count; ++j) {
    ++r;
    const size_t last_legal = bins - (lock_count - j);
    while (r < last_legal && Cost{lock_count} * prefix[r] < Cost{j} * prefix.back()) ++r;
    cuts[j] = r;
  }
  return cuts;
}

inline Cost Objective(std::span<const uint64_t> hits,
                      std::span<const uint64_t> crossings,
                      std::span<const size_t> cuts, uint64_t lambda) {
  const auto prefix = partition_detail::Prefix(hits);
  partition_detail::ValidateCost(prefix.back(), crossings, lambda, hits.size());
  if (hits.empty() && cuts.size() == 1 && cuts.front() == 0) return 0;
  if (cuts.size() < 2 || cuts.front() != 0 || cuts.back() != hits.size())
    throw std::invalid_argument("partition must cover the histogram");
  Cost cost = 0;
  for (size_t j = 1; j < cuts.size(); ++j) {
    if (cuts[j] <= cuts[j - 1] || cuts[j] > hits.size())
      throw std::invalid_argument("partition must contain nonempty consecutive groups");
    const Cost load = prefix[cuts[j]] - prefix[cuts[j - 1]];
    cost += load * load;
    if (j + 1 < cuts.size()) cost += Cost{lambda} * crossings[cuts[j]];
  }
  return cost;
}

// Exact DP for sum(segment hits)^2 + lambda*sum(selected boundary crossings).
// D[k,r] = S[r]^2 + min_l {-2*S[l]*S[r] + D[k-1,l] + S[l]^2 + lambda*c[l]}.
// Nonnegative hits imply monotone slopes and query coordinates. Each line is
// inserted and removed at most once per layer: O(KB) fixed-width operations,
// O(B) working costs/hull and O(KB) predecessors for reconstructing the cuts.
inline std::vector<size_t> CHTPartitions(std::span<const uint64_t> hits,
                                        std::span<const uint64_t> crossings,
                                        size_t lock_count, uint64_t lambda) {
  if (lock_count == 0) throw std::invalid_argument("lock count must be positive");
  const auto prefix = partition_detail::Prefix(hits);
  const size_t bins = hits.size();
  partition_detail::ValidateCost(prefix.back(), crossings, lambda, bins);
  if (bins == 0) return {0};
  lock_count = std::min(lock_count, bins);
  if (lock_count == 1) return {0, bins};
  if (lock_count == bins) {
    std::vector<size_t> cuts(bins + 1);
    for (size_t i = 0; i <= bins; ++i) cuts[i] = i;
    return cuts;
  }
  // For no hits and a positive crossing penalty (arbitrary input histograms),
  // crossings can still select the best cuts, so do not take a zero-hit shortcut.
  if (lock_count > std::numeric_limits<size_t>::max() / (bins + 1))
    throw std::length_error("predecessor table too large");
  constexpr Cost infinity = ~Cost{0};
  std::vector<Cost> previous(bins + 1, infinity), next(bins + 1, infinity);
  std::vector<size_t> predecessor(lock_count * (bins + 1));
  std::vector<partition_detail::Line> hull;
  hull.reserve(bins);
  previous[0] = 0;
  for (size_t k = 1; k <= lock_count; ++k) {
    std::fill(next.begin(), next.end(), infinity);
    hull.clear();
    size_t head = 0;
    for (size_t r = k; r <= bins; ++r) {
      const size_t l = r - 1;
      if (previous[l] != infinity) {
        const partition_detail::Line line{
            prefix[l], SignedCost(previous[l] + Cost{prefix[l]} * prefix[l] + Cost{lambda} * crossings[l]), l};
        bool insert = true;
        if (hull.size() > head && hull.back().prefix == line.prefix) {
          // Keep the older predecessor on equal intercepts for deterministic ties.
          if (hull.back().intercept <= line.intercept) insert = false;
          else hull.pop_back();
        }
        if (insert) {
          while (hull.size() - head >= 2 &&
                 partition_detail::Redundant(hull[hull.size() - 2], hull.back(), line))
            hull.pop_back();
          hull.push_back(line);
        }
      }
      while (hull.size() - head >= 2 && hull[head + 1].At(prefix[r]) < hull[head].At(prefix[r])) ++head;
      const SignedCost cost = SignedCost(Cost{prefix[r]} * prefix[r]) + hull[head].At(prefix[r]);
      next[r] = Cost(cost);
      predecessor[(k - 1) * (bins + 1) + r] = hull[head].cut;
    }
    previous.swap(next);
  }
  std::vector<size_t> cuts(lock_count + 1);
  cuts.back() = bins;
  for (size_t k = lock_count; k > 0; --k)
    cuts[k - 1] = predecessor[(k - 1) * (bins + 1) + cuts[k]];
  return cuts;
}

} // namespace paper_lock_detail
