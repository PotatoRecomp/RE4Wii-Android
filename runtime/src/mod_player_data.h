#pragma once

#include <cstdint>

// Mods: player archives larger than the Wii's slot (mod_player_data.cpp).
namespace ModPlayerData {
// After ReadPlayerData (0x80141160): points pG->pPlayer at the relocated archive when it was read there.
void AfterReadPlayerData(uint32_t pG);
} // namespace ModPlayerData
