#pragma once

#include "sabt_node_builder.h"
#include "../../common/gsat.h"
#include "constant_delimiter.h"

namespace {

template <typename Score, typename Member, int B, ClearPolicy CP>
using SabtBase = Gsat<Score, Member, CP, ConstantDelimiter<B>, SabtNode<Score, Member, B>,
                      SabtNodeBuilder<Score, Member, B, SabtNode<Score, Member, B>>>;

}

template <typename Score, typename Member, int B, ClearPolicy CP>
class Sabt : public SabtBase<Score, Member, B, CP> {
public:
    static constexpr int64_t kMinRebuildBound = 125;
    static constexpr double kRebuildFactor = 0.25;

    using Base = SabtBase<Score, Member, B, CP>;

    Sabt(Score left_bound, Score right_bound, Score no_score, int64_t min_rebuild_bound, double rebuild_factor)
        : Base(left_bound, right_bound, no_score, B, min_rebuild_bound, rebuild_factor) {
    }

    Sabt(Score left_bound, Score right_bound, Score no_score)
        : Sabt(left_bound, right_bound, no_score, kMinRebuildBound, kRebuildFactor) {
    }
};
