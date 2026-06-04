#pragma once

#include "../../../../common/naive_lock.hpp"
#include "../../../../dynamic/dynamic_lock.hpp"
#include "../../../../genetic/genetic_lock.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <type_traits>
#include <vector>

namespace adaptive_lock_benchmark {

constexpr size_t kLockCount = 64;
constexpr size_t kBlocks = 1024;
constexpr auto kDynamicRebuildInterval = std::chrono::milliseconds(500);
constexpr double kDynamicRebuildThreshold = 2.0;
constexpr size_t kStatsSampleRate = 64;
constexpr double kMinSkew = 4.0;
constexpr size_t kGeneticProbeGap = 1000000;

constexpr size_t kGeneticTrainingBatchSize = 100000;
constexpr size_t kGeneticHistoryLimit = 12000;
constexpr size_t kGeneticPopulationSize = 24;
constexpr size_t kGeneticEliteCount = 4;
constexpr size_t kGeneticGenerationCount = 0;
constexpr uint64_t kGeneticSeed = 500;

struct NaiveTag {};
struct DynamicTag {};
struct GeneticTag {};

template <typename LockKind>
struct LockTraits;

template <>
struct LockTraits<NaiveTag> {
    using Lock = NaiveLock<kLockCount>;

    static std::unique_ptr<Lock> Create(size_t array_size) {
        return std::make_unique<Lock>(array_size);
    }

    static const char* Name() {
        return "adaptive_lock_naive";
    }
};

template <>
struct LockTraits<DynamicTag> {
    using Lock = DynamicLock<kLockCount>;

    static std::unique_ptr<Lock> Create(size_t array_size) {
        return std::make_unique<Lock>(array_size, kStatsSampleRate, 1.0, kMinSkew);
    }

    static const char* Name() {
        return "adaptive_lock_dynamic";
    }
};

template <>
struct LockTraits<GeneticTag> {
    using Lock = GeneticLock<kLockCount, kBlocks>;

    static std::unique_ptr<Lock> Create(size_t array_size) {
        return std::make_unique<Lock>(array_size, kGeneticSeed, kGeneticTrainingBatchSize,
                                      kGeneticHistoryLimit, kGeneticPopulationSize,
                                      kGeneticEliteCount, kGeneticGenerationCount,
                                      kStatsSampleRate, kMinSkew, kGeneticProbeGap);
    }

    static const char* Name() {
        return "adaptive_lock_genetic";
    }
};

template <typename K, typename V, typename LockKind>
class AdapterImpl {
public:
    AdapterImpl(int num_threads, const K& key_min, const K& key_max, const V& no_value,
                Random64* const)
        : no_value_(no_value),
          key_min_(key_min),
          key_max_(key_max),
          array_size_(ComputeArraySize(key_min, key_max)),
          data_(array_size_, 0),
          present_(array_size_, 0),
          stored_keys_(array_size_, key_min),
          thread_checksums_(std::max(num_threads, 1), 0),
          lock_(LockTraits<LockKind>::Create(array_size_)) {
        lock_->StartRebuilder(kDynamicRebuildInterval, kDynamicRebuildThreshold);
    }

    ~AdapterImpl() {
        lock_->StopRebuilder();
    }

    V getNoValue() {
        return no_value_;
    }

    void initThread(int) {
    }

    void deinitThread(int) {
    }

    void warmupEnd() {
        // Keep online adaptation running; V700 test timing should include it.
    }

    void testEnd() {
        lock_->StopRebuilder();
    }

    V insert(const int, const K& key, const V&) {
        const size_t index = ToIndex(key);
        V previous = no_value_;
        lock_->WriteQuery(index, index, [&](size_t, size_t) {
            if (present_[index]) {
                previous = ValueForIndex(index);
            }
            present_[index] = 1;
            stored_keys_[index] = key;
            data_[index] = static_cast<uint64_t>(key);
        });
        return previous;
    }

    V insertIfAbsent(const int, const K& key, const V&) {
        const size_t index = ToIndex(key);
        V previous = no_value_;
        lock_->WriteQuery(index, index, [&](size_t, size_t) {
            if (present_[index]) {
                previous = ValueForIndex(index);
                return;
            }
            present_[index] = 1;
            stored_keys_[index] = key;
            data_[index] = static_cast<uint64_t>(key);
        });
        return previous;
    }

    V erase(const int, const K& key) {
        const size_t index = ToIndex(key);
        V previous = no_value_;
        lock_->WriteQuery(index, index, [&](size_t, size_t) {
            if (!present_[index]) {
                return;
            }
            previous = ValueForIndex(index);
            present_[index] = 0;
            data_[index] = 0;
        });
        return previous;
    }

    V find(const int, const K& key) {
        const size_t index = ToIndex(key);
        V value = no_value_;
        lock_->WriteQuery(index, index, [&](size_t, size_t) {
            if (present_[index]) {
                value = ValueForIndex(index);
            }
        });
        return value;
    }

    bool contains(const int, const K& key) {
        const size_t index = ToIndex(key);
        bool value = false;
        lock_->WriteQuery(index, index, [&](size_t, size_t) { value = present_[index] != 0; });
        return value;
    }

    int rangeQuery(const int tid, const K& lo, const K& hi, K* const, V* const) {
        size_t left = ToIndex(lo);
        size_t right = ToIndex(hi);
        if (left > right) {
            std::swap(left, right);
        }

        uint64_t local_checksum = 0;
        lock_->WriteQuery(left, right, [&](size_t locked_left, size_t locked_right) {
            const size_t update_index =
                locked_left + (range_queries_.fetch_add(1, std::memory_order_relaxed) %
                               (locked_right - locked_left + 1));
            data_[update_index] += 1;
            local_checksum = data_[locked_left] ^ (data_[locked_right] << 1) ^ data_[update_index];
        });

        if (tid >= 0 && static_cast<size_t>(tid) < thread_checksums_.size()) {
            thread_checksums_[tid] += local_checksum;
        }

        return 0;
    }

    void printSummary() {
        uint64_t checksum = 0;
        for (uint64_t value : thread_checksums_) {
            checksum ^= value;
        }

        std::cout << LockTraits<LockKind>::Name() << "_range_queries=" << lock_->GetOperationCount()
                  << std::endl;
        std::cout << LockTraits<LockKind>::Name()
                  << "_total_lock_ms=" << lock_->GetTotalLockTimeMs() << std::endl;
        std::cout << LockTraits<LockKind>::Name() << "_avg_lock_ms=" << lock_->GetAvgLockTimeMs()
                  << std::endl;
        std::cout << LockTraits<LockKind>::Name()
                  << "_reconfigurations=" << lock_->GetRebuildCount() << std::endl;
        std::cout << LockTraits<LockKind>::Name() << "_checksum=" << checksum << std::endl;
    }

    bool validateStructure() {
        return true;
    }

    void printObjectSizes() {
        std::cout << LockTraits<LockKind>::Name() << "_object_bytes=" << sizeof(*this) << std::endl;
    }

    void debugGCSingleThreaded() {
    }

private:
    static size_t ComputeArraySize(const K& key_min, const K& key_max) {
        if (key_max <= key_min) {
            return 1;
        }
        return static_cast<size_t>(key_max - key_min + 1);
    }

    size_t ToIndex(const K& key) const {
        if (key <= key_min_) {
            return 0;
        }
        if (key >= key_max_) {
            return array_size_ - 1;
        }
        return static_cast<size_t>(key - key_min_);
    }

    V ValueForIndex(size_t index) const {
        if constexpr (std::is_pointer_v<V>) {
            return reinterpret_cast<V>(const_cast<K*>(&stored_keys_[index]));
        } else {
            return static_cast<V>(stored_keys_[index]);
        }
    }

    const V no_value_;
    const K key_min_;
    const K key_max_;
    const size_t array_size_;
    std::vector<uint64_t> data_;
    std::vector<uint8_t> present_;
    std::vector<K> stored_keys_;
    std::vector<uint64_t> thread_checksums_;
    std::unique_ptr<typename LockTraits<LockKind>::Lock> lock_;
    std::atomic<uint64_t> range_queries_{0};
};

}
