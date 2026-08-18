#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// Unit-object S3FIFO following libCacheSim's small/main/ghost behavior.
// An eviction may first promote every small-queue entry; in that case the
// policy continues with the main queue until resident size actually drops.
class S3FifoBaseline {
 public:
  struct DebugCounters {
    std::uint64_t main_scans = 0;
    std::uint64_t main_requeues = 0;
    std::uint64_t main_evictions = 0;
    std::uint64_t versions_after_first_eviction = 0;
    std::uint64_t main_eviction_reaccesses = 0;
    std::uint64_t main_eviction_requalifications = 0;
  };

  S3FifoBaseline(std::size_t capacity, double small_ratio,
                 double ghost_ratio, int promotion_threshold)
      : capacity_(capacity),
        small_capacity_(static_cast<std::size_t>(capacity * small_ratio)),
        main_capacity_(capacity - small_capacity_),
        promotion_threshold_(promotion_threshold),
        small_(small_capacity_),
        main_(main_capacity_),
        ghost_(static_cast<std::size_t>(capacity * ghost_ratio)) {
    if (small_capacity_ == 0 || main_capacity_ == 0) {
      throw std::runtime_error("invalid S3FIFO resident queue sizes");
    }
  }

  // Optional control: keep S3FIFO's queues unchanged, but give entries that
  // reached Main through proof a turnover-based renewal lease. This isolates
  // lease retention from tree depth and LSBT scoring.
  void EnableProofLease(double turnover_factor) {
    lease_turnover_factor_ = std::max(0.0, turnover_factor);
    if (lease_turnover_factor_ == 0.0) return;
    const std::size_t span = static_cast<std::size_t>(std::ceil(
        lease_turnover_factor_ * static_cast<double>(std::max<std::size_t>(1, capacity_))));
    lease_wheel_.assign(std::max<std::size_t>(64, span + 2), {});
  }

  // Fair structural control for PRD: capacity and the S3FIFO queues remain
  // unchanged. Depth only selects the first resident queue on a cold miss.
  void EnableDepthAdmission(int max_depth, int main_depth_cutoff) {
    depth_admission_ = true;
    depth_max_ = std::max(1, max_depth);
    depth_main_cutoff_ = std::clamp(main_depth_cutoff, 1, depth_max_);
  }

  void EnableAdaptiveGhost() {
    adaptive_ghost_ = true;
    ghost_.SetCapacity(static_cast<std::size_t>(8.0 * capacity_));
    adaptive_epoch_ = std::max<std::size_t>(32, 4 * std::max<std::size_t>(1, capacity_));
  }

  void EnableAdaptiveLease() {
    adaptive_lease_ = true;
    EnableProofLease(8.0);
    adaptive_epoch_ = std::max<std::size_t>(32, 4 * std::max<std::size_t>(1, capacity_));
  }

  void EnableMainEvictionAudit() { main_eviction_audit_ = true; }

  bool Get(std::uint32_t key) {
    hit_on_ghost_ = false;
    MarkMainEvictionReaccess(key);
    if (Entry* entry = small_.Find(key)) {
      ++entry->frequency;
      return true;
    }
    if (ghost_.Remove(key)) {
      hit_on_ghost_ = true;
      ++adaptive_ghost_hits_;
      ++adaptive_lease_late_reuses_;
    }
    if (Entry* entry = main_.Find(key)) {
      ++entry->frequency;
      ++adaptive_lease_active_hits_;
      RenewLease(key);
      return true;
    }
    return false;
  }

  bool PutOnMiss(std::uint32_t key) {
    return PutOnMiss(key, depth_max_);
  }

  bool PutOnMiss(std::uint32_t key, int depth) {
    if (capacity_ == 0) {
      return false;
    }
    AdvanceLeaseExpiry();
    while (ResidentSize() >= capacity_) {
      EvictOneResident();
    }

    if (hit_on_ghost_) {
      hit_on_ghost_ = false;
      main_.Insert(key);
      MarkMainEvictionRequalification(key);
      RenewLease(key);
    } else if ((depth_admission_ && depth <= depth_main_cutoff_) ||
               (!has_evicted_ && small_.Size() >= small_capacity_)) {
      main_.Insert(key);
      RenewLease(key);
    } else {
      small_.Insert(key);
    }
    ++admission_clock_;
    MaybeAdaptGhost();
    MaybeAdaptLease();
    return true;
  }

  std::vector<std::uint32_t> TakeEvictedKeys() {
    std::vector<std::uint32_t> evicted;
    evicted.swap(pending_evicted_keys_);
    return evicted;
  }

  void FinishVersion() {
    if (measurement_started_) {
      ++counters_.versions_after_first_eviction;
    }
  }

  std::size_t main_capacity() const noexcept { return main_capacity_; }

  const DebugCounters& counters() const noexcept { return counters_; }

 private:
  struct Entry {
    std::uint32_t key = 0;
    int frequency = 0;
  };

  class FifoQueue {
   public:
    explicit FifoQueue(std::size_t capacity) : capacity_(capacity) {}

    std::size_t Size() const noexcept { return entries_.size(); }
    bool Empty() const noexcept { return entries_.empty(); }

    Entry* Find(std::uint32_t key) {
      const auto it = index_.find(key);
      return it == index_.end() ? nullptr : &*it->second;
    }

    const Entry& Oldest() const { return entries_.front(); }

    bool Insert(std::uint32_t key, int frequency = 0) {
      const auto existing = index_.find(key);
      if (existing != index_.end()) {
        existing->second->frequency = frequency;
        return false;
      }
      entries_.push_back(Entry{key, frequency});
      const auto it = std::prev(entries_.end());
      index_[key] = it;
      return true;
    }

    bool Remove(std::uint32_t key) {
      const auto it = index_.find(key);
      if (it == index_.end()) {
        return false;
      }
      entries_.erase(it->second);
      index_.erase(it);
      return true;
    }

    bool InsertGhost(std::uint32_t key) {
      if (capacity_ == 0 || Find(key) != nullptr) {
        return false;
      }
      bool expired = false;
      while (Size() >= capacity_) {
        Remove(Oldest().key);
        expired = true;
      }
      Insert(key);
      return expired;
    }

    void SetCapacity(std::size_t capacity) {
      capacity_ = capacity;
      while (Size() > capacity_) Remove(Oldest().key);
    }

   private:
    std::size_t capacity_ = 0;
    std::list<Entry> entries_;
    std::unordered_map<std::uint32_t,
                       std::list<Entry>::iterator> index_;
  };

  std::size_t ResidentSize() const noexcept {
    return small_.Size() + main_.Size();
  }

  void EvictOneResident() {
    const std::size_t before = ResidentSize();
    has_evicted_ = true;
    while (ResidentSize() >= before) {
      if (main_.Size() > main_capacity_ || small_.Empty()) {
        EvictMain();
      } else {
        EvictSmall();
      }
      if (small_.Empty() && main_.Empty()) {
        throw std::runtime_error("S3FIFO has no resident victim");
      }
    }
  }

  void EvictSmall() {
    while (!small_.Empty()) {
      const Entry victim = small_.Oldest();
      small_.Remove(victim.key);
      if (victim.frequency >= promotion_threshold_) {
        main_.Insert(victim.key);
        MarkMainEvictionRequalification(victim.key);
        RenewLease(victim.key);
        continue;
      }
      const bool expired = ghost_.InsertGhost(victim.key);
      ++adaptive_ghost_inserts_;
      if (expired) ++adaptive_ghost_expired_;
      pending_evicted_keys_.push_back(victim.key);
      CancelLease(victim.key);
      measurement_started_ = true;
      return;
    }
  }

  void EvictMain() {
    while (!main_.Empty()) {
      const Entry victim = main_.Oldest();
      main_.Remove(victim.key);
      ++counters_.main_scans;
      if (victim.frequency >= 1) {
        main_.Insert(victim.key,
                     std::min(victim.frequency, 3) - 1);
        ++counters_.main_requeues;
        continue;
      }
      pending_evicted_keys_.push_back(victim.key);
      CancelLease(victim.key);
      if (main_eviction_audit_) main_eviction_states_[victim.key] = 1;
      ++counters_.main_evictions;
      measurement_started_ = true;
      return;
    }
  }

  std::size_t capacity_ = 0;
  std::size_t small_capacity_ = 0;
  std::size_t main_capacity_ = 0;
  int promotion_threshold_ = 2;
  bool hit_on_ghost_ = false;
  bool has_evicted_ = false;
  bool measurement_started_ = false;
  FifoQueue small_;
  FifoQueue main_;
  FifoQueue ghost_;
  std::vector<std::uint32_t> pending_evicted_keys_;
  DebugCounters counters_;
  struct LeaseEntry { std::uint64_t expiry = 0, generation = 0; std::uint32_t key = 0; };
  double lease_turnover_factor_ = 0.0;
  std::uint64_t admission_clock_ = 0, lease_cursor_ = 0;
  std::vector<std::deque<LeaseEntry>> lease_wheel_;
  std::unordered_map<std::uint32_t, std::uint64_t> lease_generation_;
  bool main_eviction_audit_ = false;
  std::unordered_map<std::uint32_t, std::uint8_t> main_eviction_states_;
  bool adaptive_ghost_ = false, adaptive_lease_ = false;
  bool depth_admission_ = false;
  int depth_max_ = 20, depth_main_cutoff_ = 5;
  std::size_t adaptive_epoch_ = 0, adaptive_ghost_index_ = 4,
              adaptive_lease_index_ = 3;
  std::uint64_t adaptive_ghost_hits_ = 0, adaptive_ghost_inserts_ = 0,
                adaptive_ghost_expired_ = 0, adaptive_lease_expiries_ = 0,
                adaptive_lease_late_reuses_ = 0, adaptive_lease_active_hits_ = 0;

  void RenewLease(std::uint32_t key) {
    if (lease_turnover_factor_ == 0.0) return;
    const std::uint64_t span = std::max<std::uint64_t>(1,
        static_cast<std::uint64_t>(std::ceil(lease_turnover_factor_ *
            static_cast<double>(std::max<std::size_t>(1, capacity_)))));
    const std::uint64_t generation = ++lease_generation_[key];
    const LeaseEntry entry{admission_clock_ + span, generation, key};
    lease_wheel_[entry.expiry % lease_wheel_.size()].push_back(entry);
  }
  void CancelLease(std::uint32_t key) {
    if (lease_turnover_factor_ > 0.0) ++lease_generation_[key];
  }
  void AdvanceLeaseExpiry() {
    if (lease_turnover_factor_ == 0.0) return;
    while (lease_cursor_ <= admission_clock_) {
      auto pending = std::move(lease_wheel_[lease_cursor_ % lease_wheel_.size()]);
      lease_wheel_[lease_cursor_ % lease_wheel_.size()].clear();
      for (const LeaseEntry& entry : pending) {
        if (entry.expiry > admission_clock_) {
          lease_wheel_[entry.expiry % lease_wheel_.size()].push_back(entry);
          continue;
        }
        const auto it = lease_generation_.find(entry.key);
        if (it == lease_generation_.end() || it->second != entry.generation) continue;
        if (main_.Remove(entry.key)) {
          ++lease_generation_[entry.key];
          ++adaptive_lease_expiries_;
          if (adaptive_lease_) {
            const bool expired = ghost_.InsertGhost(entry.key);
            ++adaptive_ghost_inserts_;
            if (expired) ++adaptive_ghost_expired_;
          }
          pending_evicted_keys_.push_back(entry.key);
          ++counters_.main_evictions;
        }
      }
      ++lease_cursor_;
    }
  }

  void MaybeAdaptGhost() {
    if (!adaptive_ghost_ || adaptive_epoch_ == 0 || admission_clock_ % adaptive_epoch_ != 0) return;
    static constexpr double kRatios[] = {0.5, 0.9, 2.0, 4.0, 8.0, 16.0};
    const double hit_rate = adaptive_ghost_inserts_ == 0 ? 0.0 : static_cast<double>(adaptive_ghost_hits_) / adaptive_ghost_inserts_;
    const double expire_rate = adaptive_ghost_inserts_ == 0 ? 0.0 : static_cast<double>(adaptive_ghost_expired_) / adaptive_ghost_inserts_;
    if (expire_rate > 0.45 && hit_rate > 0.01 && adaptive_ghost_index_ + 1 < 6) ++adaptive_ghost_index_;
    else if (expire_rate < 0.12 && hit_rate < 0.005 && adaptive_ghost_index_ > 0) --adaptive_ghost_index_;
    ghost_.SetCapacity(static_cast<std::size_t>(kRatios[adaptive_ghost_index_] * capacity_));
    adaptive_ghost_hits_ = adaptive_ghost_inserts_ = adaptive_ghost_expired_ = 0;
  }

  void MaybeAdaptLease() {
    if (!adaptive_lease_ || adaptive_epoch_ == 0 || admission_clock_ % adaptive_epoch_ != 0) return;
    static constexpr double kLeases[] = {1.0, 2.0, 4.0, 8.0, 16.0};
    const double late_ratio = adaptive_lease_expiries_ == 0 ? 0.0 : static_cast<double>(adaptive_lease_late_reuses_) / adaptive_lease_expiries_;
    const double expiry_ratio = static_cast<double>(adaptive_lease_expiries_) / std::max<std::uint64_t>(1, adaptive_lease_expiries_ + adaptive_lease_active_hits_);
    if (late_ratio > 0.01 && adaptive_lease_index_ + 1 < 5) ++adaptive_lease_index_;
    else if (expiry_ratio > 0.75 && late_ratio < 0.005 && adaptive_lease_index_ > 0) --adaptive_lease_index_;
    lease_turnover_factor_ = kLeases[adaptive_lease_index_];
    adaptive_lease_expiries_ = adaptive_lease_late_reuses_ = adaptive_lease_active_hits_ = 0;
  }

  void MarkMainEvictionReaccess(std::uint32_t key) {
    if (!main_eviction_audit_) return;
    const auto it = main_eviction_states_.find(key);
    if (it == main_eviction_states_.end() || it->second != 1) return;
    it->second = 2;
    ++counters_.main_eviction_reaccesses;
  }

  void MarkMainEvictionRequalification(std::uint32_t key) {
    if (!main_eviction_audit_) return;
    const auto it = main_eviction_states_.find(key);
    if (it == main_eviction_states_.end() || it->second != 2) return;
    main_eviction_states_.erase(it);
    ++counters_.main_eviction_requalifications;
  }
};
