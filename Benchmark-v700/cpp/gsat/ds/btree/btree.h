#pragma once

#include <assert.h>
#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <vector>
#include <tuple>

#include "../../common/error.h"

#include "btree_node.h"
#include "btree_node_handler.h"

#ifdef KEY_DEPTH_TOTAL_STAT
extern int64_t key_depth_total_sum__;
extern int64_t key_depth_total_cnt__;
#endif

#ifdef KEY_DEPTH_STAT
extern int64_t* key_depth_sum__;
extern int64_t* key_depth_cnt__;
#endif

template<typename Key, typename Value, int kMinKeys>
class BTree {
public:
    static_assert(kMinKeys > 0, "min keys must be greater zero");
    static constexpr int kMaxKeys = 2 * kMinKeys;

    using Node = BTreeNode<Key, Value, kMaxKeys>;
    using NodeHandler = BTreeNodeHandler<Key, Value, kMaxKeys>;

    static_assert(std::is_same<Node *, typename NodeHandler::NodePtrType>::value);

    explicit BTree(const Value &no_value) : no_value_(no_value), root_(new Node()) {
        root_->InitializeDefault();
    }

    ~BTree() {
        Free(root_);
    }

    Value Find(const Key &key) const {
#if defined KEY_DEPTH_TOTAL_STAT || defined KEY_DEPTH_STAT
        int d__ = 0;
#endif
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
                return node->values[index];
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

    Value Insert(Key key, Value value) {
        Node *nodes[kMaxDepth];
        int indices[kMaxDepth];

        Node *node = root_;
        int depth = 0;

        while (true) {
            assert(depth < kMaxDepth);
            int index = node->Search(key);
            nodes[depth] = node;
            indices[depth] = index;
            if (node->KeyFound(index, key)) {
                return node->values[index];
            } else if (!node->children[index]) {
                break;
            }
            node = node->children[index];
            ++depth;
        }

        Node *next_node = nullptr;
        while (depth >= 0) {
            node = nodes[depth];
            int index = indices[depth];
            if (node->size == kMaxKeys) {
                Split(node, index, &key, &value, &next_node);
            } else {
                for (int at = node->size - 1; at >= index; --at) {
                    node->keys[at + 1] = std::move(node->keys[at]);
                    node->values[at + 1] = std::move(node->values[at]);
                }
                for (int at = node->size; at >= index; --at) {
                    node->children[at + 1] = node->children[at];
                }
                node->keys[index] = std::move(key);
                node->values[index] = std::move(value);
                node->children[index] = next_node;
                ++node->size;
                return no_value_;
            }
            --depth;
        }

        assert(next_node);

        // tree grows up
        Node *new_root = new Node();
        new_root->InitializeDefault();
        new_root->keys[0] = std::move(key);
        new_root->values[0] = std::move(value);
        new_root->children[0] = next_node;
        new_root->children[1] = root_;
        new_root->size = 1;

        root_ = new_root;

        return no_value_;
    }

    Value Delete(const Key &key) {
        Node *nodes[kMaxDepth];
        int indices[kMaxDepth];

        Node *node = root_;
        int depth = 0;

        int index;
        while (node) {
            assert(depth < kMaxDepth);
            index = node->Search(key);
            nodes[depth] = node;
            indices[depth] = index;
            if (node->KeyFound(index, key)) {
                break;
            }
            node = node->children[index];
            ++depth;
        }

        if (!node) {
            return no_value_;
        }

        Value result = std::move(node->values[index]);

        //  go down to the child with the largest key
        if (node->children[index]) {
            Node *remove_node = node;
            int remove_index = index;

            while (node->children[index]) {
                node = node->children[index];
                ++depth;
                nodes[depth] = node;
                index = node->size;
                indices[depth] = index;
            }
            --index;
            remove_node->keys[remove_index] = std::move(node->keys[index]);
            remove_node->values[remove_index] = std::move(node->values[index]);
        }

        // remove key from the child
        std::move(node->keys + index + 1, node->keys + node->size, node->keys + index);
        std::move(node->values + index + 1, node->values + node->size, node->values + index);
        --node->size;

        if (node->size >= kMinKeys) {
            return result;
        }

        while (depth > 0) {
            node = nodes[depth - 1];
            index = indices[depth - 1];
            Node *child = nodes[depth];

            if (child->size >= kMinKeys) {
                break;
            }

            assert(child->size == kMinKeys - 1);

            if (index != 0 && node->children[index - 1]->size > kMinKeys) {
                TransferFromLeftToRight(node, index - 1);
                return result;
            } else if (index != node->size && node->children[index + 1]->size > kMinKeys) {
                TransferFromRightToLeft(node, index);
                return result;
            } else {
                MergeChildren(node, index == 0 ? index : index - 1);
            }

            --depth;
        }

        // move root down if necessary
        Node *new_root = root_->children[0];
        if (root_->size == 0 && new_root) {
            delete root_;
            root_ = new_root;
        }

        return result;
    }

    void Validate() const {
        Validate(root_, nullptr, nullptr);
    }

    Node *GetRoot() {
        return root_;
    }

    NodeHandler *GetNodeHandler() {
        return new NodeHandler();
    }

private:
    void Validate(const Node *node, const Key *left_key_bound, const Key *right_key_bound) const {
        if (!node) {
            return;
        }

        Check(node == root_ || node->size >= kMinKeys)
        Check(node->size <= kMaxKeys)

        for (int index = 0; index <= node->size; ++index) {
            const Key *left_key = index == 0 ? left_key_bound : &node->keys[index - 1];
            const Key *right_key = index == node->size ? right_key_bound : &node->keys[index];

            CheckIfTrue(left_key && right_key, *left_key < *right_key)
            CheckIfTrue(index < node->size, node->values[index] != no_value_)

            Validate(node->children[index], left_key, right_key);
        }
    }

    static void Free(Node *node) {
        if (!node) {
            return;
        }
        for (int index = 0; index <= node->size; ++index) {
            Free(node->children[index]);
        }
        delete node;
    }

    static void Split(Node *node, int index, Key *key, Value *value, Node **next_node) {
        Key keys[kMaxKeys + 1];
        Value values[kMaxKeys + 1];
        Node *children[kMaxKeys + 2];

        std::move(node->keys, node->keys + index, keys);
        std::move(node->values, node->values + index, values);
        std::copy(node->children, node->children + index, children);

        keys[index] = std::move(*key);
        values[index] = std::move(*value);
        children[index] = *next_node;

        std::move(node->keys + index, node->keys + node->size, keys + index + 1);
        std::move(node->values + index, node->values + node->size, values + index + 1);
        std::copy(node->children + index, node->children + node->size + 1, children + index + 1);

        Node *new_node = new Node();
        new_node->InitializeMove(keys, values, children, kMinKeys);

        node->InitializeMove(keys + kMinKeys + 1, values + kMinKeys + 1, children + kMinKeys + 1, kMinKeys);

        *key = std::move(keys[kMinKeys]);
        *value = std::move(values[kMinKeys]);
        *next_node = new_node;
    }

    static void TransferFromLeftToRight(Node *node, int left_index) {
        Node *left_child = node->children[left_index];
        Node *right_child = node->children[left_index + 1];

        const int left_size = left_child->size;

        for (int at = right_child->size - 1; at >= 0; --at) {
            right_child->keys[at + 1] = std::move(right_child->keys[at]);
            right_child->values[at + 1] = std::move(right_child->values[at]);
        }
        for (int at = right_child->size; at >= 0; --at) {
            right_child->children[at + 1] = right_child->children[at];
        }

        right_child->keys[0] = std::move(node->keys[left_index]);
        right_child->values[0] = std::move(node->values[left_index]);
        right_child->children[0] = left_child->children[left_size];

        node->keys[left_index] = std::move(left_child->keys[left_size - 1]);
        node->values[left_index] = std::move(left_child->values[left_size - 1]);

        --left_child->size;
        ++right_child->size;
    }

    static void TransferFromRightToLeft(Node *node, int left_index) {
        Node *left_child = node->children[left_index];
        Node *right_child = node->children[left_index + 1];

        const int left_size = left_child->size;
        const int right_size = right_child->size;

        left_child->keys[left_size] = std::move(node->keys[left_index]);
        left_child->values[left_size] = std::move(node->values[left_index]);
        left_child->children[left_size + 1] = right_child->children[0];

        node->keys[left_index] = std::move(right_child->keys[0]);
        node->values[left_index] = std::move(right_child->values[0]);

        std::move(right_child->keys + 1, right_child->keys + right_size, right_child->keys);
        std::move(right_child->values + 1, right_child->values + right_size, right_child->values);
        std::copy(right_child->children + 1, right_child->children + right_size + 1, right_child->children);

        ++left_child->size;
        --right_child->size;
    }

    static void MergeChildren(Node *node, int left_index) {
        Node *left_child = node->children[left_index];
        Node *right_child = node->children[left_index + 1];

        const int left_size = left_child->size;
        const int right_size = right_child->size;

        assert(left_size + right_size + 1 == kMaxKeys);

        left_child->keys[left_size] = std::move(node->keys[left_index]);
        left_child->values[left_size] = std::move(node->values[left_index]);

        // delete from node
        std::move(node->keys + left_index + 1, node->keys + node->size, node->keys + left_index);
        std::move(node->values + left_index + 1, node->values + node->size, node->values + left_index);
        std::copy(node->children + left_index + 2, node->children + node->size + 1, node->children + left_index + 1);
        node->size--;

        // move from right
        std::move(right_child->keys, right_child->keys + right_size, left_child->keys + left_size + 1);
        std::move(right_child->values, right_child->values + right_size, left_child->values + left_size + 1);
        std::copy(right_child->children, right_child->children + right_size + 1, left_child->children + left_size + 1);

        left_child->size = left_size + right_size + 1;

        delete right_child;
    }

    static constexpr int kMaxDepth = 16;

    const Value no_value_;
    Node *root_;
};
