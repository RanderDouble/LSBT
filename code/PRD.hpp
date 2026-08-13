#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

// Two-exit LSBT prototype. Cold holds unproved depth-prior admissions; Hot
// holds reuse-proved items. Both victim queues retain exact LSBT heat order.
class PRDCore {
 public:
  struct DebugCounters {
    std::uint64_t protected_installs = 0;
    std::uint64_t protected_rejects = 0;
    std::uint64_t protected_candidates = 0;
    std::uint64_t evictions = 0;
    std::uint64_t stale_heap_pops = 0;
    std::uint64_t promotions = 0;
    std::uint64_t proof_expiries = 0;
    std::uint64_t expired_to_cold = 0;
    std::uint64_t lease_active_hits = 0;
    std::uint64_t lease_late_reuses = 0;
  };
  struct LeaseControlSample {
    std::uint64_t admission = 0;
    double lease_before = 0.0, lease_after = 0.0;
    std::uint64_t active_hits = 0, late_reuses = 0, expiries = 0;
  };

  PRDCore(std::size_t key_count, std::size_t capacity, int max_depth,
                  double history_retention, int /*signal_cap*/ = 1,
                  std::size_t /*bucket_count_override*/ = 0,
                  bool neutral_depth_gain = false,
                  bool unified_capacity = false,
                  bool ghost_hit_hot = false,
                  bool preserve_ghost_heat = false,
                  std::uint64_t fixed_hot_ttl = 0,
                  double turnover_factor = 0.0,
                  bool bucketed_cold = false,
                  bool pure_depth_cold = false,
                  double ghost_capacity_ratio = 8.0,
                  bool all_victims_to_ghost = true,
                  bool fifo_cold = false)
      : capacity_(capacity), max_depth_(std::max(1, max_depth)),
        cold_capacity_(capacity == 0 ? 0 : (unified_capacity ? capacity : std::max<std::size_t>(1, capacity / 5))),
        hot_capacity_(unified_capacity ? capacity : capacity - cold_capacity_),
        ghost_capacity_(ComputeGhostCapacity(capacity, ghost_capacity_ratio)),
        decay_(std::clamp(history_retention, 0.92, 0.98)),
        log_decay_(std::log(decay_)),
        depth_stride_(max_depth_ <= 1 ? 0.0 : 1.0 / (max_depth_ - 1.0)),
        neutral_depth_gain_(neutral_depth_gain), unified_capacity_(unified_capacity),
        ghost_hit_hot_(ghost_hit_hot), preserve_ghost_heat_(preserve_ghost_heat),
        fixed_hot_ttl_(fixed_hot_ttl), turnover_factor_(std::max(0.0, turnover_factor)),
        bucketed_cold_(bucketed_cold || pure_depth_cold),
        pure_depth_cold_(pure_depth_cold),
        cold_bucket_count_(bucketed_cold_ ? kColdBucketCount : 0),
        cold_buckets_(cold_bucket_count_),
        all_victims_to_ghost_(all_victims_to_ghost), fifo_cold_(fifo_cold) {
    states_.reserve(std::min(key_count, capacity_ + ghost_capacity_));
    ghost_ring_.reserve(ghost_capacity_ + 1);
    if (turnover_factor_ > 0.0) {
      const double span = turnover_factor_ *
          static_cast<double>(std::max<std::size_t>(1, capacity_));
      turnover_wheel_size_ = std::max<std::size_t>(
          64, static_cast<std::size_t>(std::ceil(span)) + 2);
      turnover_wheel_.resize(turnover_wheel_size_);
    }
  }

  bool Access(std::uint32_t key, int depth, double gain_multiplier = 1.0) {
    if (turnover_factor_ > 0.0) AdvanceTurnoverExpiry();
    State& s = states_.try_emplace(key).first->second;
    const bool hit = s.place != Place::kNone;
    const bool ghost_hit = !hit && s.in_ghost;
    if (hit && s.place == Place::kHotActive) {
      ++adaptive_lease_active_hits_;
      ++counters_.lease_active_hits;
    }
    if (ghost_hit) {
      ++adaptive_ghost_hits_;
      if (s.ghost_from_hot) {
        ++adaptive_lease_late_reuses_;
        ++counters_.lease_late_reuses;
      }
      // A ghost is reuse evidence, not a saved burst score.
      if (!preserve_ghost_heat_) s.log_heat = NegInf();
      RemoveGhost(s);
      s.promote_hot_on_admit = ghost_hit_hot_;
    }
    AddHeat(s, depth, gain_multiplier);
    if (s.place == Place::kCold) {
      if (!fifo_cold_) PushHeat(key, s);
      Promote(key, s);
    } else if (s.place == Place::kHotActive || s.place == Place::kHotStale) {
      s.place = Place::kHotActive;
      if (turnover_factor_ == 0.0) PushHeat(key, s);
      ScheduleProof(key, s);
    }
    return hit;
  }

  bool Admit(std::uint32_t key, int /*depth*/) {
    if (turnover_factor_ > 0.0) AdvanceTurnoverExpiry();
    auto it = states_.find(key);
    if (it == states_.end()) it = states_.try_emplace(key).first;
    State& s = it->second;
    if (s.place != Place::kNone) return true;
    if (capacity_ == 0 || cold_capacity_ == 0) {
      if (!s.in_ghost) states_.erase(it);
      return false;
    }
    const bool full = unified_capacity_ ? ResidentSize() >= capacity_
                                        : (s.promote_hot_on_admit
                                               ? hot_size_ >= hot_capacity_
                                               : cold_size_ >= cold_capacity_);
    if (full && !(EvictCold() || EvictHot())) {
      if (!s.in_ghost) states_.erase(it);
      return false;
    }
    s.place = s.promote_hot_on_admit ? Place::kHotActive : Place::kCold;
    s.promote_hot_on_admit = false;
    if (s.place == Place::kCold) ++cold_size_; else ++hot_size_;
    if (s.place == Place::kCold) {
      if (fifo_cold_) cold_fifo_.push_back(key); else PushHeat(key, s);
    } else if (turnover_factor_ == 0.0) {
      PushHeat(key, s);
    }
    if (s.place == Place::kHotActive) {
      ++counters_.promotions;
      ScheduleProof(key, s);
    }
    ++admission_clock_;
    MaybeAdaptGhost();
    MaybeAdaptLease();
    return true;
  }

  void BeginProtectedKeyInstall() { active_protection_epoch_ = NextEpoch(); }
  void ClearProtectedKeys() noexcept { active_protection_epoch_ = 0; }
  void ProtectKeyIfResident(std::uint32_t key) {
    if (active_protection_epoch_ == 0) return;
    const auto it = states_.find(key);
    if (it != states_.end() && it->second.place != Place::kNone) {
      it->second.protection_epoch = active_protection_epoch_;
      ++counters_.protected_installs;
    }
  }
  void FinishBlock() {
    ClearProtectedKeys();
    ++version_;
    if (turnover_factor_ > 0.0) return;
    while (!proof_heap_.empty() && proof_heap_.top().expiry <= version_) {
      const ProofEntry e = proof_heap_.top(); proof_heap_.pop();
      auto it = states_.find(e.key);
      if (it == states_.end() || it->second.place != Place::kHotActive ||
          it->second.proof_generation != e.generation) continue;
      it->second.place = Place::kHotStale;
      ++counters_.proof_expiries;
      // The deadline identifies the exact stale object. Remove it directly
      // from the independent Hot exit; do not rescan the heat heap.
      FinalizeEviction(e.key, it->second);
    }
  }
  const DebugCounters& counters() const noexcept { return counters_; }
  const std::vector<LeaseControlSample>& lease_control_samples() const noexcept {
    return lease_control_samples_;
  }
  double current_lease_factor() const noexcept {
    return adaptive_lease_ ? adaptive_lease_factor_ : turnover_factor_;
  }
  std::size_t resident_size() const noexcept { return ResidentSize(); }
  bool Contains(std::uint32_t key) const noexcept {
    const auto it = states_.find(key);
    return it != states_.end() && it->second.place != Place::kNone;
  }
  std::vector<std::uint32_t> TakeEvictedKeys() {
    std::vector<std::uint32_t> evicted;
    evicted.swap(pending_evicted_keys_);
    return evicted;
  }
  void SetDepthBucketStride(double stride) { depth_stride_ = std::max(0.0, stride); }
  void EnableAdaptiveGhost() {
    adaptive_ghost_ = true;
    adaptive_epoch_ = std::max<std::size_t>(32, 4 * std::max<std::size_t>(1, capacity_));
  }
  void EnableAdaptiveLease() {
    adaptive_lease_ = true;
    adaptive_epoch_ = std::max<std::size_t>(32, 4 * std::max<std::size_t>(1, capacity_));
  }

 private:
  enum class Place : std::uint8_t { kNone, kCold, kHotActive, kHotStale };
  struct State {
    double log_heat = NegInf(); std::uint64_t heat_generation = 0;
    std::uint64_t cold_bucket_generation = 0;
    std::uint64_t proof_generation = 0; std::uint32_t ghost_generation = 0;
    std::uint16_t protection_epoch = 0; Place place = Place::kNone;
    int last_depth = 1;
    bool in_ghost = false, ghost_from_hot = false, promote_hot_on_admit = false;
  };
  struct HeatEntry { double log_heat; std::uint64_t sequence, generation; std::uint32_t key; };
  struct HeatGreater { bool operator()(const HeatEntry& a, const HeatEntry& b) const {
    return a.log_heat != b.log_heat ? a.log_heat > b.log_heat : a.sequence > b.sequence; }};
  struct ProofEntry { std::uint64_t expiry, generation; std::uint32_t key; };
  struct ProofGreater { bool operator()(const ProofEntry& a, const ProofEntry& b) const {
    return a.expiry != b.expiry ? a.expiry > b.expiry : a.key > b.key; }};
  struct GhostEntry { std::uint32_t key, generation; };
  struct ColdBucketEntry { std::uint32_t key; std::uint64_t generation; };
  static constexpr std::size_t kColdBucketCount = 64;
  static constexpr double NegInf() { return -std::numeric_limits<double>::infinity(); }
  static double LogAdd(double a, double b) {
    if (a == NegInf()) return b;
    if (b == NegInf()) return a;
    const double m = std::max(a,b); return m + std::log1p(std::exp(std::min(a,b)-m));
  }
  static std::size_t ComputeGhostCapacity(std::size_t cap, double ratio) {
    if (ratio <= 0.0) return 0;
    const double scaled = static_cast<double>(cap) * ratio;
    if (!std::isfinite(scaled) ||
        scaled >= static_cast<double>(std::numeric_limits<std::size_t>::max())) {
      return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(scaled);
  }
  bool Protected(const State& s) const { return active_protection_epoch_ && s.protection_epoch == active_protection_epoch_; }
  std::size_t ResidentSize() const { return cold_size_ + hot_size_; }
  double DepthWeight(int depth) const {
    if (neutral_depth_gain_) return 1.0;
    return std::pow(decay_, depth_stride_ *
                            (std::clamp(depth, 1, static_cast<int>(max_depth_)) - 1));
  }
  void AddHeat(State& s, int depth, double multiplier) {
    s.last_depth = std::clamp(depth, 1, static_cast<int>(max_depth_));
    const double gain = 51.2 * DepthWeight(depth) * std::max(0.0, multiplier);
    if (gain > 0) s.log_heat = LogAdd(s.log_heat, std::log(gain) - version_ * log_decay_);
  }
  void PushHeat(std::uint32_t key, State& s) {
    if (s.place == Place::kCold && bucketed_cold_) {
      PushColdBucket(key, s, CurrentColdBucket(s));
      return;
    }
    ++s.heat_generation;
    HeatEntry entry{s.log_heat, next_sequence_++, s.heat_generation, key};
    if (s.place == Place::kCold) cold_heat_.push(entry);
    else if (s.place != Place::kNone) hot_heat_.push(entry);
  }
  std::size_t CurrentColdBucket(const State& s) const {
    if (pure_depth_cold_) {
      return static_cast<std::size_t>(max_depth_ -
          std::clamp(s.last_depth, 1, static_cast<int>(max_depth_)));
    }
    // Heat is normalized at the current logical version. A bucket spans one
    // decay step, so a cold object moves only when examined for eviction.
    const double current_log_heat = s.log_heat + version_ * log_decay_;
    const double floor_log_heat = std::log(51.2) + log_decay_;
    const double steps = (current_log_heat - floor_log_heat) / -log_decay_;
    if (!std::isfinite(steps) || steps <= 0.0) return 0;
    return std::min<std::size_t>(kColdBucketCount - 1,
        static_cast<std::size_t>(std::floor(steps)));
  }
  void PushColdBucket(std::uint32_t key, State& s, std::size_t bucket) {
    bucket = std::min(bucket, cold_bucket_count_ - 1);
    ++s.cold_bucket_generation;
    cold_buckets_[bucket].push_back({key, s.cold_bucket_generation});
  }
  std::uint64_t ProofDuration(const State& s) const {
    const double heat = std::exp(s.log_heat + version_ * log_decay_);
    // Proof ends when its present LSBT evidence decays below one base gain.
    return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(std::max(0.0, std::log(heat / 51.2) / -log_decay_))));
  }
  void ScheduleProof(std::uint32_t key, State& s) {
    ++s.proof_generation;
    const std::uint64_t ttl = fixed_hot_ttl_ == 0 ? ProofDuration(s) : fixed_hot_ttl_;
    if (turnover_factor_ > 0.0) {
      const double span = std::max(1.0, (adaptive_lease_ ? adaptive_lease_factor_ : turnover_factor_) *
          static_cast<double>(std::max<std::size_t>(1, capacity_)));
      const ProofEntry entry{admission_clock_ +
                                 static_cast<std::uint64_t>(std::ceil(span)),
                             s.proof_generation, key};
      turnover_wheel_[entry.expiry % turnover_wheel_size_].push_back(entry);
      hot_fifo_.push_back(entry);
    } else {
      proof_heap_.push({version_ + ttl, s.proof_generation, key});
    }
  }

  void AdvanceTurnoverExpiry() {
    while (turnover_wheel_cursor_ <= admission_clock_) {
      auto& bucket = turnover_wheel_[turnover_wheel_cursor_ % turnover_wheel_size_];
      std::deque<ProofEntry> pending;
      pending.swap(bucket);
      for (const ProofEntry& e : pending) {
        if (e.expiry > admission_clock_) {
          turnover_wheel_[e.expiry % turnover_wheel_size_].push_back(e);
          continue;
        }
        auto it = states_.find(e.key);
        if (it == states_.end() || it->second.place != Place::kHotActive ||
            it->second.proof_generation != e.generation) continue;
        it->second.place = Place::kHotStale;
        ++counters_.proof_expiries;
        ++adaptive_lease_expiries_;
        FinalizeEviction(e.key, it->second);
      }
      ++turnover_wheel_cursor_;
    }
  }
  bool Current(const HeatEntry& e, Place wanted) const {
    const auto it=states_.find(e.key); return it != states_.end() && it->second.place == wanted && it->second.heat_generation == e.generation;
  }
  bool EvictCold() {
    if (fifo_cold_) return EvictColdFifo();
    return bucketed_cold_ ? EvictColdBucketed() : EvictByPlace(Place::kCold);
  }
  bool EvictColdFifo() {
    std::size_t scanned = 0;
    const std::size_t limit = cold_size_;
    while (!cold_fifo_.empty() && scanned < limit) {
      const std::uint32_t key = cold_fifo_.front(); cold_fifo_.pop_front();
      auto it = states_.find(key);
      if (it == states_.end() || it->second.place != Place::kCold) continue;
      if (Protected(it->second)) {
        ++counters_.protected_candidates;
        cold_fifo_.push_back(key);
        ++scanned;
        continue;
      }
      FinalizeEviction(key, it->second);
      return true;
    }
    if (scanned > 0) ++counters_.protected_rejects;
    return false;
  }
  bool EvictColdBucketed() {
    std::size_t scanned = 0;
    const std::size_t limit = cold_size_;
    while (scanned < limit) {
      std::size_t bucket = cold_bucket_count_;
      for (std::size_t i = 0; i < cold_bucket_count_; ++i) {
        if (!cold_buckets_[i].empty()) { bucket = i; break; }
      }
      if (bucket == cold_bucket_count_) return false;
      const ColdBucketEntry entry = cold_buckets_[bucket].front();
      cold_buckets_[bucket].pop_front();
      auto it = states_.find(entry.key);
      if (it == states_.end() || it->second.place != Place::kCold ||
          it->second.cold_bucket_generation != entry.generation) continue;
      State& s = it->second;
      const std::size_t actual = CurrentColdBucket(s);
      if (actual < bucket) {
        PushColdBucket(entry.key, s, actual);
        continue;
      }
      if (Protected(s)) {
        ++counters_.protected_candidates;
        PushColdBucket(entry.key, s, bucket);
        ++scanned;
        continue;
      }
      FinalizeEviction(entry.key, s);
      return true;
    }
    if (scanned > 0) ++counters_.protected_rejects;
    return false;
  }
  bool EvictHot() {
    if (turnover_factor_ > 0.0) return EvictHotFifo();
    if (EvictHotState(Place::kHotStale)) return true;
    return EvictHotState(Place::kHotActive);
  }
  bool EvictHotFifo() {
    std::size_t scanned = 0;
    const std::size_t limit = hot_size_;
    while (!hot_fifo_.empty() && scanned < limit) {
      const ProofEntry e = hot_fifo_.front(); hot_fifo_.pop_front();
      auto it = states_.find(e.key);
      if (it == states_.end() || it->second.place != Place::kHotActive ||
          it->second.proof_generation != e.generation) continue;
      if (Protected(it->second)) {
        ++counters_.protected_candidates;
        hot_fifo_.push_back(e);
        ++scanned;
        continue;
      }
      FinalizeEviction(e.key, it->second);
      return true;
    }
    if (scanned > 0) ++counters_.protected_rejects;
    return false;
  }
  bool EvictHotState(Place wanted) {
    std::vector<HeatEntry> deferred;
    while (!hot_heat_.empty()) {
      HeatEntry e=hot_heat_.top(); hot_heat_.pop();
      const auto it=states_.find(e.key);
      if (it == states_.end() || it->second.heat_generation != e.generation ||
          (it->second.place != Place::kHotActive && it->second.place != Place::kHotStale)) {
        ++counters_.stale_heap_pops; continue;
      }
      State& s=it->second;
      if (s.place != wanted || Protected(s)) { if (Protected(s)) ++counters_.protected_candidates; deferred.push_back(e); continue; }
      for (const auto& d:deferred) hot_heat_.push(d);
      FinalizeEviction(e.key,s);
      return true;
    }
    for (const auto& d:deferred) hot_heat_.push(d);
    if (!deferred.empty()) ++counters_.protected_rejects;
    return false;
  }
  bool EvictByPlace(Place wanted) {
    std::vector<HeatEntry> deferred;
    auto& queue = wanted == Place::kCold ? cold_heat_ : hot_heat_;
    while (!queue.empty()) {
      HeatEntry e=queue.top(); queue.pop();
      if (!Current(e,wanted)) { ++counters_.stale_heap_pops; continue; }
      State& s=states_.at(e.key);
      if (Protected(s)) { ++counters_.protected_candidates; deferred.push_back(e); continue; }
      for (const auto& d:deferred) queue.push(d);
      FinalizeEviction(e.key,s);
      return true;
    }
    for (const auto& d:deferred) queue.push(d);
    if (!deferred.empty()) ++counters_.protected_rejects;
    return false;
  }
  void Promote(std::uint32_t key, State& s) {
    if (hot_capacity_ == 0 || (!unified_capacity_ && hot_size_ >= hot_capacity_ && !EvictHot())) return;
    --cold_size_; ++hot_size_; s.place=Place::kHotActive; ++counters_.promotions;
    if (turnover_factor_ == 0.0) PushHeat(key,s);
    ScheduleProof(key,s);
  }
  void FinalizeEviction(std::uint32_t key, State& s) {
    const bool was_hot = s.place != Place::kCold;
    if (!was_hot) --cold_size_; else --hot_size_;
    s.place=Place::kNone; s.protection_epoch=0; ++counters_.evictions;
    pending_evicted_keys_.push_back(key);
    if (was_hot && !all_victims_to_ghost_) {
      s.in_ghost = false;
      if (ghost_capacity_ == 0) states_.erase(key);
      return;
    }
    s.ghost_from_hot = was_hot;
    InsertGhost(key,s);
    if (ghost_capacity_ == 0) states_.erase(key);
  }
  void InsertGhost(std::uint32_t key, State& s) {
    if (s.in_ghost || ghost_capacity_ == 0) return;
    s.in_ghost=true; ++s.ghost_generation; ++ghost_size_; ++adaptive_ghost_inserts_; ghost_ring_.push_back({key,s.ghost_generation});
    while (ghost_size_ > ghost_capacity_) { const GhostEntry e=ghost_ring_[ghost_head_++]; auto it=states_.find(e.key);
      if (it != states_.end() && it->second.in_ghost && it->second.ghost_generation == e.generation) { states_.erase(it); --ghost_size_; ++adaptive_ghost_expired_; }}
  }
  void RemoveGhost(State& s) { s.in_ghost=false; s.ghost_from_hot=false; ++s.ghost_generation; --ghost_size_; }
  void MaybeAdaptGhost() {
    if (!adaptive_ghost_ || adaptive_epoch_ == 0 || admission_clock_ % adaptive_epoch_ != 0) return;
    const double hit_rate = adaptive_ghost_inserts_ == 0 ? 0.0 : static_cast<double>(adaptive_ghost_hits_) / adaptive_ghost_inserts_;
    const double expire_rate = adaptive_ghost_inserts_ == 0 ? 0.0 : static_cast<double>(adaptive_ghost_expired_) / adaptive_ghost_inserts_;
    static constexpr double kRatios[] = {0.5, 0.9, 2.0, 4.0, 8.0, 16.0};
    if (expire_rate > 0.45 && hit_rate > 0.01 && adaptive_ratio_index_ + 1 < 6) ++adaptive_ratio_index_;
    else if (expire_rate < 0.12 && hit_rate < 0.005 && adaptive_ratio_index_ > 0) --adaptive_ratio_index_;
    else { adaptive_ghost_hits_ = adaptive_ghost_expired_ = adaptive_ghost_inserts_ = 0; return; }
    ghost_capacity_ = ComputeGhostCapacity(capacity_, kRatios[adaptive_ratio_index_]);
    while (ghost_size_ > ghost_capacity_ && ghost_head_ < ghost_ring_.size()) {
      const GhostEntry e = ghost_ring_[ghost_head_++]; auto it = states_.find(e.key);
      if (it != states_.end() && it->second.in_ghost && it->second.ghost_generation == e.generation) { states_.erase(it); --ghost_size_; }
    }
    adaptive_ghost_hits_ = adaptive_ghost_expired_ = adaptive_ghost_inserts_ = 0;
  }
  void MaybeAdaptLease() {
    if (!adaptive_lease_ || adaptive_epoch_ == 0 || admission_clock_ % adaptive_epoch_ != 0) return;
    static constexpr double kLeases[] = {1.0, 2.0, 4.0, 8.0, 16.0};
    const double late_ratio = adaptive_lease_expiries_ == 0 ? 0.0 :
        static_cast<double>(adaptive_lease_late_reuses_) / adaptive_lease_expiries_;
    const double expiry_ratio = static_cast<double>(adaptive_lease_expiries_) /
        std::max<std::uint64_t>(1, adaptive_lease_expiries_ + adaptive_lease_active_hits_);
    const double before = adaptive_lease_factor_;
    if (late_ratio > 0.01 && adaptive_lease_index_ + 1 < 5) ++adaptive_lease_index_;
    else if (expiry_ratio > 0.75 && late_ratio < 0.005 && adaptive_lease_index_ > 0) --adaptive_lease_index_;
    adaptive_lease_factor_ = kLeases[adaptive_lease_index_];
    lease_control_samples_.push_back({admission_clock_, before,
                                      adaptive_lease_factor_,
                                      adaptive_lease_active_hits_,
                                      adaptive_lease_late_reuses_,
                                      adaptive_lease_expiries_});
    adaptive_lease_expiries_ = adaptive_lease_late_reuses_ = adaptive_lease_active_hits_ = 0;
  }
  std::uint16_t NextEpoch() { if (++next_protection_epoch_ == 0) { for (auto& x:states_) x.second.protection_epoch=0; next_protection_epoch_=1; } return next_protection_epoch_; }
  std::size_t capacity_, max_depth_, cold_capacity_, hot_capacity_, ghost_capacity_;
  double decay_, log_decay_, depth_stride_; bool neutral_depth_gain_, unified_capacity_, ghost_hit_hot_, preserve_ghost_heat_; std::uint64_t fixed_hot_ttl_, admission_clock_=0, turnover_wheel_cursor_=0, version_=1, next_sequence_=0; double turnover_factor_ = 0.0; std::size_t turnover_wheel_size_ = 0;
  std::size_t cold_size_=0, hot_size_=0, ghost_size_=0, ghost_head_=0;
  bool adaptive_ghost_ = false, adaptive_lease_ = false; std::size_t adaptive_epoch_ = 0, adaptive_ratio_index_ = 4, adaptive_lease_index_ = 3;
  double adaptive_lease_factor_ = 8.0;
  std::uint64_t adaptive_ghost_hits_ = 0, adaptive_ghost_expired_ = 0, adaptive_ghost_inserts_ = 0;
  std::uint64_t adaptive_lease_expiries_ = 0, adaptive_lease_late_reuses_ = 0, adaptive_lease_active_hits_ = 0;
  std::vector<LeaseControlSample> lease_control_samples_;
  std::uint16_t active_protection_epoch_=0, next_protection_epoch_=0; DebugCounters counters_;
  std::unordered_map<std::uint32_t,State> states_;
  std::priority_queue<HeatEntry,std::vector<HeatEntry>,HeatGreater> cold_heat_, hot_heat_;
  std::priority_queue<ProofEntry,std::vector<ProofEntry>,ProofGreater> proof_heap_;
  bool bucketed_cold_ = false, pure_depth_cold_ = false;
  std::size_t cold_bucket_count_ = 0;
  std::vector<std::deque<ColdBucketEntry>> cold_buckets_;
  bool all_victims_to_ghost_ = true;
  bool fifo_cold_ = false;
  std::vector<std::deque<ProofEntry>> turnover_wheel_;
  std::deque<ProofEntry> hot_fifo_;
  std::deque<std::uint32_t> cold_fifo_;
  std::vector<GhostEntry> ghost_ring_;
  std::vector<std::uint32_t> pending_evicted_keys_;
};

class PRD final : public PRDCore {
 public:
  PRD(std::size_t key_count, std::size_t capacity, int max_depth = 20,
      double history_retention = 0.95)
      : PRDCore(key_count, capacity, max_depth, history_retention, 1, 0,
                false, false, true, true, 0, 8.0, true, false, 8.0) {
    EnableAdaptiveGhost();
    EnableAdaptiveLease();
  }
};
