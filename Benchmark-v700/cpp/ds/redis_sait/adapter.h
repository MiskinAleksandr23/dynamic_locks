#pragma once

#include <iostream>
#include <csignal>
#include <bits/stdc++.h>
using namespace std;

#include "errors.h"
#include "record_manager.h"
#include "sait.h"

// PARAMETERS BEGIN
constexpr ClearPolicy CP = ClearPolicy::kRoot;
constexpr int LEAF_SIZE = 4;
constexpr int64_t MIN_REBUILD_BOUND = 225 /* 125 */;
constexpr double REBUILD_FACTOR = 0.75 /* 0.25 */;
// PARAMETERS END

#define DATA_STRUCTURE_T Sait<K, V, CP>

#define REDIS

template <typename K, typename V, class Reclaim = reclaimer_debra<K>, class Alloc = allocator_new<K>, class Pool = pool_none<K>>
class ds_adapter {
private:
    const V NO_VALUE;
    DATA_STRUCTURE_T * const ds;

public:
    ds_adapter(const int NUM_THREADS,
               const K& KEY_MIN,
               const K& KEY_MAX,
               const V& VALUE_RESERVED,
               Random64 * const unused2)
            : NO_VALUE(VALUE_RESERVED)
            , ds(new DATA_STRUCTURE_T(KEY_MIN, KEY_MAX + 1, NO_VALUE, LEAF_SIZE, MIN_REBUILD_BOUND, REBUILD_FACTOR))
    { }

    ~ds_adapter() {
        delete ds;
    }

    V getNoValue() {
        return NO_VALUE;
    }

    void initThread(const int tid) {
        // ds->initThread(tid);
    }
    
    void deinitThread(const int tid) {
       // ds->deinitThread(tid);
    }

    void warmupEnd() {
    }

    V insert(const int tid, const K& key, const V& val) {
        setbench_error("not implemented");
    }

    V insertIfAbsent(const int tid, const K& key, const V& val) {
        return ds->InsertIfAbsent(val, key);
    }

    V erase(const int tid, const K& key) {
        return ds->Delete(key);
    }

    V find(const int tid, const K& key) {
        setbench_error("not implemented");
    }

    bool contains(const int tid, const K& key) {
        return ds->Count(key, key + 5);
    }

    int rangeQuery(const int tid, const K& lo, const K& hi, K * const resultKeys, V * const resultValues) {
        return ds->GetRange(lo, hi, resultKeys, resultValues);
    }

    void printSummary() {
       // ds->printDebuggingDetails();
       // ds->print_inner_structure();
    }

    bool validateStructure() {
        try {
            ds->Validate();
            return true;
        } catch(const std::runtime_error& e) {
            std::cout << "ERROR WHILE VALIDATING: " << e.what() << '\n';
            return false;
        }
    }

    void printObjectSizes() {
        // std::cout<< "sizes: node=" << (sizeof(typename DATA_STRUCTURE_T::Node)) << std::endl;
    }
};
