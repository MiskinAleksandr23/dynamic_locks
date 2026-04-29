#pragma once

#include <cstddef>

#include "splay_node.h"

template<typename Key, typename Value>
class SplayNodeHandler {
public:
    using NodePtrType = SplayNode<Key, Value> *;

    class ChildIterator {
    public:
        explicit ChildIterator(NodePtrType node)
                : node_(node), at_(0) {
            Advance();
        }

        bool HasNext() {
            return at_ != 2;
        }

        NodePtrType Next() {
            auto result = at_++ ? node_->right : node_->left;
            Advance();
            return result;
        }

    private:
        void Advance() {
            if (at_ == 0 && !node_->left) {
                ++at_;
            }
            if (at_ == 1 && !node_->right) {
                ++at_;
            }
        }

        NodePtrType node_;
        int at_;
    };

    bool IsLeaf(NodePtrType node) const {
        return !node->left && !node->right;
    }

    int64_t GetNumKeys(NodePtrType node) {
        return 1;
    }

    int64_t GetSumKeys(NodePtrType node) {
        return node->key;
    }

    ChildIterator GetChildIterator(NodePtrType node) const {
        return ChildIterator(node);
    }
};
