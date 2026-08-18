#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace packed_lsbt {

inline constexpr std::int32_t kQueueEnd = -1;
inline constexpr std::int32_t kNotResident = -2;
inline constexpr std::int32_t kGhost = -3;
inline constexpr std::uint32_t kRenewalBit = 0x80000000u;
inline constexpr std::uint32_t kExpiryMask = 0x7fffffffu;

struct MacroState {
  // High bit of expiry_flags is the one-bit renewal credit.
  std::uint32_t expiry_flags = 0;
  // Resident: next key in timing-wheel queue (>=-1).
  // Absent: kNotResident. Ghost: kGhost.
  std::int32_t next_resident = kNotResident;
  std::int32_t ghost_prev = kQueueEnd;
  std::int32_t ghost_next = kQueueEnd;
};
static_assert(sizeof(MacroState) == 16);

struct MicroState {
  // Same packed expiry/renewal representation; no ghost fields.
  std::uint32_t expiry_flags = 0;
  std::int32_t next_resident = kNotResident;
};
static_assert(sizeof(MicroState) == 8);

struct Meta {
  MacroState macro;
  MicroState micro;
  // lower 30 bits: epoch id; bit30: listed in micro_keys; bit31: reused.
  std::uint32_t epoch_tag = 0;
};
static_assert(sizeof(Meta) == 28);

inline std::uint32_t Expiry(std::uint32_t flags) {
  return flags & kExpiryMask;
}
inline bool Renewal(std::uint32_t flags) {
  return (flags & kRenewalBit) != 0;
}
inline void SetExpiry(std::uint32_t& flags, std::uint32_t expiry) {
  flags = (flags & kRenewalBit) | (expiry & kExpiryMask);
}
inline void SetRenewal(std::uint32_t& flags, bool value) {
  if (value) flags |= kRenewalBit;
  else flags &= kExpiryMask;
}

inline bool IsMacroResident(const MacroState& s) {
  return s.next_resident != kNotResident && s.next_resident != kGhost;
}
inline bool IsMacroGhost(const MacroState& s) {
  return s.next_resident == kGhost;
}
inline bool IsMicroResident(const MicroState& s) {
  return s.next_resident != kNotResident;
}

// Exact LSBT bucket mapping, but the structural prior has only three distinct
// depth classes: depth 1, depth 2, and depth >= 3.
class Scoring {
 public:
  static constexpr double kDefaultRetention = 0.95;

  Scoring(std::size_t capacity, double retention = kDefaultRetention)
      : retention_(std::clamp(retention, 0.92, 0.98)),
        reference_heat_(1.0 / (1.0 - retention_)) {
    Reconfigure(capacity);
  }

  void Reconfigure(std::size_t capacity) {
    bucket_count_ = ComputeBucketCount(capacity);
    max_score_ = bucket_count_ - 1;
    minimum_heat_ = reference_heat_ *
        std::pow(retention_, static_cast<double>(max_score_));
    lut_.assign(3 * bucket_count_, 0);
    for (std::size_t b = 0; b < bucket_count_; ++b) {
      const double base = minimum_heat_ *
          std::pow(1.0 / retention_, static_cast<double>(b));
      for (int cls = 0; cls < 3; ++cls) {
        const double gain = std::pow(retention_, static_cast<double>(cls));
        lut_[static_cast<std::size_t>(cls) * bucket_count_ + b] =
            static_cast<std::uint16_t>(HeatToBucket(base + gain));
      }
    }
  }

  std::size_t bucket_count() const { return bucket_count_; }
  std::size_t max_score() const { return max_score_; }

  std::uint16_t Transition(int depth, std::size_t bucket) const {
    const int cls = depth <= 1 ? 0 : (depth == 2 ? 1 : 2);
    return lut_[static_cast<std::size_t>(cls) * bucket_count_ + bucket];
  }

 private:
  static std::size_t ComputeBucketCount(std::size_t capacity) {
    const double value = static_cast<double>(std::max<std::size_t>(capacity, 1));
    return std::max<std::size_t>(
        1, static_cast<std::size_t>(std::llround(32.0 * std::log2(value + 1.0))));
  }

  std::size_t HeatToBucket(double heat) const {
    if (heat <= 1e-12 || max_score_ == 0) return 0;
    const double bucket = std::log(std::max(heat, 1e-12) / minimum_heat_) /
                          std::log(1.0 / retention_);
    return static_cast<std::size_t>(std::clamp(
        static_cast<long long>(std::llround(bucket)), 0LL,
        static_cast<long long>(max_score_)));
  }

  double retention_ = kDefaultRetention;
  double reference_heat_ = 1.0;
  double minimum_heat_ = 1.0;
  std::size_t bucket_count_ = 1;
  std::size_t max_score_ = 0;
  std::vector<std::uint16_t> lut_;
};

template <class State>
class TimingWheel {
 public:
  struct Candidate {
    std::uint32_t key = 0;
    std::size_t physical_remaining = 0;
  };

  TimingWheel(std::size_t bucket_count, std::vector<Meta>& meta,
              State Meta::* member)
      : meta_(meta), member_(member) {
    Reconfigure(bucket_count);
  }

  void Reconfigure(std::size_t bucket_count) {
    bucket_count_ = std::max<std::size_t>(bucket_count, 1);
    heads_.assign(bucket_count_, kQueueEnd);
    tails_.assign(bucket_count_, kQueueEnd);
    bitmap_.assign((bucket_count_ + 63) / 64, 0);
    cursor_ = 0;
    expired_head_ = expired_tail_ = kQueueEnd;
  }

  void Clear() {
    std::fill(heads_.begin(), heads_.end(), kQueueEnd);
    std::fill(tails_.begin(), tails_.end(), kQueueEnd);
    std::fill(bitmap_.begin(), bitmap_.end(), 0);
    cursor_ = 0;
    expired_head_ = expired_tail_ = kQueueEnd;
  }

  void Schedule(std::uint32_t key, State& state, std::size_t remaining) {
    if (remaining == 0) PushExpiredTail(key, state);
    else PushBucketTail(key, state, (cursor_ + remaining) % bucket_count_);
  }

  void Advance() {
    cursor_ = (cursor_ + 1) % bucket_count_;
    const std::size_t b = cursor_;
    if (heads_[b] == kQueueEnd) return;
    if (expired_tail_ == kQueueEnd) expired_head_ = heads_[b];
    else StateAt(static_cast<std::uint32_t>(expired_tail_)).next_resident = heads_[b];
    expired_tail_ = tails_[b];
    heads_[b] = tails_[b] = kQueueEnd;
    ClearBitIfEmpty(b);
  }

  std::optional<Candidate> Front() const {
    const Source src = FindSource();
    if (!src.expired && src.bucket == bucket_count_) return std::nullopt;
    const std::int32_t key = src.expired ? expired_head_ : heads_[src.bucket];
    if (key == kQueueEnd) return std::nullopt;
    return Candidate{static_cast<std::uint32_t>(key), Remaining(src)};
  }

  std::uint32_t PopFront() {
    const Source src = FindSource();
    if (src.expired) return PopExpired();
    if (src.bucket == bucket_count_) throw std::runtime_error("empty timing wheel");
    return PopBucket(src.bucket);
  }

 private:
  struct Source { bool expired; std::size_t bucket; };

  State& StateAt(std::uint32_t key) { return meta_[key].*member_; }
  const State& StateAt(std::uint32_t key) const { return meta_[key].*member_; }

  void SetBit(std::size_t b) { bitmap_[b / 64] |= std::uint64_t{1} << (b % 64); }
  void ClearBitIfEmpty(std::size_t b) {
    if (heads_[b] == kQueueEnd) bitmap_[b / 64] &= ~(std::uint64_t{1} << (b % 64));
  }

  void PushBucketTail(std::uint32_t key, State& s, std::size_t b) {
    s.next_resident = kQueueEnd;
    if (tails_[b] == kQueueEnd) {
      heads_[b] = static_cast<std::int32_t>(key);
      SetBit(b);
    } else {
      StateAt(static_cast<std::uint32_t>(tails_[b])).next_resident =
          static_cast<std::int32_t>(key);
    }
    tails_[b] = static_cast<std::int32_t>(key);
  }

  void PushExpiredTail(std::uint32_t key, State& s) {
    s.next_resident = kQueueEnd;
    if (expired_tail_ == kQueueEnd) expired_head_ = static_cast<std::int32_t>(key);
    else StateAt(static_cast<std::uint32_t>(expired_tail_)).next_resident =
        static_cast<std::int32_t>(key);
    expired_tail_ = static_cast<std::int32_t>(key);
  }

  std::uint32_t PopBucket(std::size_t b) {
    const auto head = heads_[b];
    const auto next = StateAt(static_cast<std::uint32_t>(head)).next_resident;
    heads_[b] = next;
    if (next == kQueueEnd) {
      tails_[b] = kQueueEnd;
      ClearBitIfEmpty(b);
    }
    return static_cast<std::uint32_t>(head);
  }

  std::uint32_t PopExpired() {
    if (expired_head_ == kQueueEnd) throw std::runtime_error("empty expired queue");
    const auto head = expired_head_;
    const auto next = StateAt(static_cast<std::uint32_t>(head)).next_resident;
    expired_head_ = next;
    if (next == kQueueEnd) expired_tail_ = kQueueEnd;
    return static_cast<std::uint32_t>(head);
  }

  static unsigned Tz(std::uint64_t x) {
#if defined(_MSC_VER)
    unsigned long idx = 0; _BitScanForward64(&idx, x); return static_cast<unsigned>(idx);
#else
    return static_cast<unsigned>(__builtin_ctzll(x));
#endif
  }

  std::size_t FindNonEmpty(std::size_t start) const {
    if (start >= bucket_count_) return bucket_count_;
    std::size_t wi = start / 64;
    const std::size_t bi = start % 64;
    std::uint64_t word = bitmap_[wi];
    if (bi) word &= ~std::uint64_t{0} << bi;
    if (word) return wi * 64 + Tz(word);
    for (++wi; wi < bitmap_.size(); ++wi) if (bitmap_[wi]) return wi * 64 + Tz(bitmap_[wi]);
    return bucket_count_;
  }

  std::size_t FindNextBucket() const {
    const auto after = FindNonEmpty(cursor_ + 1);
    if (after != bucket_count_) return after;
    const auto wrapped = FindNonEmpty(0);
    return wrapped < cursor_ ? wrapped : bucket_count_;
  }

  Source FindSource() const {
    if (expired_head_ != kQueueEnd) return {true, bucket_count_};
    return {false, FindNextBucket()};
  }

  std::size_t Remaining(const Source& src) const {
    if (src.expired) return 0;
    return (src.bucket + bucket_count_ - cursor_) % bucket_count_;
  }

  std::vector<Meta>& meta_;
  State Meta::* member_;
  std::size_t bucket_count_ = 1;
  std::vector<std::int32_t> heads_, tails_;
  std::vector<std::uint64_t> bitmap_;
  std::size_t cursor_ = 0;
  std::int32_t expired_head_ = kQueueEnd, expired_tail_ = kQueueEnd;
};

class MacroGhostFIFO {
 public:
  MacroGhostFIFO(std::size_t capacity, std::vector<Meta>& meta)
      : capacity_(capacity), meta_(meta) {}

  std::size_t size() const { return size_; }
  std::size_t capacity() const { return capacity_; }
  std::optional<std::uint32_t> TakeDropped() {
    auto out = dropped_;
    dropped_.reset();
    return out;
  }

  void Insert(std::uint32_t key, MacroState& s) {
    if (capacity_ == 0 || IsMacroGhost(s)) return;
    if (size_ >= capacity_) EvictOldest();
    s.next_resident = kGhost;
    s.ghost_prev = tail_;
    s.ghost_next = kQueueEnd;
    if (tail_ == kQueueEnd) head_ = static_cast<std::int32_t>(key);
    else meta_[static_cast<std::uint32_t>(tail_)].macro.ghost_next = static_cast<std::int32_t>(key);
    tail_ = static_cast<std::int32_t>(key);
    ++size_;
  }

  void Remove(MacroState& s) {
    if (!IsMacroGhost(s)) return;
    Unlink(s);
    s.next_resident = kNotResident;
    s.ghost_prev = s.ghost_next = kQueueEnd;
    --size_;
  }

 private:
  void Unlink(MacroState& s) {
    if (s.ghost_prev == kQueueEnd) head_ = s.ghost_next;
    else meta_[static_cast<std::uint32_t>(s.ghost_prev)].macro.ghost_next = s.ghost_next;
    if (s.ghost_next == kQueueEnd) tail_ = s.ghost_prev;
    else meta_[static_cast<std::uint32_t>(s.ghost_next)].macro.ghost_prev = s.ghost_prev;
  }

  void EvictOldest() {
    if (head_ == kQueueEnd) throw std::runtime_error("empty ghost fifo");
    const std::uint32_t key = static_cast<std::uint32_t>(head_);
    MacroState& s = meta_[key].macro;
    Unlink(s);
    s = MacroState{};
    --size_;
    dropped_ = key;
  }

  std::size_t capacity_ = 0, size_ = 0;
  std::vector<Meta>& meta_;
  std::int32_t head_ = kQueueEnd, tail_ = kQueueEnd;
  std::optional<std::uint32_t> dropped_;
};

class MacroLSBT {
 public:
  MacroLSBT(std::vector<Meta>& meta, std::size_t capacity,
            double retention = Scoring::kDefaultRetention)
      : meta_(meta), capacity_(capacity), resident_limit_(capacity),
        scoring_(capacity, retention),
        ghost_(ComputeGhostCapacity(capacity), meta_),
        wheel_(scoring_.bucket_count(), meta_, &Meta::macro) {}

  bool Contains(std::uint32_t key) const { return IsMacroResident(meta_[key].macro); }
  std::size_t resident_size() const { return resident_size_; }
  std::size_t ghost_size() const { return ghost_.size(); }
  std::optional<std::uint32_t> TakeDroppedGhost() { return ghost_.TakeDropped(); }

  bool Access(std::uint32_t key, int depth) {
    MacroState& s = meta_[key].macro;
    const bool hit = IsMacroResident(s);
    const bool inserted = !hit && !IsMacroGhost(s);
    if (!hit) ghost_.Insert(key, s);
    const std::size_t cur = CurrentBucket(s);
    const auto next = scoring_.Transition(inserted ? depth : 1, cur);
    SetExpiry(s.expiry_flags, current_version_ + static_cast<std::uint32_t>(next));
    if (hit) SetRenewal(s.expiry_flags, true);
    return hit;
  }

  // Returns admitted? and writes evicted victim to victim (or UINT32_MAX).
  bool Admit(std::uint32_t key, std::uint32_t& victim) {
    victim = UINT32_MAX;
    MacroState& s = meta_[key].macro;
    if (IsMacroResident(s)) return true;
    if (resident_limit_ == 0) return false;
    const bool restore_ghost = IsMacroGhost(s);
    if (restore_ghost) ghost_.Remove(s);
    const std::size_t incoming = CurrentBucket(s);
    if (resident_size_ >= resident_limit_) {
      const EvictResult er = EvictOne(incoming);
      if (!er.accept) {
        if (restore_ghost) ghost_.Insert(key, s);
        else s = MacroState{};
        return false;
      }
      victim = er.victim;
    }
    InsertResident(key, s);
    return true;
  }

  std::uint32_t ForceEvictOne() {
    while (resident_size_) {
      const auto candidate = wheel_.Front();
      if (!candidate) throw std::runtime_error("macro wheel empty");
      MacroState& s = meta_[candidate->key].macro;
      const std::size_t actual = CurrentBucket(s);
      if (actual > candidate->physical_remaining) {
        wheel_.PopFront();
        wheel_.Schedule(candidate->key, s, actual);
        continue;
      }
      wheel_.PopFront();
      const std::uint32_t key = candidate->key;
      s.next_resident = kNotResident;
      SetRenewal(s.expiry_flags, false);
      --resident_size_;
      ghost_.Insert(key, s);
      return key;
    }
    throw std::runtime_error("cannot evict empty macro");
  }

  void SetResidentLimit(std::size_t limit) { resident_limit_ = std::min(limit, capacity_); }
  std::size_t resident_limit() const { return resident_limit_; }

  void Tick() {
    if (current_version_ >= kExpiryMask - static_cast<std::uint32_t>(scoring_.max_score()) - 2)
      throw std::overflow_error("macro clock exhausted");
    ++current_version_;
    wheel_.Advance();
  }

 private:
  struct EvictResult { bool accept = false; std::uint32_t victim = UINT32_MAX; };

  static std::size_t ComputeGhostCapacity(std::size_t C) {
    if (C == 0) return 0;
    const double scaled = std::ceil(static_cast<double>(C) * 8.0);
    return static_cast<std::size_t>(std::min(
        scaled, static_cast<double>(std::numeric_limits<std::int32_t>::max())));
  }

  std::size_t CurrentBucket(const MacroState& s) const {
    const std::uint32_t expiry = Expiry(s.expiry_flags);
    if (expiry < current_version_) return 0;
    const std::uint32_t rem = expiry - current_version_;
    return rem <= scoring_.max_score() ? rem : 0;
  }

  void InsertResident(std::uint32_t key, MacroState& s) {
    if (IsMacroGhost(s)) ghost_.Remove(s);
    SetRenewal(s.expiry_flags, false);
    wheel_.Schedule(key, s, CurrentBucket(s));
    ++resident_size_;
  }

  EvictResult EvictOne(std::size_t incoming) {
    while (resident_size_) {
      const auto candidate = wheel_.Front();
      if (!candidate) return {};
      MacroState& s = meta_[candidate->key].macro;
      const std::size_t actual = CurrentBucket(s);
      if (actual > candidate->physical_remaining) {
        wheel_.PopFront();
        wheel_.Schedule(candidate->key, s, actual);
        continue;
      }
      if (incoming <= actual) {
        if (Renewal(s.expiry_flags)) SetRenewal(s.expiry_flags, false);
        return {};
      }
      if (Renewal(s.expiry_flags)) {
        SetRenewal(s.expiry_flags, false);
        wheel_.PopFront();
        SetExpiry(s.expiry_flags, current_version_ + static_cast<std::uint32_t>(incoming));
        wheel_.Schedule(candidate->key, s, incoming);
        continue;
      }
      wheel_.PopFront();
      const std::uint32_t victim = candidate->key;
      s.next_resident = kNotResident;
      SetRenewal(s.expiry_flags, false);
      --resident_size_;
      ghost_.Insert(victim, s);
      return {true, victim};
    }
    return {};
  }

  std::vector<Meta>& meta_;
  std::size_t capacity_ = 0, resident_limit_ = 0, resident_size_ = 0;
  std::uint32_t current_version_ = 1;
  Scoring scoring_;
  MacroGhostFIFO ghost_;
  TimingWheel<MacroState> wheel_;
};

class MicroLSBT {
 public:
  struct ProbeResult { bool hit = false; std::uint16_t incoming_bucket = 0; };

  MicroLSBT(std::vector<Meta>& meta, std::size_t capacity,
            double retention = Scoring::kDefaultRetention)
      : meta_(meta), capacity_(capacity), scoring_(capacity, retention),
        wheel_(scoring_.bucket_count(), meta_, &Meta::micro) {}

  bool Contains(std::uint32_t key) const { return IsMicroResident(meta_[key].micro); }
  std::size_t resident_size() const { return resident_size_; }
  std::size_t capacity() const { return capacity_; }

  ProbeResult Access(std::uint32_t key, int depth) {
    MicroState& s = meta_[key].micro;
    if (IsMicroResident(s)) {
      const auto next = scoring_.Transition(1, CurrentBucket(s));
      SetExpiry(s.expiry_flags, current_version_ + static_cast<std::uint32_t>(next));
      SetRenewal(s.expiry_flags, true);
      return {true, next};
    }
    // Ghostless: every miss is a fresh cold-start observation.
    const auto next = scoring_.Transition(depth, 0);
    return {false, next};
  }

  bool Admit(std::uint32_t key, std::uint16_t incoming_bucket) {
    MicroState& s = meta_[key].micro;
    if (IsMicroResident(s)) return true;
    if (capacity_ == 0) return false;
    if (resident_size_ >= capacity_ && !EvictOne(incoming_bucket)) return false;
    s.expiry_flags = 0;
    SetExpiry(s.expiry_flags, current_version_ + incoming_bucket);
    SetRenewal(s.expiry_flags, false);
    wheel_.Schedule(key, s, incoming_bucket);
    ++resident_size_;
    return true;
  }

  void Tick() {
    if (current_version_ >= kExpiryMask - static_cast<std::uint32_t>(scoring_.max_score()) - 2)
      throw std::overflow_error("micro clock exhausted");
    ++current_version_;
    wheel_.Advance();
  }

  void ResetForCapacity(std::size_t capacity,
                        const std::vector<std::uint32_t>& touched_keys) {
    for (std::uint32_t key : touched_keys) meta_[key].micro = MicroState{};
    capacity_ = capacity;
    resident_size_ = 0;
    current_version_ = 1;
    scoring_.Reconfigure(capacity_);
    wheel_.Reconfigure(scoring_.bucket_count());
  }

 private:
  std::size_t CurrentBucket(const MicroState& s) const {
    const std::uint32_t expiry = Expiry(s.expiry_flags);
    if (expiry < current_version_) return 0;
    const std::uint32_t rem = expiry - current_version_;
    return rem <= scoring_.max_score() ? rem : 0;
  }

  bool EvictOne(std::size_t incoming) {
    while (resident_size_) {
      const auto candidate = wheel_.Front();
      if (!candidate) return false;
      MicroState& s = meta_[candidate->key].micro;
      const std::size_t actual = CurrentBucket(s);
      if (actual > candidate->physical_remaining) {
        wheel_.PopFront();
        wheel_.Schedule(candidate->key, s, actual);
        continue;
      }
      if (incoming <= actual) {
        if (Renewal(s.expiry_flags)) SetRenewal(s.expiry_flags, false);
        return false;
      }
      if (Renewal(s.expiry_flags)) {
        SetRenewal(s.expiry_flags, false);
        wheel_.PopFront();
        SetExpiry(s.expiry_flags, current_version_ + static_cast<std::uint32_t>(incoming));
        wheel_.Schedule(candidate->key, s, incoming);
        continue;
      }
      wheel_.PopFront();
      s = MicroState{};
      --resident_size_;
      return true;
    }
    return false;
  }

  std::vector<Meta>& meta_;
  std::size_t capacity_ = 0, resident_size_ = 0;
  std::uint32_t current_version_ = 1;
  Scoring scoring_;
  TimingWheel<MicroState> wheel_;
};

}  // namespace packed_lsbt
