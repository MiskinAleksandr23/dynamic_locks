#pragma once

#include "btree_node.h"

template<typename Key, typename Value, int kMaxKeys>
class BTreeNodeHandler {
public:
    using NodePtrType = BTreeNode<Key, Value, kMaxKeys> *;

    class ChildIterator {
    public:
        explicit ChildIterator(NodePtrType node) : node_(node), at_(0) {
            Advance();
        }

        bool HasNext() const {
            return at_ != node_->size + 1;
        }

        NodePtrType Next() {
            auto result = node_->children[at_++];
            Advance();
            return result;
        }

    private:
        void Advance() {
            while (at_ <= node_->size && !node_->children[at_]) {
                ++at_;
            }
        }

        NodePtrType node_;
        int at_;
    };

    bool IsLeaf(NodePtrType node) const {
        for (int index = 0; index <= node->size; ++index) {
            if (node->children[index]) {
                return false;
            }
        }
        return true;
    }

    int64_t GetNumKeys(NodePtrType node) {
        return node->size;
    }

    int64_t GetSumKeys(NodePtrType node) {
        int64_t sum = 0;
        for (int index = 0; index < node->size; ++index) {
            sum += node->keys[index];
        }
        return sum;
    }

    ChildIterator GetChildIterator(NodePtrType node) const {
        return ChildIterator(node);
    }
};
