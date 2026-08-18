#pragma once

#include "LSBTState.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace lsbt_detail {

class GhostFIFO {
 public:
  GhostFIFO(std::size_t capacity, StateTable& states)
      : capacity_(capacity), states_(states) {
    nodes_.reserve(capacity_);
  }

  void Insert(std::uint32_t key_id, State& state) {
    if (IsGhost(state) || capacity_ == 0) {
      return;
    }
#ifndef NDEBUG
    assert(!IsResident(state));
#endif
    if (size_ >= capacity_) {
      EvictOldest();
    }

    const std::int32_t slot = AllocateSlot();
    GhostNode& node = nodes_[static_cast<std::size_t>(slot)];
    node.key_id = key_id;
    node.previous = tail_;
    node.next = kQueueEnd;
    if (tail_ == kQueueEnd) {
      head_ = slot;
    } else {
      nodes_[static_cast<std::size_t>(tail_)].next = slot;
    }
    tail_ = slot;
    state.ghost_slot = slot;
    ++size_;
  }

  void Remove(State& state) {
    if (!IsGhost(state)) {
      return;
    }
    const std::int32_t slot = state.ghost_slot;
    Unlink(slot);
    state.ghost_slot = kQueueEnd;
    ReleaseSlot(slot);
    --size_;
  }

  std::size_t size() const { return size_; }
  std::size_t capacity() const { return capacity_; }

 private:
  struct GhostNode {
    std::uint32_t key_id = 0;
    std::int32_t previous = kQueueEnd;
    std::int32_t next = kQueueEnd;
  };

  static_assert(sizeof(GhostNode) == 12);

  std::int32_t AllocateSlot() {
    if (free_head_ != kQueueEnd) {
      const std::int32_t slot = free_head_;
      free_head_ = nodes_[static_cast<std::size_t>(slot)].next;
      return slot;
    }
    if (nodes_.size() >= capacity_) {
      std::abort();
    }
    nodes_.push_back(GhostNode{});
    return static_cast<std::int32_t>(nodes_.size() - 1);
  }

  void ReleaseSlot(std::int32_t slot) {
    GhostNode& node = nodes_[static_cast<std::size_t>(slot)];
    node.previous = kQueueEnd;
    node.next = free_head_;
    free_head_ = slot;
  }

  void Unlink(std::int32_t slot) {
    const GhostNode node = nodes_[static_cast<std::size_t>(slot)];
    if (node.previous == kQueueEnd) {
      head_ = node.next;
    } else {
      nodes_[static_cast<std::size_t>(node.previous)].next = node.next;
    }
    if (node.next == kQueueEnd) {
      tail_ = node.previous;
    } else {
      nodes_[static_cast<std::size_t>(node.next)].previous = node.previous;
    }
  }

  void EvictOldest() {
    if (head_ == kQueueEnd) {
      std::abort();
    }
    const std::int32_t slot = head_;
    const std::uint32_t key_id =
        nodes_[static_cast<std::size_t>(slot)].key_id;
    auto iterator = states_.find(key_id);
    if (iterator == states_.end() || !IsGhost(iterator->second) ||
        iterator->second.ghost_slot != slot) {
      std::abort();
    }
    Unlink(slot);
    iterator->second.ghost_slot = kQueueEnd;
    --size_;
    ReleaseSlot(slot);
    states_.erase(iterator);
  }

  std::size_t capacity_ = 0;
  StateTable& states_;
  std::vector<GhostNode> nodes_;
  std::int32_t head_ = kQueueEnd;
  std::int32_t tail_ = kQueueEnd;
  std::int32_t free_head_ = kQueueEnd;
  std::size_t size_ = 0;
};

}  // namespace lsbt_detail
