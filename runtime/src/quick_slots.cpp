// Mods: D-pad quick slots, the 2023 remake's weapon change without opening the attache case.
//
// The idea and the slot lists follow CubeEvil4's mod fork (runtime/quick_slots.cpp, GPL-3.0); this is
// written for the Wii binary. On by default while a mod folder is active ([paths] overlay_roots),
// RE4_QUICK_SLOTS=0 turns it off, =1 turns it on without mods.
//
// In the field a tap of the D-pad equips the next weapon of its kind from the case (up handguns and
// magnums, right shotguns and machine guns, down rifles and the bow, left grenades); a direction held
// for kHolster frames puts the weapon away (bare hands; the knife and kicks stay). Meanwhile the D-pad
// neither walks nor steps the aim: its bits leave Key and Joy[0] for the rest of the frame.
//
// Hook: cPlayer::keyConfig (Wii 0x80133848, called by cPlayer::move at 0x80072C58, before the routines
// read Key). The change is the one the Wii's sub screen exit does when the case closes (0x8007C9C0..
// 0x8007CA34): ItemMgr.arm, then, when the weapon number, type or bullet level changed, SndBlkStop(2),
// weaponRelease, weaponLoad, pG->bullet_type, weaponInit and 0x80076368. weaponLoad reads the weapon's
// file and sleeps the main task until the read is done (ReadWepData 0x80141590, TaskSleep 0x8015209C);
// the RE4_TEST_ARM path (cheats.cpp) showed that this works from inside the main task's frame.
//
// Wii addresses (main.dol):
//   Key 0x80332E80 (on u64 +0x10, trg u64 +0x18), Joy[0] 0x803324D0 (on +0x10, trg +0x14),
//   Key_type_tbl 0x802D8500 (64 words per pad type; entries 0..3 up, down, right, left, D-pad 8/4/2/1
//   or'ed with the main stick's bits), pSys 0x8043D584 (+0x0B pad type), pG 0x8043D580,
//   pPL 0x8043DEA0 (+0xFC r_no_0, +0xFD r_no_1), ItemMgr 0x8031F600 (+0x0C equipped cItem*,
//   +0x10 the case's character, +0x14 cItem array of 0x0E bytes, +0x1C count).
//   pG +0x4FC0 weapon_no, +0x4FC1 weapon_type, +0x4FC2 bullet_type, +0x4FC8 pl_type,
//   +0x501C Status_flg[4] (bits from the MSB: 13 sub screen, 19 event, 21 binoculars, 25 scope, 42 boat).
#include "abi_bridge.h"
#include "hle/storage/riivolution.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

#include <cstdint>
#include <cstdlib>

extern "C" void func_80133848(CpuContext* ctx);  // cPlayer::keyConfig
extern "C" void func_800357B8(CpuContext* ctx);  // cItemMgr::arm(r4 cItem*, 0 = bare hands) -> r3 ok
extern "C" void func_80036C7C(CpuContext* ctx);  // cItemMgr::weaponId -> r3
extern "C" void func_80036A40(CpuContext* ctx);  // WeaponId2WeaponNo(r3 id) -> r3
extern "C" void func_80036A84(CpuContext* ctx);  // WeaponId2WeaponType(r3 id) -> r3
extern "C" void func_80036D28(CpuContext* ctx);  // cItem::getBulletType(r3 cItem*) -> r3
extern "C" void func_80078390(CpuContext* ctx);  // SndBlkStop(r3 block)
extern "C" void func_80138C94(CpuContext* ctx);  // cPlayer::weaponRelease
extern "C" void func_80138F78(CpuContext* ctx);  // cPlayer::weaponLoad(r4 no, r5 type)
extern "C" void func_80138F94(CpuContext* ctx);  // cPlayer::weaponInit
extern "C" void func_80076368(CpuContext* ctx);  // the sub screen exit's last step after weaponInit (Wii)

namespace ModSound {
void PlayQuickSlot();  // mod_sound.cpp
}

namespace {
constexpr uint32_t kKey = 0x80332E80u;
constexpr uint32_t kJoy0 = 0x803324D0u;
constexpr uint32_t kKeyTypeTbl = 0x802D8500u;
constexpr uint32_t kPSys = 0x8043D584u;
constexpr uint32_t kPG = 0x8043D580u;
constexpr uint32_t kPPL = 0x8043DEA0u;
constexpr uint32_t kItemMgr = 0x8031F600u;
constexpr uint32_t kItemSize = 0x0Eu;

// item ids in the order repeated taps go through them (the fork's lists; ids are the game's)
constexpr uint16_t kUp[] = {0x23, 0x24, 0x21, 0x22, 0x40, 0x25, 0x26, 0x27, 0x28, 0x03, 0x29, 0x2A, 0x2B, 0x37};
constexpr uint16_t kRight[] = {0x2C, 0x94, 0x2D, 0x30, 0x31, 0x32, 0x33, 0x3E, 0x34, 0x36, 0xAB, 0x35, 0x17, 0x6D};
constexpr uint16_t kDown[] = {0x2E, 0x6B, 0x99, 0x2F, 0x6C, 0x51, 0x52};
constexpr uint16_t kLeft[] = {0x01, 0x02, 0x0E};
struct Slot {
    const uint16_t* ids;
    int count;
    uint32_t joyBit;
    const char* name;
};
constexpr Slot kSlots[4] = {
    {kUp, int(sizeof kUp / sizeof kUp[0]), 8u, "up"},
    {kRight, int(sizeof kRight / sizeof kRight[0]), 2u, "right"},
    {kDown, int(sizeof kDown / sizeof kDown[0]), 4u, "down"},
    {kLeft, int(sizeof kLeft / sizeof kLeft[0]), 1u, "left"},
};
constexpr int kHolster = 15;  // frames (half a second at 30 fps)

uint32_t g_dpadPrev = 0;
int g_holdSlot = -1;
int g_holdFrames = 0;
bool g_holdUsed = false;

bool Enabled() {
    static const bool s_on = [] {
        const char* env = std::getenv("RE4_QUICK_SLOTS");
        if (env != nullptr && *env != '\0') return *env != '0';
        return !RuntimeRiivolution::Overlays().empty();
    }();
    return s_on;
}

bool GuestPtr(uint32_t p) { return (p & 0xFE000000u) == 0x80000000u || (p & 0xF0000000u) == 0x90000000u; }

uint8_t Read8(uint32_t a) {
    uint32_t w = 0;
    if (!Memory::TryRead32(a & ~3u, w)) return 0;
    return static_cast<uint8_t>(w >> (24 - 8 * (a & 3u)));
}
uint16_t Read16(uint32_t a) {
    uint32_t w = 0;
    if (!Memory::TryRead32(a & ~3u, w)) return 0;
    return (a & 2u) ? static_cast<uint16_t>(w) : static_cast<uint16_t>(w >> 16);
}
bool StaFlag(uint32_t pG, int bit) {
    uint32_t w = 0;
    return Memory::TryRead32(pG + 0x501Cu + 4u * uint32_t(bit >> 5), w) && (w & (0x80000000u >> (bit & 31))) != 0;
}

// The case's live cItem of `id` for its current character (0 when none or none left).
uint32_t FindItem(uint16_t id) {
    uint32_t items = 0, count = 0, charWord = 0;
    if (!Memory::TryRead32(kItemMgr + 0x14u, items) || !GuestPtr(items) || !Memory::TryRead32(kItemMgr + 0x1Cu, count) ||
        count > 0x400u || !Memory::TryRead32(kItemMgr + 0x10u, charWord)) {
        return 0;
    }
    const uint8_t chr = static_cast<uint8_t>(charWord);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t it = items + i * kItemSize;
        const uint16_t flagsType = Read16(it + 4u);
        if (Read16(it) == id && (flagsType & 0x0100u) != 0 && (flagsType & 0xFFu) == chr && Read16(it + 2u) != 0) return it;
    }
    return 0;
}

// The slot's next weapon after the one in hand (or its first); 0 when the case has none of its kind
// besides the one held.
uint32_t Pick(int slot) {
    const Slot& s = kSlots[slot];
    uint32_t held = 0;
    Memory::TryRead32(kItemMgr + 0x0Cu, held);
    uint32_t found[16];
    int n = 0, heldAt = -1;
    for (int i = 0; i < s.count && n < 16; ++i) {
        const uint32_t it = FindItem(s.ids[i]);
        if (it == 0) continue;
        if (it == held) heldAt = n;
        found[n++] = it;
    }
    const uint32_t it = n == 0 ? 0 : found[heldAt < 0 ? 0 : (heldAt + 1) % n];
    if (it == 0 || it == held) {
        RT_LOGF(RT_TAG_RUNTIME, "quick slot %s: nothing to change (%d of its kind in the case)\n", s.name, n);
        return 0;
    }
    return it;
}

// Walking (routines 0..4) or holding a weapon / the knife (6, 0xB), no sub screen, event, scope,
// binoculars or boat.
bool InField(uint32_t pG, uint32_t pl) {
    const uint8_t r0 = Read8(pl + 0xFCu), r1 = Read8(pl + 0xFDu);
    if (r0 != 0 || (r1 > 4 && r1 != 6 && r1 != 0xB)) return false;
    return !StaFlag(pG, 13) && !StaFlag(pG, 19) && !StaFlag(pG, 21) && !StaFlag(pG, 25) && !StaFlag(pG, 42);
}

uint32_t Call(CpuContext* ctx, void (*fn)(CpuContext*), uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0) {
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    fn(ctx);
    return static_cast<uint32_t>(ctx->gpr[3]);
}

// The sub screen exit's change for `item` (0: bare hands).
void Equip(CpuContext* ctx, uint32_t pG, uint32_t pl, uint32_t item, const char* slot) {
    if (Call(ctx, func_800357B8, kItemMgr, item) == 0 && item != 0) {
        RT_LOGF(RT_TAG_RUNTIME, "quick slot %s: the case refused item at %08X\n", slot, item);
        return;
    }
    const uint32_t id = Call(ctx, func_80036C7C, kItemMgr) & 0xFFFFu;
    const uint32_t no = Call(ctx, func_80036A40, id) & 0xFFu;
    const uint32_t type = Call(ctx, func_80036A84, id) & 0xFFu;
    uint32_t wep = 0;
    Memory::TryRead32(kItemMgr + 0x0Cu, wep);
    const uint32_t lv = wep != 0 ? Call(ctx, func_80036D28, wep) & 0xFFu : 0u;
    if (Read8(pG + 0x4FC0u) != no || Read8(pG + 0x4FC1u) != type || Read8(pG + 0x4FC2u) != lv) {
        Call(ctx, func_80078390, 2);
        Call(ctx, func_80138C94, pl);
        Call(ctx, func_80138F78, pl, no, type);
        Memory::Write8(pG + 0x4FC2u, static_cast<uint8_t>(lv));
        Call(ctx, func_80138F94, pl);
        Call(ctx, func_80076368, pl);
    }
    ModSound::PlayQuickSlot();
    RT_LOGF(RT_TAG_RUNTIME, "quick slot %s: %s (item %X, weapon %u, type %u, level %u)\n", slot,
            item == 0 ? "weapon put away" : "equipped", id, no, type, lv);
}

void QuickSlots(CpuContext* ctx) {
    uint32_t pG = 0, pSys = 0, pl = 0, joyOn = 0;
    if (!Memory::TryRead32(kPG, pG) || !GuestPtr(pG) || !Memory::TryRead32(kPSys, pSys) || !GuestPtr(pSys) ||
        !Memory::TryRead32(kPPL, pl) || !GuestPtr(pl) || !Memory::TryRead32(kJoy0 + 0x10u, joyOn)) {
        return;
    }
    const uint32_t dpad = joyOn & 0xFu;
    if (Read8(pG + 0x4FC8u) == 1 || !InField(pG, pl)) {  // pl_type 1: Ashley has no case
        g_dpadPrev = dpad;
        g_holdSlot = -1;
        return;
    }
    const uint32_t pressed = dpad & ~g_dpadPrev;
    static const bool s_log = std::getenv("RE4_QUICK_SLOTS_LOG") != nullptr;
    if (s_log && dpad != g_dpadPrev) {
        RT_LOGF(RT_TAG_RUNTIME, "quick slot: D-pad %X -> %X (Joy[0].on %08X)\n", g_dpadPrev, dpad, joyOn);
    }
    g_dpadPrev = dpad;

    // The D-pad's share of Key's four directions out; the stick's own bits stay (PadRead's table).
    const uint32_t padType = Read8(pSys + 0x0Bu) & 1u;
    uint32_t stick = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t bits = 0;
        Memory::TryRead32(kKeyTypeTbl + padType * 0x100u + 4u * i, bits);
        if ((joyOn & bits & ~0xFu) != 0) stick |= 1u << i;
    }
    if ((stick & 3u) == 3u) stick &= ~3u;
    if ((stick & 0xCu) == 0xCu) stick &= ~0xCu;
    uint32_t keyOn = 0, keyTrg = 0, joyTrg = 0;
    Memory::TryRead32(kKey + 0x14u, keyOn);
    Memory::TryRead32(kKey + 0x1Cu, keyTrg);
    Memory::TryRead32(kJoy0 + 0x14u, joyTrg);
    Memory::Write32(kKey + 0x14u, (keyOn & ~0xFu) | stick);
    Memory::Write32(kKey + 0x1Cu, keyTrg & (~0xFu | stick));
    Memory::Write32(kJoy0 + 0x10u, joyOn & ~0xFu);
    Memory::Write32(kJoy0 + 0x14u, joyTrg & ~0xFu);

    if (g_holdSlot < 0) {
        for (int i = 0; i < 4; ++i) {
            if ((pressed & kSlots[i].joyBit) != 0) {
                g_holdSlot = i;
                g_holdFrames = 0;
                g_holdUsed = false;
                break;
            }
        }
        return;
    }
    if ((dpad & kSlots[g_holdSlot].joyBit) != 0) {
        uint32_t held = 0;
        if (++g_holdFrames == kHolster && Memory::TryRead32(kItemMgr + 0x0Cu, held) && held != 0) {
            g_holdUsed = true;
            Equip(ctx, pG, pl, 0, kSlots[g_holdSlot].name);
        }
        return;
    }
    const int slot = g_holdSlot;
    g_holdSlot = -1;
    if (g_holdUsed) return;
    const uint32_t item = Pick(slot);
    if (item != 0) Equip(ctx, pG, pl, item, kSlots[slot].name);
}
} // namespace

extern "C" void QuickSlots_KeyConfig_80133848(CpuContext* ctx) {
    if (Enabled()) {
        const CpuContext saved = *ctx;
        QuickSlots(ctx);
        *ctx = saved;
    }
    func_80133848(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80133848, QuickSlots_KeyConfig_80133848, "QuickSlots_KeyConfig_80133848");
