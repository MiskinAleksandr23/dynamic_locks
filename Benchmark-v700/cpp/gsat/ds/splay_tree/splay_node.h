#pragma once

template<typename Key, typename Value>
struct SplayNode {
    const Key key;
    Value value;
    SplayNode *left;
    SplayNode *right;
    SplayNode *parent;

    SplayNode(const Key &key, const Value &value)
            : key(key), value(value), left(nullptr), right(nullptr), parent(nullptr) {}
};
