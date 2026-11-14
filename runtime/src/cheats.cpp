#include "cheats.h"
#include "game_tweaks.h"

#include "memory.h"
#include "netplay_session.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

// Resident Evil 4 Wii (RB4E08) cheats: Unlock all, infinite health, infinite ammo, special weapons, max money.
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
std::atomic<bool> g_infiniteHealth{false};
std::atomic<bool> g_infiniteAmmo{false};
std::atomic<bool> g_specialWeapons{false};
std::atomic<bool> g_maxMoney{false};
std::atomic<bool> g_autoAim{false};
std::atomic<bool> g_autoQte{false};

// Auto aim: the system save's Config_flg (+0x00) CFG_LOCK_ON (flag 2, 0x20000000), an option the game
// only sets from its debug menu: aiming starts on the nearest enemy's lock point and the gun follows it
// for the lock frames (pl_wep PlSetLockPitch, lockMove; the right stick cancels it). Only that bit is
// changed: a Gecko code going round wrote the whole byte, dropping vibration, knife key and aim reverse.
constexpr uint32_t kCfgLockOn = 0x20000000u;

bool GuestPointer(uint32_t p);  // below

void KeepAutoAim(bool on) {
    static bool applied = false;
    if (!on && !applied) return;
    uint32_t sys = 0, flags = 0;
    if (!Memory::TryRead32(kPSysAddr, sys) || !GuestPointer(sys) || !Memory::TryRead32(sys, flags)) return;
    const uint32_t want = on ? (flags | kCfgLockOn) : (flags & ~kCfgLockOn);
    if (want != flags && Memory::TryWrite32(sys, want)) {
        RT_LOGF(RT_TAG_OS, "cheats: auto aim %s\n", on ? "on" : "off");
    }
    applied = on;
}

// pG->peseta: the GameCube's 0x4F98 + 0x10 (the Wii's save block starts at pG + 0x4F90, the GC's at
// 0x4F80). The game caps it at 99,999,999 (addMoney).
constexpr uint32_t kPesetaOffset = 0x4FA8u;
constexpr uint32_t kMaxMoney = 99999999u;

// wep_info (main.dol 0x802AF910, 43 rows of 22 bytes): u16 weapon id, ..., u16 magazine size per
// capacity tune level at +0x08 (the game reads the low byte; 0 = no magazine). cItem::lv's low nibble
// is the capacity level.
constexpr uint32_t kWepInfoAddr = 0x802AF910u;
constexpr uint32_t kWepInfoRows = 43;
constexpr uint32_t kWepInfoRowSize = 22;

// pSaveData (0x8043E0C4) +0x18: the merchant's MERCHANT_DATA (stock: 64 entries of 8 bytes, u16 id,
// s16 num, u8 isNew; tune levels at +0x200: 32 entries of 8 bytes, u16 id, u8 lv[4], u8 isNew; both
// end at the first id 0xFFFF).
constexpr uint32_t kPSaveDataAddr = 0x8043E0C4u;

// GLOBAL_WK (pG) on Wii sits 0x10 later than in the GC debug build from about 0x4FA0 on (main.dol's
// lha/sth displacements): u16 pl_life, pl_life_max at 0x4FB4, ashley_life, ashley_life_max at 0x4FB8.
constexpr uint32_t kPGAddress = 0x8043D580u;
constexpr uint32_t kPlLifeOffset = 0x4FB4u;
constexpr uint32_t kAshleyLifeOffset = 0x4FB8u;
// ItemMgr (cItemMgr, GC .bss 0x80271A88) is 0x8031F600 on Wii: gameRoomInit's ItemMgr.roomInit() loads
// it at 0x80106868. +0x0C cItem* m_pWep (equipped weapon slot), +0x10 u16 m_wep_id. cItem: u16 id,
// u16 num (+0x00), u8 flags (+0x04, bit0 in use), u16 bullet (+0x08: top 3 bits slot attribute, low
// 13 bits rounds loaded).
constexpr uint32_t kItemMgrAddr = 0x8031F600u;

bool GuestPointer(uint32_t p) {
    return (p & 0xF0000000u) == 0x80000000u || (p & 0xF0000000u) == 0x90000000u;
}

// Tops a (life, max) u16 pair up to max while it is alive and below it.
void RefillLife(uint32_t addr) {
    uint32_t word = 0;
    if (!Memory::TryRead32(addr, word)) {
        return;
    }
    const auto life = static_cast<int16_t>(word >> 16);
    const auto max = static_cast<int16_t>(word & 0xFFFFu);
    if (max <= 0 || life <= 0 || life >= max) {
        return;
    }
    Memory::TryWrite32(addr, (word & 0xFFFFu) << 16 | (word & 0xFFFFu));
}

void KeepHealth() {
    uint32_t pG = 0;
    if (!Memory::TryRead32(kPGAddress, pG) || !GuestPointer(pG)) {
        return;
    }
    RefillLife(pG + kPlLifeOffset);
    RefillLife(pG + kAshleyLifeOffset);
}

// The equipped slot as last seen: a shot (rounds loaded) or a throw (num) taken from the same slot
// holding the same item is put back. Reloads and capacity upgrades raise what is kept.
struct AmmoWatch {
    uint32_t slot = 0;
    uint16_t id = 0;
    uint16_t loaded = 0;
    uint16_t num = 0;
};
AmmoWatch g_ammo;

// A u16 read that never throws (Contains first: the minimap's catch path corrupted registers on Android).
bool Read16(uint32_t addr, uint16_t& value) {
    if (!Memory::Contains(addr, 2)) return false;
    value = Memory::Read16(addr);
    return true;
}

// Magazine size of weapon `id` at capacity level `level` (0-based), 0 when it has none.
uint16_t MagazineSize(uint16_t id, uint32_t level) {
    if (level > 6) return 0;
    for (uint32_t row = 0; row < kWepInfoRows; ++row) {
        const uint32_t at = kWepInfoAddr + row * kWepInfoRowSize;
        uint16_t rowId = 0, bulletId = 0, size = 0;
        if (!Read16(at, rowId)) return 0;
        if (rowId != id) continue;
        // Bullet id 0xFF: thrown or one-shot (grenades, the disposable launchers), no magazine.
        if (!Read16(at + 0x06u, bulletId) || bulletId == 0xFFu) return 0;
        if (!Read16(at + 0x08u + level * 2u, size)) return 0;
        return static_cast<uint16_t>(size & 0xFFu);
    }
    return 0;
}

void KeepAmmo() {
    uint32_t slot = 0, wepWord = 0, head = 0, flagsWord = 0, bulletWord = 0;
    if (!Memory::TryRead32(kItemMgrAddr + 0x0Cu, slot) || !GuestPointer(slot) ||
        !Memory::TryRead32(kItemMgrAddr + 0x10u, wepWord) || !Memory::TryRead32(slot, head) ||
        !Memory::TryRead32(slot + 0x04u, flagsWord) || !Memory::TryRead32(slot + 0x08u, bulletWord)) {
        g_ammo = {};
        return;
    }
    const auto id = static_cast<uint16_t>(head >> 16);
    const auto num = static_cast<uint16_t>(head & 0xFFFFu);
    const auto loaded = static_cast<uint16_t>((bulletWord >> 16) & 0x1FFFu);
    if (id != (wepWord >> 16) || (flagsWord & 0x01000000u) == 0) {
        g_ammo = {};
        return;
    }
    // A weapon with a magazine is kept full, so it fires with no spare ammo in the case (only putting
    // back what a shot took left a weapon that came empty, bought or picked up, unable to fire).
    // Grenades (0x01, 0x02, 0x0E): throwing the last one of a stack empties its slot (dump), past what
    // can be put back, so the readied stack is never left at 1.
    if ((id == 0x01u || id == 0x02u || id == 0x0Eu) && num == 1) {
        Memory::TryWrite32(slot, (head & 0xFFFF0000u) | 2u);
        g_ammo = {slot, id, loaded, 2};
        return;
    }
    const uint16_t magazine = MagazineSize(id, flagsWord & 0x0Fu);
    if (magazine != 0 && loaded < magazine) {
        Memory::TryWrite32(slot + 0x08u, (bulletWord & 0xE000FFFFu) | static_cast<uint32_t>(magazine) << 16);
        g_ammo = {slot, id, magazine, num};
        return;
    }
    if (g_ammo.slot != slot || g_ammo.id != id) {
        g_ammo = {slot, id, loaded, num};
        return;
    }
    if (loaded < g_ammo.loaded) {
        Memory::TryWrite32(slot + 0x08u, (bulletWord & 0xE000FFFFu) | static_cast<uint32_t>(g_ammo.loaded) << 16);
    } else {
        g_ammo.loaded = loaded;
    }
    if (num < g_ammo.num) {
        Memory::TryWrite32(slot, (head & 0xFFFF0000u) | g_ammo.num);
    } else {
        g_ammo.num = num;
    }
}

void KeepMoney() {
    uint32_t pG = 0, money = 0;
    if (!Memory::TryRead32(kPGAddress, pG) || !GuestPointer(pG) || !Memory::TryRead32(pG + kPesetaOffset, money)) {
        return;
    }
    if (money < kMaxMoney) Memory::TryWrite32(pG + kPesetaOffset, kMaxMoney);
}

// Special Weapons: the merchant stocks what the game only adds in a cleared game's next round
// (MerchantRoomInit, game_cnt != 0) or after the extra modes: Matilda and the infinite launcher
// (stock_ext_normal), the Handcannon, the Chicago Typewriter and the Wii's P.R.L. 412, with the
// Wii's tune tables for Matilda and the Handcannon. Prices come from the game's own table.
// pG->pl_type (GC 0x4FB8, Wii 0x4FC8: 116 lbz in main.dol): 0 Leon, 2 Ada (Separate Ways, Assignment
// Ada), 3..5 The Mercenaries.
constexpr uint32_t kPlTypeOffset = 0x4FC8u;
constexpr uint16_t kSpecialStock[] = {0x03, 0x6D, 0x37, 0x34, 0x41};

// Takes the Special Weapons out of a stock table of `count` 8-byte entries (shifting the rest down to
// the 0xFFFF end). Beta 4 stocked them for Ada too: Separate Ways froze buying from that merchant
// (REL 81 reading wild addresses), and the stock was kept in her saves.
void RemoveSpecialStock(uint32_t table, uint32_t count) {
    uint32_t end = 0;
    while (end < count) {
        uint16_t id = 0;
        if (!Read16(table + end * 8u, id)) return;
        if (id == 0xFFFFu) break;
        ++end;
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < end; ++i) {
        uint32_t a = 0, b = 0;
        if (!Memory::TryRead32(table + i * 8u, a) || !Memory::TryRead32(table + i * 8u + 4u, b)) return;
        const auto id = static_cast<uint16_t>(a >> 16);
        if (std::find(std::begin(kSpecialStock), std::end(kSpecialStock), id) != std::end(kSpecialStock)) {
            RT_LOGF(RT_TAG_OS, "cheats: merchant stock item %02X removed (not Leon)\n", id);
            continue;
        }
        if (kept != i) {
            Memory::TryWrite32(table + kept * 8u, a);
            Memory::TryWrite32(table + kept * 8u + 4u, b);
        }
        ++kept;
    }
    for (uint32_t i = kept; i < end; ++i) {
        Memory::TryWrite32(table + i * 8u, 0xFFFF0000u);
        Memory::TryWrite32(table + i * 8u + 4u, 0u);
    }
}

// Ada's games are cleaned whether or not the cheat is on: a save stocked by Beta 4 kept the Leon
// weapons, and buying from that merchant still crashed with the cheat off (Beta 6, room 508).
void StockSpecialWeapons(bool stock) {
    struct Level {
        uint16_t id;
        uint8_t lv[4];
    };
    static constexpr Level kLevels[] = {{0x03, {6, 1, 3, 7}}, {0x37, {7, 1, 3, 7}}};
    uint32_t head = 0, data = 0, pG = 0, room = 0, plWord = 0;
    if (!Memory::TryRead32(kPSaveDataAddr, head) || !GuestPointer(head) || !Memory::TryRead32(head + 0x18u, data) ||
        !GuestPointer(data) || !Memory::TryRead32(kPGAddress, pG) || !GuestPointer(pG) ||
        !Memory::TryRead32(pG + 0x4FACu, room) || (room >> 16) == 0 ||
        !Memory::TryRead32(pG + kPlTypeOffset, plWord)) {
        return;  // no game running yet
    }
    // Leon's weapons only: Ada's game (Separate Ways) has its own weapon set.
    if ((plWord >> 24) != 0) {
        RemoveSpecialStock(data, 64);
        return;
    }
    if (!stock) {
        return;
    }
    // Adds `id` to a table of `count` 8-byte entries unless it is there; returns the free entry used.
    auto add = [](uint32_t table, uint32_t count, uint16_t id) -> uint32_t {
        for (uint32_t i = 0; i < count; ++i) {
            uint16_t at = 0;
            if (!Read16(table + i * 8u, at)) return 0;
            if (at == id) return 0;
            if (at == 0xFFFFu) return table + i * 8u;
        }
        return 0;
    };
    for (const uint16_t id : kSpecialStock) {
        if (const uint32_t e = add(data, 64, id); e != 0) {
            // num 1 (a weapon is sold once), isNew; the next entry already reads 0xFFFF (stockDataInit).
            Memory::TryWrite32(e, static_cast<uint32_t>(id) << 16 | 1u);
            Memory::TryWrite32(e + 4u, 0x01000000u);
            RT_LOGF(RT_TAG_OS, "cheats: merchant stocks item %02X\n", id);
        }
    }
    for (const Level& level : kLevels) {
        if (const uint32_t e = add(data + 0x200u, 32, level.id); e != 0) {
            Memory::TryWrite32(e, static_cast<uint32_t>(level.id) << 16 | level.lv[0] << 8 | level.lv[1]);
            Memory::TryWrite32(e + 4u, static_cast<uint32_t>(level.lv[2]) << 24 | level.lv[3] << 16 | 0x0100u);
        }
    }
}

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



// Auto aim through the rifle scope. The game's lock-on (CFG_LOCK_ON) only points the shoulder camera at
// the locked enemy's feet before the scope opens, and the scope then starts level and from the
// player's facing (GC pl_rifle wep09_r2_ready / CameraScope). So when the scope opens right after the
// aim's ready step, the view is turned here, over a few game frames, onto the head (highest parts) of
// the enemy nearest the crosshair: Leon's yaw (cModel ang.y +0xA4) and the scope's pitch (CameraScope
// angle_x, found by the +-70 degree limits beside it in CamCtrl's work). Each correction is relative
// to the view the game drew (pG->Camera Campos / Target), so the frames' conventions cancel out.
constexpr uint32_t kPGAddr = 0x8043D580u;
constexpr uint32_t kPPLAddr = 0x8043DEA0u;     // pPL (r13 - 0x6B80)
constexpr uint32_t kEmArrayPtr = 0x8032E274u;  // EmMgr pArray
constexpr uint32_t kEmCount = 60u, kEmStride = 0xE04u;
constexpr uint32_t kCamCtrlAddr = 0x8030ED4Cu;           // CamCtrl (CameraMove's first argument)
constexpr uint32_t kCampos = 0x118u, kTarget = 0x124u;  // pG->Camera.param
constexpr int kStaScopeCamera = 25;                      // Status_flg bit (as GameTweaks' camera code)
constexpr float kScopeLimit = 1.2217305f;                // CameraScope angle_min / angle_max (70 degrees)
constexpr int kScopeSteps = 6;

float ReadF(uint32_t a, bool& ok) {
    uint32_t bits = 0;
    ok = ok && Memory::TryRead32(a, bits);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
bool WriteF(uint32_t a, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return Memory::TryWrite32(a, bits);
}
float WrapAngle(float a) {
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}
bool StaBit(uint32_t pg, int bit) {
    uint32_t w = 0;
    return Memory::TryRead32(pg + 0x501Cu + 4u * (bit >> 5), w) && (w & (0x80000000u >> (bit & 31)));
}

// The scope's angle_x, or 0: angle_min / angle_max sit 0xC and 0x18 after it (GC cam_extra.h).
uint32_t FindScopePitch() {
    for (uint32_t a = kCamCtrlAddr + 0xCu; a < kCamCtrlAddr + 0x1000u; a += 4) {
        bool ok = true;
        const float lo = ReadF(a, ok), hi = ReadF(a + 0xCu, ok), x = ReadF(a - 0xCu, ok);
        if (ok && lo == -kScopeLimit && hi == kScopeLimit && x >= lo - 0.01f && x <= hi + 0.01f) return a - 0xCu;
    }
    return 0;
}

// The highest parts of the live enemy closest in angle to the view ray (yaw, pitch up positive), within
// 20 degrees and 60 m of the eye. Out: its world position.
bool FindHeadTarget(const float eye[3], float yaw, float pitch, float out[3]) {
    uint32_t arr = 0;
    if (!Memory::TryRead32(kEmArrayPtr, arr) || !GuestPointer(arr)) return false;
    float best = 0.35f;
    bool found = false;
    for (uint32_t i = 0; i < kEmCount; ++i) {
        const uint32_t em = arr + i * kEmStride;
        uint32_t flags = 0, parts = 0, idWord = 0, hpWord = 0;
        if (!Memory::TryRead32(em, flags) || (flags & 1u) == 0 || !Memory::TryRead32(em + 0x100u, idWord) ||
            (idWord >> 24) <= 0x0Fu || !Memory::TryRead32(em + 0x320u, hpWord) ||
            static_cast<int16_t>(hpWord & 0xFFFFu) <= 0 || !Memory::TryRead32(em + 0xF4u, parts)) {
            continue;
        }
        float head[3] = {0.0f, -1e30f, 0.0f};
        for (int n = 0; n < 64 && GuestPointer(parts); ++n) {
            bool ok = true;
            const float p[3] = {ReadF(parts + 0x70u, ok), ReadF(parts + 0x74u, ok), ReadF(parts + 0x78u, ok)};
            if (ok && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && p[1] > head[1]) {
                std::memcpy(head, p, sizeof(head));
            }
            if (!Memory::TryRead32(parts + 0xF4u, parts)) break;
        }
        if (head[1] < -1e29f) continue;
        const float dx = head[0] - eye[0], dy = head[1] - eye[1], dz = head[2] - eye[2];
        const float flat = std::sqrt(dx * dx + dz * dz);
        if (flat < 100.0f || flat > 60000.0f) continue;
        const float dyaw = WrapAngle(std::atan2(dx, dz) - yaw), dpitch = std::atan2(dy, flat) - pitch;
        const float miss = std::sqrt(dyaw * dyaw + dpitch * dpitch);
        if (miss < best) {
            best = miss;
            std::memcpy(out, head, sizeof(head));
            found = true;
        }
    }
    return found;
}

void ScopeAutoAim(bool on) {
    static bool s_wasScope = false, s_wasReady = false;
    static int s_steps = 0;
    static float s_sign = 1.0f, s_lastMiss = 0.0f;
    static uint32_t s_lastTarget = 0;
    static float s_head[3] = {};
    uint32_t pg = 0, pl = 0, rno = 0;
    if (!Memory::TryRead32(kPGAddr, pg) || !GuestPointer(pg) || !Memory::TryRead32(kPPLAddr, pl) ||
        !GuestPointer(pl) || !Memory::TryRead32(pl + 0xFCu, rno)) {
        s_wasScope = s_wasReady = false;
        return;
    }
    const bool scope = StaBit(pg, kStaScopeCamera);
    const bool ready = ((rno >> 16) & 0xFFu) == 6u && ((rno >> 8) & 0xFFu) == 0u;  // r_no_1 6, r_no_2 0
    if (scope && !s_wasScope) {
        // Only a scope opened from the aim's ready step: not the return from the inventory or the
        // bolt cycle, which keep the player's own view.
        s_steps = on && s_wasReady ? kScopeSteps : 0;
        s_sign = 1.0f;
        s_lastMiss = 0.0f;
        s_lastTarget = 0;
    }
    s_wasScope = scope;
    if (!scope) s_wasReady = ready;
    if (!scope || s_steps <= 0) return;
    // One correction per drawn game frame: the camera copy changes (retraces run twice as often).
    uint32_t targetBits = 0;
    Memory::TryRead32(pg + kTarget, targetBits);
    if (targetBits == s_lastTarget) return;
    s_lastTarget = targetBits;
    bool ok = true;
    float eye[3], at[3];
    for (uint32_t i = 0; i < 3; ++i) {
        eye[i] = ReadF(pg + kCampos + 4u * i, ok);
        at[i] = ReadF(pg + kTarget + 4u * i, ok);
    }
    const uint32_t pitchAddr = FindScopePitch();
    if (!ok || pitchAddr == 0) {
        RT_LOGF(RT_TAG_OS, "cheats: scope auto aim: no scope camera found\n");
        s_steps = 0;
        return;
    }
    const float vx = at[0] - eye[0], vy = at[1] - eye[1], vz = at[2] - eye[2];
    const float yaw = std::atan2(vx, vz), pitch = std::atan2(vy, std::sqrt(vx * vx + vz * vz));
    if (s_steps == kScopeSteps && !FindHeadTarget(eye, yaw, pitch, s_head)) {
        RT_LOGF(RT_TAG_OS, "cheats: scope auto aim: no enemy near the crosshair\n");
        s_steps = 0;
        return;
    }
    const float dx = s_head[0] - eye[0], dy = s_head[1] - eye[1], dz = s_head[2] - eye[2];
    const float dyaw = WrapAngle(std::atan2(dx, dz) - yaw);
    const float dpitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz)) - pitch;
    // angle_x grows downwards in the GC source; if a correction made the miss worse, this build turns
    // the other way round.
    if (s_steps < kScopeSteps && std::fabs(dpitch) > s_lastMiss * 1.2f && std::fabs(dpitch) > 0.01f) {
        s_sign = -s_sign;
    }
    s_lastMiss = std::fabs(dpitch);
    const float angY = ReadF(pl + 0xA4u, ok), angX = ReadF(pitchAddr, ok);
    if (!ok) return;
    WriteF(pl + 0xA4u, WrapAngle(angY + dyaw));
    WriteF(pitchAddr, std::clamp(angX - s_sign * dpitch, -kScopeLimit, kScopeLimit));
    RT_LOGF(RT_TAG_OS, "cheats: scope auto aim step %d: miss yaw %+.3f pitch %+.3f rad (scope pitch 0x%08X)\n",
            kScopeSteps + 1 - s_steps, dyaw, dpitch, pitchAddr);
    --s_steps;
}
// Auto QTE: Cockpit's ActionButton (0x80329528 + 0xBD on Wii, the GC's Cckpt.m_ActBttn) keeps
// m_disp_flag, the button of the prompt cActionButton::move showed this frame (0 none; DISP_FLAG:
// 2 A rapid, 3 L+R, 4 A+B, 11 mash, 13 B rapid). Events, dodges and mashes all go through those
// prompts and read Key.trg, so the remote gets the buttons pressed and let go every 50 ms while one
// is up: a fresh press each time for the dodges, about ten a second for the mashes.
constexpr uint32_t kActBtnDispWord = 0x803295E4u;  // m_disp_flag is its second byte
constexpr uint64_t kQtePulseNs = 50'000'000;

uint32_t QteButtonsFor(uint8_t disp) {
    switch (disp) {
    case 2:
    case 11:
        return kQteA;
    case 3:
        return kQteL | kQteR;
    case 4:
        return kQteA | kQteB;
    case 13:
        return kQteB;
    default:
        return 0;
    }
}
} // namespace

uint32_t AutoQteButtons(uint64_t nowNs) {
    static const bool s_env = std::getenv("RE4_AUTO_QTE") != nullptr;  // Windows tests
    if (!(s_env || g_autoQte.load(std::memory_order_relaxed)) || !Available() || GameTweaks::SpeedrunMode()) {
        return 0;
    }
    uint32_t word = 0;
    if (!Memory::TryRead32(kActBtnDispWord, word)) {
        return 0;
    }
    const uint8_t disp = static_cast<uint8_t>(word >> 16);
    static uint8_t s_lastDisp = 0;
    const uint32_t buttons = QteButtonsFor(disp);
    if (disp != s_lastDisp) {
        s_lastDisp = disp;
        if (buttons != 0) RT_LOGF(RT_TAG_OS, "cheats: auto QTE prompt %u\n", disp);
    }
    return (nowNs / kQtePulseNs) % 2u == 0 ? buttons : 0;
}

bool Available() {
    return !NetplaySession::Active();
}

void SetFromLauncher(bool unlockAll, bool infiniteHealth, bool infiniteAmmo, bool specialWeapons, bool maxMoney,
                     bool autoAim, bool autoQte) {
    g_autoAim = autoAim;
    g_autoQte = autoQte;
    g_unlockAll = unlockAll;
    g_infiniteHealth = infiniteHealth;
    g_infiniteAmmo = infiniteAmmo;
    g_specialWeapons = specialWeapons;
    g_maxMoney = maxMoney;
}

void Apply() {
    if (!Available()) {
        return;
    }
    // Speedrun Mode: every cheat off (special weapons and auto aim cleared below as when turned off).
    const bool allowed = !GameTweaks::SpeedrunMode();
    if (allowed && g_infiniteHealth.load(std::memory_order_relaxed)) {
        KeepHealth();
    }
    if (allowed && g_infiniteAmmo.load(std::memory_order_relaxed)) {
        KeepAmmo();
    } else {
        g_ammo = {};
    }
    if (allowed && g_maxMoney.load(std::memory_order_relaxed)) {
        KeepMoney();
    }
    static const bool s_envAutoAim = std::getenv("RE4_AUTO_AIM") != nullptr;  // Windows tests
    const bool autoAim = allowed && (s_envAutoAim || g_autoAim.load(std::memory_order_relaxed));
    ScopeAutoAim(autoAim);
    // Once a second: the system save read from NAND at the title replaces the flags set before it,
    // and loading a game replaces the merchant's stock.
    static uint32_t s_frame = 0;
    if (++s_frame % 60u != 1u) {
        return;
    }
    StockSpecialWeapons(allowed && g_specialWeapons.load(std::memory_order_relaxed));
    KeepAutoAim(autoAim);
    static const bool s_envUnlockAll = std::getenv("RE4_UNLOCK_ALL") != nullptr;  // Windows tests
    if (!allowed || !(s_envUnlockAll || g_unlockAll.load(std::memory_order_relaxed))) {
        return;
    }
    if (const uint32_t added = UnlockAllNow(); added != 0) {
        RT_LOGF(RT_TAG_OS, "cheats: unlock all set Extra_flg bits %08X\n", added);
    }
}

} // namespace Cheats
