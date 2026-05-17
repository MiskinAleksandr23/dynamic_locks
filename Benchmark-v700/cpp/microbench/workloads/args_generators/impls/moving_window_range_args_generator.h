#pragma once

#include <algorithm>
#include <utility>
#include <vector>

#include "errors.h"
#include "random_xoshiro256p.h"
#include "workloads/args_generators/args_generator.h"
#include "workloads/data_maps/data_map.h"

namespace microbench::workload {

template <typename K>
class MovingWindowRangeArgsGenerator : public ArgsGenerator<K> {
private:
    Random64& rng_;
    DataMap<K>* data_map_;
    std::vector<size_t> window_begins_;
    size_t window_size_;
    size_t interval_;
    size_t stage_duration_;
    size_t stage_index_ = 0;
    size_t stage_operation_ = 0;

    void advance_stage() {
        ++stage_operation_;
        if (stage_operation_ < stage_duration_) {
            return;
        }

        stage_operation_ = 0;
        stage_index_ = (stage_index_ + 1) % window_begins_.size();
    }

public:
    MovingWindowRangeArgsGenerator(Random64& rng, DataMap<K>* data_map,
                                   std::vector<size_t> window_begins,
                                   size_t window_size, size_t interval,
                                   size_t stage_duration)
        : rng_(rng),
          data_map_(data_map),
          window_begins_(std::move(window_begins)),
          window_size_(std::max(window_size, interval + 1)),
          interval_(interval),
          stage_duration_(std::max(stage_duration, size_t{1})) {
    }

    K next_get() override {
        setbench_error("Operation not supported");
    }

    K next_insert() override {
        setbench_error("Operation not supported");
    }

    K next_remove() override {
        setbench_error("Operation not supported");
    }

    std::pair<K, K> next_range() override {
        const size_t offset_range = std::max(window_size_ - interval_, size_t{1});
        const size_t left_index = window_begins_[stage_index_] + rng_.next(offset_range);
        const size_t right_index = left_index + interval_;
        advance_stage();

        K left = data_map_->get(left_index);
        K right = data_map_->get(right_index);
        if (left > right) {
            std::swap(left, right);
        }
        return {left, right};
    }

    ~MovingWindowRangeArgsGenerator() override {
        delete data_map_;
    }
};

}  // namespace microbench::workload
