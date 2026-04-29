#pragma once

#include "../gsat/gsat_node.h"
#include <cmath>

#ifdef KEY_SEARCH_TOTAL_STAT
extern int64_t key_search_total_iters_cnt__;
extern int64_t key_search_total_cnt__;
#endif

template<typename Key, typename Value>
struct SASTNode : public GSATNode<Key, Value> {
    using Base = GSATNode<Key, Value>;

    Key rep[1];
    typename Base::ValueData value_data[1];
    SASTNode* children[2];

    SASTNode(const Key &left, const Key &right, int capacity, int64_t rebuild_bound)
            : Base(left, right, rebuild_bound) {
        std::fill(children, children + 2, nullptr);
    }

    ~SASTNode() {
        for (int index = 0; index <= this->rep_size; ++index) {
            delete children[index];
        }
    }

    int GetCapacity() const {
        return 1;
    }

    void Complete(int64_t total_accesses) {
        INITIALIZE_LEAF(this)
    }

    int Search(const Key &key) const {
#ifdef KEY_SEARCH_TOTAL_STAT
            ++key_search_total_iters_cnt__;
#endif
        return (this->rep_size == 0 || key <= rep[0]) ? 0 : 1;
    }
};
