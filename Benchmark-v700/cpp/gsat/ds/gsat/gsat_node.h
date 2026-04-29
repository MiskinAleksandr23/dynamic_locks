#pragma once

#include <algorithm>
#include <cstdint>

#include "../../common/error.h"

#define INITIALIZE_LEAF(node)                           \
for (int index = 0; index <= node->rep_size; ++index) { \
if (node->children[index]) {                            \
node->leaf = false;                                     \
break;                                                  \
}                                                       \
}                                                       \

template<typename Key, typename Value>
struct GSATNode {
    struct ValueData {
        Value value;
        int64_t accesses;
    };

    const Key left;   // inclusive
    const Key right;  // exclusive

    int rep_size;
    int total_asize;  // approximate size (total_asize >= total_size)
    bool leaf;

    const int64_t rebuild_bound;
    int64_t counter;

    GSATNode(const Key& left, const Key& right, int64_t rebuild_bound)
            : left(left), right(right), rep_size(0), total_asize(0), leaf(true), rebuild_bound(rebuild_bound), counter(0) {}

    bool Access() {
        ++counter;
        return counter > rebuild_bound;
    }
};
