#pragma once

#include <cstdint>

struct CpuContext;

// Cheats for Resident Evil 4 Wii (RB4E08). Set in Config.toml [cheats] at start and from the Android
// launcher through JNI (SetFromLauncher); never applied in a LAN session. The setters may be called
// from any thread.
namespace Cheats {

// False in a LAN session: a cheat there would make this player's game diverge.
bool Available();

// unlockAll, infiniteHealth, infiniteAmmo, specialWeapons, maxMoney: see Apply().
void SetFromLauncher(bool unlockAll, bool infiniteHealth, bool infiniteAmmo, bool specialWeapons, bool maxMoney);

// Keeps the enabled cheats applied. Call once a retrace on the guest thread. Unlock all sets the
// system save's extra-content flags once a second; what the game saves with them stays after the
// cheat is turned off. Infinite health tops Leon's and Ashley's life up to their max; infinite ammo
// keeps the equipped weapon's magazine full (grenades: puts back what a throw took). Special weapons
// adds the cleared-game weapons to the merchant's stock; max money holds the pesetas at 99,999,999.
void Apply();
// vi.cpp, once a shown game frame: its length, for the 60 fps mode's Speed (RE4_60FPS=1).
void OnGameFrame(double frameMs);

} // namespace Cheats
