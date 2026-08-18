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

class LSBTSimple {
 public:
  struct Counters {
    std::uint64_t evictions = 0;
    std::uint64_t admission_rejects = 0;
    std::uint64_t renewals = 0;
    std::uint64_t proof_expiries = 0;
    std::uint64_t lazy_rescues = 0;
    std::uint64_t wheel_advances = 0;
    std::uint64_t ghost_peak = 0;
    std::uint64_t fractional_clock_misses = 0;
    std::uint64_t fractional_clock_units = 0;
    std::uint64_t fractional_clock_ticks = 0;
    std::uint64_t fractional_clock_max_ticks_per_unit = 0;
  };

  static constexpr double kDefaultRetention = 0.95;
  static constexpr double kGhostCapacityRatio = 8.0;

  LSBTSimple(std::size_t key_count, std::size_t capacity, int max_depth,
             double retention = kDefaultRetention)
      : capacity_(capacity),
        max_depth_(std::max(1, max_depth)),
        ghost_capacity_(ComputeGhostCapacity(capacity)),
        bucket_count_(ComputeBucketCount(capacity)),
        max_score_(bucket_count_ - 1),
        retention_(std::clamp(retention, 0.92, 0.98)),
        reference_heat_(1.0 / (1.0 - retention_)),
        minimum_heat_(reference_heat_ *
                      std::pow(retention_, static_cast<double>(max_score_))),
        ghost_(ghost_capacity_, states_),
        wheel_(bucket_count_, states_),
        transition_lut_((max_depth_ + 1) * bucket_count_, 0) {
    const std::size_t metadata_capacity =
        capacity_ > std::numeric_limits<std::size_t>::max() - ghost_capacity_
            ? std::numeric_limits<std::size_t>::max()
            : capacity_ + ghost_capacity_;
    states_.reserve(std::min(key_count, metadata_capacity));
    BuildTransitionTable();
  }

  bool Access(std::uint32_t key_id, int depth) {
    auto [iterator, inserted] = states_.try_emplace(key_id);
    lsbt_detail::State& state = iterator->second;
    const bool hit = lsbt_detail::IsResident(state);
    if (!hit) {
      ObserveFractionalTurnoverMiss(depth);
      RememberGhost(key_id, state);
    }

    const int evidence_depth = inserted ? std::clamp(depth, 1, max_depth_) : 1;
    const std::uint16_t next_bucket =
        Transition(evidence_depth, CurrentBucket(state));
    state.expiry_version =
        current_version_ + static_cast<std::uint32_t>(next_bucket);
    if (hit) {
      state.renewal_credit = 1;
    }
    return hit;
  }

  bool Admit(std::uint32_t key_id, int) {
    auto iterator = states_.find(key_id);
    if (iterator == states_.end()) {
      iterator = states_.try_emplace(key_id).first;
    }
    lsbt_detail::State& state = iterator->second;
    if (lsbt_detail::IsResident(state)) {
      return true;
    }
    if (capacity_ == 0) {
      if (!lsbt_detail::IsGhost(state)) {
        states_.erase(iterator);
      }
      return false;
    }

    const bool restore_ghost = lsbt_detail::IsGhost(state);
    if (restore_ghost) {
      ghost_.Remove(state);
    }
    const std::size_t incoming_bucket = CurrentBucket(state);
    if (resident_size_ >= capacity_ && !EvictOne(incoming_bucket)) {
      if (restore_ghost) {
        RememberGhost(key_id, state);
      } else {
        states_.erase(iterator);
      }
      return false;
    }

    InsertResident(key_id, state);
    return true;
  }

  void FinishBlock() {
    ++current_version_;
    wheel_.Advance();
    ++counters_.wheel_advances;
  }

  void EnableFractionalTurnoverClock() {
    if (resident_size_ != 0 || ghost_.size() != 0) {
      throw std::logic_error(
          "fractional turnover clock must be enabled before replay");
    }
    fractional_turnover_clock_enabled_ = true;
  }

  std::size_t FinishFractionalTurnoverUnit() {
    if (!fractional_turnover_clock_enabled_) {
      throw std::logic_error("fractional turnover clock is not enabled");
    }
    ++counters_.fractional_clock_units;
    const std::size_t ticks = static_cast<std::size_t>(
        std::floor(fractional_clock_credit_ + 1e-12));
    fractional_clock_credit_ -= static_cast<double>(ticks);
    counters_.fractional_clock_ticks += ticks;
    counters_.fractional_clock_max_ticks_per_unit =
        std::max<std::uint64_t>(
            counters_.fractional_clock_max_ticks_per_unit, ticks);
    for (std::size_t tick = 0; tick < ticks; ++tick) {
      FinishBlock();
    }
    return ticks;
  }

  bool Contains(std::uint32_t key_id) const {
    const auto iterator = states_.find(key_id);
    return iterator != states_.end() &&
           lsbt_detail::IsResident(iterator->second);
  }

  std::vector<std::uint32_t> TakeEvictedKeys() {
    std::vector<std::uint32_t> evicted;
    evicted.swap(pending_evicted_keys_);
    return evicted;
  }

  std::size_t resident_size() const { return resident_size_; }
  std::size_t peak_resident_size() const { return peak_resident_size_; }
  std::size_t ghost_size() const { return ghost_.size(); }
  std::size_t ghost_capacity() const { return ghost_.capacity(); }
  double fractional_clock_credit() const { return fractional_clock_credit_; }
  double fractional_clock_lifetime_mass() const {
    return fractional_clock_lifetime_mass_;
  }
  const Counters& counters() const { return counters_; }

  static constexpr std::size_t StateSizeBytes() {
    return sizeof(lsbt_detail::State);
  }

 private:
  static std::size_t ComputeBucketCount(std::size_t capacity) {
    const double value =
        static_cast<double>(std::max<std::size_t>(capacity, 1));
    return std::max<std::size_t>(
        1, static_cast<std::size_t>(
               std::llround(32.0 * std::log2(value + 1.0))));
  }

  static std::size_t ComputeGhostCapacity(std::size_t capacity) {
    if (capacity == 0) {
      return 0;
    }
    const double scaled =
        std::ceil(static_cast<double>(capacity) * kGhostCapacityRatio);
    return static_cast<std::size_t>(std::min(
        scaled,
        static_cast<double>(std::numeric_limits<std::int32_t>::max())));
  }

  std::size_t LutIndex(int depth, std::size_t bucket) const {
    return static_cast<std::size_t>(depth) * bucket_count_ + bucket;
  }

  double StructuralWeight(int depth) const {
    const int structural_class = std::min(std::max(depth - 1, 0), 2);
    return std::pow(retention_, static_cast<double>(structural_class));
  }

  std::size_t HeatToBucket(double heat) const {
    if (heat <= 1e-12 || max_score_ == 0) {
      return 0;
    }
    const double bucket =
        std::log(std::max(heat, 1e-12) / minimum_heat_) /
        std::log(1.0 / retention_);
    return static_cast<std::size_t>(std::clamp(
        static_cast<long long>(std::llround(bucket)), 0LL,
        static_cast<long long>(max_score_)));
  }

  void BuildTransitionTable() {
    representative_heat_.resize(bucket_count_, 0.0);
    for (std::size_t bucket = 0; bucket < bucket_count_; ++bucket) {
      representative_heat_[bucket] =
          minimum_heat_ *
          std::pow(1.0 / retention_, static_cast<double>(bucket));
    }
    for (int depth = 1; depth <= max_depth_; ++depth) {
      const double gain = StructuralWeight(depth);
      for (std::size_t bucket = 0; bucket < bucket_count_; ++bucket) {
        transition_lut_[LutIndex(depth, bucket)] =
            static_cast<std::uint16_t>(
                HeatToBucket(representative_heat_[bucket] + gain));
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

  void ObserveFractionalTurnoverMiss(int depth) {
    if (!fractional_turnover_clock_enabled_ || capacity_ == 0) {
      return;
    }
    const std::size_t lifetime =
        Transition(std::clamp(depth, 1, max_depth_), 0);
    fractional_clock_lifetime_mass_ += static_cast<double>(lifetime);
    fractional_clock_credit_ +=
        static_cast<double>(lifetime) / static_cast<double>(capacity_);
    ++counters_.fractional_clock_misses;
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
    if (resident_size_ > capacity_) {
      throw std::runtime_error("LSBTSimple resident capacity exceeded");
    }
  }

  bool EvictOne(std::size_t incoming_bucket) {
    while (resident_size_ != 0) {
      const auto candidate = wheel_.Front();
      if (!candidate.has_value()) {
        return false;
      }

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
      pending_evicted_keys_.push_back(key_id);
      ++counters_.evictions;
      RememberGhost(key_id, state);
      if (ghost_.capacity() == 0) {
        states_.erase(key_id);
      }
      return true;
    }
    return false;
  }

  std::size_t capacity_ = 0;
  int max_depth_ = 1;
  std::size_t ghost_capacity_ = 0;
  std::size_t bucket_count_ = 1;
  std::size_t max_score_ = 0;
  double retention_ = kDefaultRetention;
  double reference_heat_ = 1.0;
  double minimum_heat_ = 1.0;
  std::uint32_t current_version_ = 1;
  std::size_t resident_size_ = 0;
  std::size_t peak_resident_size_ = 0;
  bool fractional_turnover_clock_enabled_ = false;
  double fractional_clock_credit_ = 0.0;
  double fractional_clock_lifetime_mass_ = 0.0;

  lsbt_detail::StateTable states_;
  lsbt_detail::GhostFIFO ghost_;
  lsbt_detail::TimingWheel wheel_;
  std::vector<std::uint16_t> transition_lut_;
  std::vector<double> representative_heat_;
  std::vector<std::uint32_t> pending_evicted_keys_;
  Counters counters_;
};
