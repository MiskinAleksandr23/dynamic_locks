#pragma once

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>
#include <concepts>

#include <error.h>
#include "operation.h"

class RandomGenerator {
public:
    explicit RandomGenerator(uint32_t seed = kDefaultSeed)
            : gen_(seed) {
    }

    template<typename T>
    T GenKey(T from, T to) {
        return GenInt<T>(from, to);
    }

    template<typename T>
    T GenInt(T from, T to) {
        std::uniform_int_distribution<T> dist(from, to);
        return dist(gen_);
    }

    template<typename T>
    T GenInt() {
        return GenInt(std::numeric_limits<T>::min(), std::numeric_limits<T>::max());
    }

    template<typename T>
    std::vector<T> GenIntegralVector(size_t count, T from, T to) {
        std::uniform_int_distribution<T> dist(from, to);
        std::vector<T> result(count);
        std::generate(result.begin(), result.end(), [&]() {
            return dist(gen_);
        });
        return result;
    }

    template<typename Iterator>
    void Shuffle(Iterator first, Iterator last) {
        std::shuffle(first, last, gen_);
    }

    Operation GenOperation() {
        std::uniform_int_distribution<int> dist(0, kCntOperation - 1);
        return static_cast<Operation>(dist(gen_));
    }

private:
    std::mt19937 gen_;

    static constexpr int32_t kDefaultSeed = 547748385u;
};
