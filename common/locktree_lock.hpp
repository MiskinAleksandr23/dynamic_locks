#pragma once

#include <cstddef>
#include <memory>

// Optional PerconaFT LockTree / RocksDB userspace range-lock baseline.
// Its native headers and synchronization macros are isolated in the .cpp.
class LockTreeRangeLock {
public:
  explicit LockTreeRangeLock(size_t size);
  ~LockTreeRangeLock();
  LockTreeRangeLock(const LockTreeRangeLock &) = delete;
  LockTreeRangeLock &operator=(const LockTreeRangeLock &) = delete;

  template <class Func> void WriteQuery(size_t left, size_t right, Func &&func) {
    auto callback = [&](size_t a, size_t b) { func(a, b); };
    Invoke(left, right, true, &callback, [](void *context, size_t a, size_t b) {
      (*static_cast<decltype(callback) *>(context))(a, b);
    });
  }
  template <class Func> void ReadQuery(size_t left, size_t right, Func &&func) {
    auto callback = [&](size_t a, size_t b) { func(a, b); };
    Invoke(left, right, false, &callback, [](void *context, size_t a, size_t b) {
      (*static_cast<decltype(callback) *>(context))(a, b);
    });
  }
  size_t GetRebuildCount() const { return 0; }

private:
  // The callback is invoked synchronously and is never retained. This type
  // erasure adds no per-query heap allocation or additional lock.
  void Invoke(size_t left, size_t right, bool write, void *context,
              void (*callback)(void *, size_t, size_t));
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
