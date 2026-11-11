#include "cheats.h"

#include "memory.h"
#include "netplay_session.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

// Resident Evil 4 Wii (RB4E08) cheats: Unlock all, infinite health, infinite ammo, experimental 60 fps.
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
void StockSpecialWeapons() {
    struct Level {
        uint16_t id;
        uint8_t lv[4];
    };
    static constexpr uint16_t kStock[] = {0x03, 0x6D, 0x37, 0x34, 0x41};
    static constexpr Level kLevels[] = {{0x03, {6, 1, 3, 7}}, {0x37, {7, 1, 3, 7}}};
    uint32_t head = 0, data = 0, pG = 0, room = 0;
    if (!Memory::TryRead32(kPSaveDataAddr, head) || !GuestPointer(head) || !Memory::TryRead32(head + 0x18u, data) ||
        !GuestPointer(data) || !Memory::TryRead32(kPGAddress, pG) || !GuestPointer(pG) ||
        !Memory::TryRead32(pG + 0x4FACu, room) || (room >> 16) == 0) {
        return;  // no game running yet
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
    for (const uint16_t id : kStock) {
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


// Experimental 60 fps (RE4_60FPS=1), the Dolphin GC code (002B3108 / 0427F840) moved to the Wii build:
// the main loop waits for system_vcnt retraces a step (2 = 30 fps; SetSystemVcnt 0x8011558C stores it at
// r13-0x687D) and motion advances pG->Speed (+0x70, 1.0) frames a step. One retrace a step at Speed 0.5
// keeps the pace; Speed follows the measured step time (0.5 at 60 fps .. 1.0 at 30), so a phone that
// manages 40 fps is not in slow motion. What does not go through Speed (timers, some AI) runs faster.
constexpr uint32_t kSystemVcntWord = 0x8043E1A0u;  // system_vcnt is its last byte (0x8043E1A3)
constexpr uint32_t kPGAddr = kPGAddress;
constexpr uint32_t kSpeedOffset = 0x70u;
std::atomic<float> g_stepMs{33.3f};
float g_writtenSpeed = 0.0f;

bool SixtyFpsEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("RE4_60FPS");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

float AsFloat(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }
uint32_t AsBits(float f) { uint32_t bits; std::memcpy(&bits, &f, 4); return bits; }

// Events, movies and door / death demos step their own frame counts and fades (the r100 intro event
// stopped on its first fade at 60 fps), so they run at the game's 30 fps: Status_flg (+0x501C) bits
// STA_MOVIE_ON 3, STA_MOVIE2_ON 5, STA_DIEDEMO 11, STA_EVENT 19, STA_EVENT_SYSYTEM 76, and System_flg
// (+0x54) SYS_DOORDEMO 10.
bool InCutscene(uint32_t pG) {
    static constexpr int kStatusBits[] = {3, 5, 11, 19, 76};
    for (int bit : kStatusBits) {
        uint32_t w = 0;
        if (Memory::TryRead32(pG + 0x501Cu + 4u * (bit >> 5), w) && (w & (0x80000000u >> (bit & 31)))) return true;
    }
    uint32_t sys = 0;
    return Memory::TryRead32(pG + 0x54u, sys) && (sys & (0x80000000u >> 10));
}

void ApplySixtyFps() {
    uint32_t pG = 0, bits = 0;
    if (!Memory::TryRead32(kPGAddr, pG) || (pG & 0xFE000000u) != 0x80000000u ||
        !Memory::TryRead32(pG + kSpeedOffset, bits)) {
        return;
    }
    const bool cutscene = InCutscene(pG);
    uint32_t word = 0;
    if (Memory::TryRead32(kSystemVcntWord, word)) {
        const uint32_t vcnt = word & 0xFFu;
        if (!cutscene && vcnt == 2u) Memory::TryWrite32(kSystemVcntWord, (word & ~0xFFu) | 1u);
        if (cutscene && vcnt == 1u) Memory::TryWrite32(kSystemVcntWord, (word & ~0xFFu) | 2u);
    }
    // Leave a Speed the game set itself (anything but its 1.0 or the value written here) alone.
    const float current = AsFloat(bits);
    if (current != 1.0f && current != g_writtenSpeed) {
        return;
    }
    const float target =
        cutscene ? 1.0f : std::clamp(g_stepMs.load(std::memory_order_relaxed) / 33.333f, 0.5f, 1.0f);
    if (current != target) {
        Memory::TryWrite32(pG + kSpeedOffset, AsBits(target));
    }
    g_writtenSpeed = target;
}
} // namespace
void OnGameFrame(double frameMs) {
    if (!SixtyFpsEnabled() || frameMs <= 0.0 || frameMs > 200.0) {
        return;
    }
    const float ms = std::clamp(static_cast<float>(frameMs), 16.667f, 33.333f);
    const float previous = g_stepMs.load(std::memory_order_relaxed);
    g_stepMs.store(previous * 0.75f + ms * 0.25f, std::memory_order_relaxed);
}
bool Available() {
    return !NetplaySession::Active();
}

void SetFromLauncher(bool unlockAll, bool infiniteHealth, bool infiniteAmmo, bool specialWeapons, bool maxMoney) {
    g_unlockAll = unlockAll;
    g_infiniteHealth = infiniteHealth;
    g_infiniteAmmo = infiniteAmmo;
    g_specialWeapons = specialWeapons;
    g_maxMoney = maxMoney;
}

void Apply() {
    if (SixtyFpsEnabled() && Available()) {
        ApplySixtyFps();
    }
    if (!Available()) {
        return;
    }
    if (g_infiniteHealth.load(std::memory_order_relaxed)) {
        KeepHealth();
    }
    if (g_infiniteAmmo.load(std::memory_order_relaxed)) {
        KeepAmmo();
    } else {
        g_ammo = {};
    }
    if (g_maxMoney.load(std::memory_order_relaxed)) {
        KeepMoney();
    }
    // Once a second: the system save read from NAND at the title replaces the flags set before it,
    // and loading a game replaces the merchant's stock.
    static uint32_t s_frame = 0;
    if (++s_frame % 60u != 1u) {
        return;
    }
    if (g_specialWeapons.load(std::memory_order_relaxed)) {
        StockSpecialWeapons();
    }
    if (!g_unlockAll.load(std::memory_order_relaxed)) {
        return;
    }
    if (const uint32_t added = UnlockAllNow(); added != 0) {
        RT_LOGF(RT_TAG_OS, "cheats: unlock all set Extra_flg bits %08X\n", added);
    }
}

} // namespace Cheats
