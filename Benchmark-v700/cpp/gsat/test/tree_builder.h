#pragma once

#include <optional>

template<typename Tree, typename Value>
class TreeBuilder {
public:
    virtual ~TreeBuilder() = default;

    virtual Tree *Build() = 0;

    Value GetNoValue() const {
        return *no_value_;
    }

    void SetNoValue(const Value &no_value) {
        no_value_ = no_value;
    }

private:
    std::optional<Value> no_value_;
};
