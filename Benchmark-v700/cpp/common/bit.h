#pragma once

#include <algorithm>

template <typename T>
void Init(T* src, T* dst, int count) {
    dst[0] = src[0];
    for (int i = 1; i < count; ++i) {
        dst[i] = dst[i - 1] + src[i];
    }

    for (int i = count - 1; i >= 0; --i) {
        auto next = i & (i + 1);
        dst[i] -= (next == 0 ? 0 : dst[next - 1]);
    }
}

template <typename T>
void Modify(T* bit, int count, int index, T delta) {
    while (index < count) {
        bit[index] += delta;
        index = index | (index + 1);
    }
}

template <typename T>
T Calculate(T* bit, int right) {
    T result = 0;
    while (right >= 0) {
        result += bit[right];
        right = (right & (right + 1)) - 1;
    }
    return result;
}

template <typename T>
class Bit {
public:
    explicit Bit(int count)
        : count_(count),
          t_(new T[count]) {
        std::fill(t_, t_ + count, 0);
    }

    Bit(T* init, int count)
        : Bit(count) {
        Init(t_, init, count);
    }

    ~Bit() {
        delete[] t_;
    }

    Bit(const Bit& other)
        : count_(other.count_) {
        std::copy(other.t_, other.t_ + count_, t_);
    }

    Bit(Bit&& other)
        : count_(other.count_),
          t_(other.t_) {
        other.t_ = nullptr;
    }

    void Modify(int index, T delta) {
        ::Modify(t_, count_, index, delta);
    }

    // [left, right)
    T Calculate(int left, int right) const {
        return Calculate(right - 1) - Calculate(left - 1);
    }

    T Calculate(int right) const {
        return ::Calculate(t_, right);
    }

private:
    T* t_;
    int count_;
};
