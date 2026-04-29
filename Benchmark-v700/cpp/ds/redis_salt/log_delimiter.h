#pragma once

#include <cstdint>

struct LogDelimiter {
    int operator()(int64_t total_accesses) {
        int pow = 0;
        while ((1ll << pow) < total_accesses) {
            ++pow;
        }
        return pow;
    }
};
