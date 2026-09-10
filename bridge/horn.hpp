#pragma once
#include "ht2mp/game/profile.hpp"
#include "ht2mp/protocol/replication.hpp"
#include <array>
#include <cstdint>
#include <string>

namespace ht2mp::bridge {
bool InitializeSteamHorn(const ht2mp::game::ProfileVerification&, std::uintptr_t, std::string&);
void TickHorn(bool ready) noexcept;
void DisableHorn() noexcept;
bool CaptureHorn(std::uint32_t vehicle, ht2mp::protocol::HornState&) noexcept;
void UpdateRemoteHorn(std::uint32_t vehicle, const ht2mp::protocol::HornState&,
                      std::uint64_t received_us, const std::array<float, 3>& position) noexcept;
void ReleaseRemoteHorn(std::uint32_t vehicle) noexcept;
struct HornStats {
  std::uint32_t presses{}, local_active{}, voices{}, starts{}, stops{}, frames{}, failures{};
};
HornStats GetHornStats() noexcept;
} // namespace ht2mp::bridge
