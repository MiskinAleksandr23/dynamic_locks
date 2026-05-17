#pragma once

#include <algorithm>
#include <vector>

#include "globals_extern.h"
#include "workloads/args_generators/args_generator_builder.h"
#include "workloads/args_generators/impls/moving_window_range_args_generator.h"
#include "workloads/data_maps/builders/id_data_map_builder.h"
#include "workloads/data_maps/data_map_builder.h"
#include "workloads/data_maps/data_map_json_convector.h"

namespace microbench::workload {

class MovingWindowRangeArgsGeneratorBuilder : public ArgsGeneratorBuilder {
private:
    size_t range_ = 1;
    size_t window_size_ = 1;
    size_t interval_ = 0;
    size_t stage_duration_ = 1;
    std::vector<double> window_begin_fractions_;
    std::vector<size_t> window_begins_;
    DataMapBuilder* data_map_builder_ = new IdDataMapBuilder();

public:
    MovingWindowRangeArgsGeneratorBuilder* init(size_t range) override {
        range_ = std::max(range, size_t{1});
        window_size_ = std::min(std::max(window_size_, interval_ + 1), range_);

        const size_t max_begin = range_ - window_size_;
        window_begins_.clear();
        window_begins_.reserve(window_begin_fractions_.size());
        for (double fraction : window_begin_fractions_) {
            fraction = std::clamp(fraction, 0.0, 1.0);
            window_begins_.push_back(static_cast<size_t>(max_begin * fraction));
        }
        if (window_begins_.empty()) {
            window_begins_.push_back(0);
        }
        return this;
    }

    MovingWindowRangeArgsGenerator<K>* build(Random64& rng) override {
        return new MovingWindowRangeArgsGenerator<K>(rng, data_map_builder_->build(),
                                                     window_begins_, window_size_, interval_,
                                                     stage_duration_);
    }

    void to_json(nlohmann::json& j) const override {
        j["ClassName"] = "MovingWindowRangeArgsGeneratorBuilder";
        j["windowSize"] = window_size_;
        j["interval"] = interval_;
        j["stageDuration"] = stage_duration_;
        j["windowBeginFractions"] = window_begin_fractions_;
        j["dataMapBuilder"] = *data_map_builder_;
    }

    void from_json(const nlohmann::json& j) override {
        window_size_ = j["windowSize"];
        interval_ = j["interval"];
        stage_duration_ = j["stageDuration"];
        window_begin_fractions_ = j["windowBeginFractions"].get<std::vector<double>>();
        data_map_builder_ = get_data_map_from_json(j["dataMapBuilder"]);
    }

    std::string to_string(size_t indents = 1) override {
        std::string result;
        result += indented_title_with_str_data("Type", "MovingWindowRange", indents);
        result += indented_title_with_data("Window size", window_size_, indents);
        result += indented_title_with_data("Interval", interval_, indents);
        result += indented_title_with_data("Stage duration", stage_duration_, indents);
        result += indented_title("Window begin fractions", indents);
        for (size_t i = 0; i < window_begin_fractions_.size(); ++i) {
            result += indented_title_with_data("Stop " + std::to_string(i),
                                               window_begin_fractions_[i], indents + 1);
        }
        result += indented_title("Data Map", indents);
        result += data_map_builder_->to_string(indents + 1);
        return result;
    }

    ~MovingWindowRangeArgsGeneratorBuilder() override = default;
};

}  // namespace microbench::workload
