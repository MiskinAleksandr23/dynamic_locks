/* Zskiplist implementation based on Redis 7.0
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <random>

template <typename Score, typename Member, typename ScoreCompare, typename MemberCompare>
class Zskiplist {
public:
    class Node;

    struct Level {
        Node* forward;
        uint64_t span;
    };

    class Node {
    public:
        Node() = delete;

        static Node* Create(size_t levels_count, Score score, const Member& member) {
            Node* node = Create(levels_count, score);
            ::new (&node->member_) Member(member);
            // std::construct_at(&node->member_, member);
            return node;
        }

        static Node* Create(size_t levels_count, Score score, Member&& member) {
            Node* node = Create(levels_count, score);
            ::new (&node->member_) Member(std::move(member));
            // std::construct_at(&node->member_, std::move(member));
            return node;
        }

        static void Free(Node* node) {
            std::destroy_at(&node->member_);
            std::destroy_at(&node->score_);
            free(node);
        }

        Score GetScore() const {
            return score_;
        }

        const Member& GetMember() const {
            return member_;
        }

        const Level* GetLevel() const {
            return level_;
        }

    private:
        static Node* Create(size_t levels_count, Score score) {
            auto node = static_cast<Node*>(malloc(sizeof(Node) + levels_count * sizeof(Level)));
            ::new (&node->score_) Score(score);
            // std::construct_at(&node->score_, score);
            node->level_ = reinterpret_cast<Level*>(node + 1);
            return node;
        }

    private:
        Score score_;
        Member member_;

        Node* backward_;
        Level* level_;

        friend Zskiplist;
    };

public:
    static constexpr int kMaxLevel = 32;
    static constexpr double kProb = 0.25;

public:
    Zskiplist(ScoreCompare sc, MemberCompare mc, uint32_t seed = 547748385u)
        : header_(nullptr),
          tail_(nullptr),
          length_(0),
          level_(1),
          sc_(sc),
          mc_(mc),
          gen_(seed) {
        header_ = Node::Create(kMaxLevel, Score(), Member());
        for (int j = 0; j < kMaxLevel; ++j) {
            header_->level_[j].forward = nullptr;
            header_->level_[j].span = 0;
        }
        header_->backward_ = nullptr;
        tail_ = nullptr;
    }

    Zskiplist()
        : Zskiplist(ScoreCompare(), MemberCompare()) {
    }

    ~Zskiplist() {
        Node* node = header_;
        while (node) {
            Node* next = node->level_[0].forward;
            Node::Free(node);
            node = next;
        }
    }

    /* Insert a new node in the skiplist. Assumes the element does not already
     * exist (up to the caller to enforce that). */
    Node* Insert(Score score, const Member& member) {
        Node* update[kMaxLevel];
        uint64_t rank[kMaxLevel];
        auto level = PreInsert(score, member, update, rank);
        auto x = Node::Create(level, score, member);
        PostInsert(x, level, update, rank);
        return x;
    }

    Node* Insert(Score score, Member&& member) {
        Node* update[kMaxLevel];
        uint64_t rank[kMaxLevel];
        auto level = PreInsert(score, member, update, rank);
        auto x = Node::Create(level, score, std::move(member));
        PostInsert(x, level, update, rank);
        return x;
    }

    /* Delete an element with matching value.
     *
     * The function returns 'true' if the node was found and deleted, otherwise 'false' is returned.
     *
     * If 'node' is NULL the deleted node is freed, otherwise it is not freed (but just unlinked) and
     * *node is set to the node pointer, so that it is possible for the caller to reuse the node. */
    bool Delete(Score score, const Member& member, Node** node) {
        Node* update[kMaxLevel];
        Node* x = header_;
        for (int i = level_ - 1; i >= 0; --i) {
            while (x->level_[i].forward &&
                   IsLess(x->level_[i].forward->score_, x->level_[i].forward->member_, score, member)) {
                x = x->level_[i].forward;
            }
            update[i] = x;
        }
        x = x->level_[0].forward;

        if (x && AreScoresEqual(score, x->score_) && AreMembersEqual(member, x->member_)) {
            DeleteNode(x, update);
            if (!node) {
                Node::Free(x);
            } else {
                *node = x;
            }
            return true;
        }
        return false;
    }

    /* Update the score of an element inside the sorted set skiplist.
     * Note that the element must exist and must match 'score'.
     *
     * Note that this function attempts to just update the node, in case after
     * the score update, the node would be exactly at the same position.
     * Otherwise the skiplist is modified by removing and re-adding a new
     * element, which is more costly.
     *
     * The function returns the updated element skiplist node pointer. */
    Node* UpdateScore(Score cur_score, const Member& member, Score new_score) {
        Node* update[kMaxLevel];
        Node* x = header_;
        for (int i = level_ - 1; i >= 0; --i) {
            while (x->level_[i].forward &&
                   IsLess(x->level_[i].forward->score_, x->level_[i].forward->member_, cur_score, member)) {
                x = x->level_[i].forward;
            }
            update[i] = x;
        }
        x = x->level_[0].forward;

        assert(x && AreScoresEqual(cur_score, x->score_) && AreMembersEqual(x->member_, member));

        if ((x->backward_ == nullptr || sc_(x->backward_->score_, new_score)) &&
            (x->level_[0].forward == nullptr || sc_(new_score, x->level_[0].forward->score_))) {
            x->score_ = new_score;
            return x;
        }

        DeleteNode(x, update);
        Node* new_node = Insert(new_score, std::move(x->member_));
        Node::Free(x);
        return new_node;
    }

    // [min, max)
    bool IsInRange(Score min, Score max) const {
        if (!sc_(min, max)) {
            return false;
        }

        Node* x = tail_;
        if (x == nullptr || sc_(x->score_, min)) {
            return false;
        }

        x = header_->level_[0].forward;
        if (x == nullptr || !sc_(x->score_, max)) {
            return false;
        }

        return true;
    }

    /* Find the first node that is contained in the [min, max) range.
     * Returns NULL when no element is contained in the range. */
    Node* GetFirstInRange(Score min, Score max) const {
        if (!IsInRange(min, max)) {
            return nullptr;
        }

        Node* x = header_;
        for (int i = level_ - 1; i >= 0; --i) {
            while (x->level_[i].forward && sc_(x->level_[i].forward->score_, min)) {
                x = x->level_[i].forward;
            }
        }
        x = x->level_[0].forward;

        assert(x != nullptr);

        if (!sc_(x->score_, max)) {
            return nullptr;
        }

        return x;
    }

    /* Find the last node that is contained in the [min, max) range.
     * Returns NULL when no element is contained in the range. */
    Node* GetLastInRange(Score min, Score max) const {
        if (!IsInRange(min, max)) {
            return nullptr;
        }

        Node* x = header_;
        for (int i = level_ - 1; i >= 0; --i) {
            while (x->level_[i].forward && sc_(x->level_[i].forward->score_, max)) {
                x = x->level_[i].forward;
            }
        }

        assert(x != nullptr);

        if (sc_(x->score_, min)) {
            return nullptr;
        }

        return x;
    }

    /* Find the rank for an element by both score and member.
     * Returns 0 when the element cannot be found, rank otherwise.
     * Note that the rank is 1-based due to the span of header to the first element. */
    uint64_t GetRank(Score score, const Member& member) {
        uint64_t rank = 0;
        Node* x = header_;
        for (int i = level_ - 1; i >= 0; --i) {
            while (x->level_[i].forward &&
                   IsLessOrEqual(x->level_[i].forward->score_, x->level_[i].forward->member_, score, member)) {
                rank += x->level_[i].span;
                x = x->level_[i].forward;
            }

            if (x != header_ && AreScoresEqual(x->score_, score) && AreMembersEqual(x->member_, member)) {
                return rank;
            }
        }
        return 0;
    }

    uint64_t GetLength() const {
        return length_;
    }

private:
    int PreInsert(Score score, const Member& member, Node** update, uint64_t* rank) {
        Node* x = header_;

        for (int i = level_ - 1; i >= 0; --i) {
            rank[i] = i == (level_ - 1) ? 0 : rank[i + 1];
            while (x->level_[i].forward &&
                   IsLess(x->level_[i].forward->score_, x->level_[i].forward->member_, score, member)) {
                rank[i] += x->level_[i].span;
                x = x->level_[i].forward;
            }
            update[i] = x;
        }

        int level = GetRandomLevel();
        if (level > level_) {
            for (int i = level_; i < level; ++i) {
                rank[i] = 0;
                update[i] = header_;
                update[i]->level_[i].span = length_;
            }
            level_ = level;
        }

        return level;
    }

    void PostInsert(Node* x, int level, Node** update, uint64_t* rank) {
        for (int i = 0; i < level; ++i) {
            x->level_[i].forward = update[i]->level_[i].forward;
            update[i]->level_[i].forward = x;

            x->level_[i].span = update[i]->level_[i].span - (rank[0] - rank[i]);
            update[i]->level_[i].span = (rank[0] - rank[i]) + 1;
        }

        for (int i = level; i < level_; ++i) {
            ++update[i]->level_[i].span;
        }

        x->backward_ = (update[0] == header_) ? nullptr : update[0];

        if (x->level_[0].forward) {
            x->level_[0].forward->backward_ = x;
        } else {
            tail_ = x;
        }
        ++length_;
    }

    inline bool IsLess(Score score, const Member& member, Score other_score, const Member& other_member) const {
        return sc_(score, other_score) || (!sc_(other_score, score) && mc_(member, other_member));
    }

    inline bool IsLessOrEqual(Score score, const Member& member, Score other_score, const Member& other_member) const {
        return sc_(score, other_score) || (!sc_(other_score, score) && (mc_(member, other_member) || !mc_(other_member, member)));
    }

    inline bool AreScoresEqual(Score score, Score other_score) const {
        return !sc_(score, other_score) && !sc_(other_score, score);
    }

    inline bool AreMembersEqual(const Member& member, const Member& other_member) const {
        return !mc_(member, other_member) && !mc_(other_member, member);
    }

    int GetRandomLevel() {
        int level = 1;
        while (dist_(gen_) < kProb) {
            ++level;
        }
        return std::min(level, kMaxLevel);
    }

    void DeleteNode(Node* x, Node** update) {
        for (int i = 0; i < level_; ++i) {
            if (update[i]->level_[i].forward == x) {
                update[i]->level_[i].span += x->level_[i].span - 1;
                update[i]->level_[i].forward = x->level_[i].forward;
            } else {
                --update[i]->level_[i].span;
            }
        }

        if (x->level_[0].forward) {
            x->level_[0].forward->backward_ = x->backward_;
        } else {
            tail_ = x->backward_;
        }

        while (level_ > 1 && header_->level_[level_ - 1].forward == nullptr) {
            --level_;
        }
        --length_;
    }

private:
    Node* header_;
    Node* tail_;
    uint64_t length_;
    int level_;

    ScoreCompare sc_;
    MemberCompare mc_;
    std::mt19937 gen_;
    std::uniform_real_distribution<double> dist_;
};
