#pragma once

#include <cstdint>
#include <random>
#include <vector>

enum class TrafficSide { kLeft, kRight };

struct Query {
  uint32_t left = 0;
  uint32_t right = 0;
  uint32_t target = 0;
  int delta = 0;
};

class WorkloadGenerator {
public:
  WorkloadGenerator(size_t array_size, size_t hot_window_size,
                    size_t query_length, uint64_t seed)
    : array_size_(array_size),
      hot_window_size_(hot_window_size),
      query_length_(query_length),
      rng_(seed) {}

  std::vector<Query> BuildBatch(size_t query_count, TrafficSide side) {
    const size_t hot_begin =
      side == TrafficSide::kLeft ? 0 : array_size_ - hot_window_size_;

    std::bernoulli_distribution hot_query_dist(0.99);
    std::uniform_int_distribution<int> delta_dist(1, 7);
    std::vector<Query> queries;
    queries.reserve(query_count);

    for (size_t i = 0; i < query_count; ++i) {
      const size_t range_begin =
        hot_query_dist(rng_) ? hot_begin : RandomColdWindowStart(hot_begin);

      std::uniform_int_distribution<size_t> left_dist(
        range_begin, range_begin + hot_window_size_ - query_length_);
      const size_t left = left_dist(rng_);

      std::uniform_int_distribution<size_t> target_offset_dist(
        0, query_length_ - 1);
      const size_t target = left + target_offset_dist(rng_);

      queries.push_back({static_cast<uint32_t>(left),
                         static_cast<uint32_t>(left + query_length_ - 1),
                         static_cast<uint32_t>(target), delta_dist(rng_)});
    }

    return queries;
  }

private:
  size_t RandomColdWindowStart(size_t excluded_hot_begin) {
    if (excluded_hot_begin == 0) {
      std::uniform_int_distribution<size_t> dist(hot_window_size_,
                                                 array_size_ - hot_window_size_);
      return dist(rng_);
    }

    std::uniform_int_distribution<size_t> dist(0, array_size_ - 2 * hot_window_size_);
    return dist(rng_);
  }

  size_t array_size_;
  size_t hot_window_size_;
  size_t query_length_;
  std::mt19937_64 rng_;
};
