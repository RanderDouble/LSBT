#pragma once

#include "PackedLSBT.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// Sparse Macro-only cache for a stream already deduplicated within each epoch.
// It shares the same packed LSBT/ghost/wheel machinery as TwoTimescaleCacheSparse.
class VersionDedupCacheSparse {
 public:
  VersionDedupCacheSparse(std::size_t key_count, std::size_t capacity,
                          int /*max_depth*/,
                          double retention = packed_lsbt::Scoring::kDefaultRetention)
      : macro_(meta_, capacity, retention) {
    const std::size_t reserve = std::min(key_count,
        std::max<std::size_t>(64, 9 * capacity));
    meta_.reserve(reserve);
    slot_key_.reserve(reserve);
    index_.reserve(reserve);
    free_slots_.reserve(reserve / 8 + 1);
  }

  bool Access(std::uint32_t key, int depth) {
    const std::uint32_t slot = GetOrCreate(key);
    const bool hit = macro_.Access(slot, depth);
    RecycleDroppedGhost();
    if (!hit) {
      std::uint32_t ignored = UINT32_MAX;
      macro_.Admit(slot, ignored);
      RecycleDroppedGhost();
    }
    return hit;
  }

  void EndEpoch() {
    macro_.Tick();
    RecycleDroppedGhost();
  }

  std::size_t resident_size() const { return macro_.resident_size(); }
  std::size_t ghost_size() const { return macro_.ghost_size(); }
  std::size_t active_slots() const { return index_.size(); }

 private:
  std::uint32_t GetOrCreate(std::uint32_t key) {
    auto it = index_.find(key);
    if (it != index_.end()) return it->second;
    std::uint32_t slot;
    if (free_slots_.empty()) {
      if (meta_.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        throw std::overflow_error("slot id exhausted");
      slot = static_cast<std::uint32_t>(meta_.size());
      meta_.push_back(packed_lsbt::Meta{});
      slot_key_.push_back(key);
    } else {
      slot = free_slots_.back(); free_slots_.pop_back();
      meta_[slot] = packed_lsbt::Meta{};
      slot_key_[slot] = key;
    }
    index_.emplace(key, slot);
    return slot;
  }

  void RecycleDroppedGhost() {
    const auto dropped = macro_.TakeDroppedGhost();
    if (!dropped) return;
    const std::uint32_t slot = *dropped;
    if (packed_lsbt::IsMacroResident(meta_[slot].macro) ||
        packed_lsbt::IsMacroGhost(meta_[slot].macro)) return;
    const std::uint32_t key = slot_key_[slot];
    auto it = index_.find(key);
    if (it != index_.end() && it->second == slot) index_.erase(it);
    meta_[slot] = packed_lsbt::Meta{};
    slot_key_[slot] = UINT32_MAX;
    free_slots_.push_back(slot);
  }

  std::unordered_map<std::uint32_t, std::uint32_t> index_;
  std::vector<packed_lsbt::Meta> meta_;
  std::vector<std::uint32_t> slot_key_, free_slots_;
  packed_lsbt::MacroLSBT macro_;
};
