#pragma once

#include "sabt_node.h"
#include <assert.h>

template <typename Score, typename Member, int B, typename Node>
class SabtNodeBuilder {
public:
    SabtNodeBuilder()
        : rebuild_bound_(-1),
          rep_size_(0),
          asize_(0) {
        std::fill(children_, children_ + (B + 1), nullptr);
        std::fill(counts_, counts_ + (2 * B + 1), 0);
    }

    void SetLeftBound(Score left_bound) {
        left_bound_ = left_bound;
    }

    void SetRightBound(Score right_bound) {
        right_bound_ = right_bound;
    }

    void SetCapacity(int capacity) {
        assert(capacity == B);
    }

    void SetRebuildBound(int64_t rebuild_bound) {
        rebuild_bound_ = rebuild_bound;
    }

    void SetRepSize(int rep_size) {
        rep_size_ = rep_size;
    }

    void SetApproximateSize(int asize) {
        asize_ = asize;
    }

    void SetScore(int index, Score score) {
        scores_[index] = score;
    }

    void SetMemberData(int index, typename Node::MemberData md) {
        member_data_[index] = md;
    }

    void SetChild(int index, Node* child) {
        children_[index] = child;
    }

    void IncrementCount(int index, int value = 1) {
        counts_[index] += value;
    }

    bool IsMarked(int index) {
        return member_data_[index].accesses < 0;
    }

    Node* Build([[maybe_unused]] int64_t total_accesses = -1) {
        bool is_leaf = true;
        for (int i = 0; i <= rep_size_; ++i) {
            if (children_[i]) {
                is_leaf = false;
                break;
            }
        }
        return new Node(scores_, member_data_, children_, counts_, left_bound_, right_bound_, rebuild_bound_, rep_size_,
                        asize_, is_leaf);
    }

private:
    Score scores_[B];
    typename Node::MemberData member_data_[B];
    Node* children_[B + 1];
    int counts_[2 * B + 1];

    Score left_bound_;
    Score right_bound_;
    int64_t rebuild_bound_;
    int rep_size_;
    int asize_;
};
