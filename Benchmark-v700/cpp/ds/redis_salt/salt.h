#pragma once

#include "salt_node_builder.h"
#include "../../common/gsat.h"
#include "log_delimiter.h"

namespace {

template <typename Score, typename Member, ClearPolicy CP>
using SaltBase =
    Gsat<Score, Member, CP, LogDelimiter, SaltNode<Score, Member>, SaltNodeBuilder<Score, Member>>;

}

template <typename Score, typename Member, ClearPolicy CP>
class Salt : public SaltBase<Score, Member, CP> {
public:
    static constexpr int kLeafSize = 8;
    static constexpr int64_t kMinRebuildBound = 125;
    static constexpr double kRebuildFactor = 0.25;

    using Base = SaltBase<Score, Member, CP>;

    Salt(Score left_bound, Score right_bound, Score no_score, int leaf_size, int64_t min_rebuild_bound, double rebuild_factor)
        : Base(left_bound, right_bound, no_score, leaf_size, min_rebuild_bound, rebuild_factor) {}

    Salt(Score left_bound, Score right_bound, Score no_score)
        : Salt(left_bound, right_bound, no_score, kLeafSize, kMinRebuildBound, kRebuildFactor) {}

};
