#pragma once

#include "PackedLSBT.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

class TwoTimescaleCachePacked {
 public:
  struct Stats {
    std::uint64_t requests = 0, hits = 0;
    std::uint64_t macro_first_touches = 0, macro_hits = 0;
    std::uint64_t intra_requests = 0, intra_hits = 0;
    std::uint64_t boundary_carries = 0, macro_to_micro_spills = 0;
    std::size_t peak_micro = 0, peak_total_resident = 0;
  };

  static constexpr std::uint32_t kEpochMask = 0x3fffffffu;
  static constexpr std::uint32_t kListedBit = 0x40000000u;
  static constexpr std::uint32_t kReusedBit = 0x80000000u;

  TwoTimescaleCachePacked(std::size_t key_count, std::size_t capacity, int /*max_depth*/,
                          double retention = packed_lsbt::Scoring::kDefaultRetention)
      : key_count_(key_count), capacity_(capacity), meta_(key_count),
        macro_(meta_, capacity, retention),
        micro_(meta_, InitialMicroCapacity(capacity), retention) {
    micro_keys_.reserve(InitialMicroCapacity(capacity));
  }

  bool Access(std::uint32_t key, int depth,
              bool speculative_micro_eligible = true) {
    if (key >= key_count_) throw std::out_of_range("key id");
    ++stats_.requests;
    auto& tag = meta_[key].epoch_tag;
    const bool first_touch = (tag & kEpochMask) != epoch_id_;
    if (first_touch) {
      tag = epoch_id_;
      ++epoch_distinct_;
      ++stats_.macro_first_touches;
      SyncMacroLimitAndSpill();

      if (macro_.Access(key, depth)) {
        ++stats_.hits; ++stats_.macro_hits; UpdatePeaks(); return true;
      }
      std::uint32_t victim = UINT32_MAX;
      const bool admitted = macro_.Admit(key, victim);
      if (victim != UINT32_MAX) HandleMacroVictim(victim);
      SyncMacroLimitAndSpill();

      if (!admitted && speculative_micro_eligible) {
        const auto probe = micro_.Access(key, depth);
        if (micro_.Admit(key, probe.incoming_bucket)) NoteMicroKey(key);
        SyncMacroLimitAndSpill();
      }
      UpdatePeaks();
      return false;
    }

    tag |= kReusedBit;
    ++stats_.intra_requests;
    if (macro_.Contains(key)) {
      ++stats_.hits; ++stats_.intra_hits; UpdatePeaks(); return true;
    }
    const auto probe = micro_.Access(key, depth);
    if (probe.hit) {
      ++stats_.hits; ++stats_.intra_hits; UpdatePeaks(); return true;
    }
    if (micro_.Admit(key, probe.incoming_bucket)) NoteMicroKey(key);
    SyncMacroLimitAndSpill();
    UpdatePeaks();
    return false;
  }

  void EndMicroUnit() { micro_.Tick(); }

  void EndEpoch() {
    SelectiveBoundaryCarry();
    macro_.Tick();
    previous_epoch_distinct_ = epoch_distinct_;
    epoch_distinct_ = 0;
    const std::size_t next_cap = MicroCapacity(capacity_, previous_epoch_distinct_);
    micro_.ResetForCapacity(next_cap, micro_keys_);
    micro_keys_.clear();
    if (++epoch_id_ > kEpochMask) throw std::overflow_error("epoch id wrapped");
    macro_.SetResidentLimit(capacity_);
  }

  const Stats& stats() const { return stats_; }
  std::size_t macro_resident() const { return macro_.resident_size(); }
  std::size_t micro_resident() const { return micro_.resident_size(); }
  std::size_t macro_ghost() const { return macro_.ghost_size(); }
  std::size_t current_micro_capacity() const { return micro_.capacity(); }
  std::size_t metadata_bytes() const {
    return meta_.capacity() * sizeof(packed_lsbt::Meta) +
           micro_keys_.capacity() * sizeof(std::uint32_t);
  }

  static std::size_t MicroCapacity(std::size_t C, std::size_t previous_U) {
    if (C == 0) return 0;
    const std::size_t structural_cap = static_cast<std::size_t>(std::llround((2.0 / 3.0) * C));
    const std::size_t footprint_cap = previous_U == 0
        ? structural_cap
        : static_cast<std::size_t>(std::llround(0.5 * previous_U));
    return std::max<std::size_t>(1, std::min({C, structural_cap, footprint_cap}));
  }

 private:
  static std::size_t InitialMicroCapacity(std::size_t C) { return MicroCapacity(C, 0); }

  bool IsReused(std::uint32_t key) const {
    const auto tag = meta_[key].epoch_tag;
    return (tag & kEpochMask) == epoch_id_ && (tag & kReusedBit);
  }

  void NoteMicroKey(std::uint32_t key) {
    auto& tag = meta_[key].epoch_tag;
    if ((tag & kEpochMask) != epoch_id_) tag = epoch_id_;
    if (tag & kListedBit) return;
    tag |= kListedBit;
    micro_keys_.push_back(key);
  }

  void HandleMacroVictim(std::uint32_t key) {
    if (!IsReused(key) || macro_.Contains(key) || micro_.Contains(key)) return;
    const auto probe = micro_.Access(key, 1);
    if (micro_.Admit(key, probe.incoming_bucket)) {
      NoteMicroKey(key);
      ++stats_.macro_to_micro_spills;
    }
  }

  void SyncMacroLimitAndSpill() {
    while (true) {
      const std::size_t target = capacity_ - micro_.resident_size();
      macro_.SetResidentLimit(target);
      if (macro_.resident_size() <= target) break;
      HandleMacroVictim(macro_.ForceEvictOne());
    }
  }

  void SelectiveBoundaryCarry() {
    macro_.SetResidentLimit(capacity_);
    for (std::uint32_t key : micro_keys_) {
      if (!IsReused(key) || !micro_.Contains(key) || macro_.Contains(key)) continue;
      std::uint32_t ignored = UINT32_MAX;
      if (macro_.Admit(key, ignored)) ++stats_.boundary_carries;
      // Exact v2 semantics: a victim caused by boundary carry is discarded,
      // not spilled into the expiring Micro tier.
    }
  }

  void UpdatePeaks() {
#ifdef TT_CACHE_DIAGNOSTICS
    stats_.peak_micro = std::max(stats_.peak_micro, micro_.resident_size());
    const std::size_t total = macro_.resident_size() + micro_.resident_size();
    stats_.peak_total_resident = std::max(stats_.peak_total_resident, total);
    if (total > capacity_) throw std::runtime_error("resident capacity exceeded");
#endif
  }

  std::size_t key_count_ = 0, capacity_ = 0;
  std::uint32_t epoch_id_ = 1;
  std::size_t epoch_distinct_ = 0, previous_epoch_distinct_ = 0;
  std::vector<packed_lsbt::Meta> meta_;
  packed_lsbt::MacroLSBT macro_;
  packed_lsbt::MicroLSBT micro_;
  std::vector<std::uint32_t> micro_keys_;
  Stats stats_;
};
