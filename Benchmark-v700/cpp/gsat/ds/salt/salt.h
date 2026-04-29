#pragma once

#include "../gsat/gsat.h"

#include "log_delimiter.h"
#include "salt_node.h"

template<typename Key, typename Value, ClearPolicy CP>
using SALTBase = GSAT<LogDelimiter, SALTNode<Key, Value>, CP, Key, Value>;

template<typename Key, typename Value, ClearPolicy CP>
class SALT : public SALTBase<Key, Value, CP> {
public:
    static constexpr int kMinLeafSize = 8;
    static constexpr int64_t kMinRebuildBound = 125;
    static constexpr double kRebuildFactor = 0.25;

    using Node = SALTNode<Key, Value>;
    using Base = SALTBase<Key, Value, CP>;
    using NodeHandler = typename Base::NodeHandler;

    SALT(const Value &no_value, const Key &left, const Key &right, int leaf_size, int64_t min_rebuild_bound,
         double rebuild_factor)
            : Base(no_value, left, right, leaf_size, min_rebuild_bound, rebuild_factor) {}

    SALT(const Value &no_value, const Key &left, const Key &right)
            : SALT(no_value, left, right, kMinLeafSize, kMinRebuildBound, kRebuildFactor) {}
};
