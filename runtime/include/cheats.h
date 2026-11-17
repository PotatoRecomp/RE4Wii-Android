#pragma once

#include <cstdint>
#include <string>

struct CpuContext;

// Cheats for Resident Evil 4 Wii (RB4E08). Set in Config.toml [cheats] at start and from the Android
// launcher through JNI (SetFromLauncher); never applied in a LAN session. The setters may be called
// from any thread.
namespace Cheats {

// False in a LAN session: a cheat there would make this player's game diverge.
bool Available();

// unlockAll, infiniteHealth, infiniteAmmo, specialWeapons, maxMoney, autoAim: see Apply(); autoQte:
// AutoQteButtons(); itemStack: ItemStackOn(). playAs (pl_type: 0 Leon, 2 Ada, 3 HUNK, 4 Krauser,
// 5 Wesker), silencer, biggestCase, oneShotKills: the wrappers at the end of cheats.cpp.
void SetFromLauncher(bool unlockAll, bool infiniteHealth, bool infiniteAmmo, bool specialWeapons, bool maxMoney,
                     bool autoAim, bool autoQte, bool itemStack, int playAs, bool silencer, bool biggestCase,
                     bool oneShotKills);

// Item stack: every item in the case stacks up to 31,999, and two of the same item stack when combined
// (cheats.cpp, the ItemInfo / getType / combine wrappers). Read on the guest thread.
bool ItemStackOn();

// Windows game menu (settings_overlay.cpp): the Play As character (0 Leon, 2 Ada, 3 HUNK, 4 Krauser,
// 5 Wesker; RE4_PLAY_AS sets it at start, a change applies at the next room start).
int PlayAs();
void SetPlayAs(int who);
// Replay cutscene: the last cutscene's name ("" when none is kept), and the request to play it again (the
// flags from before it come back and the room restarts where it was triggered; done when the player is free).
std::string CutsceneReplayName();
void RequestCutsceneReplay();
void CutsceneReplayMark(const std::string& eventName);  // the event start (cheats.cpp)
// Windows overlay (F10 > Cheats): the phone's toggles of the same name.
bool InfiniteHealth();
void SetInfiniteHealth(bool on);
bool InfiniteAmmo();
void SetInfiniteAmmo(bool on);
void SetItemStack(bool on);

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
