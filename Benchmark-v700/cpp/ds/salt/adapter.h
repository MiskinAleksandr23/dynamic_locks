#pragma once

#include <iostream>
#include <csignal>
#include <bits/stdc++.h>
using namespace std;

#include "errors.h"
#include "record_manager.h"
#include "../../gsat/common/adapter_node_handler.h"

#ifdef USE_TREE_STATS
#   include "tree_stats.h"
#endif

#include "../../gsat/ds/salt/salt.h"

// PARAMETERS BEGIN
constexpr int LEAF_SIZE = 4;
constexpr int64_t MIN_REBUILD_BOUND = 225;
constexpr double REBUILD_FACTOR = 0.75;
// PARAMETERS END

#define RECORD_MANAGER_T record_manager<Reclaim, Alloc, Pool, Node<K, V>>
#define DATA_STRUCTURE_T SALT<K, V, ClearPolicy::kRoot>

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
            , ds(new DATA_STRUCTURE_T(VALUE_RESERVED, KEY_MIN, KEY_MAX + 1, LEAF_SIZE, MIN_REBUILD_BOUND, REBUILD_FACTOR))
    { }

    ~ds_adapter() {
        delete ds;
    }

    V getNoValue() {
        return NO_VALUE;
    }

    void initThread(const int tid) {
//      ds->initThread(tid);
    }
    
    void deinitThread(const int tid) {
//        ds->deinitThread(tid);
    }

    void warmupEnd() {
    }

    V insert(const int tid, const K& key, const V& val) {
        setbench_error("insert-replace functionality not implemented for this data structure");
    }

    V insertIfAbsent(const int tid, const K& key, const V& val) {
        return ds->Insert(key, val);
    }

    V erase(const int tid, const K& key) {
        return ds->Delete(key);
    }

    V find(const int tid, const K& key) {
        return ds->Find(key);
    }

    bool contains(const int tid, const K& key) {
        return ds->Contains(key);
    }

    int rangeQuery(const int tid, const K& lo, const K& hi, K * const resultKeys, V * const resultValues) {
        setbench_error("not implemented");
    }

    void printSummary() {
//        ds->printDebuggingDetails();
//        ds->print_inner_structure();
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
        std::cout<< "sizes: node=" << (sizeof(typename DATA_STRUCTURE_T::Node)) << std::endl;
    }

#ifdef USE_TREE_STATS
    ADAPTER_NODE_HANDLER
#endif
};
