/* Hash Tables Implementation based on Redis 7.0
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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
#include <assert.h>
#include <algorithm>
#include <limits>
#include <optional>

template <typename Key, typename Value, typename Hash, typename KeyEqual, typename KeyDeleter, typename ValueDeleter>
class Dict {
public:
    class Entry {
    public:
        const Key& GetKey() const {
            return key_;
        }

        Value& GetValue() {
            return value_;
        }

    private:
        Key key_;
        Value value_;
        Entry* next_;

        friend Dict;
    };

private:
    static constexpr int8_t kHtInitialExp = 2;
    static constexpr uint64_t kHtInitialSize = 1 << kHtInitialExp;

    static constexpr int kRehashMultiplier = 10;

    static constexpr int kHtMinFill = 10;
    static constexpr int kHtResizeFactor = 100;

public:
    Dict(Hash hash, KeyEqual key_equal, KeyDeleter key_deleter, ValueDeleter value_deleter)
        : rehash_idx_(-1),
          hash_(hash),
          key_equal_(key_equal),
          key_deleter_(key_deleter),
          value_deleter_(value_deleter) {
        ResetHt(0);
        ResetHt(1);
    }

    Dict()
        : Dict(Hash(), KeyEqual(), KeyDeleter(), ValueDeleter()) {
    }

    ~Dict() {
        ClearHt(0);
        ClearHt(1);
    }

    // Return 'true' if the key was added or 'false' if the key already exists
    bool Add(const Key& key, const Value& value) {
        Entry* he = AddRaw(key, nullptr);
        if (!he) {
            return false;
        }
        he->value_ = value;
        return true;
    }

    /* Add or Overwrite:
     * Add an element, discarding the old value if the key already exists.
     * Return 'true' if the key was added from scratch, 'false' if there was already an
     * element with such key and dictReplace() just performed a value update operation. */
    bool Replace(const Key& key, const Value& value) {
        Entry* existing;
        Entry* he = AddRaw(key, &existing);

        if (he) {
            he->value_ = value;
            return true;
        }

        value_deleter_(existing->value_);
        existing->value_ = value;
        return false;
    }

    /* Low level add or find:
     * This function adds the entry but instead of setting a value returns the
     * Entry structure to the user, that will make sure to fill the value field as they wish.
     *
     * Return values:
     *
     * If key already exists NULL is returned, and "*existing" is populated
     * with the existing entry if existing is not NULL.
     *
     * If key was added, the hash entry is returned to be manipulated by the caller.
     */
    Entry* AddRaw(const Key& key, Entry** existing) {
        if (IsRehashing()) {
            Rehash();
        }

        size_t hash = hash_(key);
        auto index = KeyIndex(key, hash, existing);
        if (index == -1) {
            return nullptr;
        }

        uint64_t ht_idx = IsRehashing() ? 1 : 0;
        Entry* he = new Entry();
        he->next_ = ht_table_[ht_idx][index];
        ht_table_[ht_idx][index] = he;
        ht_used_[ht_idx]++;
        he->key_ = key;
        return he;
    }

    // Resize the table to the minimal size that contains all the elements
    bool Resize() {
        if (IsRehashing()) {
            return false;
        }
        return Expand(std::max(ht_used_[0], kHtInitialSize));
    }

    bool IsNeedResize() const {
        uint64_t size = GetSlots();
        uint64_t used = GetSize();
        return (size > kHtInitialSize && (used * kHtResizeFactor / size) < kHtMinFill);
    }

    /* Remove an element from the table, but without actually releasing
     * the key, value and dictionary entry. The dictionary entry is returned
     * if the element was found (and unlinked from the table), and the user
     * should later call `dictFreeUnlinkedEntry()` with it in order to release it.
     * Otherwise if the key is not found, NULL is returned. */
    Entry* Unlink(const Key& key) {
        return GenericDelete(key, true);
    }

    // Remove an element, returning 'true' on success or 'false' if the element was not found.
    bool Delete(const Key& key) {
        return GenericDelete(key, false);
    }

    std::optional<Value> Get(const Key& key) {
        auto he = Find(key);
        if (he) {
            return {he->GetValue()};
        } else {
            return std::nullopt;
        }
    }

    Entry* Find(const Key& key) {
        if (GetSize() == 0) {
            return nullptr;
        }

        if (IsRehashing()) {
            Rehash();
        }

        size_t hash = hash_(key);
        for (uint64_t table = 0; table <= 1; table++) {
            uint64_t idx = hash & CalculateSizeMask(ht_size_exp_[table]);
            Entry* he = ht_table_[table][idx];
            while (he) {
                if (key_equal_(key, he->key_)) {
                    return he;
                }
                he = he->next_;
            }
            if (!IsRehashing()) {
                return nullptr;
            }
        }
        return nullptr;
    }

    void FreeUnlinkedEntry(Entry* he) {
        key_deleter_(he->key_);
        value_deleter_(he->value_);
        delete he;
    }

private:
    static uint64_t CalculateSize(int8_t exp) {
        return exp == -1 ? 0 : (1ul << exp);
    }

    static uint64_t CalculateSizeMask(int8_t exp) {
        return exp == -1 ? 0 : (CalculateSize(exp) - 1);
    }

    uint64_t GetSlots() const {
        return CalculateSize(ht_size_exp_[0]) + CalculateSize(ht_size_exp_[1]);
    }

    uint64_t GetSize() const {
        return ht_used_[0] + ht_used_[1];
    }

    bool IsRehashing() const {
        return rehash_idx_ != -1;
    }

    /* Performs steps of incremental rehashing. Returns 'true' if there are still
     *  keys to move from the old to the new hash table, otherwise 'false' is returned. */
    bool Rehash(int steps = 1) {
        int empty_visits = steps * kRehashMultiplier;

        while (static_cast<bool>(steps--) && ht_used_[0] != 0) {
            assert(CalculateSize(ht_size_exp_[0]) > static_cast<uint64_t>(rehash_idx_));

            while (ht_table_[0][rehash_idx_] == nullptr) {
                rehash_idx_++;
                if (--empty_visits == 0) {
                    return true;
                }
            }

            Entry* he = ht_table_[0][rehash_idx_];

            while (he) {
                Entry* next_he = he->next_;
                size_t hash = hash_(he->key_) & CalculateSizeMask(ht_size_exp_[1]);
                he->next_ = ht_table_[1][hash];
                ht_table_[1][hash] = he;
                ht_used_[0]--;
                ht_used_[1]++;
                he = next_he;
            }
            ht_table_[0][rehash_idx_] = nullptr;
            rehash_idx_++;
        }

        if (ht_used_[0] == 0) {
            delete[] ht_table_[0];
            ht_table_[0] = ht_table_[1];
            ht_used_[0] = ht_used_[1];
            ht_size_exp_[0] = ht_size_exp_[1];
            ResetHt(1);
            rehash_idx_ = -1;
            return false;
        }

        return true;
    }

    bool ExpandIfNeeded() {
        if (IsRehashing()) {
            return true;
        } else if (CalculateSize(ht_size_exp_[0]) == 0) {
            return Expand(kHtInitialSize);
        } else if (ht_used_[0] >= CalculateSize(ht_size_exp_[0])) {
            return Expand(ht_used_[0] + 1);
        } else {
            return true;
        }
    }

    // Expand or create the hash table. Returns 'true' if expand was performed, and 'false' if skipped.
    bool Expand(uint64_t size) {
        if (IsRehashing() || ht_used_[0] > size) {
            return false;
        }

        int8_t new_ht_size_exp = NextExp(size);
        uint64_t new_size = 1ul << new_ht_size_exp;

        if (new_size < size || new_ht_size_exp == ht_size_exp_[0]) {
            return false;
        }

        Entry** new_ht_table = new Entry*[new_size];
        std::fill(new_ht_table, new_ht_table + new_size, nullptr);

        uint64_t new_ht_used = 0;

        if (ht_table_[0] == nullptr) {
            ht_size_exp_[0] = new_ht_size_exp;
            ht_used_[0] = new_ht_used;
            ht_table_[0] = new_ht_table;
        } else {
            ht_size_exp_[1] = new_ht_size_exp;
            ht_used_[1] = new_ht_used;
            ht_table_[1] = new_ht_table;
            rehash_idx_ = 0;
        }

        return true;
    }

    int8_t NextExp(uint64_t size) {
        int8_t exp = kHtInitialExp;

        if (size >= std::numeric_limits<int64_t>::max()) {
            return (8 * sizeof(int64_t) - 1);
        }

        while ((1ul << exp) < size) {
            ++exp;
        }
        return exp;
    }

    /* Returns the index of a free slot that can be populated with
     * a hash entry for the given 'key'.
     * If the key already exists, -1 is returned
     * and the optional output parameter may be filled.
     *
     * Note that if we are in the process of rehashing the hash table, the
     * index is always returned in the context of the second (new) hash table. */
    int64_t KeyIndex(const Key& key, size_t hash, Entry** existing) {
        if (existing) {
            *existing = nullptr;
        }

        if (!ExpandIfNeeded()) {
            return -1;
        }

        uint64_t idx;
        for (uint64_t table = 0; table <= 1; table++) {
            idx = hash & CalculateSizeMask(ht_size_exp_[table]);
            Entry* he = ht_table_[table][idx];
            while (he) {
                if (key_equal_(key, he->key_)) {
                    if (existing) {
                        *existing = he;
                    }
                    return -1;
                }
                he = he->next_;
            }
            if (!IsRehashing()) {
                break;
            }
        }
        return idx;
    }

    Entry* GenericDelete(const Key& key, bool no_free) {
        if (GetSize() == 0) {
            return nullptr;
        }

        if (IsRehashing()) {
            Rehash();
        }

        size_t hash = hash_(key);
        for (uint64_t table = 0; table <= 1; table++) {
            uint64_t idx = hash & CalculateSizeMask(ht_size_exp_[table]);
            Entry* he = ht_table_[table][idx];
            Entry* prev_he = nullptr;
            while (he) {
                if (key_equal_(key, he->key_)) {
                    if (prev_he) {
                        prev_he->next_ = he->next_;
                    } else {
                        ht_table_[table][idx] = he->next_;
                    }
                    if (!no_free) {
                        FreeUnlinkedEntry(he);
                    }
                    ht_used_[table]--;
                    return he;
                }
                prev_he = he;
                he = he->next_;
            }
            if (!IsRehashing()) {
                break;
            }
        }
        return nullptr;
    }

    void ResetHt(int ht_idx) {
        ht_table_[ht_idx] = nullptr;
        ht_size_exp_[ht_idx] = -1;
        ht_used_[ht_idx] = 0;
    }

    void ClearHt(uint64_t ht_idx) {
        for (uint64_t i = 0; i < CalculateSize(ht_size_exp_[ht_idx]) && ht_used_[ht_idx] > 0; i++) {
            Entry* he = ht_table_[ht_idx][i];
            if (he == nullptr) {
                continue;
            }
            while (he) {
                Entry* next_he = he->next_;
                FreeUnlinkedEntry(he);
                --ht_used_[ht_idx];
                he = next_he;
            }
        }
        delete[] ht_table_[ht_idx];
        ResetHt(ht_idx);
    }

private:
    Entry** ht_table_[2];
    uint64_t ht_used_[2];
    int64_t rehash_idx_;
    int8_t ht_size_exp_[2];

    Hash hash_;
    KeyEqual key_equal_;
    KeyDeleter key_deleter_;
    ValueDeleter value_deleter_;
};
