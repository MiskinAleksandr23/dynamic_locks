#pragma once

// Standalone compatibility interfaces used by the vendored LockTree only.
// These are not a complete RocksDB API or an ABI-compatible replacement for it.
// The namespace is deliberately separate from a real RocksDB installation.
#ifndef ROCKSDB_NAMESPACE
#define ROCKSDB_NAMESPACE locktree_rocksdb_compat
#endif

#include <cstdint>
#include <memory>

namespace ROCKSDB_NAMESPACE {

class Status {
 public:
  static Status OK() noexcept { return Status(true); }
  static Status TimedOut() noexcept { return Status(false); }
  bool ok() const noexcept { return ok_; }

 private:
  explicit Status(bool value) noexcept : ok_(value) {}
  bool ok_;
};

class TransactionDBMutex {
 public:
  virtual ~TransactionDBMutex() = default;
  virtual Status Lock() = 0;
  virtual void UnLock() = 0;
};

class TransactionDBCondVar {
 public:
  virtual ~TransactionDBCondVar() = default;
  virtual Status WaitFor(std::shared_ptr<TransactionDBMutex> mutex,
                         int64_t timeout_microseconds) = 0;
  virtual void Notify() = 0;
  virtual void NotifyAll() = 0;
};

class TransactionDBMutexFactory {
 public:
  virtual ~TransactionDBMutexFactory() = default;
  virtual std::shared_ptr<TransactionDBMutex> AllocateMutex() = 0;
  virtual std::shared_ptr<TransactionDBCondVar> AllocateCondVar() = 0;
};

} // namespace ROCKSDB_NAMESPACE
