#pragma once

#include "../redis_sabt/sabt_node_builder.h"
#include "../../common/gsat.h"
#include "../redis_sabt/constant_delimiter.h"
#include "sabpt_node.h"

namespace {

template <typename Score, typename Member, int B, ClearPolicy CP>
using SabptBase =
    Gsat<Score, Member, CP, ConstantDelimiter<B>, SabptNode<Score, Member, B>, SabtNodeBuilder<Score, Member, B, SabptNode<Score, Member, B>>>;

}

template <typename Score, typename Member, int B, ClearPolicy CP>
class Sabpt : public SabptBase<Score, Member, B, CP> {
public:
    static constexpr int64_t kMinRebuildBound = 125;
    static constexpr double kRebuildFactor = 0.25;

    using Base = SabptBase<Score, Member, B, CP>;

    Sabpt(Score left_bound, Score right_bound, Score no_score, int64_t min_rebuild_bound, double rebuild_factor)
        : Base(left_bound, right_bound, no_score, B, min_rebuild_bound, rebuild_factor) {}

    Sabpt(Score left_bound, Score right_bound, Score no_score)
        : Sabpt(left_bound, right_bound, no_score, kMinRebuildBound, kRebuildFactor) {}

};
