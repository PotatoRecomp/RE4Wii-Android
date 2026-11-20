#pragma once

#include <cstddef>
#include <cstdint>

// Experimental "PB MODE" entry on the title menu (local experiment, off unless RE4_PB_MODE is set).
// RE4_PB_MODE=2 (tests): PB content is on in every game, e.g. after LOAD.
namespace PbMode {
bool Enabled();
// True after the player started a game from the PB MODE entry, until a normal entry is chosen.
bool Active();

// The held RE4 weapon (pG weapon_no, 0xFF when unknown) and the PB model / sound bank names for it
// (null: no PB counterpart, the RE4 one stays). RE4_PB_GUN / RE4_PB_WEAPON force one name.
uint32_t WeaponNo();
const char* WeaponModel();
const char* WeaponSound();

// EspDrawLaserLine's start and end points (guest Vec addresses), to log the laser's direction in the
// weapon's parts space (grip tuning).
void NoteLaser(uint32_t posAddr, uint32_t crossAddr);

// PB weapon sounds, mixed into the game's audio output (AudioBackend, stereo int16).
bool HasVoices();
void MixInto(int16_t* stereo, size_t frames, uint32_t sampleRate);
} // namespace PbMode
