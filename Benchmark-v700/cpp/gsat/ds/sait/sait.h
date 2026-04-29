#pragma once

#include "../gsat/gsat.h"

#include "sqrt_delimiter.h"
#include "sait_node.h"

template<typename Key, typename Value, ClearPolicy CP>
using SAITBase = GSAT<SqrtDelimiter, SAITNode<Key, Value>, CP, Key, Value>;

template<typename Key, typename Value, ClearPolicy CP>
class SAIT : public SAITBase<Key, Value, CP> {
public:
    static constexpr int kMinLeafSize = 8;
    static constexpr int64_t kMinRebuildBound = 125;
    static constexpr double kRebuildFactor = 0.25;

    using Node = SAITNode<Key, Value>;
    using Base = SAITBase<Key, Value, CP>;
    using NodeHandler = typename Base::NodeHandler;

    SAIT(const Value &no_value, const Key &left, const Key &right, int leaf_size, int64_t min_rebuild_bound,
         double rebuild_factor)
            : Base(no_value, left, right, leaf_size, min_rebuild_bound, rebuild_factor) {}

    SAIT(const Value &no_value, const Key &left, const Key &right)
            : SAIT(no_value, left, right, kMinLeafSize, kMinRebuildBound, kRebuildFactor) {}
};
