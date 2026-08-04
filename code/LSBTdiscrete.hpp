#pragma once

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

class LSBTdiscrete {
 public:
  using DynamicProtectionFn = std::function<bool(std::uint32_t)>;
  using DynamicEvictionScoreFn = std::function<float(std::uint32_t)>;

  struct DebugCounters {
    std::uint64_t protected_installs = 0;
    std::uint64_t protected_candidates = 0;
    std::uint64_t protected_rotations = 0;
    std::uint64_t protected_rejects = 0;
    std::uint64_t rescues = 0;
    std::uint64_t evictions = 0;
  };

  static constexpr std::size_t kInvalidBucket =
      std::numeric_limits<std::size_t>::max();
  static constexpr std::int32_t kQueueEnd = -1;
  static constexpr std::int32_t kNotResident = -2;
  static constexpr double kDefaultGainScale = 51.2;
  static constexpr double kDefaultHistoryRetention = 0.95;
  static constexpr double kDefaultMinHistoryRetention = 0.92;
  static constexpr double kDefaultMaxHistoryRetention = 0.98;
  static constexpr double kDefaultGhostSizeRatio = 8.0;

  LSBTdiscrete(std::size_t key_count,
                                      std::size_t capacity, int max_depth,
                                      double history_retention,
                                      int /*signal_cap*/ = 1,
                                      std::size_t bucket_count_override = 0,
                                      int rescue_limit = -1,
                                      int protect_scan_budget = 16,
                                      bool rotate_protected_candidates = true,
                                      double min_history_retention =
                                          kDefaultMinHistoryRetention,
                                      double max_history_retention =
                                          kDefaultMaxHistoryRetention,
                                      double gain_scale = kDefaultGainScale,
                                      double reference_gain_scale =
                                          kDefaultGainScale,
                                      double reward_window_capacity_ratio = 0.0,
                                      double repeat_hit_gain_scale = 0.25,
                                      bool first_touch_per_block = true,
                                      double ghost_size_ratio =
                                          kDefaultGhostSizeRatio)
      : capacity_(capacity),
        max_depth_(std::max(1, max_depth)),
        ghost_capacity_(ComputeGhostCapacity(capacity, ghost_size_ratio)),
        bucket_count_(bucket_count_override > 0
                          ? bucket_count_override
                          : ComputeBucketCount(capacity)),
        max_score_(bucket_count_ == 0 ? 0 : bucket_count_ - 1),
        bucket_bitmap_words_((bucket_count_ + 63) / 64),
        decay_(ClampHistoryRetention(history_retention, min_history_retention,
                                     max_history_retention)),
        gain_scale_(std::max(1e-12, gain_scale)),
        reference_gain_scale_(std::max(1e-12, reference_gain_scale)),
        reward_window_capacity_ratio_(
            std::max(0.0, reward_window_capacity_ratio)),
        repeat_hit_gain_scale_(std::max(0.0, repeat_hit_gain_scale)),
        first_touch_per_block_(first_touch_per_block),
        reference_heat_(std::max(1e-12,
                                 reference_gain_scale_ /
                                     std::max(1.0 - decay_, 1e-9))),
        rescue_limit_(rescue_limit),
        protect_scan_budget_(std::max(1, protect_scan_budget)),
        rotate_protected_candidates_(rotate_protected_candidates),
        bucket_heads_(bucket_count_, kQueueEnd),
        bucket_tails_(bucket_count_, kQueueEnd),
        bucket_bitmap_(bucket_bitmap_words_, 0),
        depth_bucket_lut_((max_depth_ + 1) * bucket_count_, 0) {
    const std::size_t state_reserve =
        std::min<std::size_t>(key_count,
                              capacity_ > std::numeric_limits<std::size_t>::max() -
                                      ghost_capacity_
                                  ? std::numeric_limits<std::size_t>::max()
                                  : capacity_ + ghost_capacity_);
    states_.reserve(state_reserve);
    if (ghost_capacity_ > 0) {
      ghost_ring_.reserve(ghost_capacity_ + 1);
    }
    BuildTables();
  }

  bool Access(std::uint32_t key_id, int depth, double gain_multiplier = 1.0) {
    State& state = GetOrCreateState(key_id);
    const bool hit = IsResident(state);
    if (!hit) {
      InsertGhost(key_id, state);
    }
    (void)ObserveAccess(state, depth, gain_multiplier);
    return hit;
  }

  std::vector<std::uint32_t> TakeEvictedKeys() {
    std::vector<std::uint32_t> evicted;
    evicted.swap(pending_evicted_keys_);
    return evicted;
  }

  bool Contains(std::uint32_t key_id) const noexcept {
    const auto it = states_.find(key_id);
    return it != states_.end() && IsResident(it->second);
  }

  std::size_t resident_size() const noexcept { return resident_size_; }

  bool Admit(std::uint32_t key_id, int /*depth*/) {
    auto state_it = states_.find(key_id);
    if (state_it == states_.end()) {
      state_it = states_.try_emplace(key_id).first;
    }
    State& state = state_it->second;
    if (IsResident(state)) {
      return true;
    }
    if (capacity_ == 0) {
      if (!state.in_ghost) {
        states_.erase(state_it);
      }
      return false;
    }

    // Access() records a miss in ghost before admission. Remove the current
    // key first so victim insertion cannot evict the State referenced here.
    const bool restore_ghost_on_failure = state.in_ghost;
    if (restore_ghost_on_failure) {
      RemoveGhost(state);
    }
    if (resident_size_ >= capacity_ && !EvictOne()) {
      if (restore_ghost_on_failure) {
        InsertGhost(key_id, state);
      } else {
        states_.erase(state_it);
      }
      return false;
    }
    InsertResident(key_id, state);
    return true;
  }

  void SetDynamicProtectionFn(DynamicProtectionFn fn) {
    dynamic_protection_fn_ = std::move(fn);
  }

  void ClearDynamicProtectionFn() { dynamic_protection_fn_ = nullptr; }

  void SetDynamicEvictionScoreFn(DynamicEvictionScoreFn fn) {
    dynamic_eviction_score_fn_ = std::move(fn);
  }

  void ClearDynamicEvictionScoreFn() { dynamic_eviction_score_fn_ = nullptr; }

  void ClearProtectedKeys() noexcept { active_protection_epoch_ = 0; }

  void BeginProtectedKeyInstall() { active_protection_epoch_ = NextProtectionEpoch(); }

  void ProtectKeyIfResident(std::uint32_t key_id) {
    if (active_protection_epoch_ == 0) {
      return;
    }
    const auto it = states_.find(key_id);
    if (it == states_.end() || !IsResident(it->second) ||
        it->second.protection_epoch == active_protection_epoch_) {
      return;
    }
    it->second.protection_epoch = active_protection_epoch_;
    ++counters_.protected_installs;
  }

  void InstallProtectedKeys(const std::vector<std::uint32_t>& key_ids) {
    if (key_ids.empty()) {
      ClearProtectedKeys();
      return;
    }

    BeginProtectedKeyInstall();
    for (const std::uint32_t key_id : key_ids) {
      ProtectKeyIfResident(key_id);
    }
  }

  void FinishBlock() {
    ClearProtectedKeys();
    ++current_version_;
  }

  const DebugCounters& counters() const noexcept { return counters_; }

 private:
  struct State {
    std::uint32_t expiry_version = 0;
    std::int32_t next_resident = kNotResident;
    std::uint32_t ghost_generation = 0;
    std::uint16_t protection_epoch = 0;
    bool in_ghost = false;
  };

  struct GhostRecord {
    std::uint32_t key_id = 0;
    std::uint32_t generation = 0;
  };

 public:
  static constexpr std::size_t DebugStateSizeBytes() { return sizeof(State); }

 private:
  static double ClampHistoryRetention(double value, double min_value,
                                      double max_value) {
    const double bounded_min =
        std::clamp(std::min(min_value, max_value), 1e-6, 1.0 - 1e-9);
    const double bounded_max =
        std::clamp(std::max(min_value, max_value), bounded_min, 1.0 - 1e-9);
    return std::clamp(value, bounded_min, bounded_max);
  }

  static std::size_t ComputeBucketCount(std::size_t capacity) {
    const double cap = static_cast<double>(std::max<std::size_t>(1, capacity));
    return std::max<std::size_t>(
        1, static_cast<std::size_t>(std::llround(32.0 * std::log2(cap + 1.0))));
  }

  static std::size_t ComputeGhostCapacity(std::size_t capacity,
                                          double ghost_size_ratio) {
    if (capacity == 0 || !std::isfinite(ghost_size_ratio) ||
        ghost_size_ratio <= 0.0) {
      return 0;
    }
    const double max_size = static_cast<double>(
        std::numeric_limits<std::size_t>::max());
    if (ghost_size_ratio >= max_size / static_cast<double>(capacity)) {
      return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(
        static_cast<double>(capacity) * ghost_size_ratio);
  }

  State& GetOrCreateState(std::uint32_t key_id) {
    return states_.try_emplace(key_id).first->second;
  }

  static unsigned CountTrailingZeros64(std::uint64_t value) {
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanForward64(&index, value);
    return static_cast<unsigned>(index);
#else
    return static_cast<unsigned>(__builtin_ctzll(value));
#endif
  }

  void BuildTables() {
    representative_heat_lut_.resize(bucket_count_, 0.0);
    for (std::size_t bucket = 0; bucket < bucket_count_; ++bucket) {
      representative_heat_lut_[bucket] =
          reference_heat_ *
          std::pow(decay_, static_cast<double>(max_score_ - bucket));
    }

    for (int depth = 1; depth <= max_depth_; ++depth) {
      const double gain = gain_scale_ * DepthWeight(depth);
      for (std::size_t bucket = 0; bucket < bucket_count_; ++bucket) {
        depth_bucket_lut_[DepthBucketIndex(depth, bucket)] =
            static_cast<std::uint16_t>(
                HeatToBucket(representative_heat_lut_[bucket] + gain));
      }
    }
  }

  std::size_t DepthBucketIndex(int depth, std::size_t bucket) const {
    return static_cast<std::size_t>(depth) * bucket_count_ + bucket;
  }

  double NormalizeDepth(double depth) const {
    if (max_depth_ <= 1) {
      return 0.0;
    }
    return std::clamp((depth - 1.0) / static_cast<double>(max_depth_ - 1), 0.0,
                      1.0);
  }

  double DepthWeight(int depth) const {
    const int clipped_depth = std::clamp(depth, 1, max_depth_);
    return std::pow(decay_, NormalizeDepth(static_cast<double>(clipped_depth)));
  }

  std::size_t HeatToBucket(double heat) const {
    if (heat <= 1e-12 || max_score_ == 0) {
      return 0;
    }
    const double ratio = reference_heat_ / std::max(heat, 1e-12);
    const double tier_delta =
        std::log(ratio) / std::log(1.0 / std::max(decay_, 1e-9));
    return static_cast<std::size_t>(std::clamp(
        static_cast<long long>(
            std::llround(static_cast<double>(max_score_) - tier_delta)),
        0LL, static_cast<long long>(max_score_)));
  }

  bool IsResident(const State& state) const {
    return state.next_resident != kNotResident;
  }

  bool IsProtected(std::uint32_t key_id) const {
    if (active_protection_epoch_ == 0) {
      return false;
    }
    const auto it = states_.find(key_id);
    return it != states_.end() &&
           it->second.protection_epoch == active_protection_epoch_;
  }

  std::uint16_t NextProtectionEpoch() {
    ++next_protection_epoch_;
    if (next_protection_epoch_ == 0) {
      for (auto& entry : states_) {
        entry.second.protection_epoch = 0;
      }
      next_protection_epoch_ = 1;
    }
    return next_protection_epoch_;
  }

  void CompactGhostRingIfNeeded() {
    if (ghost_head_index_ == 0 || ghost_head_index_ * 2 < ghost_ring_.size()) {
      return;
    }
    ghost_ring_.erase(
        ghost_ring_.begin(),
        ghost_ring_.begin() + static_cast<std::ptrdiff_t>(ghost_head_index_));
    ghost_head_index_ = 0;
  }

  void EvictGhostIfNeeded() {
    while (ghost_live_size_ > ghost_capacity_ &&
           ghost_head_index_ < ghost_ring_.size()) {
      const GhostRecord record = ghost_ring_[ghost_head_index_++];
      const auto it = states_.find(record.key_id);
      if (it == states_.end() || !it->second.in_ghost ||
          it->second.ghost_generation != record.generation) {
        continue;
      }
      states_.erase(it);
      --ghost_live_size_;
    }
    CompactGhostRingIfNeeded();
  }

  void InsertGhost(std::uint32_t key_id, State& state) {
    if (state.in_ghost) {
      return;
    }
    if (ghost_capacity_ == 0) {
      state.in_ghost = false;
      return;
    }
    ++state.ghost_generation;
    state.in_ghost = true;
    ++ghost_live_size_;
    ghost_ring_.push_back(GhostRecord{key_id, state.ghost_generation});
    EvictGhostIfNeeded();
  }

  void RemoveGhost(State& state) {
    if (!state.in_ghost) {
      return;
    }
    state.in_ghost = false;
    ++state.ghost_generation;
    if (ghost_live_size_ > 0) {
      --ghost_live_size_;
    }
  }

  std::uint16_t CurrentBucket(const State& state) const {
    if (state.expiry_version <= current_version_) {
      return 0;
    }
    return static_cast<std::uint16_t>(state.expiry_version - current_version_);
  }

  std::uint16_t ApplyDepthBucket(int depth, std::uint16_t bucket) const {
#ifndef NDEBUG
    assert(depth >= 1);
    assert(depth <= max_depth_);
#endif
    return depth_bucket_lut_[DepthBucketIndex(depth, bucket)];
  }

  std::uint16_t ObserveAccess(State& state, int depth,
                              double gain_multiplier) {
    const std::uint16_t current_bucket = CurrentBucket(state);
    const int clipped_depth = std::clamp(depth, 1, max_depth_);
    const double clipped_gain_multiplier = std::max(0.0, gain_multiplier);
    const std::uint16_t next_bucket =
        clipped_gain_multiplier == 1.0
            ? ApplyDepthBucket(clipped_depth, current_bucket)
            : static_cast<std::uint16_t>(
                  HeatToBucket(representative_heat_lut_[current_bucket] +
                               gain_scale_ * DepthWeight(clipped_depth) *
                                   clipped_gain_multiplier));
    state.expiry_version =
        current_version_ + static_cast<std::uint32_t>(next_bucket);
    return next_bucket;
  }

  void SetBucketBit(std::size_t bucket_index) {
    bucket_bitmap_[bucket_index / 64] |=
        (std::uint64_t{1} << (bucket_index % 64));
  }

  void ClearBucketBitIfEmpty(std::size_t bucket_index) {
    if (bucket_heads_[bucket_index] == kQueueEnd) {
      bucket_bitmap_[bucket_index / 64] &=
          ~(std::uint64_t{1} << (bucket_index % 64));
    }
  }

  void PushResidentTail(std::uint32_t key_id, State& state,
                        std::size_t bucket_index) {
    const bool was_empty = bucket_tails_[bucket_index] == kQueueEnd;
    state.next_resident = kQueueEnd;
    if (!was_empty) {
      states_.at(static_cast<std::uint32_t>(bucket_tails_[bucket_index]))
          .next_resident =
          static_cast<std::int32_t>(key_id);
    } else {
      bucket_heads_[bucket_index] = static_cast<std::int32_t>(key_id);
      SetBucketBit(bucket_index);
      if (current_cold_bucket_ == kInvalidBucket ||
          bucket_index < current_cold_bucket_) {
        current_cold_bucket_ = bucket_index;
      }
    }
    bucket_tails_[bucket_index] = static_cast<std::int32_t>(key_id);
  }

  std::uint32_t PopResidentHead(std::size_t bucket_index) {
    const std::int32_t head = bucket_heads_[bucket_index];
    const std::int32_t next =
        states_.at(static_cast<std::uint32_t>(head)).next_resident;
    bucket_heads_[bucket_index] = next;
    if (next == kQueueEnd) {
      bucket_tails_[bucket_index] = kQueueEnd;
      ClearBucketBitIfEmpty(bucket_index);
    }
    return static_cast<std::uint32_t>(head);
  }

  void InsertResident(std::uint32_t key_id, State& state) {
    RemoveGhost(state);
    state.protection_epoch = 0;
    PushResidentTail(key_id, state, CurrentBucket(state));
    ++resident_size_;
  }

  std::size_t FindNextNonEmptyBucket(std::size_t start_bucket) const {
    if (start_bucket >= bucket_count_) {
      return kInvalidBucket;
    }

    std::size_t word_index = start_bucket / 64;
    const std::size_t bit_index = start_bucket % 64;
    std::uint64_t word = bucket_bitmap_[word_index];
    if (bit_index > 0) {
      word &= (~std::uint64_t{0} << bit_index);
    }
    if (word != 0) {
      return word_index * 64 + CountTrailingZeros64(word);
    }

    for (++word_index; word_index < bucket_bitmap_words_; ++word_index) {
      word = bucket_bitmap_[word_index];
      if (word != 0) {
        return word_index * 64 + CountTrailingZeros64(word);
      }
    }
    return kInvalidBucket;
  }

  bool EvictOne() {
    if (resident_size_ == 0) {
      return false;
    }
    if (current_cold_bucket_ == kInvalidBucket) {
      current_cold_bucket_ = 0;
    }

    int rescues = 0;
    int protected_skips = 0;
    while (true) {
      current_cold_bucket_ = FindNextNonEmptyBucket(current_cold_bucket_);
      if (current_cold_bucket_ == kInvalidBucket) {
        return false;
      }

      const std::size_t physical_bucket = current_cold_bucket_;
      const std::int32_t candidate = bucket_heads_[physical_bucket];
      if (candidate == kQueueEnd) {
        ++current_cold_bucket_;
        continue;
      }

      if (dynamic_eviction_score_fn_) {
        const bool evicted = EvictOneScoredCandidate(physical_bucket);
        if (evicted) {
          return true;
        }
        ++counters_.protected_rejects;
        return false;
      }

      State& state = states_.at(static_cast<std::uint32_t>(candidate));
      const std::size_t actual_bucket = CurrentBucket(state);
      const std::uint32_t key_id = static_cast<std::uint32_t>(candidate);
      const bool statically_protected = IsProtected(key_id);
      const bool dynamically_protected =
          !statically_protected && dynamic_protection_fn_ &&
          dynamic_protection_fn_(key_id);

      if (statically_protected || dynamically_protected) {
        ++counters_.protected_candidates;
        if (dynamically_protected) {
          ++counters_.protected_installs;
        }
        if (!rotate_protected_candidates_) {
          ++counters_.protected_rejects;
          return false;
        }
        if (protected_skips >= protect_scan_budget_) {
          ++counters_.protected_rejects;
          return false;
        }
        const std::uint32_t skipped = PopResidentHead(physical_bucket);
        PushResidentTail(skipped, state, std::max(actual_bucket, physical_bucket));
        ++counters_.protected_rotations;
        ++protected_skips;
        continue;
      }

      const bool can_rescue =
          actual_bucket > physical_bucket &&
          (rescue_limit_ < 0 || rescues < rescue_limit_);

      const std::uint32_t victim = PopResidentHead(physical_bucket);
      if (can_rescue) {
        PushResidentTail(victim, state, actual_bucket);
        ++counters_.rescues;
        ++rescues;
        continue;
      }

      state.next_resident = kNotResident;
      state.protection_epoch = 0;
      --resident_size_;
      pending_evicted_keys_.push_back(key_id);
      ++counters_.evictions;
      InsertGhost(key_id, state);
      if (ghost_capacity_ == 0) {
        states_.erase(key_id);
      }
      return true;
    }
  }

  bool EvictOneScoredCandidate(std::size_t physical_bucket) {
    struct CandidateChoice {
      std::uint32_t key_id = 0;
      std::size_t actual_bucket = 0;
      float score = 0.0f;
      bool protected_now = false;
    };

    std::vector<CandidateChoice> scanned;
    scanned.reserve(static_cast<std::size_t>(protect_scan_budget_));

    int rescues = 0;
    while (static_cast<int>(scanned.size()) < protect_scan_budget_) {
      const std::int32_t candidate = bucket_heads_[physical_bucket];
      if (candidate == kQueueEnd) {
        break;
      }

      State& state = states_.at(static_cast<std::uint32_t>(candidate));
      const std::size_t actual_bucket = CurrentBucket(state);
      if (actual_bucket > physical_bucket &&
          (rescue_limit_ < 0 || rescues < rescue_limit_)) {
        const std::uint32_t rescued = PopResidentHead(physical_bucket);
        PushResidentTail(rescued, state, actual_bucket);
        ++counters_.rescues;
        ++rescues;
        continue;
      }

      const std::uint32_t key_id = PopResidentHead(physical_bucket);
      const bool statically_protected = IsProtected(key_id);
      const bool dynamically_protected =
          !statically_protected && dynamic_protection_fn_ &&
          dynamic_protection_fn_(key_id);
      if (statically_protected || dynamically_protected) {
        ++counters_.protected_candidates;
        if (dynamically_protected) {
          ++counters_.protected_installs;
        }
      }

      scanned.push_back(CandidateChoice{
          key_id,
          actual_bucket,
          (statically_protected || dynamically_protected)
              ? std::numeric_limits<float>::infinity()
              : dynamic_eviction_score_fn_(key_id),
          statically_protected || dynamically_protected,
      });
    }

    if (scanned.empty()) {
      return false;
    }

    std::size_t victim_index = scanned.size();
    float lowest_score = 0.0f;
    for (std::size_t i = 0; i < scanned.size(); ++i) {
      if (scanned[i].protected_now) {
        continue;
      }
      if (victim_index == scanned.size() || scanned[i].score < lowest_score) {
        victim_index = i;
        lowest_score = scanned[i].score;
      }
    }

    if (victim_index == scanned.size()) {
      for (const CandidateChoice& choice : scanned) {
        State& state = states_.at(choice.key_id);
        PushResidentTail(choice.key_id, state,
                         std::max(choice.actual_bucket, physical_bucket));
        ++counters_.protected_rotations;
      }
      return false;
    }

    for (std::size_t i = 0; i < scanned.size(); ++i) {
      const CandidateChoice& choice = scanned[i];
      State& state = states_.at(choice.key_id);
      if (i == victim_index) {
        state.next_resident = kNotResident;
        state.protection_epoch = 0;
        --resident_size_;
        pending_evicted_keys_.push_back(choice.key_id);
        ++counters_.evictions;
        InsertGhost(choice.key_id, state);
        if (ghost_capacity_ == 0) {
          states_.erase(choice.key_id);
        }
      } else {
        PushResidentTail(choice.key_id, state,
                         std::max(choice.actual_bucket, physical_bucket));
        if (choice.protected_now) {
          ++counters_.protected_rotations;
        }
      }
    }

    return true;
  }

  std::size_t capacity_ = 0;
  int max_depth_ = 1;
  std::size_t ghost_capacity_ = 0;
  std::size_t bucket_count_ = 0;
  std::size_t max_score_ = 0;
  std::size_t bucket_bitmap_words_ = 0;
  double decay_ = kDefaultHistoryRetention;
  double gain_scale_ = kDefaultGainScale;
  double reference_gain_scale_ = kDefaultGainScale;
  double reward_window_capacity_ratio_ = 0.0;
  double repeat_hit_gain_scale_ = 0.25;
  bool first_touch_per_block_ = true;
  double reference_heat_ = 1.0;
  int rescue_limit_ = -1;
  int protect_scan_budget_ = 16;
  bool rotate_protected_candidates_ = true;
  std::uint32_t current_version_ = 1;
  std::size_t current_cold_bucket_ = kInvalidBucket;
  std::size_t resident_size_ = 0;
  DebugCounters counters_;

  std::unordered_map<std::uint32_t, State> states_;
  std::vector<std::int32_t> bucket_heads_;
  std::vector<std::int32_t> bucket_tails_;
  std::vector<std::uint64_t> bucket_bitmap_;
  std::vector<std::uint16_t> depth_bucket_lut_;
  std::vector<double> representative_heat_lut_;
  std::vector<std::uint32_t> pending_evicted_keys_;
  std::vector<GhostRecord> ghost_ring_;
  std::uint16_t active_protection_epoch_ = 0;
  std::uint16_t next_protection_epoch_ = 0;
  std::size_t ghost_live_size_ = 0;
  std::size_t ghost_head_index_ = 0;
  DynamicProtectionFn dynamic_protection_fn_;
  DynamicEvictionScoreFn dynamic_eviction_score_fn_;
};
