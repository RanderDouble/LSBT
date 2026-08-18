#pragma once

#include "LSBTState.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace lsbt_detail {

class TimingWheel {
 public:
  struct Candidate {
    std::uint32_t key_id = 0;
    std::size_t physical_remaining = 0;
  };

  TimingWheel(std::size_t bucket_count, StateTable& states)
      : bucket_count_(bucket_count),
        states_(states),
        bucket_heads_(bucket_count_, kQueueEnd),
        bucket_tails_(bucket_count_, kQueueEnd),
        bucket_bitmap_((bucket_count_ + 63) / 64, 0) {}

  void Schedule(std::uint32_t key_id, State& state,
                std::size_t remaining) {
    if (remaining == 0) {
      PushExpiredTail(key_id, state);
      return;
    }
    PushBucketTail(key_id, state,
                   (cursor_ + remaining) % bucket_count_);
  }

  void Advance() {
    cursor_ = (cursor_ + 1) % bucket_count_;
    const std::size_t due_bucket = cursor_;
    const std::int32_t due_head = bucket_heads_[due_bucket];
    if (due_head == kQueueEnd) {
      return;
    }
    if (expired_tail_ == kQueueEnd) {
      expired_head_ = due_head;
    } else {
      states_.at(static_cast<std::uint32_t>(expired_tail_)).next_resident =
          due_head;
    }
    expired_tail_ = bucket_tails_[due_bucket];
    bucket_heads_[due_bucket] = kQueueEnd;
    bucket_tails_[due_bucket] = kQueueEnd;
    ClearBucketBitIfEmpty(due_bucket);
  }

  std::optional<Candidate> Front() const {
    const QueueSource source = FindSource();
    if (!source.expired && source.bucket == bucket_count_) {
      return std::nullopt;
    }
    const std::int32_t key = SourceHead(source);
    if (key == kQueueEnd) {
      return std::nullopt;
    }
    return Candidate{static_cast<std::uint32_t>(key),
                     PhysicalRemaining(source)};
  }

  std::uint32_t PopFront() {
    const QueueSource source = FindSource();
    if ((!source.expired && source.bucket == bucket_count_) ||
        SourceHead(source) == kQueueEnd) {
      std::abort();
    }
    return source.expired ? PopExpiredHead()
                          : PopBucketHead(source.bucket);
  }

 private:
  struct QueueSource {
    bool expired = false;
    std::size_t bucket = 0;
  };

  void SetBucketBit(std::size_t bucket) {
    bucket_bitmap_[bucket / 64] |=
        std::uint64_t{1} << (bucket % 64);
  }

  void ClearBucketBitIfEmpty(std::size_t bucket) {
    if (bucket_heads_[bucket] == kQueueEnd) {
      bucket_bitmap_[bucket / 64] &=
          ~(std::uint64_t{1} << (bucket % 64));
    }
  }

  void PushBucketTail(std::uint32_t key_id, State& state,
                      std::size_t bucket) {
    state.next_resident = kQueueEnd;
    if (bucket_tails_[bucket] == kQueueEnd) {
      bucket_heads_[bucket] = static_cast<std::int32_t>(key_id);
      SetBucketBit(bucket);
    } else {
      states_.at(static_cast<std::uint32_t>(bucket_tails_[bucket]))
          .next_resident = static_cast<std::int32_t>(key_id);
    }
    bucket_tails_[bucket] = static_cast<std::int32_t>(key_id);
  }

  std::uint32_t PopBucketHead(std::size_t bucket) {
    const std::int32_t head = bucket_heads_[bucket];
    const std::int32_t next =
        states_.at(static_cast<std::uint32_t>(head)).next_resident;
    bucket_heads_[bucket] = next;
    if (next == kQueueEnd) {
      bucket_tails_[bucket] = kQueueEnd;
      ClearBucketBitIfEmpty(bucket);
    }
    return static_cast<std::uint32_t>(head);
  }

  void PushExpiredTail(std::uint32_t key_id, State& state) {
    state.next_resident = kQueueEnd;
    if (expired_tail_ == kQueueEnd) {
      expired_head_ = static_cast<std::int32_t>(key_id);
    } else {
      states_.at(static_cast<std::uint32_t>(expired_tail_)).next_resident =
          static_cast<std::int32_t>(key_id);
    }
    expired_tail_ = static_cast<std::int32_t>(key_id);
  }

  std::uint32_t PopExpiredHead() {
    const std::int32_t head = expired_head_;
    const std::int32_t next =
        states_.at(static_cast<std::uint32_t>(head)).next_resident;
    expired_head_ = next;
    if (next == kQueueEnd) {
      expired_tail_ = kQueueEnd;
    }
    return static_cast<std::uint32_t>(head);
  }

  static unsigned TrailingZeros(std::uint64_t value) {
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanForward64(&index, value);
    return static_cast<unsigned>(index);
#else
    return static_cast<unsigned>(__builtin_ctzll(value));
#endif
  }

  std::size_t FindNonEmpty(std::size_t start) const {
    if (start >= bucket_count_) {
      return bucket_count_;
    }
    std::size_t word_index = start / 64;
    const std::size_t bit_index = start % 64;
    std::uint64_t word = bucket_bitmap_[word_index];
    if (bit_index != 0) {
      word &= ~std::uint64_t{0} << bit_index;
    }
    if (word != 0) {
      return word_index * 64 + TrailingZeros(word);
    }
    for (++word_index; word_index < bucket_bitmap_.size(); ++word_index) {
      word = bucket_bitmap_[word_index];
      if (word != 0) {
        return word_index * 64 + TrailingZeros(word);
      }
    }
    return bucket_count_;
  }

  std::size_t FindNextBucket() const {
    const std::size_t after = FindNonEmpty(cursor_ + 1);
    if (after != bucket_count_) {
      return after;
    }
    const std::size_t wrapped = FindNonEmpty(0);
    return wrapped < cursor_ ? wrapped : bucket_count_;
  }

  QueueSource FindSource() const {
    if (expired_head_ != kQueueEnd) {
      return QueueSource{true, bucket_count_};
    }
    return QueueSource{false, FindNextBucket()};
  }

  std::int32_t SourceHead(const QueueSource& source) const {
    return source.expired ? expired_head_ : bucket_heads_[source.bucket];
  }

  std::size_t PhysicalRemaining(const QueueSource& source) const {
    if (source.expired) {
      return 0;
    }
    return (source.bucket + bucket_count_ - cursor_) % bucket_count_;
  }

  std::size_t bucket_count_ = 1;
  StateTable& states_;
  std::vector<std::int32_t> bucket_heads_;
  std::vector<std::int32_t> bucket_tails_;
  std::vector<std::uint64_t> bucket_bitmap_;
  std::size_t cursor_ = 0;
  std::int32_t expired_head_ = kQueueEnd;
  std::int32_t expired_tail_ = kQueueEnd;
};

}  // namespace lsbt_detail
