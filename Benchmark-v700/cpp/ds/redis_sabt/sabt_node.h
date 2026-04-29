#pragma once

#include "../../common/gsat_node.h"
#include <assert.h>

template <typename Score, typename Member, int B>
class SabtNode : public GsatNode<Score, Member> {
public:
    static_assert(B > 0);

    using Base = GsatNode<Score, Member>;

    SabtNode(Score* scores, typename Base::MemberData* member_data, SabtNode** children, int* counts, Score left_bound,
             Score right_bound, int64_t rebuild_bound, int rep_size, int asize, bool is_leaf)
        : Base(left_bound, right_bound, rebuild_bound, rep_size, asize, is_leaf) {
        std::copy(scores, scores + B, scores_);
        std::copy(member_data, member_data + B, member_data_);
        std::copy(children, children + (B + 1), children_);
        std::copy(counts, counts + (2 * B + 1), counts_);
    }

    ~SabtNode() {
        for (int index = 0; index <= Base::rep_size_; ++index) {
            delete children_[index];
        }
    }

    void IncrementCount(int index) {
        ++counts_[index];
    }

    void DecrementCount(int index) {
        --counts_[index];
    }

    int GetPrefixCount(int end_index) {
        int count = 0;
        for (int i = 0; i < end_index; ++i) {
            count += counts_[i];
        }
        return count;
    }

    SabtNode*& GetChild(int index) {
        return children_[index];
    }

    Score GetScore(int index) const {
        return scores_[index];
    }

    typename Base::MemberData& GetMemberData(int index) {
        return member_data_[index];
    }

    typename Base::MemberIterator GetMemberIterator(int index) {
        return member_data_[index].mit;
    }

    void SetMemberIterator(int index, typename Base::MemberIterator mit) {
        member_data_[index].mit = mit;
    }

    const Member& GetMember(int index) const {
        return *member_data_[index].mit;
    }

    int Search(Score score, const Member& member) {
        int index = Search(score);
        while (index != Base::rep_size_ && scores_[index] == score && GetMember(index) < member) {
            ++index;
        }
        return index;
    }

    int Search(Score score) {
        int l = -1;
        int r = Base::rep_size_;
        while (r - l != 1) {
            int m = (l + r) >> 1;
            if (scores_[m] < score) {
                l = m;
            } else {
                r = m;
            }
        }
        return r;
    }

    bool IsFound(int index, Score score, const Member& member) {
        return index != Base::rep_size_ && score == scores_[index] && GetMember(index) == member;
    }

    int GetCapacity() const {
        return B;
    }

    void Insert(int index, Score score, typename Base::MemberIterator mit) {
        assert(Base::is_leaf_);
        for (int i = Base::rep_size_; i > index; --i) {
            scores_[i] = scores_[i - 1];
            member_data_[i] = member_data_[i - 1];
            counts_[2 * i + 1] = counts_[2 * i - 1];
        }
        scores_[index] = score;
        member_data_[index] = {1, mit};
        counts_[2 * index + 1] = 1;
        ++Base ::rep_size_;
    }

    void Mark(int index) {
        member_data_[index].accesses = -member_data_[index].accesses;
        --counts_[2 * index + 1];
    }

    void Unmark(int index) {
        member_data_[index].accesses = -member_data_[index].accesses;
        ++counts_[2 * index + 1];
    }

    bool IsMarked(int index) const {
        return member_data_[index].accesses < 0;
    }

    void Access(int index, int value = 1) {
        if (IsMarked(index)) {
            member_data_[index].accesses -= value;
        } else {
            member_data_[index].accesses += value;
        }
    }

private:
    Score scores_[B];
    typename Base::MemberData member_data_[B];
    SabtNode* children_[B + 1];
    int counts_[2 * B + 1];
};
