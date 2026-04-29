#pragma once

#include <cstdint>
#include <cmath>

struct SqrtDelimiter {
    int operator()(int64_t total_accesses) {
        return std::sqrt(total_accesses);
    }
};
