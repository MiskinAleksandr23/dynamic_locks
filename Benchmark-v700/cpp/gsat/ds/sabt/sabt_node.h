#pragma once

#include "../gsat/gsat_node.h"

#ifdef KEY_SEARCH_TOTAL_STAT
extern int64_t key_search_total_iters_cnt__;
extern int64_t key_search_total_cnt__;
#endif

template<typename Key, typename Value, int kMaxKeys>
struct SABTNode : public GSATNode<Key, Value> {
    using Base = GSATNode<Key, Value>;

    Key rep[kMaxKeys];
    typename Base::ValueData value_data[kMaxKeys];
    SABTNode *children[kMaxKeys + 1];

    SABTNode(const Key &left, const Key &right, int capacity, int64_t rebuild_bound)
            : Base(left, right, rebuild_bound) {
        std::fill(children, children + kMaxKeys + 1, nullptr);
    }

    ~SABTNode() {
        for (int index = 0; index <= this->rep_size; ++index) {
            delete children[index];
        }
    }

    int GetCapacity() const {
        return kMaxKeys;
    }

    void Complete(int64_t total_accesses) {
        INITIALIZE_LEAF(this)
    }

    int Search(const Key& key) {
        int l = -1;
        int r = this->rep_size;
        while (r - l > 1) {
#ifdef KEY_SEARCH_TOTAL_STAT
            ++key_search_total_iters_cnt__;
#endif
            int m = (l + r) >> 1;
            if (rep[m] < key) {
                l = m;
            } else {
                r = m;
            }
        }
#ifdef KEY_SEARCH_TOTAL_STAT
        ++key_search_total_cnt__;
#endif
        return r;
    }
};
