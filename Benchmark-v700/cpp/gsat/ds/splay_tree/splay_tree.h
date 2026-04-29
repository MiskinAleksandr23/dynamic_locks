#pragma once

#include <assert.h>
#include <utility>

#include "../../common/error.h"

#include "splay_node.h"
#include "splay_node_handler.h"

#ifdef KEY_DEPTH_TOTAL_STAT
extern int64_t key_depth_total_sum__;
extern int64_t key_depth_total_cnt__;
#endif

#ifdef KEY_DEPTH_STAT
extern int64_t* key_depth_sum__;
extern int64_t* key_depth_cnt__;
#endif

template <typename Key, typename Value>
class SplayTree {
public:
    using Node = SplayNode<Key, Value>;
    using NodeHandler = SplayNodeHandler<Key, Value>;
    static_assert(std::is_same<Node*, typename NodeHandler::NodePtrType>::value);

private:
    const Value no_value_;
    Node* root_;

public:
    explicit SplayTree(const Value& no_value) : no_value_(no_value), root_(nullptr) {
    }

    ~SplayTree() {
        Free(root_);
    }

    Value Find(const Key& key) {
        auto [node, parent] = FindNodeAndParent(key);
        if (node) {
            SplayAssign(node);
            return node->value;
        } else {
            SplayAssign(parent);
            return no_value_;
        }
    }

    bool Contains(const Key& key) {
        return Find(key) != no_value_;
    }

    Value Insert(const Key& key, const Value& value) {
        auto [node, parent] = FindNodeAndParent(key);

        Value result = no_value_;

        if (node) {
            result = node->value;
        } else {
            node = new Node(key, value);
            AddNode(parent, node);
        }

        SplayAssign(node);

        return result;
    }

    Value Delete(const Key& key) {
        auto [node, parent] = FindNodeAndParent(key);

        if (!node) {
            SplayAssign(parent);
            return no_value_;
        }

        Splay(node);

        Value result = std::move(node->value);
        auto left = node->left;
        auto right = node->right;

        delete node;

        Node* new_root = nullptr;

        if (left) {
            left->parent = nullptr;
            new_root = Max(left);
            Splay(new_root);
            SetRight(new_root, right);
        } else if (right) {
            right->parent = nullptr;
            new_root = Min(right);
            Splay(new_root);
            SetLeft(new_root, left);
        }

        root_ = new_root;

        return result;
    }

    void Validate() const {
        Validate(root_, nullptr, nullptr);
    }

    Node* GetRoot() {
        return root_;
    }

    NodeHandler* GetNodeHandler() {
        return new NodeHandler();
    }

private:
    std::pair<Node*, Node*> FindNodeAndParent(const Key& key) const {
#if defined KEY_DEPTH_TOTAL_STAT || defined KEY_DEPTH_STAT
        int d__ = 0;
#endif
        Node* parent = nullptr;
        Node* node = root_;
        while (node && node->key != key) {
#if defined KEY_DEPTH_TOTAL_STAT || defined KEY_DEPTH_STAT
            ++d__;
#endif
            parent = node;
            node = key < node->key ? node->left : node->right;
        }
#ifdef KEY_DEPTH_TOTAL_STAT
        if (node && node->key == key) {
            key_depth_total_sum__ += d__;
            ++key_depth_total_cnt__;
        }
#endif
#ifdef KEY_DEPTH_STAT
        if (node && node->key == key) {
            key_depth_sum__[key] += d__;
            ++key_depth_cnt__[key];
        }
#endif
        return {node, parent};
    }

    void AddNode(Node* parent, Node* node) {
        if (parent) {
            if (node->key < parent->key) {
                SetLeft(parent, node);
            } else {
                SetRight(parent, node);
            }
        } else {
            root_ = node;
        }
    }

    void SplayAssign(Node* node) {
        Splay(node);
        root_ = node;
    }

    void Validate(Node* node, const Key* left, const Key* right) const {
        if (!node) {
            return;
        }

        CheckIfTrue(left, *left < node->key);
        CheckIfTrue(right, node->key < *right);
        Check(node->value != no_value_);

        Validate(node->left, left, &node->key);
        Validate(node->right, &node->key, right);
    }

    static Node* Min(Node* node) {
        assert(node);
        while (node->left) {
            node = node->left;
        }
        return node;
    }

    static Node* Max(Node* node) {
        assert(node);
        while (node->right) {
            node = node->right;
        }
        return node;
    }

    static void Free(Node* node) {
        if (!node) {
            return;
        }
        Free(node->left);
        Free(node->right);
        delete node;
    }

    static void Splay(Node* node) {
        while (GetParent(node)) {
            auto parent = node->parent;
            auto grand_parent = GetParent(parent);
            if (grand_parent) {
                if (parent->right == node) {
                    if (grand_parent->right == parent) {
                        RotateLeft(parent);
                        RotateLeft(node);
                    } else {
                        RotateLeft(node);
                        RotateRight(node);
                    }
                } else {
                    if (grand_parent->left == parent) {
                        RotateRight(parent);
                        RotateRight(node);
                    } else {
                        RotateRight(node);
                        RotateLeft(node);
                    }
                }
            } else {
                if (parent->right == node) {
                    RotateLeft(node);
                } else {
                    RotateRight(node);
                }
            }
        }
    }

    static void RotateRight(Node* node) {
        assert(node);
        Node* parent = node->parent;
        UpdateParents(parent, node);
        SetLeft(parent, node->right);
        SetRight(node, parent);
    }

    static void RotateLeft(Node* node) {
        assert(node);
        Node* parent = node->parent;
        UpdateParents(parent, node);
        SetRight(parent, node->left);
        SetLeft(node, parent);
    }

    static void UpdateParents(Node* parent, Node* node) {
        Node* grand_parent = GetParent(parent);
        if (GetRight(grand_parent) == parent) {
            SetRight(grand_parent, node);
        } else {
            SetLeft(grand_parent, node);
        }
        SetParent(parent, node);
    }

    static inline Node* GetLeft(Node* node) {
        return node ? node->left : nullptr;
    }

    static inline Node* GetRight(Node* node) {
        return node ? node->right : nullptr;
    }

    static inline Node* GetParent(Node* node) {
        return node ? node->parent : nullptr;
    }

    static inline void SetLeft(Node* node, Node* left) {
        if (node) {
            node->left = left;
        }
        SetParent(left, node);
    }

    static inline void SetRight(Node* node, Node* right) {
        if (node) {
            node->right = right;
        }
        SetParent(right, node);
    }

    static inline void SetParent(Node* node, Node* parent) {
        if (node) {
            node->parent = parent;
        }
    }
};
