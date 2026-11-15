#pragma once

#include <cstdint>

struct CpuContext;

// Cheats for Resident Evil 4 Wii (RB4E08). Set in Config.toml [cheats] at start and from the Android
// launcher through JNI (SetFromLauncher); never applied in a LAN session. The setters may be called
// from any thread.
namespace Cheats {

// False in a LAN session: a cheat there would make this player's game diverge.
bool Available();

// unlockAll, infiniteHealth, infiniteAmmo, specialWeapons, maxMoney, autoAim: see Apply(); autoQte:
// AutoQteButtons(); itemStack: ItemStackOn().
void SetFromLauncher(bool unlockAll, bool infiniteHealth, bool infiniteAmmo, bool specialWeapons, bool maxMoney,
                     bool autoAim, bool autoQte, bool itemStack);

// Item stack: every item in the case stacks up to 31,999, and two of the same item stack when combined
// (cheats.cpp, the ItemInfo / getType / combine wrappers). Read on the guest thread.
bool ItemStackOn();

// Auto QTE: the buttons (kQte*) to press on player 1 at time nowNs, 0 when off or no prompt is up.
// Called on the guest thread by the input reads (virtual_remote.cpp).
enum : uint32_t { kQteA = 1, kQteB = 2, kQteL = 4, kQteR = 8 };
uint32_t AutoQteButtons(uint64_t nowNs);

// Keeps the enabled cheats applied. Call once a retrace on the guest thread. Unlock all sets the
// system save's extra-content flags once a second; what the game saves with them stays after the
// cheat is turned off. Infinite health tops Leon's and Ashley's life up to their max; infinite ammo
// keeps the equipped weapon's magazine full (grenades: puts back what a throw took). Special weapons
// adds the cleared-game weapons to the merchant's stock; max money holds the pesetas at 99,999,999.
// Auto aim sets the game's own lock-on option (Config_flg CFG_LOCK_ON), cleared once when turned off.
void Apply();

} // namespace Cheats
