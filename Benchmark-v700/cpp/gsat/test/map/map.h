#pragma once

#include <map>
#include <assert.h>
#include <type_traits>

#include "../../common/error.h"

template<typename Key, typename Value>
class Map {
public:
    class NodeHandler {
    public:
        using NodePtrType = Map<Key, Value> *;

        class ChildIterator {
        public:
            explicit ChildIterator(NodePtrType node) {
                assert(false);
            }

            bool HasNext() {
                assert(false);
                return false;
            }

            NodePtrType Next() {
                assert(false);
                return 0;
            }
        };

        bool IsLeaf(NodePtrType node) {
            // avoids any ChildIterator stuff
            return true;
        }

        int64_t GetNumKeys(NodePtrType node) {
            return node->map_.size();
        }

        int64_t GetSumKeys(NodePtrType node) {
            int64_t sum = 0;
            for (auto [key, _] : node->map_) {
                sum += key;
            }
            return sum;
        }

        ChildIterator GetChildIterator(NodePtrType node) {
            assert(false);
            return ChildIterator(node);
        }
    };

    using iterator = typename std::map<Key, Value>::iterator;              // NOLINT
    using const_iterator = typename std::map<Key, Value>::const_iterator;  // NOLINT

    using Node = Map<Key, Value>;
    using NodeHandler = Map<Key, Value>::NodeHandler;

    static_assert(std::is_same<Node *, typename NodeHandler::NodePtrType>::value);

    explicit Map(const Value &no_value) : no_value_(std::move(no_value)) {
    }

    Value Find(const Key &key) const {
        auto it = map_.find(key);
        return it == map_.end() ? no_value_ : it->second;
    }

    bool Contains(const Key &key) const {
        return map_.count(key);
    }

    Value Insert(const Key &key, const Value &value) {
        assert(value != no_value_);
        auto [it, ok] = map_.insert({key, value});
        return ok ? no_value_ : it->second;
    }

    Value Delete(const Key &key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return no_value_;
        } else {
            auto result = it->second;
            map_.erase(it);
            return result;
        }
    }

    void Validate() const {
        for (const auto &[k, v] : map_) {
            Check(v != no_value_)
        }
    }

    Node *GetRoot() {
        return this;
    }

    NodeHandler *GetNodeHandler() {
        return new NodeHandler();
    }

    iterator begin() {  // NOLINT
        return map_.begin();
    }

    const_iterator begin() const {  // NOLINT
        return map_.begin();
    }

    iterator end() {  // NOLINT
        return map_.end();
    }

    const_iterator end() const {  // NOLINT
        return map_.end();
    }

private:
    const Value no_value_;
    std::map<Key, Value> map_;
};
