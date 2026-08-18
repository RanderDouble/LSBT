#pragma once

#include "LSBTElasticCore.hpp"

#include <cstddef>
#include <cstdint>

// Macro-only view for a stream already deduplicated within each control epoch.
// It is the degenerate case of the two-timescale design when no intra-epoch
// repeated request remains in the representation.
class VersionDedupLSBT {
 public:
  VersionDedupLSBT(std::size_t key_count, std::size_t capacity, int max_depth,
                   double retention = LSBTElasticCore::kDefaultRetention)
      : cache_(key_count, capacity, max_depth, retention, 8.0, false) {}

  bool Access(std::uint32_t key, int depth) {
    if (cache_.Access(key, depth)) return true;
    cache_.Admit(key, depth);
    return false;
  }

  void EndEpoch() { cache_.Tick(); }
  std::size_t resident_size() const { return cache_.resident_size(); }
  std::size_t ghost_size() const { return cache_.ghost_size(); }

 private:
  LSBTElasticCore cache_;
};
