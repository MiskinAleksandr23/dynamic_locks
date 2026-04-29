#pragma once

#include <algorithm>
#include <cstdint>
#include <list>

// NodeBuilder methods:
//
// void SetLeftBound(Score left_bound);
// void SetRightBound(Score right_bound);
// void SetCapacity(int capacity);
// void SetRebuildBound(int64_t rebuild_bound);
// void SetRepSize(int rep_size);
// void SetApproximateSize(int asize);
//
// void SetScore(int index, Score score);
// void SetMemberData(int index, MemberData md);
// void SetChild(int index, Node* child);
// void IncrementCount(int index, int value = 1);
//
// bool IsMarked(int index);
//
// Node* Build();

///////////////////////////////////////////////////////////////

// DerivedNode methods:
//
// void IncrementCount(int index);
// void DecrementCount(int index);
// int GetPrefixCount(int end_index);
//
// DerivedNode*& GetChild(int child_index);
// Score GetScore(int rep_index);
// MemberData& GetMemberData(int rep_index);
// MemberIterator GetMemberIterator(int index)
// void SetMemberIterator(int index, MemberIterator mit);
// const Member& GetMember(int rep_index);
//
// int Search(Score score, const Member& member)
// int Search(Score score)
//
// bool IsFound(int index, Score score, const Member& member)
//
// int GetCapacity() const;
//
// void Insert(int index, Score score, MemberIterator mit) // increment count as well
//
// void Mark(int index); // automatically increment count
// void Unmark(int index); // automatically decrement count
// bool IsMarked(int index);
//
// void Access(int index, int value);

template <typename Score, typename Member>
class GsatNode {
public:
    using MemberIterator = typename std::list<Member>::const_iterator;

    struct MemberData {
        int64_t accesses;
        MemberIterator mit;
    };

    GsatNode(Score left_bound, Score right_bound, int64_t rebuild_bound, int rep_size, int asize, bool is_leaf)
        : left_bound_(left_bound),
          right_bound_(right_bound),
          rebuild_bound_(rebuild_bound),
          counter_(0),
          rep_size_(rep_size),
          asize_(asize),
          is_leaf_(is_leaf) {
    }

    bool Count(int value = 1) {
        counter_ += value;
        return counter_ > rebuild_bound_;
    }

    int GetRepSize() const {
        return rep_size_;
    }

    int GetApproximateSize() const {
        return asize_;
    }

    Score GetLeftBound() const {
        return left_bound_;
    }

    Score GetRightBound() const {
        return right_bound_;
    }

    void IncrementApproximateSize() {
        ++asize_;
    }

    bool IsLeaf() const {
        return is_leaf_;
    }

protected:
    const Score left_bound_;   // inclusive
    const Score right_bound_;  // exclusive

    const int64_t rebuild_bound_;
    int64_t counter_;

    int rep_size_;
    int asize_;  // asize_ >= total_size

    bool is_leaf_;
};
