/* Zset Implementation based on Redis 7.0
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

#include "zskiplist.h"
#include "../../common/dict.h"
#include "../../common/dict_helpers.h"

#include <functional>

// Suppose that 'Score' is light-weight copyable type and 'Member' is a high-weight type
template <typename Score, typename Member, typename ScoreCompare = std::less<Score>,
          typename MemberCompare = std::less<Member>, typename MemberHash = std::hash<Member>>
class Zset {
    using ZSKIPLIST = Zskiplist<Score, Member, ScoreCompare, MemberCompare>;

    using DICT = Dict<const Member*, Score, PtrMemberHash<Member, MemberHash>, PtrMemberEqual<Member, MemberCompare>,
                      DummyDeleter, DummyDeleter>;

public:
    Zset(ScoreCompare sc, MemberCompare mc, MemberHash mh, Score no_score)
        : zsl_(sc, mc),
          dict_(mh, mc, DummyDeleter(), DummyDeleter()),
          sc_(sc),
          no_score_(no_score) {
    }

    explicit Zset(Score no_score)
        : Zset(ScoreCompare(), MemberCompare(), MemberHash(), no_score) {
    }

    Score InsertIfAbsent(const Member& member, Score score) {
        auto de = dict_.Find(&member);
        if (de) {
            return de->GetValue();
        } else {
            auto znode = zsl_.Insert(score, member);
            auto added = dict_.Add(&znode->GetMember(), znode->GetScore());
            assert(added);
            return no_score_;
        }
    }

    Score Delete(const Member& member) {
        auto de = dict_.Unlink(&member);
        if (!de) {
            return no_score_;
        }

        Score score = de->GetValue();
        dict_.FreeUnlinkedEntry(de);
        bool deleted = zsl_.Delete(score, member, nullptr);
        assert(deleted);

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
        auto znode = zsl_.GetFirstInRange(min, max);
        size_t size = 0;
        while (znode && sc_(znode->GetScore(), max)) {
            ++size;
            *sit++ = znode->GetScore();
            *mit++ = znode->GetMember();
            znode = znode->GetLevel()[0].forward;
        }
        return size;
    }

    // [min, max)
    size_t Count(Score min, Score max) {
        size_t count = 0;
        size_t length = zsl_.GetLength();
        auto znode = zsl_.GetFirstInRange(min, max);
        if (znode) {
            auto rank = zsl_.GetRank(znode->GetScore(), znode->GetMember());
            count = (length - (rank - 1));

            znode = zsl_.GetLastInRange(min, max);
            if (znode) {
                rank = zsl_.GetRank(znode->GetScore(), znode->GetMember());
                count -= (length - rank);
            }
        }
        return count;
    }

    void Validate() const {
        // dummy
    }

private:
    ZSKIPLIST zsl_;
    DICT dict_;
    ScoreCompare sc_;
    const Score no_score_;
};
