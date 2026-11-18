#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

// Local C++20 implementation of Kogan, Dice and Issa, EuroSys 2020:
// https://arxiv.org/html/2006.12144
// Section 4.1, exclusive list algorithm, with the per-thread pool/epoch
// reclamation scheme from Section 4.4 (128 nodes per initial pool).
// All linked-list and epoch atomics are sequentially consistent as in the
// published pseudocode. This is the exclusive-only list variant: ReadQuery
// also obtains exclusive access. Fairness and Section 4.5's fast path are absent.
// Callbacks must not recursively acquire the same lock. Destroy at quiescence.
class KdiExclusiveLock {
  struct Node {
    size_t begin = 0, end = 0; // Half-open interval.
    std::atomic<uintptr_t> next{0};
    Node *pool_next = nullptr; // Private free-list linkage, never traversed by readers.
  };
  static_assert(alignof(Node) >= 2);
  static_assert(std::atomic<uintptr_t>::is_always_lock_free);
  static constexpr size_t kPoolSize = 128;

  struct alignas(128) ThreadState {
    std::atomic<uint64_t> epoch{0}; // Odd while traversing/inserting into the list.
    Node *available = nullptr, *retired = nullptr;
    size_t available_count = 0, retired_count = 0;
    bool in_query = false;
  };
  struct Registration { uint64_t instance; ThreadState *state; };

  static void Pause() noexcept {
#if defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
  }
  static bool Marked(uintptr_t pointer) noexcept { return (pointer & 1) != 0; }
  static Node *Pointer(uintptr_t pointer) noexcept {
    return reinterpret_cast<Node *>(pointer & ~uintptr_t{1});
  }
  static uintptr_t Bits(Node *node) noexcept { return reinterpret_cast<uintptr_t>(node); }

  ThreadState &State() {
    // Generation IDs distinguish destroyed/recreated objects at the same address.
    // Entries remain inert until thread exit after an instance is destroyed.
    static thread_local std::vector<Registration> registrations;
    for (const auto &entry : registrations)
      if (entry.instance == instance_) return *entry.state;
    const size_t slot = registered_.fetch_add(1);
    if (slot >= max_threads_)
      throw std::length_error("KDI maximum registered worker count exceeded");
    registrations.push_back({instance_, &states_[slot]});
    return states_[slot];
  }

  void Recycle(ThreadState &state) {
    // Called before entering our acquisition epoch. A peer that started after
    // an unlink cannot reach that retired node. Wait for every earlier odd
    // epoch to change before reclaiming/reusing nodes from this local list.
    const size_t count = std::min(registered_.load(), max_threads_);
    for (size_t i = 0; i < count; ++i) {
      const uint64_t observed = states_[i].epoch.load();
      if ((observed & 1) != 0)
        while (states_[i].epoch.load() == observed) Pause();
    }
    while (state.retired_count > 2 * kPoolSize) {
      Node *node = state.retired;
      state.retired = node->pool_next;
      --state.retired_count;
      delete node;
    }
    state.available = state.retired;
    state.available_count = state.retired_count;
    state.retired = nullptr;
    state.retired_count = 0;
    if (state.available_count < kPoolSize / 2) {
      while (state.available_count < kPoolSize) {
        Node *node = new Node;
        node->pool_next = state.available;
        state.available = node;
        ++state.available_count;
      }
    }
  }

  Node *Allocate(ThreadState &state, size_t begin, size_t end) {
    if (state.available == nullptr) Recycle(state);
    Node *node = state.available;
    state.available = node->pool_next;
    --state.available_count;
    node->begin = begin;
    node->end = end;
    node->next.store(0);
    return node;
  }

  void Insert(ThreadState &state, Node *mine) noexcept {
    for (;;) {
      auto *previous = &head_;
      uintptr_t current = previous->load();
      for (;;) {
        if (Marked(current)) break; // The predecessor was logically removed.
        Node *node = Pointer(current);
        const uintptr_t next = node ? node->next.load() : 0;
        if (node && Marked(next)) {
          const uintptr_t successor = next & ~uintptr_t{1};
          if (previous->compare_exchange_strong(current, successor)) {
            // Exactly the successful physical remover owns retirement.
            node->pool_next = state.retired;
            state.retired = node;
            ++state.retired_count;
            current = successor;
          }
          continue; // CAS failure refreshed current, including predecessor marks.
        }
        if (node && node->end <= mine->begin) {
          previous = &node->next;
          current = next;
        } else if (!node || mine->end <= node->begin) {
          mine->next.store(current);
          if (previous->compare_exchange_strong(current, Bits(mine))) return;
        } else {
          // Our epoch keeps the conflicting node alive until we stop observing it.
          while (!Marked(node->next.load())) Pause();
        }
      }
    }
  }

  class Release {
  public:
    Release(Node *node, ThreadState &state) noexcept : node_(node), state_(state) {}
    ~Release() {
      node_->next.fetch_or(1); // Only the owner logically removes this node.
      state_.in_query = false;
    }
    Release(const Release &) = delete;
  private:
    Node *node_;
    ThreadState &state_;
  };

  template <class Func> void Query(size_t left, size_t right, Func &&func) {
    if (size_ == 0) return;
    left = std::min(left, size_ - 1);
    right = std::min(right, size_ - 1);
    if (left > right) std::swap(left, right);
    auto &state = State();
    if (state.in_query) throw std::logic_error("recursive KDI range acquisition");
    Node *node = Allocate(state, left, right + 1);
    state.in_query = true;
    state.epoch.fetch_add(1);
    Insert(state, node);
    state.epoch.fetch_add(1);
    Release guard(node, state);
    std::forward<Func>(func)(left, right);
  }

  static void DeletePool(Node *node) noexcept {
    while (node) {
      Node *next = node->pool_next;
      delete node;
      node = next;
    }
  }

public:
  // max_threads counts distinct querying threads over this instance's lifetime.
  explicit KdiExclusiveLock(size_t elements, size_t max_threads = 128)
      : size_(elements), max_threads_(max_threads), instance_(next_instance_.fetch_add(1)),
        states_(std::make_unique<ThreadState[]>(max_threads)) {
    if (!max_threads) throw std::invalid_argument("KDI maximum worker count must be positive");
    if (!instance_) throw std::overflow_error("KDI instance identifier exhausted");
  }
  KdiExclusiveLock(const KdiExclusiveLock &) = delete;
  KdiExclusiveLock &operator=(const KdiExclusiveLock &) = delete;
  ~KdiExclusiveLock() {
    Node *node = Pointer(head_.load());
    while (node) {
      Node *next = Pointer(node->next.load());
      delete node;
      node = next;
    }
    for (size_t i = 0; i < max_threads_; ++i) {
      DeletePool(states_[i].available);
      DeletePool(states_[i].retired);
    }
  }
  template <class Func> void WriteQuery(size_t left, size_t right, Func &&func) {
    Query(left, right, std::forward<Func>(func));
  }
  template <class Func> void ReadQuery(size_t left, size_t right, Func &&func) {
    Query(left, right, std::forward<Func>(func));
  }
  size_t GetRebuildCount() const noexcept { return 0; }

private:
  const size_t size_, max_threads_;
  const uint64_t instance_;
  std::unique_ptr<ThreadState[]> states_;
  std::atomic<size_t> registered_{0};
  std::atomic<uintptr_t> head_{0};
  inline static std::atomic<uint64_t> next_instance_{1};
};
