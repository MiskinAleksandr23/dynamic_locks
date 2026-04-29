#pragma once

#include <cstdint>

template<int C>
struct ConstantDelimiter {
    int operator()(int64_t) {
        return C;
    }
};
