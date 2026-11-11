#pragma once

#include <cstdint>

struct CpuContext;

// Cheats for Resident Evil 4 Wii (RB4E08). Set in Config.toml [cheats] at start and from the Android
// launcher through JNI (SetFromLauncher); never applied in a LAN session. The setters may be called
// from any thread.
namespace Cheats {

// The money cap (launcher setting; not applied to RE4 yet).
constexpr uint32_t kMaxMoney = 9'999'999;

enum MoneyMode : int32_t { kMoneyOff = 0, kMoneyInfinite = 1, kMoneyOnce = 2 };

// False in a LAN session: a cheat there would make this player's game diverge.
bool Available();

// money: kMoney*; amount: the money Infinite holds and Once sets (clamped to kMaxMoney); onceSeq: a Once
// request is applied at the next frame when it differs from the last one seen; unlockAll: see Apply().
void SetFromLauncher(int32_t money, uint32_t amount, uint32_t onceSeq, bool infiniteHp, bool infiniteGauge,
                     bool unlockAll);

// Camera distance (1 = the game's own camera). Not implemented for RE4 yet: always 1.
float CameraDistance();
void SetCameraDistance(float distance);

// Keeps the enabled cheats applied. Call once a retrace on the guest thread. Unlock all sets the
// system save's extra-content flags once a second; what the game saves with them stays after the
// cheat is turned off.
void Apply();

} // namespace Cheats
