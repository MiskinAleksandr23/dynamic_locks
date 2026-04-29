#pragma once

#include <assert.h>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <utility>
#include <list>
#include <tuple>

#include "error.h"
#include "dict.h"
#include "dict_helpers.h"

#ifdef KEY_DEPTH_TOTAL_STAT
extern int64_t key_depth_total_sum__;
extern int64_t key_depth_total_cnt__;
#endif

#ifdef KEY_DEPTH_STAT
extern int64_t* key_depth_sum__;
extern int64_t* key_depth_cnt__;
#endif

enum class ClearPolicy { kNone, kRoot, kRapid };

template <typename Score, typename Member, ClearPolicy CP, typename D, typename Node, typename NodeBuilder>
class Gsat {
    using DICT = Dict<const Member*, Score, PtrMemberHash<Member, std::hash<Member>>,
                      PtrMemberEqual<Member, std::less<Member>>, DummyDeleter, DummyDeleter>;

    using MemberIterator = typename std::list<Member>::const_iterator;
    using MemberData = typename Node::MemberData;

public:
    // [left_bound, right_bound)
    Gsat(D d, Score left_bound, Score right_bound, Score no_score, int leaf_size, int64_t min_rebuild_bound,
         double rebuild_factor)
        : d_(d),
          left_bound_(left_bound),
          right_bound_(right_bound),
          no_score_(no_score),
          leaf_size_(leaf_size),
          min_rebuild_bound_(min_rebuild_bound),
          rebuild_factor_(rebuild_factor),
          root_(nullptr) {
        root_ = CreateRoot();
        Check(left_bound_ < right_bound_);
        Check(leaf_size_ > 0);
        Check(min_rebuild_bound_ > 1);
        Check(rebuild_factor_ > 0);
    }

    Gsat(Score left_bound, Score right_bound, Score no_score, int leaf_size, int64_t min_rebuild_bound,
         double rebuild_factor)
        : Gsat(D(), left_bound, right_bound, no_score, leaf_size, min_rebuild_bound, rebuild_factor) {
    }

    ~Gsat() {
        delete root_;
    }

    Score InsertIfAbsent(const Member& member, Score score) {
        assert(root_->GetLeftBound() <= score && score < root_->GetRightBound());

        auto de = dict_.Find(&member);
        if (de) {
            return de->GetValue();
        }

        members_.push_front(member);
        auto mit = members_.begin();

        dict_.Add(&*mit, score);
        AddToTree(score, mit);

        return no_score_;
    }

    Score Delete(const Member& member) {
        auto de = dict_.Unlink(&member);
        if (!de) {
            return no_score_;
        }

        Score score = de->GetValue();
        dict_.FreeUnlinkedEntry(de);
        DeleteFromTree(score, member);

        if (dict_.IsNeedResize()) {
            dict_.Resize();
        }

        return score;
    }

    bool GetScore(const Member& member, Score* score) {
        auto de = dict_.Find(&member);
        if (de) {
            *score = de->GetValue();
            return true;
        } else {
            return false;
        }
    }

    // [min, max)
    template <typename OScoreIt, typename OMemberIt>
    size_t GetRange(Score min, Score max, OScoreIt sit, OMemberIt mit) {
        min = std::max(min, left_bound_);
        max = std::min(max, right_bound_);

        if (max <= min) {
            return 0;
        }

        size_t count = 0;

        Node* rebuild_node = nullptr;
        Node** rebuild_node_at = nullptr;

        Node* node = root_;
        Node** node_at = &root_;

        while (node) {
            if (node->Count() && !rebuild_node) {
                rebuild_node = node;
                rebuild_node_at = node_at;
            }
            const auto left_index = node->Search(min);
            if (left_index != node->GetRepSize() && node->GetScore(left_index) < max) {
                const auto right_index = node->Search(max);
                if (node->Count(right_index - left_index) && !rebuild_node) {
                    rebuild_node = node;
                    rebuild_node_at = node_at;
                }

                count +=
                    GetLeftRange(node->GetChild(left_index), &node->GetChild(left_index), min, rebuild_node, sit, mit);

                CollectElement(node, left_index, sit, mit, count);
                for (int i = left_index + 1; i < right_index; ++i) {
                    count += CollectNonMarked(node->GetChild(i), &node->GetChild(i), rebuild_node, sit, mit);
                    CollectElement(node, i, sit, mit, count);
                }

                count += GetRightRange(node->GetChild(right_index), &node->GetChild(right_index), max, rebuild_node,
                                       sit, mit);

                break;
            }
            node_at = &node->GetChild(left_index);
            node = node->GetChild(left_index);
        }

        if (rebuild_node) {
            *rebuild_node_at = RebuildTree(rebuild_node);
        }

        return count;
    }

    // [min, max)
    size_t Count(Score min, Score max) {
        min = std::max(min, left_bound_);
        max = std::min(max, right_bound_);
        auto min_rank = GetRank(min);
        auto max_rank = GetRank(max);
        return max_rank - min_rank;
    }

    void Validate() {
        Validate(root_, left_bound_, right_bound_);
    }

private:
    void AddToTree(Score score, MemberIterator mit) {
        Node* rebuild_node = nullptr;
        Node** rebuild_node_at = nullptr;

        Node* node = root_;
        Node** node_at = &root_;

        while (true) {
            node->IncrementApproximateSize();
            if (node->Count() && !rebuild_node) {
                rebuild_node = node;
                rebuild_node_at = node_at;
            }
            auto index = node->Search(score, *mit);
            if (node->IsFound(index, score, *mit)) {
                node->Unmark(index);
                node->Access(index);

                auto old_mit = node->GetMemberIterator(index);
                members_.erase(old_mit);
                node->SetMemberIterator(index, mit);

                break;
            }
            if (node->GetChild(index) == nullptr) {
                if (node->IsLeaf() && node->GetRepSize() < node->GetCapacity()) {
                    node->Insert(index, score, mit);
                } else {
                    NodeBuilder node_builder;
                    node_builder.SetLeftBound((index == 0) ? node->GetLeftBound() : node->GetScore(index - 1));
                    node_builder.SetRightBound((index == node->GetRepSize()) ? node->GetRightBound()
                                                                             : node->GetScore(index));
                    node_builder.SetCapacity(leaf_size_);
                    node_builder.SetRebuildBound(min_rebuild_bound_);

                    auto child = node_builder.Build();
                    child->Insert(0, score, mit);
                    child->IncrementApproximateSize();
                    child->Count();

                    node->GetChild(index) = child;
                    node->IncrementCount(2 * index);
                }
                break;
            }
            node->IncrementCount(2 * index);
            node_at = &node->GetChild(index);
            node = node->GetChild(index);
        }

        if (rebuild_node) {
            *rebuild_node_at = RebuildTree(rebuild_node);
        }
    }

    void DeleteFromTree(Score score, const Member& member) {
        Node* rebuild_node = nullptr;
        Node** rebuild_node_at = nullptr;

        Node* node = root_;
        Node** node_at = &root_;

        while (true) {
            if (node->Count() && !rebuild_node) {
                rebuild_node = node;
                rebuild_node_at = node_at;
            }
            auto index = node->Search(score, member);
            if (node->IsFound(index, score, member)) {
                node->Mark(index);
                node->Access(index);
                break;
            }
            node->DecrementCount(2 * index);
            node_at = &node->GetChild(index);
            node = node->GetChild(index);
        }

        if (rebuild_node) {
            *rebuild_node_at = RebuildTree(rebuild_node);
        }
    }

    size_t GetRank(Score score) {
        size_t rank = 0;

        Node* rebuild_node = nullptr;
        Node** rebuild_node_at = nullptr;

        Node* node = root_;
        Node** node_at = &root_;

        while (node) {
            if (node->Count() && !rebuild_node) {
                rebuild_node = node;
                rebuild_node_at = node_at;
            }
            auto index = node->Search(score);
            rank += node->GetPrefixCount(2 * index);
            node_at = &node->GetChild(index);
            node = node->GetChild(index);
        }

        if (rebuild_node) {
            *rebuild_node_at = RebuildTree(rebuild_node);
        }

        return rank;
    }

    template <typename OScoreIt, typename OMemberIt>
    size_t GetLeftRange(Node* node, Node** node_at, Score min, bool prev, OScoreIt& sit, OMemberIt& mit) {
        if (!node) {
            return 0;
        }

        size_t count = 0;
        auto left_index = node->Search(min);
        bool to_rebuild = node->Count(node->GetRepSize() - left_index) && !prev;

        if (left_index == node->GetRepSize() || min <= node->GetScore(left_index)) {
            count += GetLeftRange(node->GetChild(left_index), &node->GetChild(left_index), min, to_rebuild || prev, sit,
                                  mit);
        }

        for (int i = left_index; i < node->GetRepSize(); ++i) {
            CollectElement(node, i, sit, mit, count);
            count += CollectNonMarked(node->GetChild(i + 1), &node->GetChild(i + 1), to_rebuild || prev, sit, mit);
        }

        if (to_rebuild) {
            *node_at = RebuildTree(node);
        }

        return count;
    }

    template <typename OScoreIt, typename OMemberIt>
    size_t GetRightRange(Node* node, Node** node_at, Score max, bool prev, OScoreIt& sit, OMemberIt& mit) {
        if (!node) {
            return 0;
        }

        size_t count = 0;
        auto right_index = node->Search(max);
        bool to_rebuild = node->Count(right_index) && !prev;

        for (int i = 0; i < right_index; ++i) {
            count += CollectNonMarked(node->GetChild(i), &node->GetChild(i), to_rebuild || prev, sit, mit);
            CollectElement(node, i, sit, mit, count);
        }

        if (right_index == node->GetRepSize() || max <= node->GetScore(right_index)) {
            count += GetRightRange(node->GetChild(right_index), &node->GetChild(right_index), max, to_rebuild || prev,
                                   sit, mit);
        } else {
            count += CollectNonMarked(node->GetChild(right_index), &node->GetChild(right_index), to_rebuild || prev,
                                      sit, mit);
        }

        if (to_rebuild) {
            *node_at = RebuildTree(node);
        }

        return count;
    }

    template <typename OScoreIt, typename OMemberIt>
    size_t CollectNonMarked(Node* node, Node** node_at, bool prev, OScoreIt& sit, OMemberIt& mit) {
        if (!node) {
            return 0;
        }

        size_t count = 0;
        bool to_rebuild = node->Count(node->GetRepSize()) && !prev;

        count += CollectNonMarked(node->GetChild(0), &node->GetChild(0), to_rebuild || prev, sit, mit);

        for (int i = 0; i < node->GetRepSize(); ++i) {
            CollectElement(node, i, sit, mit, count);
            count += CollectNonMarked(node->GetChild(i + 1), &node->GetChild(i + 1), to_rebuild || prev, sit, mit);
        }

        if (to_rebuild) {
            *node_at = RebuildTree(node);
        }

        return count;
    }

    template <typename OScoreIt, typename OMemberIt>
    static inline void CollectElement(Node* node, int i, OScoreIt& sit, OMemberIt& mit, size_t& count) {
        node->Access(i);
        if (!node->IsMarked(i)) {
            *sit++ = node->GetScore(i);
            *mit++ = node->GetMember(i);
            ++count;
        }
    }

    Node* RebuildTree(Node* node) {
        const int asize = node->GetApproximateSize();

        Score* scores = new Score[asize];
        MemberData* member_data = new MemberData[asize];
        int at = 0;

        bool clear;
        if constexpr (CP == ClearPolicy::kNone) {
            clear = false;
        } else if constexpr (CP == ClearPolicy::kRapid) {
            clear = true;
        } else if constexpr (CP == ClearPolicy::kRoot) {
            clear = node == root_;
        }

        CollectAndClear(node, scores, member_data, at, clear);

        int64_t* pac = new int64_t[at + 1];
        pac[0] = 0;
        for (int index = 0; index < at; ++index) {
            pac[index + 1] = pac[index] + std::abs(member_data[index].accesses);
        }

        auto [result, _] = BuildIdealTree(scores, member_data, pac, 0, at, node->GetLeftBound(), node->GetRightBound());
        if (!result && root_ == node) {
            result = CreateRoot();
        }

        delete[] pac;
        delete[] member_data;
        delete[] scores;

        delete node;

        return result;
    }

    void CollectAndClear(Node* node, Score* scores, MemberData* member_data, int& at, bool clear) {
        assert(node);

        if (node->GetChild(0)) {
            CollectAndClear(node->GetChild(0), scores, member_data, at, clear);
        }

        for (int i = 0; i < node->GetRepSize(); ++i) {
            auto md = node->GetMemberData(i);
            if (clear && node->IsMarked(i)) {
                members_.erase(md.mit);
            } else {
                scores[at] = node->GetScore(i);
                member_data[at] = md;
                ++at;
            }
            if (node->GetChild(i + 1)) {
                CollectAndClear(node->GetChild(i + 1), scores, member_data, at, clear);
            }
        }
    }

    // [left, right)
    std::pair<Node*, int> BuildIdealTree(Score* scores, MemberData* member_data, int64_t* pac, int left, int right,
                                         Score left_bound, Score right_bound) {
        const int size = right - left;
        if (size <= 0) {
            return {nullptr, 0};
        }

        const int64_t total_accesses = pac[right] - pac[left];
        const int64_t rebuild_bound =
            std::max(min_rebuild_bound_, static_cast<int64_t>(rebuild_factor_ * total_accesses));

        int count = 0;

        NodeBuilder node_builder;
        node_builder.SetLeftBound(left_bound);
        node_builder.SetRightBound(right_bound);
        node_builder.SetApproximateSize(size);
        node_builder.SetRebuildBound(rebuild_bound);

        if (size <= leaf_size_) {
            node_builder.SetCapacity(leaf_size_);
            node_builder.SetRepSize(size);
            for (int i = 0; i < size; ++i) {
                node_builder.SetScore(i, scores[left + i]);
                node_builder.SetMemberData(i, member_data[left + i]);
                if (!node_builder.IsMarked(i)) {
                    node_builder.IncrementCount(2 * i + 1);
                    ++count;
                }
            }
            return {node_builder.Build(), count};
        }

        const int capacity = ceil(d_(total_accesses));
        const int child_accesses = ceil(total_accesses / (capacity + 1.0));

        node_builder.SetCapacity(capacity);

        int i = 0;
        while (i < capacity) {
            int from = left;
            int to = right;
            while (to - from > 1) {
                int m = (from + to) >> 1;
                if (pac[m] - pac[left] < child_accesses) {
                    from = m;
                } else {
                    to = m;
                }
            }

            node_builder.SetScore(i, scores[from]);
            node_builder.SetMemberData(i, member_data[from]);
            if (!node_builder.IsMarked(i)) {
                node_builder.IncrementCount(2 * i + 1);
                ++count;
            }

            auto [child, child_count] = BuildIdealTree(scores, member_data, pac, left, from, left_bound, scores[from]);
            node_builder.SetChild(i, child);
            node_builder.IncrementCount(2 * i, child_count);
            count += child_count;

            left_bound = scores[from];
            ++i;
            left = to;
            if (left == right) {
                break;
            }
        }
        auto [child, child_count] = BuildIdealTree(scores, member_data, pac, left, right, left_bound, right_bound);
        node_builder.SetChild(i, child);
        node_builder.IncrementCount(2 * i, child_count);
        count += child_count;

        node_builder.SetRepSize(i);

        return {node_builder.Build(total_accesses), count};
    }

    int Validate(Node* node, Score left_bound, Score right_bound) {
        if (!node) {
            return 0;
        }

        Check(node->GetLeftBound() == left_bound);
        Check(node->GetRightBound() == right_bound);

        int count = 0;

        for (int i = 0; i <= node->GetRepSize(); ++i) {
            const Score left_score = (i == 0) ? left_bound : node->GetScore(i - 1);
            const Score right_score = (i == node->GetRepSize()) ? right_bound : node->GetScore(i);
            Check(left_score <= right_score);

            if (i < node->GetRepSize() && !node->IsMarked(i)) {
                ++count;
            }

            count += Validate(node->GetChild(i), left_score, right_score);
            Check(node->GetPrefixCount(std::min(2 * (i + 1), 2 * node->GetRepSize() + 1)) == count);
        }

        return count;
    }

    Node* CreateRoot() const {
        NodeBuilder node_builder;
        node_builder.SetLeftBound(left_bound_);
        node_builder.SetRightBound(right_bound_);
        node_builder.SetCapacity(leaf_size_);
        node_builder.SetRebuildBound(min_rebuild_bound_);
        node_builder.SetRepSize(0);
        node_builder.SetApproximateSize(0);
        return node_builder.Build();
    }

private:
    D d_;
    const Score left_bound_;
    const Score right_bound_;
    const Score no_score_;
    const int leaf_size_;
    const int64_t min_rebuild_bound_;
    const double rebuild_factor_;

    std::list<Member> members_;
    Node* root_;
    DICT dict_;
};
