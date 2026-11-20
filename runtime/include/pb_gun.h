#pragma once

#include <cstdint>

// PB MODE weapon model drawn in place of Leon's weapon (local experiment, see pb_gun.cpp).
namespace PbGun {
// Draws UserData/pb/PB_<name>_WEAPON.bin with the guest view matrix and the weapon's parts world
// matrix (both 3x4, big-endian), lit by the lights the game set for its own weapon model.
// `infoAddr` is that model's cModelInfo (material color). False when the model is not there.
bool Draw(const char* name, uint32_t viewMtxAddr, uint32_t partsMtxAddr, uint32_t infoAddr);
} // namespace PbGun
