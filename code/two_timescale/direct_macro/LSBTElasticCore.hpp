#pragma once

#include "LSBTGhostFIFO.hpp"
#include "LSBTTimingWheel.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

// Paper-candidate LSBT core.
// - Same resident/renewal semantics as LSBTSimple.
// - Dynamic resident limit for elastic sharing.
// - Configurable ghost ratio (Macro: 8C, Micro: 0).
// - ResetForCapacity() lets the epoch-local Micro reuse its allocations.
class LSBTElasticCore {
 public:
  struct Counters {
    std::uint64_t evictions = 0;
    std::uint64_t admission_rejects = 0;
    std::uint64_t renewals = 0;
    std::uint64_t proof_expiries = 0;
    std::uint64_t lazy_rescues = 0;
    std::uint64_t wheel_advances = 0;
    std::uint64_t ghost_peak = 0;
  };

  static constexpr double kDefaultRetention = 0.95;

  LSBTElasticCore(std::size_t key_count, std::size_t capacity, int max_depth,
                  double retention = kDefaultRetention,
                  double ghost_ratio = 8.0, bool capture_evictions = true)
      : key_count_(key_count),
        capacity_(capacity),
        resident_limit_(capacity),
        max_depth_(std::max(1, max_depth)),
        ghost_ratio_(std::max(0.0, ghost_ratio)),
        capture_evictions_(capture_evictions),
        ghost_capacity_(ComputeGhostCapacity(capacity_, ghost_ratio_)),
        bucket_count_(ComputeBucketCount(capacity_)),
        max_score_(bucket_count_ - 1),
        retention_(std::clamp(retention, 0.92, 0.98)),
        reference_heat_(1.0 / (1.0 - retention_)),
        minimum_heat_(reference_heat_ *
                      std::pow(retention_, static_cast<double>(max_score_))),
        ghost_(ghost_capacity_, states_),
        wheel_(bucket_count_, states_) {
    ReserveMetadata();
    BuildTransitionTable();
  }

  bool Access(std::uint32_t key_id, int depth) {
    auto [it, inserted] = states_.try_emplace(key_id);
    lsbt_detail::State& state = it->second;
    const bool hit = lsbt_detail::IsResident(state);
    if (!hit) RememberGhost(key_id, state);

    const int evidence_depth = inserted ? std::clamp(depth, 1, max_depth_) : 1;
    const std::uint16_t next_bucket =
        Transition(evidence_depth, CurrentBucket(state));
    state.expiry_version =
        current_version_ + static_cast<std::uint32_t>(next_bucket);
    if (hit) state.renewal_credit = 1;
    return hit;
  }

  bool Admit(std::uint32_t key_id, int /*depth*/) {
    auto it = states_.find(key_id);
    if (it == states_.end()) it = states_.try_emplace(key_id).first;
    lsbt_detail::State& state = it->second;
    if (lsbt_detail::IsResident(state)) return true;
    if (resident_limit_ == 0) {
      if (!lsbt_detail::IsGhost(state)) states_.erase(it);
      return false;
    }

    const bool restore_ghost = lsbt_detail::IsGhost(state);
    if (restore_ghost) ghost_.Remove(state);
    const std::size_t incoming_bucket = CurrentBucket(state);
    if (resident_size_ >= resident_limit_ && !EvictOne(incoming_bucket)) {
      if (restore_ghost) RememberGhost(key_id, state);
      else states_.erase(it);
      return false;
    }
    InsertResident(key_id, state);
    return true;
  }

  void Tick() {
    ++current_version_;
    wheel_.Advance();
    ++counters_.wheel_advances;
  }

  void SetResidentLimit(std::size_t limit) {
    resident_limit_ = std::min(limit, capacity_);
    while (resident_size_ > resident_limit_) {
      if (!ForceEvictOne()) throw std::runtime_error("LSBT shrink failed");
    }
  }

  // Intended for the epoch-local Micro tier. For exact semantics, call with the
  // new epoch's Micro capacity. The StateTable buckets are retained by clear().
  // Micro uses ghost_ratio=0, so no ghost allocation is rebuilt.
  void ResetForCapacity(std::size_t capacity) {
    if (resident_size_ != 0 || ghost_.size() != 0 || !states_.empty()) {
      wheel_.Clear();
      ghost_.Clear();
      states_.clear();
    }
    capacity_ = capacity;
    resident_limit_ = capacity;
    ghost_capacity_ = ComputeGhostCapacity(capacity_, ghost_ratio_);
    if (ghost_capacity_ != ghost_.capacity()) {
      // The paper candidate only dynamically resets the ghostless Micro tier.
      // Macro is never reset. Guard against accidentally changing a ghostful
      // tier's capacity because GhostFIFO has fixed capacity by construction.
      throw std::logic_error(
          "ResetForCapacity only supports unchanged ghost capacity; use ghost_ratio=0 for Micro");
    }
    bucket_count_ = ComputeBucketCount(capacity_);
    max_score_ = bucket_count_ - 1;
    minimum_heat_ = reference_heat_ *
                    std::pow(retention_, static_cast<double>(max_score_));
    current_version_ = 1;
    resident_size_ = 0;
    peak_resident_size_ = 0;
    pending_evicted_keys_.clear();
    counters_ = Counters{};
    wheel_.Reconfigure(bucket_count_);
    BuildTransitionTable();
    ReserveMetadata();
  }

  bool Contains(std::uint32_t key_id) const {
    const auto it = states_.find(key_id);
    return it != states_.end() && lsbt_detail::IsResident(it->second);
  }

  void DrainEvictedKeys(std::vector<std::uint32_t>& out) {
    if (pending_evicted_keys_.empty()) return;
    out.insert(out.end(), pending_evicted_keys_.begin(),
               pending_evicted_keys_.end());
    pending_evicted_keys_.clear();
  }

  std::size_t resident_size() const { return resident_size_; }
  std::size_t resident_limit() const { return resident_limit_; }
  std::size_t ghost_size() const { return ghost_.size(); }
  std::size_t capacity() const { return capacity_; }
  const Counters& counters() const { return counters_; }

 private:
  static std::size_t ComputeBucketCount(std::size_t capacity) {
    const double value = static_cast<double>(std::max<std::size_t>(capacity, 1));
    return std::max<std::size_t>(
        1, static_cast<std::size_t>(std::llround(32.0 * std::log2(value + 1.0))));
  }

  static std::size_t ComputeGhostCapacity(std::size_t capacity,
                                          double ghost_ratio) {
    if (capacity == 0 || ghost_ratio <= 0.0) return 0;
    const double scaled = std::ceil(static_cast<double>(capacity) * ghost_ratio);
    return static_cast<std::size_t>(std::min(
        scaled, static_cast<double>(std::numeric_limits<std::int32_t>::max())));
  }

  void ReserveMetadata() {
    const std::size_t metadata_capacity =
        capacity_ > std::numeric_limits<std::size_t>::max() - ghost_capacity_
            ? std::numeric_limits<std::size_t>::max()
            : capacity_ + ghost_capacity_;
    states_.reserve(std::min(key_count_, metadata_capacity));
  }

  std::size_t LutIndex(int depth, std::size_t bucket) const {
    return static_cast<std::size_t>(depth) * bucket_count_ + bucket;
  }

  double StructuralWeight(int depth) const {
    const int c = std::min(std::max(depth - 1, 0), 2);
    return std::pow(retention_, static_cast<double>(c));
  }

  std::size_t HeatToBucket(double heat) const {
    if (heat <= 1e-12 || max_score_ == 0) return 0;
    const double bucket =
        std::log(std::max(heat, 1e-12) / minimum_heat_) /
        std::log(1.0 / retention_);
    return static_cast<std::size_t>(std::clamp(
        static_cast<long long>(std::llround(bucket)), 0LL,
        static_cast<long long>(max_score_)));
  }

  void BuildTransitionTable() {
    representative_heat_.assign(bucket_count_, 0.0);
    transition_lut_.assign((max_depth_ + 1) * bucket_count_, 0);
    for (std::size_t b = 0; b < bucket_count_; ++b) {
      representative_heat_[b] =
          minimum_heat_ * std::pow(1.0 / retention_, static_cast<double>(b));
    }
    for (int depth = 1; depth <= max_depth_; ++depth) {
      const double gain = StructuralWeight(depth);
      for (std::size_t b = 0; b < bucket_count_; ++b) {
        transition_lut_[LutIndex(depth, b)] = static_cast<std::uint16_t>(
            HeatToBucket(representative_heat_[b] + gain));
      }
    }
  }

  std::uint16_t Transition(int depth, std::size_t bucket) const {
    return transition_lut_[LutIndex(depth, bucket)];
  }

  std::uint16_t CurrentBucket(const lsbt_detail::State& state) const {
    const std::uint32_t remaining = state.expiry_version - current_version_;
    return remaining <= max_score_ ? static_cast<std::uint16_t>(remaining) : 0;
  }

  void RememberGhost(std::uint32_t key_id, lsbt_detail::State& state) {
    ghost_.Insert(key_id, state);
    counters_.ghost_peak =
        std::max<std::uint64_t>(counters_.ghost_peak, ghost_.size());
  }

  void InsertResident(std::uint32_t key_id, lsbt_detail::State& state) {
    ghost_.Remove(state);
    state.renewal_credit = 0;
    wheel_.Schedule(key_id, state, CurrentBucket(state));
    ++resident_size_;
    peak_resident_size_ = std::max(peak_resident_size_, resident_size_);
    if (resident_size_ > resident_limit_)
      throw std::runtime_error("LSBT resident limit exceeded");
  }

  bool ForceEvictOne() {
    while (resident_size_ != 0) {
      const auto candidate = wheel_.Front();
      if (!candidate.has_value()) return false;
      const std::uint32_t key_id = candidate->key_id;
      lsbt_detail::State& state = states_.at(key_id);
      const std::size_t actual_bucket = CurrentBucket(state);
      if (actual_bucket > candidate->physical_remaining) {
        wheel_.PopFront();
        wheel_.Schedule(key_id, state, actual_bucket);
        ++counters_.lazy_rescues;
        continue;
      }
      wheel_.PopFront();
      state.next_resident = lsbt_detail::kNotResident;
      state.renewal_credit = 0;
      --resident_size_;
      if (capture_evictions_) pending_evicted_keys_.push_back(key_id);
      ++counters_.evictions;
      RememberGhost(key_id, state);
      if (ghost_.capacity() == 0) states_.erase(key_id);
      return true;
    }
    return false;
  }

  bool EvictOne(std::size_t incoming_bucket) {
    while (resident_size_ != 0) {
      const auto candidate = wheel_.Front();
      if (!candidate.has_value()) return false;
      const std::uint32_t key_id = candidate->key_id;
      lsbt_detail::State& state = states_.at(key_id);
      const std::size_t actual_bucket = CurrentBucket(state);
      if (actual_bucket > candidate->physical_remaining) {
        wheel_.PopFront();
        wheel_.Schedule(key_id, state, actual_bucket);
        ++counters_.lazy_rescues;
        continue;
      }
      if (incoming_bucket <= actual_bucket) {
        if (state.renewal_credit != 0) {
          state.renewal_credit = 0;
          ++counters_.proof_expiries;
        }
        ++counters_.admission_rejects;
        return false;
      }
      if (state.renewal_credit != 0) {
        state.renewal_credit = 0;
        wheel_.PopFront();
        state.expiry_version =
            current_version_ + static_cast<std::uint32_t>(incoming_bucket);
        wheel_.Schedule(key_id, state, incoming_bucket);
        ++counters_.renewals;
        continue;
      }
      wheel_.PopFront();
      state.next_resident = lsbt_detail::kNotResident;
      state.renewal_credit = 0;
      --resident_size_;
      if (capture_evictions_) pending_evicted_keys_.push_back(key_id);
      ++counters_.evictions;
      RememberGhost(key_id, state);
      if (ghost_.capacity() == 0) states_.erase(key_id);
      return true;
    }
    return false;
  }

  std::size_t key_count_ = 0;
  std::size_t capacity_ = 0;
  std::size_t resident_limit_ = 0;
  int max_depth_ = 1;
  double ghost_ratio_ = 0.0;
  bool capture_evictions_ = true;
  std::size_t ghost_capacity_ = 0;
  std::size_t bucket_count_ = 1;
  std::size_t max_score_ = 0;
  double retention_ = kDefaultRetention;
  double reference_heat_ = 1.0;
  double minimum_heat_ = 1.0;
  std::uint32_t current_version_ = 1;
  std::size_t resident_size_ = 0;
  std::size_t peak_resident_size_ = 0;

  lsbt_detail::StateTable states_;
  lsbt_detail::GhostFIFO ghost_;
  lsbt_detail::TimingWheel wheel_;
  std::vector<std::uint16_t> transition_lut_;
  std::vector<double> representative_heat_;
  std::vector<std::uint32_t> pending_evicted_keys_;
  Counters counters_;
};
