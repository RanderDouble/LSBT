#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

// Exact-heat counterpart of LSBTdiscrete.
// It uses the same online signal and protection semantics, but removes heat
// quantization: resident victims are selected by an exact min-heap.
class LSBTconsecutive {
 public:
  struct DebugCounters {
    std::uint64_t protected_installs = 0;
    std::uint64_t protected_candidates = 0;
    std::uint64_t protected_rejects = 0;
    std::uint64_t evictions = 0;
    std::uint64_t stale_heap_pops = 0;
  };

  static constexpr double kDefaultGainScale = 51.2;
  static constexpr double kDefaultMinHistoryRetention = 0.92;
  static constexpr double kDefaultMaxHistoryRetention = 0.98;
  static constexpr double kDefaultGhostSizeRatio = 8.0;

  LSBTconsecutive(
      std::size_t key_count, std::size_t capacity, int max_depth,
      double history_retention,
      int protect_scan_budget = 16,
      double min_history_retention = kDefaultMinHistoryRetention,
      double max_history_retention = kDefaultMaxHistoryRetention,
      double gain_scale = kDefaultGainScale,
      double ghost_size_ratio = kDefaultGhostSizeRatio,
      bool enable_renewal_lease = false,
      std::uint32_t lease_base_turnovers = 2,
      std::uint32_t lease_step_turnovers = 2,
      std::uint32_t lease_max_turnovers = 8,
      bool neutral_depth_gain = false)
      : capacity_(capacity),
        max_depth_(std::max(1, max_depth)),
        ghost_capacity_(ComputeGhostCapacity(capacity, ghost_size_ratio)),
        decay_(ClampHistoryRetention(history_retention, min_history_retention,
                                     max_history_retention)),
        log_decay_(std::log(decay_)),
        depth_bucket_stride_(max_depth_ <= 1
                                 ? 0.0
                                 : 1.0 / static_cast<double>(max_depth_ - 1)),
        gain_scale_(std::max(1e-12, gain_scale)),
        protect_scan_budget_(std::max(1, protect_scan_budget)),
        enable_renewal_lease_(enable_renewal_lease),
        lease_base_turnovers_(std::max<std::uint32_t>(1,
                                                      lease_base_turnovers)),
        lease_step_turnovers_(lease_step_turnovers),
        lease_max_turnovers_(std::max(lease_base_turnovers_,
                                      lease_max_turnovers)),
        neutral_depth_gain_(neutral_depth_gain) {
    const std::size_t reserve_size =
        capacity_ > std::numeric_limits<std::size_t>::max() - ghost_capacity_
            ? std::numeric_limits<std::size_t>::max()
            : capacity_ + ghost_capacity_;
    states_.reserve(std::min(key_count, reserve_size));
    if (ghost_capacity_ > 0) {
      ghost_ring_.reserve(ghost_capacity_ + 1);
    }
  }

  bool Access(std::uint32_t key_id, int depth,
              double gain_multiplier = 1.0) {
    State& state = GetOrCreateState(key_id);
    const bool hit = state.resident;
    if (!hit) {
      InsertGhost(key_id, state);
    }
    ObserveAccess(key_id, state, depth, gain_multiplier);
    if (hit) {
      RenewLease(key_id, state);
    }
    return hit;
  }

  bool Admit(std::uint32_t key_id, int /*depth*/) {
    auto state_it = states_.find(key_id);
    if (state_it == states_.end()) {
      state_it = states_.try_emplace(key_id).first;
    }
    State& state = state_it->second;
    if (state.resident) {
      return true;
    }
    if (capacity_ == 0) {
      if (!state.in_ghost) {
        states_.erase(state_it);
      }
      return false;
    }

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

    RemoveGhost(state);
    state.resident = true;
    state.protection_epoch = 0;
    ++resident_size_;
    PushHeapEntry(key_id, state);
    InitializeLease(key_id, state);
    return true;
  }

  bool Contains(std::uint32_t key_id) const noexcept {
    const auto it = states_.find(key_id);
    return it != states_.end() && it->second.resident;
  }

  std::size_t resident_size() const noexcept { return resident_size_; }

  std::vector<std::uint32_t> TakeEvictedKeys() {
    std::vector<std::uint32_t> evicted;
    evicted.swap(pending_evicted_keys_);
    return evicted;
  }

  void ClearProtectedKeys() noexcept { active_protection_epoch_ = 0; }

  void BeginProtectedKeyInstall() {
    active_protection_epoch_ = NextProtectionEpoch();
  }

  void ProtectKeyIfResident(std::uint32_t key_id) {
    if (active_protection_epoch_ == 0) {
      return;
    }
    const auto it = states_.find(key_id);
    if (it == states_.end() || !it->second.resident ||
        it->second.protection_epoch == active_protection_epoch_) {
      return;
    }
    it->second.protection_epoch = active_protection_epoch_;
    ++counters_.protected_installs;
  }

  void FinishBlock() {
    ClearProtectedKeys();
    ++current_version_;
  }

  const DebugCounters& counters() const noexcept { return counters_; }

  void SetDepthBucketStride(double stride) {
    depth_bucket_stride_ = std::max(0.0, stride);
  }

  // Uses exact subtree coverage for a complete b-ary tree. With the trace's
  // one-based depth convention, depth 1 is the root and has coverage 1.
  void SetCoverageBranchingFactor(double branching_factor) {
    coverage_branching_factor_ = std::max(1.0, branching_factor);
    use_coverage_gain_ = coverage_branching_factor_ > 1.0;
  }

  // Test/diagnostic hook: returns exact heat at the current block version.
  double DebugCurrentHeat(std::uint32_t key_id) const {
    const auto it = states_.find(key_id);
    if (it == states_.end() ||
        it->second.log_normalized_heat == NegativeInfinity()) {
      return 0.0;
    }
    return std::exp(it->second.log_normalized_heat +
                    static_cast<double>(current_version_) * log_decay_);
  }

 private:
  struct State {
    double log_normalized_heat = NegativeInfinity();
    std::uint64_t heap_generation = 0;
    std::uint64_t lease_generation = 0;
    std::uint32_t ghost_generation = 0;
    std::uint16_t protection_epoch = 0;
    std::uint64_t last_proof_version = 0;
    std::uint8_t proof_count = 0;
    bool resident = false;
    bool in_ghost = false;
  };

  struct HeapEntry {
    double log_normalized_heat = 0.0;
    std::uint64_t sequence = 0;
    std::uint64_t generation = 0;
    std::uint32_t key_id = 0;
  };

  struct HeapGreater {
    bool operator()(const HeapEntry& lhs, const HeapEntry& rhs) const {
      if (lhs.log_normalized_heat != rhs.log_normalized_heat) {
        return lhs.log_normalized_heat > rhs.log_normalized_heat;
      }
      return lhs.sequence > rhs.sequence;
    }
  };

  struct LeaseEntry {
    std::uint64_t expiry_version = 0;
    std::uint64_t generation = 0;
    std::uint32_t key_id = 0;
  };

  struct LeaseGreater {
    bool operator()(const LeaseEntry& lhs, const LeaseEntry& rhs) const {
      if (lhs.expiry_version != rhs.expiry_version) {
        return lhs.expiry_version > rhs.expiry_version;
      }
      return lhs.key_id > rhs.key_id;
    }
  };

  struct GhostRecord {
    std::uint32_t key_id = 0;
    std::uint32_t generation = 0;
  };

  static constexpr double NegativeInfinity() {
    return -std::numeric_limits<double>::infinity();
  }

  static double LogAddExp(double lhs, double rhs) {
    if (lhs == NegativeInfinity()) {
      return rhs;
    }
    if (rhs == NegativeInfinity()) {
      return lhs;
    }
    const double maximum = std::max(lhs, rhs);
    return maximum +
           std::log1p(std::exp(std::min(lhs, rhs) - maximum));
  }

  static double ClampHistoryRetention(double value, double min_value,
                                      double max_value) {
    const double bounded_min =
        std::clamp(std::min(min_value, max_value), 1e-6, 1.0 - 1e-9);
    const double bounded_max =
        std::clamp(std::max(min_value, max_value), bounded_min, 1.0 - 1e-9);
    return std::clamp(value, bounded_min, bounded_max);
  }

  static std::size_t ComputeGhostCapacity(std::size_t capacity,
                                          double ghost_size_ratio) {
    if (capacity == 0 || !std::isfinite(ghost_size_ratio) ||
        ghost_size_ratio <= 0.0) {
      return 0;
    }
    const double maximum =
        static_cast<double>(std::numeric_limits<std::size_t>::max());
    if (ghost_size_ratio >= maximum / static_cast<double>(capacity)) {
      return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(
        static_cast<double>(capacity) * ghost_size_ratio);
  }

  State& GetOrCreateState(std::uint32_t key_id) {
    return states_.try_emplace(key_id).first->second;
  }

  double NormalizeDepth(double depth) const {
    if (max_depth_ <= 1) {
      return 0.0;
    }
    return std::clamp((depth - 1.0) / static_cast<double>(max_depth_ - 1),
                      0.0, 1.0);
  }

  double DepthWeight(int depth) const {
    if (neutral_depth_gain_) {
      return 1.0;
    }
    const int clipped_depth = std::clamp(depth, 1, max_depth_);
    if (use_coverage_gain_) {
      return std::pow(coverage_branching_factor_,
                      -static_cast<double>(clipped_depth - 1));
    }
    return std::pow(decay_, depth_bucket_stride_ *
                                static_cast<double>(clipped_depth - 1));
  }

  void ObserveAccess(std::uint32_t key_id, State& state, int depth,
                     double gain_multiplier) {
    const double gain = gain_scale_ * DepthWeight(depth) *
                        std::max(0.0, gain_multiplier);
    if (gain > 0.0) {
      // H_v / d^v is time invariant between accesses. Log space avoids
      // overflow when a trace contains many block versions.
      const double log_normalized_gain =
          std::log(gain) - static_cast<double>(current_version_) * log_decay_;
      state.log_normalized_heat =
          LogAddExp(state.log_normalized_heat, log_normalized_gain);
    }
    if (state.resident) {
      PushHeapEntry(key_id, state);
    }
  }

  void PushHeapEntry(std::uint32_t key_id, State& state) {
    ++state.heap_generation;
    heap_.push(HeapEntry{state.log_normalized_heat, next_heap_sequence_++,
                         state.heap_generation, key_id});
  }

  std::uint64_t LeaseDuration(const State& state) const {
    const std::uint64_t expanded =
        static_cast<std::uint64_t>(lease_base_turnovers_) +
        static_cast<std::uint64_t>(state.proof_count) *
            lease_step_turnovers_;
    return std::min<std::uint64_t>(lease_max_turnovers_, expanded);
  }

  void ScheduleLease(std::uint32_t key_id, State& state) {
    if (!enable_renewal_lease_) {
      return;
    }
    ++state.lease_generation;
    lease_heap_.push(LeaseEntry{
        current_version_ + LeaseDuration(state), state.lease_generation,
        key_id});
  }

  void InitializeLease(std::uint32_t key_id, State& state) {
    if (!enable_renewal_lease_) {
      return;
    }
    state.proof_count = 0;
    state.last_proof_version = current_version_;
    ScheduleLease(key_id, state);
  }

  void RenewLease(std::uint32_t key_id, State& state) {
    if (!enable_renewal_lease_) {
      return;
    }
    if (current_version_ > state.last_proof_version) {
      state.last_proof_version = current_version_;
      state.proof_count = static_cast<std::uint8_t>(std::min<int>(
          3, static_cast<int>(state.proof_count) + 1));
    }
    ScheduleLease(key_id, state);
  }

  bool IsCurrent(const HeapEntry& entry) const {
    const auto it = states_.find(entry.key_id);
    return it != states_.end() && it->second.resident &&
           it->second.heap_generation == entry.generation;
  }

  bool IsProtected(const State& state) const {
    return active_protection_epoch_ != 0 &&
           state.protection_epoch == active_protection_epoch_;
  }

  bool IsCurrentLease(const LeaseEntry& entry) const {
    const auto it = states_.find(entry.key_id);
    return it != states_.end() && it->second.resident &&
           it->second.lease_generation == entry.generation;
  }

  void FinalizeEviction(std::uint32_t key_id, State& state) {
    state.resident = false;
    state.protection_epoch = 0;
    --resident_size_;
    pending_evicted_keys_.push_back(key_id);
    ++counters_.evictions;
    InsertGhost(key_id, state);
    if (ghost_capacity_ == 0) {
      states_.erase(key_id);
    }
  }

  bool EvictExpiredLease() {
    if (!enable_renewal_lease_) {
      return false;
    }
    std::vector<LeaseEntry> deferred;
    while (!lease_heap_.empty() &&
           lease_heap_.top().expiry_version <= current_version_) {
      const LeaseEntry entry = lease_heap_.top();
      lease_heap_.pop();
      if (!IsCurrentLease(entry)) {
        continue;
      }
      State& state = states_.at(entry.key_id);
      if (IsProtected(state)) {
        ++counters_.protected_candidates;
        deferred.push_back(entry);
        continue;
      }
      for (const LeaseEntry& protected_entry : deferred) {
        lease_heap_.push(protected_entry);
      }
      FinalizeEviction(entry.key_id, state);
      return true;
    }
    for (const LeaseEntry& protected_entry : deferred) {
      lease_heap_.push(protected_entry);
    }
    return false;
  }

  bool EvictOne() {
    if (EvictExpiredLease()) {
      return true;
    }
    std::vector<HeapEntry> protected_entries;
    protected_entries.reserve(static_cast<std::size_t>(protect_scan_budget_));

    while (!heap_.empty()) {
      const HeapEntry candidate = heap_.top();
      heap_.pop();
      if (!IsCurrent(candidate)) {
        ++counters_.stale_heap_pops;
        continue;
      }

      State& state = states_.at(candidate.key_id);
      if (IsProtected(state)) {
        ++counters_.protected_candidates;
        protected_entries.push_back(candidate);
        if (static_cast<int>(protected_entries.size()) >=
            protect_scan_budget_) {
          for (const HeapEntry& entry : protected_entries) {
            heap_.push(entry);
          }
          ++counters_.protected_rejects;
          return false;
        }
        continue;
      }

      for (const HeapEntry& entry : protected_entries) {
        heap_.push(entry);
      }
      FinalizeEviction(candidate.key_id, state);
      return true;
    }

    for (const HeapEntry& entry : protected_entries) {
      heap_.push(entry);
    }
    if (!protected_entries.empty()) {
      ++counters_.protected_rejects;
    }
    return false;
  }

  std::uint16_t NextProtectionEpoch() {
    ++next_protection_epoch_;
    if (next_protection_epoch_ == 0) {
      for (auto& item : states_) {
        item.second.protection_epoch = 0;
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
    if (state.in_ghost || ghost_capacity_ == 0) {
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
    --ghost_live_size_;
  }

  std::size_t capacity_ = 0;
  int max_depth_ = 1;
  std::size_t ghost_capacity_ = 0;
  double decay_ = 0.95;
  double log_decay_ = std::log(0.95);
  double depth_bucket_stride_ = 0.0;
  double gain_scale_ = kDefaultGainScale;
  int protect_scan_budget_ = 16;
  bool enable_renewal_lease_ = false;
  std::uint32_t lease_base_turnovers_ = 2;
  std::uint32_t lease_step_turnovers_ = 2;
  std::uint32_t lease_max_turnovers_ = 8;
  bool neutral_depth_gain_ = false;
  bool use_coverage_gain_ = false;
  double coverage_branching_factor_ = 1.0;
  std::uint64_t current_version_ = 1;
  std::uint64_t next_heap_sequence_ = 0;
  std::size_t resident_size_ = 0;
  DebugCounters counters_;

  std::unordered_map<std::uint32_t, State> states_;
  std::priority_queue<HeapEntry, std::vector<HeapEntry>, HeapGreater> heap_;
  std::priority_queue<LeaseEntry, std::vector<LeaseEntry>, LeaseGreater>
      lease_heap_;
  std::vector<GhostRecord> ghost_ring_;
  std::vector<std::uint32_t> pending_evicted_keys_;
  std::uint16_t active_protection_epoch_ = 0;
  std::uint16_t next_protection_epoch_ = 0;
  std::size_t ghost_live_size_ = 0;
  std::size_t ghost_head_index_ = 0;
};
