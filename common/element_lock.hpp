#pragma once

#include "common/range_lock.hpp"

#include <algorithm>
#include <memory>
#include <mutex>

// Dense mutex storage: one real mutex per element, no sparse allocation or striping.
// All mutexes covering a query remain held for its entire callback.
template <class Mutex = std::mutex> class ElementLock {
  struct Slot { Mutex mutex; };

public:
  explicit ElementLock(size_t size) : size_(size), locks_(std::make_unique<Slot[]>(size)) {}

  template <class Func> void WriteQuery(size_t left, size_t right, Func &&func) {
    if (size_ == 0) return;
    Normalize(left, right);
    dynamic_lock_detail::RangeLockGuard guard(locks_, left, right);
    func(left, right);
  }

  size_t CountLocksForRange(size_t left, size_t right) const {
    if (size_ == 0) return 0;
    Normalize(left, right);
    return right - left + 1;
  }
  size_t GetRebuildCount() const { return 0; }

  template <class Func> void ReadQuery(size_t left, size_t right, Func &&func) {
    if (size_ == 0) return;
    Normalize(left, right);
    dynamic_lock_detail::RangeLockGuard<decltype(locks_), true> guard(locks_, left, right);
    func(left, right);
  }

private:
  void Normalize(size_t &left, size_t &right) const {
    left = std::min(left, size_ - 1);
    right = std::min(right, size_ - 1);
    if (left > right) std::swap(left, right);
  }
  const size_t size_;
  std::unique_ptr<Slot[]> locks_;
};
