#pragma once

#include <algorithm>

#ifdef KEY_SEARCH_TOTAL_STAT
extern int64_t key_search_total_iters_cnt__;
extern int64_t key_search_total_cnt__;
#endif

template<typename Key, typename Value, int kMaxKeys>
struct BTreeNode {
    static_assert(kMaxKeys > 0, "max keys must be greater zero");

    Key keys[kMaxKeys];
    Value values[kMaxKeys];
    BTreeNode *children[kMaxKeys + 1];
    int size;

    void InitializeDefault() {
        std::fill(children, children + kMaxKeys + 1, nullptr);
        size = 0;
    }

    void InitializeMove(Key *ikeys, Value *ivalues, BTreeNode **ichildren, int isize) {
        std::move(ikeys, ikeys + isize, keys);

        std::move(ivalues, ivalues + isize, values);

        std::copy(ichildren, ichildren + isize + 1, children);
        std::fill(children + isize + 1, children + kMaxKeys + 1, nullptr);

        size = isize;
    }

    int Search(const Key &key) const {
        int l = -1;
        int r = size;
        while (r - l > 1) {
#ifdef KEY_SEARCH_TOTAL_STAT
            ++key_search_total_iters_cnt__;
#endif
            int m = (l + r) >> 1;
            if (keys[m] < key) {
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
        return index != size && keys[index] == key_to_find;
    }
};
