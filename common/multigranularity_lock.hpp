#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

// Classical multi-granularity locking over a fixed binary hierarchy of regions.
// Partial nodes take IS/IX; maximal fully covered nodes take S/X. All acquired
// modes remain held until the callback returns, including during exception unwind.
// Every query follows the same tree preorder, without upgrades: parent first,
// then the left subtree before the right. This avoids lock-order cycles.
// There is no FIFO or starvation-freedom guarantee. Callbacks must not recursively
// acquire this object, and destruction requires that all callbacks have finished.
class MultiGranularityRangeLock {
  enum class Mode : unsigned char { IS, IX, S, X };

  // Three independent 21-bit counters plus the exclusive bit. A failed counter
  // increment throws rather than carrying into an adjacent mode's field.
  static constexpr uint64_t kCounterMask = (uint64_t{1} << 21) - 1;
  static constexpr uint64_t kIS = 1;
  static constexpr uint64_t kIX = uint64_t{1} << 21;
  static constexpr uint64_t kS = uint64_t{1} << 42;
  static constexpr uint64_t kX = uint64_t{1} << 63;
  static constexpr uint64_t kIXMask = kCounterMask << 21;
  static constexpr uint64_t kSMask = kCounterMask << 42;

  // Match the other benchmark lock slots' spacing on the local ARM machine.
  // The complete footprint is exposed rather than pretending this uses K locks.
  struct alignas(128) Node {
    std::atomic<uint64_t> state{0};

    static bool Compatible(uint64_t value, Mode mode) noexcept {
      switch (mode) {
      case Mode::IS: return (value & kX) == 0;
      case Mode::IX: return (value & (kX | kSMask)) == 0;
      case Mode::S: return (value & (kX | kIXMask)) == 0;
      case Mode::X: return value == 0;
      }
      return false;
    }
    static uint64_t Increment(Mode mode) noexcept {
      switch (mode) {
      case Mode::IS: return kIS;
      case Mode::IX: return kIX;
      case Mode::S: return kS;
      case Mode::X: return kX;
      }
      return 0;
    }
    static uint64_t CounterField(Mode mode) noexcept {
      switch (mode) {
      case Mode::IS: return kCounterMask;
      case Mode::IX: return kIXMask;
      case Mode::S: return kSMask;
      case Mode::X: return 0;
      }
      return 0;
    }
    void Acquire(Mode mode) {
      uint64_t observed = state.load(std::memory_order_relaxed);
      const uint64_t increment = Increment(mode);
      const uint64_t field = CounterField(mode);
      for (;;) {
        if (Compatible(observed, mode)) {
          if (field != 0 && (observed & field) == field)
            throw std::overflow_error("multi-granularity mode counter exhausted");
          if (state.compare_exchange_weak(observed, observed + increment,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed))
            return;
        } else {
          // A release always notifies. Wait rechecks the value, so a release
          // between the predicate test and waiting cannot be missed.
          state.wait(observed, std::memory_order_relaxed);
          observed = state.load(std::memory_order_relaxed);
        }
      }
    }
    void Release(Mode mode) noexcept {
      state.fetch_sub(Increment(mode), std::memory_order_release);
      state.notify_all();
    }
  };

  struct Held {
    size_t node;
    Mode mode;
  };
  // At each level a contiguous interval has at most two partial nodes and two
  // completely covered boundary siblings. This bound covers every size_t tree.
  static constexpr size_t kMaxHeld = 4 * std::numeric_limits<size_t>::digits + 1;

  class Guard {
  public:
    explicit Guard(Node *nodes) noexcept : nodes_(nodes) {}
    ~Guard() { while (used_ != 0) { const auto held = held_[--used_]; nodes_[held.node].Release(held.mode); } }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;

    void Acquire(size_t node, Mode mode) {
      if (used_ == held_.size()) throw std::length_error("multi-granularity cover exceeds stack bound");
      nodes_[node].Acquire(mode);
      held_[used_++] = Held{node, mode};
    }

  private:
    Node *nodes_;
    size_t used_ = 0;
    // Intentionally not value-initialized: touch only the records actually used.
    std::array<Held, kMaxHeld> held_;
  };

public:
  explicit MultiGranularityRangeLock(size_t size, size_t leaf_count = 1024)
      : size_(size), leaves_(LeafCount(size, leaf_count)),
        leaf_width_(size == 0 ? 1 : 1 + (size - 1) / leaves_),
        node_count_(NodeCount(leaves_)), nodes_(std::make_unique<Node[]>(node_count_)) {}

  MultiGranularityRangeLock(const MultiGranularityRangeLock &) = delete;
  MultiGranularityRangeLock &operator=(const MultiGranularityRangeLock &) = delete;

  template <class Func> void ReadQuery(size_t left, size_t right, Func &&func) {
    Query<true>(left, right, std::forward<Func>(func));
  }
  template <class Func> void WriteQuery(size_t left, size_t right, Func &&func) {
    Query<false>(left, right, std::forward<Func>(func));
  }

  size_t GetRebuildCount() const noexcept { return 0; }
  size_t GetLeafCount() const noexcept { return leaves_; }
  size_t GetLeafWidth() const noexcept { return leaf_width_; }
  size_t GetNodeCount() const noexcept { return node_count_; }
  size_t GetAllocatedBytes() const noexcept { return node_count_ * sizeof(Node); }

private:
  static size_t LeafCount(size_t size, size_t requested) {
    if (!std::has_single_bit(requested))
      throw std::invalid_argument("multi-granularity leaf count must be a positive power of two");
    return size == 0 ? 0 : std::bit_floor(std::min(size, requested));
  }
  static size_t NodeCount(size_t leaves) {
    if (leaves == 0) return 0;
    // The largest possible power-of-two leaf count is at most (SIZE_MAX+1)/2.
    const size_t count = leaves + (leaves - 1);
    if (count > std::numeric_limits<size_t>::max() / sizeof(Node))
      throw std::length_error("multi-granularity hierarchy is too large");
    return count;
  }

  template <bool Read>
  void Cover(Guard &guard, size_t node, size_t first, size_t past,
             size_t query_first, size_t query_last) {
    const bool complete = query_first <= first && past - 1 <= query_last;
    guard.Acquire(node, complete ? (Read ? Mode::S : Mode::X) : (Read ? Mode::IS : Mode::IX));
    if (complete) return;
    const size_t middle = first + (past - first) / 2;
    if (query_first < middle)
      Cover<Read>(guard, node * 2 + 1, first, middle, query_first, query_last);
    if (query_last >= middle)
      Cover<Read>(guard, node * 2 + 2, middle, past, query_first, query_last);
  }

  template <bool Read, class Func> void Query(size_t left, size_t right, Func &&func) {
    if (size_ == 0) return;
    left = std::min(left, size_ - 1);
    right = std::min(right, size_ - 1);
    if (left > right) std::swap(left, right);
    Guard guard(nodes_.get());
    Cover<Read>(guard, 0, 0, leaves_, left / leaf_width_, right / leaf_width_);
    func(left, right);
  }

  const size_t size_, leaves_, leaf_width_, node_count_;
  std::unique_ptr<Node[]> nodes_;
};
