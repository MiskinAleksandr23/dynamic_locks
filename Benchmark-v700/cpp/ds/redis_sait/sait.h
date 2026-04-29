#pragma once

#include "sait_node_builder.h"
#include "../../common/gsat.h"
#include "sqrt_delimiter.h"

namespace {

template <typename Score, typename Member, ClearPolicy CP>
using SaitBase =
    Gsat<Score, Member, CP, SqrtDelimiter, SaitNode<Score, Member>, SaitNodeBuilder<Score, Member>>;

}

template <typename Score, typename Member, ClearPolicy CP>
class Sait : public SaitBase<Score, Member, CP> {
public:
    static constexpr int kLeafSize = 8;
    static constexpr int64_t kMinRebuildBound = 125;
    static constexpr double kRebuildFactor = 0.25;

    using Base = SaitBase<Score, Member, CP>;

    Sait(Score left_bound, Score right_bound, Score no_score, int leaf_size, int64_t min_rebuild_bound, double rebuild_factor)
        : Base(left_bound, right_bound, no_score, leaf_size, min_rebuild_bound, rebuild_factor) {}

    Sait(Score left_bound, Score right_bound, Score no_score)
        : Sait(left_bound, right_bound, no_score, kLeafSize, kMinRebuildBound, kRebuildFactor) {}

};
