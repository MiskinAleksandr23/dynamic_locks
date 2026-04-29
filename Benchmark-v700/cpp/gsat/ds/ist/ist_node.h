#pragma once

#include <algorithm>
#include <cstdint>

#include "id.h"

#ifdef KEY_SEARCH_TOTAL_STAT
extern int64_t key_search_total_iters_cnt__;
extern int64_t key_search_total_cnt__;
#endif

template<typename Key, typename Value>
struct ISTNode {
    struct ValueData {
        Value value;
        bool marked;
    };

    Key *rep;
    ValueData *value_data;
    ISTNode **children;
    int *id;

    const Key left;   // inclusive
    const Key right;  // exclusive

    int rep_size;
    const int capacity;
    int id_size;

    int total_asize;  // approximate size (total_asize >= total_size)

    const int rebuild_bound;
    int counter;

    ISTNode(const Key &left, const Key &right, int capacity, int rebuild_bound)
            : rep(new Key[capacity]),
              value_data(new ValueData[capacity]),
              children(new ISTNode *[capacity + 1]),
              id(nullptr),
              left(left),
              right(right),
              rep_size(0),
              capacity(capacity),
              total_asize(0),
              rebuild_bound(rebuild_bound),
              counter(0) {
        std::fill(children, children + capacity + 1, nullptr);
    }

    ~ISTNode() {
        delete[] rep;
        delete[] value_data;
        delete[] id;

        for (int index = 0; index <= rep_size; ++index) {
            delete children[index];
        }
        delete[] children;
    }

    void BuildId(int size) {
        id_size = size;
        id = new int[size];
        BUILD_ID(this)
    }

    int Search(const Key &key) const {
        int l = -1;
        int r = rep_size;

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

    inline bool KeyFound(int index, const Key &key_to_find) const {
        return index != rep_size && rep[index] == key_to_find;
    }

    bool Count() {
        ++counter;
        return counter > rebuild_bound;
    }

    void Insert(int index, const Key &key, const Value &value) {
        for (int at = rep_size - 1; at >= index; --at) {
            rep[at + 1] = std::move(rep[at]);
            value_data[at + 1] = std::move(value_data[at]);
        }
        rep[index] = key;
        value_data[index] = {value, false};
        ++rep_size;
    }
};
