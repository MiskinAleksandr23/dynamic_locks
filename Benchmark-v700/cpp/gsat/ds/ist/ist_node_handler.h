#pragma once

#include "ist_node.h"

template<typename Key, typename Value>
class ISTNodeHandler {
public:
    using NodePtrType = ISTNode<Key, Value> *;

    class ChildIterator {
    public:
        explicit ChildIterator(NodePtrType node) : node_(node), at_(0) {
            Advance();
        }

        bool HasNext() const {
            return at_ != node_->rep_size + 1;
        }

        NodePtrType Next() {
            auto result = node_->children[at_++];
            Advance();
            return result;
        }

    private:
        void Advance() {
            while (at_ <= node_->rep_size && !node_->children[at_]) {
                ++at_;
            }
        }

        NodePtrType node_;
        int at_;
    };

    bool IsLeaf(NodePtrType node) const {
        for (int index = 0; index <= node->rep_size; ++index) {
            if (node->children[index]) {
                return false;
            }
        }
        return true;
    }

    int64_t GetNumKeys(NodePtrType node) {
        int size = 0;
        for (int index = 0; index < node->rep_size; ++index) {
            size += !node->value_data[index].marked;
        }
        return size;
    }

    int64_t GetSumKeys(NodePtrType node) {
        int64_t sum = 0;
        for (int index = 0; index < node->rep_size; ++index) {
            if (!node->value_data[index].marked) {
                sum += node->rep[index];
            }
        }
        return sum;
    }

    ChildIterator GetChildIterator(NodePtrType node) const {
        return ChildIterator(node);
    }
};
