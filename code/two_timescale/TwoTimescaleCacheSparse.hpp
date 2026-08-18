#pragma once

#include "PackedLSBT.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// Production-oriented sparse implementation.
// One hash lookup maps an external key to a compact slot. Macro state, Micro
// state, and epoch flags share that slot; timing wheels and ghost FIFO link
// slots directly and never hash again.
class TwoTimescaleCacheSparse {
 public:
  struct Stats {
    std::uint64_t requests = 0, hits = 0;
    std::uint64_t macro_first_touches = 0, macro_hits = 0;
    std::uint64_t intra_requests = 0, intra_hits = 0;
    std::uint64_t boundary_carries = 0, macro_to_micro_spills = 0;
    std::size_t peak_micro = 0, peak_total_resident = 0, peak_slots = 0;
  };

  static constexpr std::uint32_t kEpochMask = 0x3fffffffu;
  static constexpr std::uint32_t kListedBit = 0x40000000u;
  static constexpr std::uint32_t kReusedBit = 0x80000000u;

  TwoTimescaleCacheSparse(std::size_t key_count, std::size_t capacity,
                          int /*max_depth*/,
                          double retention = packed_lsbt::Scoring::kDefaultRetention)
      : capacity_(capacity),
        macro_(meta_, capacity, retention),
        micro_(meta_, InitialMicroCapacity(capacity), retention) {
    const std::size_t reserve = std::min(key_count, std::max<std::size_t>(64, 9 * capacity + InitialMicroCapacity(capacity)));
    meta_.reserve(reserve);
    slot_key_.reserve(reserve);
    index_.reserve(reserve);
    epoch_slots_.reserve(std::min<std::size_t>(reserve, 2 * capacity + 1024));
    micro_slots_.reserve(InitialMicroCapacity(capacity));
    free_slots_.reserve(reserve / 8 + 1);
  }

  bool Access(std::uint32_t key, int depth,
              bool speculative_micro_eligible = true) {
    ++stats_.requests;
    const std::uint32_t slot = GetOrCreate(key);
    auto& tag = meta_[slot].epoch_tag;
    const bool first_touch = (tag & kEpochMask) != epoch_id_;
    if (first_touch) {
      tag = epoch_id_;
      epoch_slots_.push_back(slot);
      ++epoch_distinct_;
      ++stats_.macro_first_touches;
      SyncMacroLimitAndSpill();

      const bool mhit = macro_.Access(slot, depth);
      RecycleDroppedGhost();
      if (mhit) {
        ++stats_.hits; ++stats_.macro_hits; UpdatePeaks(); return true;
      }

      std::uint32_t victim = UINT32_MAX;
      const bool admitted = macro_.Admit(slot, victim);
      RecycleDroppedGhost();
      if (victim != UINT32_MAX) HandleMacroVictim(victim);
      SyncMacroLimitAndSpill();

      if (!admitted && speculative_micro_eligible) {
        const auto probe = micro_.Access(slot, depth);
        if (micro_.Admit(slot, probe.incoming_bucket)) NoteMicroSlot(slot);
        SyncMacroLimitAndSpill();
      }
      UpdatePeaks();
      return false;
    }

    tag |= kReusedBit;
    ++stats_.intra_requests;
    if (macro_.Contains(slot)) {
      ++stats_.hits; ++stats_.intra_hits; UpdatePeaks(); return true;
    }
    const auto probe = micro_.Access(slot, depth);
    if (probe.hit) {
      ++stats_.hits; ++stats_.intra_hits; UpdatePeaks(); return true;
    }
    if (micro_.Admit(slot, probe.incoming_bucket)) NoteMicroSlot(slot);
    SyncMacroLimitAndSpill();
    UpdatePeaks();
    return false;
  }

  void EndMicroUnit() { micro_.Tick(); }

  void EndEpoch() {
    SelectiveBoundaryCarry();
    macro_.Tick();
    RecycleDroppedGhost();

    previous_epoch_distinct_ = epoch_distinct_;
    epoch_distinct_ = 0;
    const std::size_t next_cap = MicroCapacity(capacity_, previous_epoch_distinct_);
    micro_.ResetForCapacity(next_cap, micro_slots_);
    micro_slots_.clear();

    if (++epoch_id_ > kEpochMask) throw std::overflow_error("epoch id wrapped");
    for (std::uint32_t slot : epoch_slots_) MaybeRecycle(slot);
    epoch_slots_.clear();
    macro_.SetResidentLimit(capacity_);
  }

  const Stats& stats() const { return stats_; }
  std::size_t macro_resident() const { return macro_.resident_size(); }
  std::size_t micro_resident() const { return micro_.resident_size(); }
  std::size_t active_slots() const { return index_.size(); }
  std::size_t allocated_slots() const { return meta_.size(); }

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

  std::uint32_t GetOrCreate(std::uint32_t key) {
    auto it = index_.find(key);
    if (it != index_.end()) return it->second;
    std::uint32_t slot;
    if (!free_slots_.empty()) {
      slot = free_slots_.back(); free_slots_.pop_back();
      meta_[slot] = packed_lsbt::Meta{};
      slot_key_[slot] = key;
    } else {
      if (meta_.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        throw std::overflow_error("slot id exhausted");
      slot = static_cast<std::uint32_t>(meta_.size());
      meta_.push_back(packed_lsbt::Meta{});
      slot_key_.push_back(key);
    }
    index_.emplace(key, slot);
    stats_.peak_slots = std::max(stats_.peak_slots, index_.size());
    return slot;
  }

  bool IsReused(std::uint32_t slot) const {
    const auto tag = meta_[slot].epoch_tag;
    return (tag & kEpochMask) == epoch_id_ && (tag & kReusedBit);
  }

  void NoteMicroSlot(std::uint32_t slot) {
    auto& tag = meta_[slot].epoch_tag;
    if ((tag & kEpochMask) != epoch_id_) tag = epoch_id_;
    if (tag & kListedBit) return;
    tag |= kListedBit;
    micro_slots_.push_back(slot);
  }

  void HandleMacroVictim(std::uint32_t slot) {
    if (!IsReused(slot) || macro_.Contains(slot) || micro_.Contains(slot)) return;
    const auto probe = micro_.Access(slot, 1);
    if (micro_.Admit(slot, probe.incoming_bucket)) {
      NoteMicroSlot(slot);
      ++stats_.macro_to_micro_spills;
    }
  }

  void SyncMacroLimitAndSpill() {
    while (true) {
      const std::size_t target = capacity_ - micro_.resident_size();
      macro_.SetResidentLimit(target);
      if (macro_.resident_size() <= target) break;
      const std::uint32_t victim = macro_.ForceEvictOne();
      RecycleDroppedGhost();
      HandleMacroVictim(victim);
    }
  }

  void SelectiveBoundaryCarry() {
    macro_.SetResidentLimit(capacity_);
    for (std::uint32_t slot : micro_slots_) {
      if (!IsReused(slot) || !micro_.Contains(slot) || macro_.Contains(slot)) continue;
      std::uint32_t ignored = UINT32_MAX;
      if (macro_.Admit(slot, ignored)) ++stats_.boundary_carries;
      RecycleDroppedGhost();
      // boundary victim intentionally not spilled; it may still be recycled if
      // its ghost later ages out.
    }
  }

  void RecycleDroppedGhost() {
    const auto dropped = macro_.TakeDroppedGhost();
    if (dropped) MaybeRecycle(*dropped);
  }

  void MaybeRecycle(std::uint32_t slot) {
    if (macro_.Contains(slot) || packed_lsbt::IsMacroGhost(meta_[slot].macro) ||
        micro_.Contains(slot)) return;
    if ((meta_[slot].epoch_tag & kEpochMask) == epoch_id_) return;
    const std::uint32_t key = slot_key_[slot];
    auto it = index_.find(key);
    if (it != index_.end() && it->second == slot) index_.erase(it);
    meta_[slot] = packed_lsbt::Meta{};
    slot_key_[slot] = UINT32_MAX;
    free_slots_.push_back(slot);
  }

  void UpdatePeaks() {
#ifdef TT_CACHE_DIAGNOSTICS
    stats_.peak_micro = std::max(stats_.peak_micro, micro_.resident_size());
    const std::size_t total = macro_.resident_size() + micro_.resident_size();
    stats_.peak_total_resident = std::max(stats_.peak_total_resident, total);
    if (total > capacity_) throw std::runtime_error("resident capacity exceeded");
#endif
  }

  std::size_t capacity_ = 0;
  std::uint32_t epoch_id_ = 1;
  std::size_t epoch_distinct_ = 0, previous_epoch_distinct_ = 0;

  std::unordered_map<std::uint32_t, std::uint32_t> index_;
  std::vector<packed_lsbt::Meta> meta_;
  std::vector<std::uint32_t> slot_key_, free_slots_, epoch_slots_, micro_slots_;
  packed_lsbt::MacroLSBT macro_;
  packed_lsbt::MicroLSBT micro_;
  Stats stats_;
};
