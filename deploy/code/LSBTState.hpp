#pragma once

#include <cstdint>
#include <unordered_map>

namespace lsbt_detail {

inline constexpr std::int32_t kQueueEnd = -1;
inline constexpr std::int32_t kNotResident = -2;

struct State {
  std::uint32_t expiry_version = 0;
  std::int32_t next_resident = kNotResident;
  std::int32_t ghost_slot = kQueueEnd;
  std::uint8_t renewal_credit = 0;
  std::uint8_t padding[3] = {0, 0, 0};
};

static_assert(sizeof(State) == 16);

using StateTable = std::unordered_map<std::uint32_t, State>;

inline bool IsResident(const State& state) {
  return state.next_resident != kNotResident;
}

inline bool IsGhost(const State& state) {
  return state.ghost_slot != kQueueEnd;
}

}  // namespace lsbt_detail
