#pragma once

#include <assert.h>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <utility>
#include <vector>
#include <tuple>

#include "../../common/error.h"

#include "gsat_node_handler.h"

#ifdef KEY_DEPTH_TOTAL_STAT
extern int64_t key_depth_total_sum__;
extern int64_t key_depth_total_cnt__;
#endif

#ifdef KEY_DEPTH_STAT
extern int64_t* key_depth_sum__;
extern int64_t* key_depth_cnt__;
#endif

#define KEY_FOUND (index != node->rep_size && node->rep[index] == key)

#define INSERT(node, index, key, value, accesses)                   \
    for (int at = node->rep_size - 1; at >= index; --at) {          \
        node->rep[at + 1] = std::move(node->rep[at]);               \
        node->value_data[at + 1] = std::move(node->value_data[at]); \
    }                                                               \
    node->rep[index] = key;                                         \
    node->value_data[index] = {value, accesses};                    \
    ++node->rep_size;

enum class ClearPolicy { kNone, kRoot, kRapid };

template <typename Delimiter, typename Node, ClearPolicy CP, typename Key, typename Value>
class GSAT {
public:
    using ValueData = typename Node::ValueData;
    using NodeHandler = GSATNodeHandler<Node>;

    static_assert(std::is_same<Node*, typename NodeHandler::NodePtrType>::value);

public:
    // [left, right)
    GSAT(Delimiter delimiter, const Value& no_value, const Key& left, const Key& right,
         int leaf_size, int64_t min_rebuild_bound, double rebuild_factor)
        : delimiter_(delimiter),
          no_value_(no_value),
          left_(left),
          right_(right),
          leaf_size_(leaf_size),
          min_rebuild_bound_(min_rebuild_bound),
          rebuild_factor_(rebuild_factor),
          root_(CreateRoot()){Check(left_ < right_) Check(leaf_size_ > 0)
                                  Check(min_rebuild_bound_ > 1) Check(rebuild_factor_ > 0)}

          GSAT(const Value& no_value, const Key& left, const Key& right, int leaf_size,
               int64_t min_rebuild_bound, double rebuild_factor)
        : GSAT(Delimiter(), no_value, left, right, leaf_size, min_rebuild_bound, rebuild_factor) {
    }

    ~GSAT() {
        delete root_;
    }

    Value Find(const Key& key) {
#if defined KEY_DEPTH_TOTAL_STAT || defined KEY_DEPTH_STAT
        int d__ = 0;
#endif
        assert(root_);

        Node* node = root_;
        Node** node_at = &root_;

        Node* rebuild_node = nullptr;
        Node** rebuild_node_at = nullptr;

        Value result = no_value_;

        while (node) {
            if (node->Access() && !rebuild_node) {
                rebuild_node = node;
                rebuild_node_at = node_at;
            }

            auto index = node->Search(key);
            if (KEY_FOUND) {
#ifdef KEY_DEPTH_TOTAL_STAT
                key_depth_total_sum__ += d__;
                ++key_depth_total_cnt__;
#endif
#ifdef KEY_DEPTH_STAT
                key_depth_sum__[key] += d__;
                ++key_depth_cnt__[key];
#endif
                auto& vd = node->value_data[index];
                if (vd.accesses < 0) {
                    --vd.accesses;
                } else {
                    ++vd.accesses;
                    result = vd.value;
                }
                break;
            }
#if defined KEY_DEPTH_TOTAL_STAT || defined KEY_DEPTH_STAT
            ++d__;
#endif
            node_at = &node->children[index];
            node = node->children[index];
        }

        if (rebuild_node) {
            *rebuild_node_at = RebuildTree(rebuild_node);
        }

        return result;
    }

    bool Contains(const Key& key) {
        return Find(key) != no_value_;
    }

    Value Insert(const Key& key, const Value& value) {
        assert(root_);
        assert(root_->left <= key && key < root_->right);

        Node* node = root_;
        Node** node_at = &root_;

        Node* rebuild_node = nullptr;
        Node** rebuild_node_at = nullptr;

        Value result = no_value_;

        while (true) {
            ++node->total_asize;

            if (node->Access() && !rebuild_node) {
                rebuild_node = node;
                rebuild_node_at = node_at;
            }

            auto index = node->Search(key);
            if (KEY_FOUND) {
                auto& vd = node->value_data[index];
                if (vd.accesses < 0) {
                    vd.accesses = -vd.accesses;
                    vd.value = value;
                } else {
                    result = vd.value;
                }
                ++vd.accesses;
                break;
            }

            if (!node->children[index]) {
                if (node->leaf && node->rep_size < node->GetCapacity()) {
                    INSERT(node, index, key, value, 1)
                } else {
                    const Key& child_left = index == 0 ? node->left : node->rep[index - 1];
                    const Key& child_right =
                        index == node->rep_size ? node->right : node->rep[index];
                    auto child = new Node(child_left, child_right, leaf_size_, min_rebuild_bound_);
                    INSERT(child, 0, key, value, 1)
                    ++child->total_asize;
                    ++child->counter;
                    node->children[index] = child;
                }
                break;
            }

            node_at = &node->children[index];
            node = node->children[index];
        }

        if (rebuild_node) {
            *rebuild_node_at = RebuildTree(rebuild_node);
        }

        return result;
    }

    Value Delete(const Key& key) {
        assert(root_);

        Node* node = root_;
        Node** node_at = &root_;

        Node* rebuild_node = nullptr;
        Node** rebuild_node_at = nullptr;

        Value result = no_value_;

        while (node) {
            if (node->Access() && !rebuild_node) {
                rebuild_node = node;
                rebuild_node_at = node_at;
            }

            auto index = node->Search(key);
            if (KEY_FOUND) {
                auto& vd = node->value_data[index];
                if (vd.accesses > 0) {
                    result = vd.value;
                    vd.accesses = -vd.accesses;
                }
                --vd.accesses;
                break;
            }

            node_at = &node->children[index];
            node = node->children[index];
        }

        if (rebuild_node) {
            *rebuild_node_at = RebuildTree(rebuild_node);
        }

        return result;
    }

    void Validate() const {
        Validate(root_, left_, right_);
    }

    Node* GetRoot() {
        return root_;
    }

    NodeHandler* GetNodeHandler() {
        return new NodeHandler();
    }

private:
    Node* RebuildTree(Node* node) {
        const int count = node->total_asize;

        auto rep = new Key[count];
        auto value_data = new ValueData[count];
        auto at = 0;

        bool clear;
        if constexpr (CP == ClearPolicy::kNone) {
            clear = false;
        } else if constexpr (CP == ClearPolicy::kRapid) {
            clear = true;
        } else if constexpr (CP == ClearPolicy::kRoot) {
            clear = node == root_;
        }

        CollectAndClear(node, rep, value_data, at, clear);

        auto pac = new int64_t[at + 1];
        pac[0] = 0;
        for (int index = 0; index < at; ++index) {
            pac[index + 1] = pac[index] + std::abs(value_data[index].accesses);
        }

        Node* result = BuildIdealTree(rep, value_data, pac, 0, at, node->left, node->right);
        if (!result && root_ == node) {
            result = CreateRoot();
        }

        delete[] pac;
        delete[] value_data;
        delete[] rep;

        delete node;

        return result;
    }

    static void CollectAndClear(Node* node, Key* rep, ValueData* value_data, int& at, bool clear) {
        assert(node);

        if (node->children[0]) {
            CollectAndClear(node->children[0], rep, value_data, at, clear);
        }

        for (int index = 0; index < node->rep_size; ++index) {
            if (!clear || node->value_data[index].accesses > 0) {
                rep[at] = std::move(node->rep[index]);
                value_data[at] = std::move(node->value_data[index]);
                ++at;
            }
            if (node->children[index + 1]) {
                CollectAndClear(node->children[index + 1], rep, value_data, at, clear);
            }
        }
    }

    // [left, right)
    Node* BuildIdealTree(Key* rep, ValueData* value_data, int64_t* pac, int left, int right,
                         Key left_bound, Key right_bound) {
        const int size = right - left;
        if (size <= 0) {
            return nullptr;
        }

        const int64_t total_accesses = pac[right] - pac[left];
        const int64_t rebuild_bound =
            std::max(min_rebuild_bound_, static_cast<int64_t>(rebuild_factor_ * total_accesses));

        if (size <= leaf_size_) {
            auto node = new Node(left_bound, right_bound, leaf_size_, rebuild_bound);
            node->total_asize = node->rep_size = size;
            for (int index = 0; index < size; ++index) {
                node->rep[index] = std::move(rep[left + index]);
                node->value_data[index] = std::move(value_data[left + index]);
            }
            return node;
        }

        const int capacity = ceil(delimiter_(total_accesses));
        const int child_accesses = ceil(total_accesses / (capacity + 1.0));

        auto node = new Node(left_bound, right_bound, capacity, rebuild_bound);
        node->total_asize = size;

        int index = 0;
        while (index < capacity) {
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

            node->rep[index] = std::move(rep[from]);
            node->value_data[index] = std::move(value_data[from]);
            node->children[index] =
                BuildIdealTree(rep, value_data, pac, left, from, left_bound, node->rep[index]);

            left_bound = node->rep[index];
            ++index;
            left = to;
            if (left == right) {
                break;
            }
        }
        node->children[index] =
            BuildIdealTree(rep, value_data, pac, left, right, left_bound, right_bound);
        node->rep_size = index;

        node->Complete(total_accesses);

        return node;
    }

    int Validate(const Node* node, const Key& left, const Key& right) const {
        if (!node) {
            return 0;
        }

        Check(node->left == left) Check(node->right == right)

            int total_size = 0;

        for (int index = 0; index <= node->rep_size; ++index) {
            const Key& child_left = index == 0 ? left : node->rep[index - 1];
            const Key& child_right = index == node->rep_size ? right : node->rep[index];

            Check(child_left <= child_right)

                if (index < node->rep_size) {
                const auto& vd = node->value_data[index];
                Check(vd.value != no_value_) total_size += vd.accesses > 0;
            }

            total_size += Validate(node->children[index], child_left, child_right);
        }

        Check(total_size <= node->total_asize)

            return total_size;
    }

    Node* CreateRoot() const {
        return new Node(left_, right_, leaf_size_, min_rebuild_bound_);
    }

    Delimiter delimiter_;
    const Value no_value_;
    const Key left_;
    const Key right_;
    const int leaf_size_;
    const int64_t min_rebuild_bound_;
    const double rebuild_factor_;
    Node* root_;
};
