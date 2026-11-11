#include "cheats.h"

#include "memory.h"
#include "netplay_session.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>

// Resident Evil 4 Wii (RB4E08) cheats. Only Unlock all is implemented; money, infinite HP and the
// camera hooks are kept as no-ops for the shared header and launcher until RE4 addresses are found.
namespace Cheats {
namespace {

// pSys (sdata 0x8043D584, next to pG at 0x8043D580) points at the system save block (GC SystemSave:
// Config_flg +0x00, Extra_flg +0x04). Extra_flg bits are numbered from the MSB (FlagOn: 0x80000000 >> n):
// 0 costumes, 1 Professional / extras menu, 2 handcannon, 3 Chicago Typewriter, 4..7 Ada, HUNK, Krauser
// and Wesker in The Mercenaries, 8 Assignment Ada, 9 Separate Ways, 10 Ashley's armor.
constexpr uint32_t kPSysAddr = 0x8043D584u;
constexpr uint32_t kExtraFlgOffset = 0x04u;
constexpr uint32_t kUnlockAllBits = 0xFFE00000u;  // bits 0..10

std::atomic<bool> g_unlockAll{RuntimeConfigFile::CheatUnlockAll()};

// Returns the bits it had to add (0 = already unlocked or the save block is not there yet).
uint32_t UnlockAllNow() {
    uint32_t sys = 0, flags = 0;
    if (!Memory::TryRead32(kPSysAddr, sys) || (sys & 0xFE000000u) != 0x80000000u ||
        !Memory::TryRead32(sys + kExtraFlgOffset, flags)) {
        return 0;
    }
    const uint32_t missing = kUnlockAllBits & ~flags;
    if (missing != 0 && !Memory::TryWrite32(sys + kExtraFlgOffset, flags | kUnlockAllBits)) {
        return 0;
    }
    return missing;
}

} // namespace

bool Available() {
    return !NetplaySession::Active();
}

float CameraDistance() { return 1.0f; }
void SetCameraDistance(float) {}

void SetFromLauncher(int32_t, uint32_t, uint32_t, bool, bool, bool unlockAll) {
    g_unlockAll = unlockAll;
}

void Apply() {
    if (!Available() || !g_unlockAll.load(std::memory_order_relaxed)) {
        return;
    }
    // Once a second: the system save read from NAND at the title replaces the flags set before it.
    static uint32_t s_frame = 0;
    if (++s_frame % 60u != 1u) {
        return;
    }
    if (const uint32_t added = UnlockAllNow(); added != 0) {
        RT_LOGF(RT_TAG_OS, "cheats: unlock all set Extra_flg bits %08X\n", added);
    }
}

} // namespace Cheats
