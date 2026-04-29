#pragma once

#include "../gsat/gsat.h"

#include "constant_delimiter.h"
#include "sabt_node.h"

constexpr int GetMaxKeys(int min_keys) {
    return 2 * min_keys;
}

template<typename Key, typename Value, int kMinKeys, ClearPolicy CP>
using SABTBase = GSAT<ConstantDelimiter<GetMaxKeys(kMinKeys)>, SABTNode<Key, Value, GetMaxKeys(kMinKeys)>, CP, Key, Value>;

template<typename Key, typename Value, int kMinKeys, ClearPolicy CP>
class SABT : public SABTBase<Key, Value, kMinKeys, CP> {
public:
    static_assert(kMinKeys > 0, "min keys must be greater zero");

    static constexpr int64_t kMinRebuildBound = 125;
    static constexpr double kRebuildFactor = 0.25;

    using Node = SABTNode<Key, Value, GetMaxKeys(kMinKeys)>;
    using Base = SABTBase<Key, Value, kMinKeys, CP>;
    using NodeHandler = typename Base::NodeHandler;

    SABT(const Value &no_value, const Key &left, const Key &right, int64_t min_rebuild_bound, double rebuild_factor)
            : Base(no_value, left, right, GetMaxKeys(kMinKeys), min_rebuild_bound, rebuild_factor) {}

    SABT(const Value &no_value, const Key &left, const Key &right)
            : SABT(no_value, left, right, kMinRebuildBound, kRebuildFactor) {}
};
