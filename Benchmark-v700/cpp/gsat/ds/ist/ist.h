#pragma once

#include <assert.h>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <utility>
#include <vector>

#include "../../common/error.h"

#include "ist_node.h"
#include "ist_node_handler.h"

#ifdef KEY_DEPTH_TOTAL_STAT
extern int64_t key_depth_total_sum__;
extern int64_t key_depth_total_cnt__;
#endif

#ifdef KEY_DEPTH_STAT
extern int64_t* key_depth_sum__;
extern int64_t* key_depth_cnt__;
#endif

template<typename Key, typename Value>
class IST {
public:
    static constexpr int kLeafSize = 16;
    static constexpr int kMinRebuildBound = 100;
    static constexpr double kRebuildFactor = 0.25;

    using Node = ISTNode<Key, Value>;
    using NodeHandler = ISTNodeHandler<Key, Value>;

    using ValueData = typename Node::ValueData;

    static_assert(std::is_same<Node *, typename NodeHandler::NodePtrType>::value);

    // [left, right)
    IST(const Value &no_value, double alpha, const Key &left, const Key &right, int leaf_size,
        int min_rebuild_bound, double rebuild_factor)
            : no_value_(no_value),
              alpha_(alpha),
              left_(left),
              right_(right),
              leaf_size_(leaf_size),
              min_rebuild_bound_(min_rebuild_bound),
              rebuild_factor_(rebuild_factor),
              root_(CreateRoot()) {
        Check(0.5 <= alpha_ && alpha_ < 1)
        Check(left < right)
        Check(leaf_size_ > 0)
        Check(min_rebuild_bound_ > 1)
        Check(rebuild_factor > 0)
    }

    IST(const Value &no_value, double alpha, const Key &left_bound, const Key &right_bound)
            : IST(no_value, alpha, left_bound, right_bound, kLeafSize, kMinRebuildBound, kRebuildFactor) {
    }

    ~IST() {
        delete root_;
    }

    Value Find(const Key &key) const {
#if defined KEY_DEPTH_TOTAL_STAT || defined KEY_DEPTH_STAT
        int d__ = 0;
#endif
        assert(root_);

        Node *node = root_;
        while (node) {
            int index = node->Search(key);
            if (node->KeyFound(index, key)) {
#ifdef KEY_DEPTH_TOTAL_STAT
                key_depth_total_sum__ += d__;
                ++key_depth_total_cnt__;
#endif
#ifdef KEY_DEPTH_STAT
                key_depth_sum__[key] += d__;
                ++key_depth_cnt__[key];
#endif
                const auto &vd = node->value_data[index];
                return vd.marked ? no_value_ : vd.value;
            }
#if defined KEY_DEPTH_TOTAL_STAT || defined KEY_DEPTH_STAT
            ++d__;
#endif
            node = node->children[index];
        }
        return no_value_;
    }

    bool Contains(const Key &key) const {
        return Find(key) != no_value_;
    }

    Value Insert(const Key &key, const Value &value) {
        assert(root_);
        assert(root_->left <= key && key <= root_->right);

        Node *node = root_;
        Node **node_at = &root_;

        Node *rebuild_node = nullptr;
        Node **rebuild_node_at = nullptr;

        Value result = no_value_;

        while (true) {
            ++node->total_asize;

            if (node->Count() && !rebuild_node) {
                rebuild_node = node;
                rebuild_node_at = node_at;
            }

            int index = node->Search(key);
            if (node->KeyFound(index, key)) {
                auto &vd = node->value_data[index];
                if (vd.marked) {
                    vd.value = value;
                    vd.marked = false;
                } else {
                    result = vd.value;
                }
                break;
            }

            if (!node->children[index]) {
                if (node->rep_size < node->capacity) {
                    node->Insert(index, key, value);
                } else {
                    const Key& left = index == 0 ? node->left : node->rep[index - 1];
                    const Key& right = index == node->rep_size ? node->right : node->rep[index];
                    auto child = new Node(left, right, leaf_size_, min_rebuild_bound_);
                    child->Insert(0, key, value);
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

    Value Delete(const Key &key) {
        assert(root_);

        Node *node = root_;
        Node **node_at = &root_;

        Node *rebuild_node = nullptr;
        Node **rebuild_node_at = nullptr;

        Value result = no_value_;

        while (node) {
            if (node->Count() && !rebuild_node) {
                rebuild_node = node;
                rebuild_node_at = node_at;
            }

            int index = node->Search(key);
            if (node->KeyFound(index, key)) {
                auto &vd = node->value_data[index];
                if (!vd.marked) {
                    result = vd.value;
                    vd.marked = true;
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

    void Validate() const {
        Validate(root_, left_, right_);
    }

    Node *GetRoot() {
        return root_;
    }

    NodeHandler *GetNodeHandler() {
        return new NodeHandler();
    }

private:
    Node *RebuildTree(Node *node) const {
        auto rep = new Key[node->total_asize];
        auto value_data = new ValueData[node->total_asize];
        int at = 0;
        CollectNonMarked(node, rep, value_data, at);

        Node *result = BuildIdealTree(rep, value_data, 0, at, node->left, node->right);
        if (!result && node == root_) {
            result = CreateRoot();
        }

        delete[] value_data;
        delete[] rep;

        delete node;

        return result;
    }

    static void CollectNonMarked(Node *node, Key *rep, ValueData *value_data, int &at) {
        assert(node);

        if (node->children[0]) {
            CollectNonMarked(node->children[0], rep, value_data, at);
        }

        for (int index = 0; index < node->rep_size; ++index) {
            if (!node->value_data[index].marked) {
                rep[at] = std::move(node->rep[index]);
                value_data[at] = std::move(node->value_data[index]);
                ++at;
            }
            if (node->children[index + 1]) {
                CollectNonMarked(node->children[index + 1], rep, value_data, at);
            }
        }
    }

    // [left, right)
    Node *BuildIdealTree(Key *rep, ValueData *value_data, int left, int right, const Key &left_bound,
                         const Key &right_bound) const {
        const int size = right - left;
        if (size <= 0) {
            return nullptr;
        }

        if (size <= leaf_size_) {
            Node *node = new Node(left_bound, right_bound, leaf_size_, min_rebuild_bound_);
            node->total_asize = node->rep_size = size;
            for (int index = 0; index < size; ++index) {
                node->rep[index] = std::move(rep[left + index]);
                node->value_data[index] = std::move(value_data[left + index]);
            }
            return node;
        }

        const int rebuild_bound = std::max(min_rebuild_bound_, static_cast<int>(rebuild_factor_ * size));
        const int step = sqrt(size);

        Node *node = new Node(left_bound, right_bound, step, rebuild_bound);
        node->total_asize = size;
        node->rep_size = step;

        for (int index = 0; index < step; ++index) {
            int child_left = left + index * step;
            int child_right = left + (index + 1) * step - 1;

            node->rep[index] = std::move(rep[child_right]);
            node->value_data[index] = std::move(value_data[child_right]);

            const Key &child_left_bound = index == 0 ? left_bound : node->rep[index - 1];
            const Key &child_right_bound = node->rep[index];

            node->children[index] =
                    BuildIdealTree(rep, value_data, child_left, child_right, child_left_bound, child_right_bound);
        }
        node->children[step] =
                BuildIdealTree(rep, value_data, left + step * step, right, node->rep[step - 1], right_bound);

        node->BuildId(ceil(pow(size, alpha_)));

        return node;
    }

    int Validate(const Node *node, const Key &left, const Key &right) const {
        if (!node) {
            return 0;
        }

        Check(node->left == left)
        Check(node->right == right)

        int total_size = 0;
        for (int index = 0; index <= node->rep_size; ++index) {
            const Key &child_left = index == 0 ? left : node->rep[index - 1];
            const Key &child_right = index == node->rep_size ? right : node->rep[index];

            Check(child_left < child_right)
            CheckIfTrue(index < node->rep_size, node->value_data[index].value != no_value_)

            total_size += index < node->rep_size && !node->value_data[index].marked;

            total_size += Validate(node->children[index], child_left, child_right);
        }

        Check(total_size <= node->total_asize)

        return total_size;
    }

    Node* CreateRoot() const {
        return new Node(left_, right_, leaf_size_, min_rebuild_bound_);
    }

    const Value no_value_;
    const double alpha_;
    const Key left_;
    const Key right_;
    const int leaf_size_;
    const int min_rebuild_bound_;
    const double rebuild_factor_;
    Node *root_;
};
