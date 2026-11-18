#pragma once

#include "common/range_lock.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

// Local C++20 adaptation of SRL(array), Kim et al., IEEE Access 2021,
// "Parallelizing Shared File I/O Operations of NVM File System for Manycore
// Servers", Algorithm 2 and Section VI. DOI: 10.1109/ACCESS.2021.3054905.
// https://discos.sogang.ac.kr/file/2021/intl_jour/IEEE_ACCESS_2021_J_H_Kim.pdf
//
// One dense 32-bit RW state per fixed byte segment: bit 31 is the writer flag,
// bits 0..30 count readers. Segments are acquired in increasing order and held
// through the callback. As in Algorithm 2, incoming readers may bypass waiting
// writers; this implementation does not add aging or a fairness guarantee.
//
// Adaptations: uint64_t array indices replace file byte offsets; C++ acquire/
// release atomics replace Linux atomics/barriers; failed attempts use CPU pause
// hints. Reader-count overflow throws instead of corrupting the writer bit.
// There is no filesystem/radix tree, allocation during queries, or padding per
// segment. Callbacks must not acquire this object recursively, and destruction
// requires that every callback has finished.
class SrlArrayLock {
  static constexpr uint32_t kWriter = uint32_t{1} << 31;
  static constexpr uint32_t kReaders = kWriter - 1;

  static void Pause() noexcept {
#if defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    // Portable compiler hint only; no scheduler policy is introduced.
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
  }

  struct ReaderWriterLock {
    std::atomic<uint32_t> state{0};

    void lock() noexcept {
      uint32_t expected = 0;
      while (!state.compare_exchange_weak(expected, kWriter,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
        Pause();
        expected = 0;
      }
    }

    void unlock() noexcept {
      state.fetch_and(~kWriter, std::memory_order_release);
    }

    void lock_shared() {
      uint32_t observed = state.load(std::memory_order_relaxed);
      for (;;) {
        if ((observed & kWriter) == 0) {
          if (observed == kReaders) {
            throw std::overflow_error("SRL segment reader counter exhausted");
          }
          // Equivalent to atomic_add_unless(state, 1, writer_flag), with an
          // explicit bound check before incrementing the reader count.
          if (state.compare_exchange_weak(observed, observed + 1,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
            return;
          }
        } else {
          observed = state.load(std::memory_order_relaxed);
        }
        Pause();
      }
    }

    void unlock_shared() noexcept {
      state.fetch_sub(1, std::memory_order_release);
    }
  };

  // The common exception-safe guard expects a .mutex member in each slot.
  struct Segment { ReaderWriterLock mutex; };
  static_assert(std::atomic<uint32_t>::is_always_lock_free,
                "SRL requires lock-free 32-bit atomic states");
  static_assert(sizeof(Segment) == sizeof(uint32_t),
                "SRL segment states must remain dense 32-bit values");

public:
  explicit SrlArrayLock(size_t elements, size_t segment_kib)
      : elements_(elements), segment_kib_(segment_kib),
        segment_elements_(SegmentElements(elements, segment_kib)),
        segment_count_(elements == 0 ? 0 : 1 + (elements - 1) / segment_elements_),
        segments_(std::make_unique<Segment[]>(segment_count_)) {}

  SrlArrayLock(const SrlArrayLock &) = delete;
  SrlArrayLock &operator=(const SrlArrayLock &) = delete;

  template <class Func> void WriteQuery(size_t left, size_t right, Func &&func) {
    Query<false>(left, right, std::forward<Func>(func));
  }
  template <class Func> void ReadQuery(size_t left, size_t right, Func &&func) {
    Query<true>(left, right, std::forward<Func>(func));
  }

  size_t GetRebuildCount() const noexcept { return 0; }
  size_t GetSegmentCount() const noexcept { return segment_count_; }
  size_t GetSegmentKiB() const noexcept { return segment_kib_; }
  size_t GetAllocatedBytes() const noexcept { return segment_count_ * sizeof(Segment); }

private:
  static size_t SegmentElements(size_t elements, size_t segment_kib) {
    if (segment_kib == 0 || segment_kib % 4 != 0) {
      throw std::invalid_argument("SRL segment size must be a positive multiple of 4 KiB");
    }
    if (segment_kib > std::numeric_limits<size_t>::max() / 1024 ||
        elements > std::numeric_limits<size_t>::max() / sizeof(uint64_t)) {
      throw std::length_error("SRL byte size exceeds size_t");
    }
    return segment_kib * 1024 / sizeof(uint64_t);
  }

  // Inclusive endpoints match the other array locks: clamp, then sort. An
  // empty array skips the callback. The full range is locked before it runs.
  template <bool Read, class Func> void Query(size_t left, size_t right, Func &&func) {
    if (elements_ == 0) return;
    left = std::min(left, elements_ - 1);
    right = std::min(right, elements_ - 1);
    if (left > right) std::swap(left, right);
    dynamic_lock_detail::RangeLockGuard<decltype(segments_), Read> guard(
        segments_, left / segment_elements_, right / segment_elements_);
    std::forward<Func>(func)(left, right);
  }

  const size_t elements_, segment_kib_, segment_elements_, segment_count_;
  std::unique_ptr<Segment[]> segments_;
};
