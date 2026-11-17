#include "common/locktree_lock.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

// Native headers are confined to this TU: they define portability macros and
// poison legacy atomic builtins. They must not leak into other lock algorithms.
#include "locktree_port.hpp"
#include "locktree/lock_request.h"
#include "locktree/locktree.h"
#include "locktree/range_buffer.h"

struct LockTreeRangeLock::Impl {
public:
  explicit Impl(size_t size)
      : size_(size), factory_(locktree_port::MakeMutexFactory()) {
    comparator_.create(&Compare, nullptr);
    manager_.create(nullptr, nullptr, nullptr, nullptr, factory_);
    tree_ = manager_.get_lt(DICTIONARY_ID{1}, comparator_, nullptr);
    if (tree_ == nullptr) {
      manager_.destroy();
      comparator_.destroy();
      throw std::runtime_error("LockTree creation failed");
    }
  }

  ~Impl() {
    // As with the other range locks, destruction requires no active callbacks.
    manager_.release_lt(tree_);
    manager_.destroy();
    comparator_.destroy();
  }
  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  void Query(size_t left, size_t right, bool write, void *context,
             void (*callback)(void *, size_t, size_t)) {
    if (size_ == 0) return;
    Normalize(left, right);
    Reservation held(*this, left, right, write);
    callback(context, left, right);
  }

private:
  static int Compare(void *, const DBT *left, const DBT *right) {
    // Native DBTs can refer to unaligned arena storage.
    uint64_t a, b;
    std::memcpy(&a, left->data, sizeof a);
    std::memcpy(&b, right->data, sizeof b);
    return (a > b) - (a < b);
  }
  void Normalize(size_t &left, size_t &right) const {
    left = std::min(left, size_ - 1);
    right = std::min(right, size_ - 1);
    if (left > right) std::swap(left, right);
  }

  class Reservation {
  public:
    Reservation(Impl &owner, uint64_t left, uint64_t right, bool write)
        : owner_(owner), left_(left), right_(right),
          txn_(static_cast<TXNID>(reinterpret_cast<uintptr_t>(this))) {
      static_assert(sizeof(uintptr_t) <= sizeof(TXNID));
      // Like RocksDB's transaction-pointer IDs, the live reservation address
      // identifies its owner. Distinct concurrent stack objects have distinct IDs;
      // it is reused only after all locks are released and waiters retried.
      if (txn_ == TXNID_NONE || txn_ == TXNID_SHARED || txn_ == TXNID_ANY)
        throw std::runtime_error("invalid LockTree transaction identifier");
      toku_fill_dbt(&left_key_, &left_, sizeof left_);
      toku_fill_dbt(&right_key_, &right_, sizeof right_);
      // LockTree key ranges are inclusive: no right+1 conversion is needed.
      ranges_.create();
      ranges_.append(&left_key_, &right_key_, write);
      request_.create(owner_.factory_);
      request_.set(owner_.tree_, txn_, &left_key_, &right_key_,
                   write ? toku::lock_request::WRITE : toku::lock_request::READ, false);
      int result = request_.start();
      // Same finite stand-in for an infinite wait as RocksDB's native adapter.
      constexpr uint64_t one_year_ms = uint64_t{1000} * 60 * 60 * 24 * 365;
      if (result == DB_LOCK_NOTGRANTED) result = request_.wait(one_year_ms);
      request_.destroy();
      if (result != 0) {
        ranges_.destroy();
        throw std::runtime_error("LockTree acquisition failed: " + std::to_string(result));
      }
    }
    ~Reservation() {
      // Each reservation is one transaction holding exactly one range.
      owner_.tree_->release_locks(txn_, &ranges_, true);
      ranges_.destroy();
      toku::lock_request::retry_all_lock_requests(owner_.tree_);
    }
    Reservation(const Reservation &) = delete;
    Reservation &operator=(const Reservation &) = delete;
  private:
    Impl &owner_;
    uint64_t left_, right_;
    TXNID txn_;
    DBT left_key_{}, right_key_{};
    toku::range_buffer ranges_;
    toku::lock_request request_;
  };

  const size_t size_;
  toku_external_mutex_factory_t factory_;
  toku::comparator comparator_;
  toku::locktree_manager manager_;
  toku::locktree *tree_ = nullptr;
};

LockTreeRangeLock::LockTreeRangeLock(size_t size) : impl_(std::make_unique<Impl>(size)) {}
LockTreeRangeLock::~LockTreeRangeLock() = default;

void LockTreeRangeLock::Invoke(size_t left, size_t right, bool write, void *context,
                               void (*callback)(void *, size_t, size_t)) {
  impl_->Query(left, right, write, context, callback);
}
