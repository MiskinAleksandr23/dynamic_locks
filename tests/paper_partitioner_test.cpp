#include "dynamic/paper_partitioner.hpp"

#include <array>
#include <iostream>
#include <random>
#include <string>

namespace {
using paper_lock_detail::Cost;
using paper_lock_detail::CHTPartitions;
using paper_lock_detail::Objective;
using paper_lock_detail::QuantilePartitions;

void Check(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

template <class Exception, class Func> void Throws(Func function) {
  try { function(); }
  catch (const Exception &) { return; }
  throw std::runtime_error("expected exception was not raised");
}

void ValidCuts(std::span<const size_t> cuts, size_t bins, size_t groups) {
  Check(cuts.size() == groups + 1, "wrong group count");
  Check(cuts.front() == 0 && cuts.back() == bins, "histogram not covered");
  for (size_t j = 1; j < cuts.size(); ++j)
    Check(cuts[j] > cuts[j - 1], "empty or unordered group");
}

// Independent exhaustive partition enumeration. It uses direct group summation,
// not prefix sums, DP or a hull. The cost includes each internal cut exactly once.
std::vector<Cost> Brute(std::span<const uint64_t> h, std::span<const uint64_t> c,
                        uint64_t lambda) {
  std::vector<Cost> best(h.size() + 1, ~Cost{0});
  for (size_t mask = 0; mask < (size_t{1} << (h.size() - 1)); ++mask) {
    size_t groups = 1;
    Cost cost = 0, load = h[0];
    for (size_t i = 1; i < h.size(); ++i) {
      if (mask & (size_t{1} << (i - 1))) {
        cost += load * load + Cost{lambda} * c[i];
        load = 0;
        ++groups;
      }
      load += h[i];
    }
    cost += load * load;
    best[groups] = std::min(best[groups], cost);
  }
  return best;
}

void CheckProfile(const std::vector<uint64_t> &h, const std::vector<uint64_t> &c,
                  uint64_t lambda) {
  const auto best = Brute(h, c, lambda);
  for (size_t k = 1; k <= h.size(); ++k) {
    const auto cuts = CHTPartitions(h, c, k, lambda);
    ValidCuts(cuts, h.size(), k);
    Check(Objective(h, c, cuts, lambda) == best[k], "CHT does not attain brute-force optimum");
    const auto quantiles = QuantilePartitions(h, k);
    ValidCuts(quantiles, h.size(), k);
    Check(Objective(h, c, quantiles, lambda) >= best[k], "quantile cost below exact optimum");
    Check(CHTPartitions(h, c, k, lambda) == cuts, "non-deterministic tie handling");
  }
}

void CheckQuantiles() {
  Check(QuantilePartitions(std::vector<uint64_t>{1, 1, 1, 1, 1, 1, 1, 1}, 4) ==
        std::vector<size_t>({0, 2, 4, 6, 8}), "uniform quantiles");
  Check(QuantilePartitions(std::vector<uint64_t>{0, 0, 0, 0, 0, 0, 0, 0}, 3) ==
        std::vector<size_t>({0, 2, 5, 8}), "zero-load fallback");
  Check(QuantilePartitions(std::vector<uint64_t>{1, 1, 8, 0, 0, 0}, 3) ==
        std::vector<size_t>({0, 3, 4, 6}), "quantile must advance past previous cut");
  Check(QuantilePartitions(std::vector<uint64_t>{0, 0, 0, 0, 0, 90}, 4) ==
        std::vector<size_t>({0, 3, 4, 5, 6}), "quantile must reserve later nonempty groups");
  Check(QuantilePartitions(std::vector<uint64_t>{100, 0, 0, 0, 0, 0}, 4) ==
        std::vector<size_t>({0, 1, 2, 3, 6}), "literal quantile rule differs from minimax splitting");
  Check(QuantilePartitions(std::vector<uint64_t>{3, 4}, 9) == std::vector<size_t>({0, 1, 2}), "K>B quantiles");
}

void CheckExhaustive() {
  for (size_t n = 1; n <= 7; ++n) {
    size_t count = 1;
    for (size_t i = 0; i < n; ++i) count *= 3;
    for (size_t word = 0; word < count; ++word) {
      std::vector<uint64_t> h(n), c(n);
      size_t remaining = word;
      for (auto &value : h) { value = remaining % 3; remaining /= 3; }
      for (size_t i = 1; i < n; ++i) c[i] = std::min(h[i - 1], h[i]);
      for (uint64_t lambda : {uint64_t{0}, uint64_t{1}, uint64_t{10'000'000}})
        CheckProfile(h, c, lambda);
    }
  }
}

void CheckRandom() {
  std::mt19937_64 rng(97);
  for (size_t trial = 0; trial < 600; ++trial) {
    const size_t n = 1 + rng() % 10;
    std::vector<uint64_t> h(n), c(n);
    if (trial % 2) {
      // Physically realizable sampled queries with arbitrary repeated weights.
      for (size_t q = 0; q < 40; ++q) {
        const size_t left = rng() % n, right = left + rng() % (n - left);
        const uint64_t weight = 1 + rng() % 50;
        for (size_t i = left; i <= right; ++i) h[i] += weight;
        for (size_t i = left + 1; i <= right; ++i) c[i] += weight;
      }
    } else {
      // Snapshot counts need not describe one coherent instant. Cover arbitrary
      // nonnegative histograms too, including zero hits with nonzero crossings.
      for (size_t i = 0; i < n; ++i) {
        h[i] = rng() % 9;
        if (i) c[i] = rng() % 100;
      }
    }
    const uint64_t lambda = trial % 3 == 0 ? 10'000'000 : rng() % 100;
    CheckProfile(h, c, lambda);
    if (trial < 50) {
      const auto best = Brute(h, c, lambda);
      auto scaled_h = h, scaled_c = c;
      for (auto &v : scaled_h) v *= 7;
      for (auto &v : scaled_c) v *= 7;
      for (size_t k = 1; k <= n; ++k) {
        const auto cuts = CHTPartitions(scaled_h, scaled_c, k, 7 * lambda);
        Check(Objective(scaled_h, scaled_c, cuts, 7 * lambda) == 49 * best[k], "count/lambda scale invariance");
      }
    }
  }
}

void CheckLargeArithmetic() {
  // These values force products in hull intersection comparisons beyond 128
  // bits while their objective values remain inside the documented envelope.
  const uint64_t scale = uint64_t{1} << 54;
  const std::vector<uint64_t> h{scale, 0, 5 * scale, scale, 0, 3 * scale, 2 * scale, scale};
  const std::vector<uint64_t> c{0, scale, 3 * scale, scale, 0, scale, 2 * scale, scale};
  CheckProfile(h, c, scale);
  CheckProfile(h, std::vector<uint64_t>(h.size()), 0);

  using paper_lock_detail::partition_detail::FractionAtLeast;
  const Cost big = Cost{1} << 120;
  Check(FractionAtLeast(big, Cost{9}, big - 1, Cost{9}), "large equal denominator");
  Check(!FractionAtLeast(big - 1, Cost{9}, big, Cost{9}), "large equal denominator reverse");
  Check(FractionAtLeast(big, Cost{1} << 63, big / 2, Cost{1} << 62), "large equal fractions");
  Check(FractionAtLeast(big, (Cost{1} << 63) - 1, big, Cost{1} << 63), "large close fractions");
  Check(!FractionAtLeast(big, Cost{1} << 63, big, (Cost{1} << 63) - 1), "large close fractions reverse");

  // An accepted profile close to the documented objective envelope.
  const uint64_t near_limit = (uint64_t{1} << 60) - 1;
  CheckProfile({near_limit, near_limit, near_limit}, {0, 1, 0}, 1);
}

void CheckPenalty() {
  CheckProfile({0, 0, 0, 0}, {0, 7, 1, 9}, 10);
  const std::vector<uint64_t> h{0, 1, 1}, c{0, 0, 1};
  Check(CHTPartitions(h, c, 2, 0) == std::vector<size_t>({0, 2, 3}),
        "zero penalty must balance the hot blocks");
  Check(CHTPartitions(h, c, 2, 10) == std::vector<size_t>({0, 1, 3}),
        "crossing penalty must avoid an expensive internal cut");
  Check(CHTPartitions(std::vector<uint64_t>{0, 10, 10},
                      std::vector<uint64_t>{0, 0, 10}, 2, 10) == std::vector<size_t>({0, 2, 3}),
        "raw-count objective has the expected sample-window dependence");
}

void CheckInvalid() {
  const std::vector<uint64_t> empty, one{1}, zero{0};
  Check(QuantilePartitions(empty, 1) == std::vector<size_t>{0}, "empty quantiles");
  Check(CHTPartitions(empty, empty, 1, 1) == std::vector<size_t>{0}, "empty CHT");
  Check(Objective(empty, empty, std::vector<size_t>{0}, 1) == 0, "empty objective");
  Check(CHTPartitions(one, zero, 20, 100) == std::vector<size_t>({0, 1}), "K>B CHT");
  Throws<std::invalid_argument>([&] { QuantilePartitions(one, 0); });
  Throws<std::invalid_argument>([&] { CHTPartitions(one, zero, 0, 1); });
  Throws<std::invalid_argument>([&] { CHTPartitions(one, empty, 1, 1); });
  Throws<std::invalid_argument>([&] { CHTPartitions(one, one, 1, 1); });
  Throws<std::invalid_argument>([&] { Objective(one, zero, std::vector<size_t>{0, 0, 1}, 0); });
  Throws<std::invalid_argument>([&] { Objective(one, zero, std::vector<size_t>{0, 2, 1}, 0); });
  Throws<std::invalid_argument>([&] { Objective(one, zero, std::vector<size_t>{1}, 0); });
  Throws<std::invalid_argument>([&] { Objective(empty, empty, std::vector<size_t>{0, 0}, 0); });
  const uint64_t maximum = std::numeric_limits<uint64_t>::max();
  Throws<std::overflow_error>([&] { QuantilePartitions(std::vector<uint64_t>{maximum, 1}, 1); });
  Throws<std::overflow_error>([&] { CHTPartitions(std::vector<uint64_t>{maximum}, zero, 1, 0); });
  Throws<std::overflow_error>([&] { CHTPartitions(std::vector<uint64_t>{0, 0}, std::vector<uint64_t>{0, maximum}, 1, maximum); });
}
} // namespace

int main() {
  try {
    CheckQuantiles();
    CheckExhaustive();
    CheckRandom();
    CheckLargeArithmetic();
    CheckPenalty();
    CheckInvalid();
    std::cout << "PASS paper quantiles and exact CHT: exhaustive partitions, crossings, all K, ties, large arithmetic and validation\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
