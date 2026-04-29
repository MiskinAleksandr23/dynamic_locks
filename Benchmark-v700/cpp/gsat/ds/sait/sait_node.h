#pragma once

#include "../gsat/gsat_node.h"
#include "../ist/id.h"
#include <cmath>

#ifdef KEY_SEARCH_TOTAL_STAT
extern int64_t key_search_total_iters_cnt__;
extern int64_t key_search_total_cnt__;
#endif

#ifndef ALPHA
#define ALPHA 0.5 // can be arbitrary between [0.5, 1)
#endif

template<typename Key, typename Value>
struct SAITNode : public GSATNode<Key, Value> {
    using Base = GSATNode<Key, Value>;

    Key* rep;
    typename Base::ValueData* value_data;
    SAITNode** children;
    int* id;

    int capacity;
    int id_size;

    SAITNode(const Key &left, const Key &right, int capacity, int64_t rebuild_bound)
            : Base(left, right, rebuild_bound)
            , rep(new Key[capacity])
            , value_data(new typename Base::ValueData[capacity])
            , children(new SAITNode*[capacity + 1])
            , id(nullptr)
            , capacity(capacity)
            , id_size(0) {
        std::fill(children, children + capacity + 1, nullptr);
    }

    ~SAITNode() {
        delete[] rep;
        delete[] value_data;
        delete[] id;
        for (int index = 0; index <= this->rep_size; ++index) {
            delete children[index];
        }
        delete[] children;
    }

    int GetCapacity() const {
        return capacity;
    }

    void Complete(int64_t total_accesses) {
        INITIALIZE_LEAF(this)
        if (this->leaf) {
            return;
        }
        id_size = ceil(pow(total_accesses, ALPHA));
        id = new int[id_size];
        BUILD_ID(this)
    }

    int Search(const Key &key) const {
        int l = -1;
        int r = this->rep_size;

        if (id) {
#ifdef KEY_SEARCH_TOTAL_STAT
            ++key_search_total_iters_cnt__;
#endif
            ID_LOCATE(this)
        }

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
