#include "cheats.h"
#include "guest_interrupt_context.h"
#include "game_tweaks.h"

#include "abi_bridge.h"
#include "ppc_runtime.h"

#include "memory.h"
#include "netplay_session.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Play As hooks that show the game Leon (pl_type 0) for one call and put the real type back after it.
// The call can span frames (an event start reads its files), so the per-frame checks saw that 0: the
// 1.0 report froze at the giant with Ada after "pl_type 0 -> 2 now: r104 starts again" while the skipped
// scene's start still ran. While one is open, those checks skip the frame.
namespace {
int g_plTypeSwapDepth = 0;  // the guest thread only
struct PlTypeSwapScope {
    PlTypeSwapScope() { ++g_plTypeSwapDepth; }
    ~PlTypeSwapScope() { --g_plTypeSwapDepth; }
    PlTypeSwapScope(const PlTypeSwapScope&) = delete;
    PlTypeSwapScope& operator=(const PlTypeSwapScope&) = delete;
};
}  // namespace

namespace {
int StandInPlaying();  // with Play As below: the stand-in (2..5) Play As made the story's player, else 0
}  // namespace

// Resident Evil 4 Wii (RB4E08) cheats: Unlock all, infinite health, infinite ammo, special weapons, max money.
namespace Cheats {
namespace {

// pSys (sdata 0x8043D584, next to pG at 0x8043D580) points at the system save block (GC SystemSave:
// Config_flg +0x00, Extra_flg +0x04). Extra_flg bits are numbered from the MSB (FlagOn: 0x80000000 >> n):
// 0 costumes, 1 Professional / extras menu, 2 handcannon, 3 Chicago Typewriter, 4..7 Ada, HUNK, Krauser
// and Wesker in The Mercenaries, 8 Assignment Ada, 9 Separate Ways, 10 Ashley's armor. The Wii adds
// rewards past the GameCube's bits (ExtFlagChk 0x80032070 / ExtFlagOn 0x80032080 in main.dol): 12 with
// costume bit 0 for the title's extras (0x80105EC0), 13 the Professional clear (set by the ending at
// 0x800627DC; the merchant's next round stocks the P.R.L. 412 table 0x802C92E8 for it, 0x8003B604), 14 the
// merchant's item 0x53 (0x8003AD64). 11 is not read by main.dol; 15 only marks a notice as seen (0x801506BC)
// and is left alone.
constexpr uint32_t kPSysAddr = 0x8043D584u;
constexpr uint32_t kExtraFlgOffset = 0x04u;
constexpr uint32_t kUnlockAllBits = 0xFFFE0000u;  // bits 0..14

std::atomic<bool> g_unlockAll{RuntimeConfigFile::CheatUnlockAll()};
std::atomic<bool> g_infiniteHealth{false};
std::atomic<bool> g_infiniteAmmo{false};
std::atomic<bool> g_specialWeapons{false};
std::atomic<bool> g_maxMoney{false};
std::atomic<bool> g_autoAim{false};
std::atomic<bool> g_autoQte{false};
std::atomic<bool> g_itemStack{false};
std::atomic<int> g_playAs{0};
std::atomic<bool> g_biggestCase{false};
std::atomic<bool> g_oneShotKills{false};
std::atomic<bool> g_steadyAim{false};

// Windows tests: RE4_PLAY_AS=<2..5>, RE4_BIGGEST_CASE, RE4_ONE_SHOT.
bool EnvOn(const char* name) { return std::getenv(name) != nullptr; }

// Auto aim: the system save's Config_flg (+0x00) CFG_LOCK_ON (flag 2, 0x20000000), an option the game
// only sets from its debug menu: aiming starts on the nearest enemy's lock point and the gun follows it
// for the lock frames (pl_wep PlSetLockPitch, lockMove; a push of the aim stick cancels it). Only that bit is
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
    // Only MEM1 and MEM2 (128 MB here; RELs live at 0x96...). A stale word read during a room load
    // can point into the unmapped gap between them (0x88F757D7, OPPO CPH1933 crash in room 300),
    // and Read32 faults there instead of failing.
    return (p >= Memory::kMem1CachedBase && p < Memory::kMem1CachedBase + Memory::kMem1Size) ||
           (p >= Memory::kMem2CachedBase && p < Memory::kMem2CachedEnd);
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

bool IsGrenade(uint16_t id) { return id == 0x01u || id == 0x02u || id == 0x0Eu; }

// Grenades: cItemMgr::trigger() (Wii 0x80035C7C) clears m_pWep after every throw and takes the next
// one with minimumSearch(m_wep_id) (0x800342D4), so the readied kind is kept by id, not by slot:
// every slot of it (cItem stride 0x0E at m_pItem +0x14, m_array_num +0x1C, alive = flags bit0 and
// type == m_char +0x13) is held at 2, as a throw dumps (num--, erase at 0) the one it takes.
void KeepGrenades(uint16_t wepId, uint8_t chr) {
    uint32_t items = 0, count = 0;
    if (!Memory::TryRead32(kItemMgrAddr + 0x14u, items) || !GuestPointer(items) ||
        !Memory::TryRead32(kItemMgrAddr + 0x1Cu, count) || count > 0x400u) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t at = items + i * 0x0Eu;
        uint16_t id = 0, num = 0, flagsType = 0;
        if (!Read16(at, id) || !Read16(at + 0x02u, num) || !Read16(at + 0x04u, flagsType)) return;
        if (id != wepId || (flagsType & 0x0100u) == 0 || (flagsType & 0xFFu) != chr || num != 1) continue;
        Memory::TryWrite32(at, static_cast<uint32_t>(id) << 16 | 2u);
    }
}

// Ada's bowgun (0x52) has no magazine: cItemMgr::trigger() takes each shot from the case with
// dump(0x72), the first arrow stack (num--, erase at 0). Every arrow stack of the character keeps the
// most it has held while the bowgun is readied; a stack at 1 is raised to 2 so a shot never erases it.
constexpr uint16_t kBowgunId = 0x52u;
constexpr uint16_t kArrowId = 0x72u;
struct ArrowStack {
    uint32_t at = 0;
    uint16_t num = 0;
};
ArrowStack g_arrows[16];

void KeepArrows(uint8_t chr) {
    uint32_t items = 0, count = 0;
    if (!Memory::TryRead32(kItemMgrAddr + 0x14u, items) || !GuestPointer(items) ||
        !Memory::TryRead32(kItemMgrAddr + 0x1Cu, count) || count > 0x400u) {
        return;
    }
    ArrowStack seen[16];
    size_t found = 0;
    for (uint32_t i = 0; i < count && found < 16; ++i) {
        const uint32_t at = items + i * 0x0Eu;
        uint16_t id = 0, num = 0, flagsType = 0;
        if (!Read16(at, id) || !Read16(at + 0x02u, num) || !Read16(at + 0x04u, flagsType)) return;
        if (id != kArrowId || (flagsType & 0x0100u) == 0 || (flagsType & 0xFFu) != chr) continue;
        uint16_t keep = num < 2 ? 2 : num;
        for (const ArrowStack& old : g_arrows) {
            if (old.at == at && old.num > keep) keep = old.num;
        }
        if (keep != num) Memory::TryWrite32(at, static_cast<uint32_t>(id) << 16 | keep);
        seen[found++] = {at, keep};
    }
    for (size_t i = 0; i < 16; ++i) g_arrows[i] = i < found ? seen[i] : ArrowStack{};
}

void KeepAmmo() {
    uint32_t slot = 0, wepWord = 0, head = 0, flagsWord = 0, bulletWord = 0;
    if (Memory::TryRead32(kItemMgrAddr + 0x10u, wepWord) && IsGrenade(static_cast<uint16_t>(wepWord >> 16))) {
        KeepGrenades(static_cast<uint16_t>(wepWord >> 16), static_cast<uint8_t>(wepWord & 0xFFu));
        g_ammo = {};
        return;
    }
    if (static_cast<uint16_t>(wepWord >> 16) == kBowgunId) {
        KeepArrows(static_cast<uint8_t>(wepWord & 0xFFu));
        g_ammo = {};
        return;
    }
    for (ArrowStack& stack : g_arrows) stack = {};
    if (!Memory::TryRead32(kItemMgrAddr + 0x0Cu, slot) || !GuestPointer(slot) ||
        !Memory::TryRead32(kItemMgrAddr + 0x10u, wepWord) || !Memory::TryRead32(slot, head) ||
        !Memory::TryRead32(slot + 0x04u, flagsWord) || !Memory::TryRead32(slot + 0x08u, bulletWord)) {
        g_ammo = {};
        return;
    }
    const auto id = static_cast<uint16_t>(head >> 16);
    const auto num = static_cast<uint16_t>(head & 0xFFFFu);
    const auto loaded = static_cast<uint16_t>((bulletWord >> 16) & 0x1FFFu);
    // m_wep_id is not compared with the slot's id: with a stock attached it holds the combined id
    // (cItemMgr::weaponId, e.g. TMP + stock), so the check left a stocked weapon losing rounds.
    if ((flagsWord & 0x01000000u) == 0) {
        g_ammo = {};
        return;
    }
    // A weapon with a magazine is kept full, so it fires with no spare ammo in the case (only putting
    // back what a shot took left a weapon that came empty, bought or picked up, unable to fire).
    // Grenades are kept above (KeepGrenades).
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
    // Leon's weapons only: Ada's game (Separate Ways) has its own weapon set. A Play As stand-in in
    // Leon's story keeps them: its weapons fall back to Leon's files (PlayAs_ReadWepData), as for the
    // story merchant's other Leon weapons. 1.1 took them away from Play As Ada (r100-r106, 2026-11-21).
    if ((plWord >> 24) != 0 && StandInPlaying() == 0) {
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



// Auto QTE: Cockpit's ActionButton (0x80329528 + 0xBD on Wii, the GC's Cckpt.m_ActBttn) keeps
// m_disp_flag, the button of the prompt cActionButton::move (0x8007D41C) showed this frame; move
// clears it first, so it is 0 without a prompt. With the Classic Controller (controller mode
// 0x8043E1FC == 2) the prompts are the GC's DISP_FLAG (act_btn.h): the Wii's ActBtn.set wrapper
// 0x8007DEF4 picks the Wii Remote's kind (17..19, motions) only in mode 1. Wii checkButton 0x8007D838
// (jump table 0x802CC63C) and the rooms read Key.trg, built through Key_type_tbl 0x802D8500: Classic
// A 0x80000, B 0x40000, Y 0x10000, L 0x400000, R 0x800000 (Windows Key dumps of Classic L and R).
//   2 A rapid, 11 gacha, 14 A accent: A.   3 L+R (dodges).   4 A+B.   5 B, 13 B rapid: B.   7 Y.
//   9 L, 10 R: the castle's pillar run (r226, the room checks Key.trg L or R itself, EXACT_KEY).
// The gacha (shake off a grab, PlGachaMove 0x80136E34) counts each trigger of A/B, of a stick
// direction and of L/R once per frame, so it gets the stick flicked left and right too. Left to the
// player, as in CubeEvil4 (qte_touch.cpp): 1 plain A (doors, ladders), 6 X (Ashley: hide, a choice)
// and the stick prompts 12/15/16 (the crane r30b and the jet ski r333 are steered, not timed).
// Pressed and let go every 40 ms while the prompt is up: a fresh press for the dodges, about twelve
// a second for the mashes. Only in play (pG->Rno0 3, no menu or sub screen): START during a mash
// stops move with the last prompt still in the byte, and A would answer the pause menu (CubeEvil4
// user report 08.11.2026, the Retry question).
constexpr uint32_t kActBtnDispWord = 0x803295E4u;  // m_disp_flag is its second byte
constexpr uint64_t kQtePulseNs = 40'000'000;

uint32_t QteButtonsFor(uint8_t disp) {
    switch (disp) {
    case 2:
    case 14:
        return kQteA;
    case 11:
        return kQteA | kQteStickL;  // the stick alternates (AutoQteButtons)
    case 3:
        return kQteL | kQteR;
    case 4:
        return kQteA | kQteB;
    case 5:
    case 13:
        return kQteB;
    case 7:
        return kQteY;
    case 9:
        return kQteL;
    case 10:
        return kQteR;
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
    uint32_t word = 0, pG = 0, rno = 0;
    if (!Memory::TryRead32(kActBtnDispWord, word) || !Memory::TryRead32(kPGAddress, pG) || !GuestPointer(pG) ||
        !Memory::TryRead32(pG + 0x20u, rno) || (rno >> 24) != 3u || GameTweaks::MenuScreen()) {
        return 0;
    }
    const uint8_t disp = static_cast<uint8_t>(word >> 16);
    uint32_t buttons = QteButtonsFor(disp);
    // One line per prompt kind, answered or not (a report then names a prompt left to the player).
    static std::atomic<uint32_t> s_logged{0};
    if (disp != 0 && disp < 32 && (s_logged.fetch_or(1u << disp, std::memory_order_relaxed) & (1u << disp)) == 0) {
        RT_LOGF(RT_TAG_OS, "cheats: auto QTE prompt %u%s\n", disp, buttons != 0 ? "" : " (left to the player)");
    }
    const uint64_t phase = nowNs / kQtePulseNs;
    if ((buttons & kQteStickL) != 0 && phase % 4u >= 2u) {
        buttons ^= kQteStickL | kQteStickR;  // left, right, left: a stick trigger at each change
    }
    if (phase % 2u != 0) {
        buttons &= kQteStickL | kQteStickR;  // the buttons let go, the stick keeps flicking
    }
    return buttons;
}

uint32_t QtePromptButtons() {
    uint32_t word = 0, pG = 0, rno = 0;
    if (!Memory::TryRead32(kActBtnDispWord, word) || !Memory::TryRead32(kPGAddress, pG) || !GuestPointer(pG) ||
        !Memory::TryRead32(pG + 0x20u, rno) || (rno >> 24) != 3u || GameTweaks::MenuScreen()) {
        return 0;
    }
    return QteButtonsFor(static_cast<uint8_t>(word >> 16));
}

bool Available() {
    return !NetplaySession::Active();
}

int PlayAs() {
    const int v = g_playAs.load(std::memory_order_relaxed);
    return v >= 2 && v <= 5 ? v : 0;
}
void SetPlayAs(int who) { g_playAs = who >= 2 && who <= 5 ? who : 0; }
bool InfiniteHealth() { return g_infiniteHealth.load(std::memory_order_relaxed); }
void SetInfiniteHealth(bool on) { g_infiniteHealth = on; }
// Windows tests: RE4_INFINITE_HEALTH turns it on at the first Apply.
void InfiniteHealthFromEnv() {
    static const bool s_env = EnvOn("RE4_INFINITE_HEALTH");
    static bool s_done = false;
    if (s_env && !s_done) {
        s_done = true;
        g_infiniteHealth = true;
    }
}
bool InfiniteAmmo() { return g_infiniteAmmo.load(std::memory_order_relaxed); }
void SetInfiniteAmmo(bool on) { g_infiniteAmmo = on; }
void SetItemStack(bool on) { g_itemStack = on; }

void SetFromLauncher(bool unlockAll, bool infiniteHealth, bool infiniteAmmo, bool specialWeapons, bool maxMoney,
                     bool autoAim, bool autoQte, bool itemStack, int playAs, bool biggestCase,
                     bool oneShotKills, bool steadyAim) {
    g_itemStack = itemStack;
    g_playAs = playAs >= 2 && playAs <= 5 ? playAs : 0;
    g_biggestCase = biggestCase;
    g_oneShotKills = oneShotKills;
    g_steadyAim = steadyAim;
    g_autoAim = autoAim;
    g_autoQte = autoQte;
    g_unlockAll = unlockAll;
    g_infiniteHealth = infiniteHealth;
    g_infiniteAmmo = infiniteAmmo;
    g_specialWeapons = specialWeapons;
    g_maxMoney = maxMoney;
}

void PlayAsSwitchNow();   // below, with Play As
void PlayAsEventWatch();  // below, after the event start wrapper
void CutsceneReplayTick();  // below, with Play As
void StandInTick();         // below, with the Ganados' moves

void Apply() {
    if (!Available()) {
        return;
    }
    PlayAsSwitchNow();
    PlayAsEventWatch();
    CutsceneReplayTick();
    StandInTick();
    // Speedrun Mode: every cheat off (special weapons and auto aim cleared below as when turned off).
    const bool allowed = !GameTweaks::SpeedrunMode();
    InfiniteHealthFromEnv();
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
    // Steady Aim in the rifle scope: the scope camera (CameraScope::move, 0x800874xx on the Wii) adds a
    // breathing sway whose strength is one constant, rnd_gain2 (0.0005, the only such float in main.dol,
    // read only by 0x8008753C and 0x80087588). Zero while the cheat is on: the sway fades within a second
    // (its smoothing keeps the last strength 95% a frame); back to the game's value when off. CubeEvil4
    // zeroes the same sway (patch 0233). The laser and the shot: SteadyAim_Pointer_8013BDD0.
    {
        static const bool s_envSteady = EnvOn("RE4_STEADY_AIM");
        constexpr uint32_t kScopeSwayGain = 0x8043CDF0u;
        constexpr uint32_t kGameGain = 0x3A03126Fu;  // 0.0005f
        const bool steady = allowed && (s_envSteady || g_steadyAim.load(std::memory_order_relaxed));
        uint32_t now = 0;
        if (Memory::TryRead32(kScopeSwayGain, now) && (now == 0u || now == kGameGain)) {
            const uint32_t want = steady ? 0u : kGameGain;
            if (now != want) Memory::TryWrite32(kScopeSwayGain, want);
        }
    }
    static const bool s_envAutoAim = std::getenv("RE4_AUTO_AIM") != nullptr;  // Windows tests
    const bool autoAim = allowed && (s_envAutoAim || g_autoAim.load(std::memory_order_relaxed));
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

// Item stacks up to 31,999 (the Gecko code "$31,999 max for any item in the inventory"). ItemInfo
// (0x8003376C: item id r3, ITEM_INFO* r4 = {u16 id, u8 type +2, u8 num +3, u16 max +4}) fills the
// item's table row; two of its callers read the stack limit from it:
// - getMaxNum (0x80036CC0, returns to 0x80036CD4), used when two stacks are combined in the case;
// - the pickup add (0x8013FCC8, returns to 0x8013FD08), which fills existing stacks up to the limit.
// Both get 31,999. cItemMgr::combine (0x800350C8, r4/r5 the two cItems) asks getType (0x80036BE8)
// from 0x80035410 whether the item stacks (type 3); two of the same item answer 3 there, so herbs,
// sprays, eggs and grenades stack too (two green herbs stack instead of mixing).
// The translated functions stay; these are registered over them (REGISTER_NATIVE_FUNCTION_AS: callers
// reach them through the dispatch table) and call them.
namespace Cheats {
bool ItemStackOn() {
    static const bool s_env = std::getenv("RE4_ITEM_STACK") != nullptr;  // Windows tests
    return (s_env || g_itemStack.load(std::memory_order_relaxed)) && Available() && !GameTweaks::SpeedrunMode();
}
} // namespace Cheats

extern "C" void func_8003376C(CpuContext* ctx);
extern "C" void func_80036BE8(CpuContext* ctx);
extern "C" void func_800350C8(CpuContext* ctx);

namespace {
constexpr uint16_t kItemStackMax = 31999;
bool g_combineSameItem = false;  // guest thread only
} // namespace

namespace {
// The function that owns the stack frame `sp`, found from its caller's side: its LR was saved at
// back chain + 4, and the word before that return address is the `bl` that entered it. ctx->lr can't
// say: the translator leaves it unset for a callee that never reads it, and ItemInfo is a leaf, so
// getMaxNum and get reach it with a stale lr (Beta 13: the cheat never fired).
uint32_t FrameOwner(uint32_t sp) {
    uint32_t chain = 0, ret = 0, insn = 0;
    if (!Memory::TryRead32(sp, chain) || !Cheats::GuestPointer(chain) || !Memory::TryRead32(chain + 4u, ret) ||
        !Cheats::GuestPointer(ret) || !Memory::TryRead32(ret - 4u, insn) || (insn & 0xFC000003u) != 0x48000001u) {
        return 0;
    }
    const int32_t offset = static_cast<int32_t>((insn & 0x03FFFFFCu) << 6) >> 6;
    return ret - 4u + static_cast<uint32_t>(offset);
}
} // namespace

extern "C" void ItemStack_ItemInfo_8003376C(CpuContext* ctx) {
    const uint32_t info = ctx->gpr[4];
    const uint32_t id = ctx->gpr[3] & 0xFFFFu;
    const uint32_t sp = ctx->gpr[1];  // the caller's frame: ItemInfo is a leaf
    func_8003376C(ctx);
    // getMaxNum (0x80036CC0), used to top up and combine stacks, and cItemMgr::get (0x8013FCC8), the
    // pickup: every item, as the Gecko code does (li r3 / li r29, 0x7CFF).
    if (!Cheats::ItemStackOn() || !Memory::Contains(info + 4u, 2)) return;
    const uint32_t owner = FrameOwner(sp);
    if (owner != 0x80036CC0u && owner != 0x8013FCC8u) return;
    uint16_t max = 0;
    if (!Cheats::Read16(info + 4u, max)) return;
    Memory::Write16(info + 4u, kItemStackMax);
    static bool s_logged[2] = {};
    const bool pickup = owner == 0x8013FCC8u;
    if (!s_logged[pickup]) {
        s_logged[pickup] = true;
        RT_LOGF(RT_TAG_OS, "cheats: item stack limit 31999 (%s, item %04X, was %u)\n",
                pickup ? "pickup" : "stack limit", id, max);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x8003376C, ItemStack_ItemInfo_8003376C, "ItemStack_ItemInfo_8003376C");

extern "C" void ItemStack_ItemType_80036BE8(CpuContext* ctx) {
    const uint32_t lr = ctx->lr;
    func_80036BE8(ctx);
    if (lr == 0x80035410u && g_combineSameItem) {
        ctx->gpr[3] = 3;
        RT_LOGF(RT_TAG_OS, "cheats: item stack combines two of one item\n");
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80036BE8, ItemStack_ItemType_80036BE8, "ItemStack_ItemType_80036BE8");

extern "C" void ItemStack_Combine_800350C8(CpuContext* ctx) {
    const bool saved = g_combineSameItem;
    const uint32_t a = ctx->gpr[4];
    const uint32_t b = ctx->gpr[5];
    uint16_t idA = 0, idB = 0;
    g_combineSameItem = a != b && Cheats::ItemStackOn() && Cheats::Read16(a, idA) && Cheats::Read16(b, idB) &&
                        idA == idB;
    // r4 is the target, r5 the item put on it. A stacked target (two green herbs) is mixed as if it
    // were one: the dropped herb vanished, the stack lost one and the mixed herb never appeared
    // (drag & drop in the case, Beta 14 tester video). The command menu takes the stack as the item
    // put on the other, which works, so a stacked target with a single item put on it is swapped.
    uint16_t numA = 0, numB = 0;
    if (Cheats::ItemStackOn() && a != b && idA != idB && Cheats::Read16(a + 2u, numA) &&
        Cheats::Read16(b + 2u, numB) && numA > 1 && numB == 1) {
        ctx->gpr[4] = b;
        ctx->gpr[5] = a;
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            RT_LOGF(RT_TAG_OS, "cheats: item stack: combine onto a stack of %u (item %04X) taken the other way\n",
                    numA, idA);
        }
    }
    func_800350C8(ctx);
    g_combineSameItem = saved;
}
REGISTER_NATIVE_FUNCTION_AS(0x800350C8, ItemStack_Combine_800350C8, "ItemStack_Combine_800350C8");


// Play As, Biggest Case and One-Shot Kills: wrappers over translated game functions, as above.
// - roomInit (0x80033604, cItemMgr, every room) then swaps the attache case item (0x7C..0x7E) for the XL
//   one (0x7F) with dump (0x80034D98) and get (0x8003443C), as the new-game setups do. Wesker's silencer
//   (item 0x3F) comes with Play As (kPlayAsKit, Cheats_ItemGameInit_80033498), not as a cheat of its own.
// - The damage function (0x8009FAFC, r3 the model hit; an enemy's id at +0x100 is 0x10..0x4F, its life
//   s16 at +0x322) clamps the damage to the life left. The Gecko code made it take all of it; here the
//   enemy's life is set to 1 before the hit, so any hit kills through the game's own death path.
extern "C" void func_80141160(CpuContext* ctx);
extern "C" void func_8014153C(CpuContext* ctx);
extern "C" void func_80136BA0(CpuContext* ctx);
extern "C" void func_80033604(CpuContext* ctx);
extern "C" void func_80034D98(CpuContext* ctx);
extern "C" void func_8003443C(CpuContext* ctx);
extern "C" void func_8013FCC8(CpuContext* ctx);
extern "C" void func_8009FAFC(CpuContext* ctx);

namespace {
bool CheatsAllowed() { return Cheats::Available() && !GameTweaks::SpeedrunMode(); }

constexpr uint32_t kItemMgr = 0x8031F600u;

// The first alive slot of the current inventory type holding `id` (its address, 0 when none; *index its
// slot number).
uint32_t FindItem(uint16_t id, uint32_t* index = nullptr) {
    uint32_t items = 0, count = 0, charWord = 0;
    if (!Memory::TryRead32(kItemMgr + 0x14u, items) || !Cheats::GuestPointer(items) ||
        !Memory::TryRead32(kItemMgr + 0x1Cu, count) || count > 0x400u || !Memory::TryRead32(kItemMgr + 0x10u, charWord)) {
        return 0;
    }
    const auto chr = static_cast<uint8_t>(charWord & 0xFFu);
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t at = 0, flagsType = 0;
        if (!Cheats::Read16(items + i * 0x0Eu, at) || !Cheats::Read16(items + i * 0x0Eu + 4u, flagsType)) return 0;
        if (at == id && (flagsType & 0x0100u) != 0 && (flagsType & 0xFFu) == chr) {
            if (index) *index = i;
            return items + i * 0x0Eu;
        }
    }
    return 0;
}
bool HasItem(uint16_t id) { return FindItem(id) != 0; }

// The case's size for the pickup add's grid: 0 S (10x6), 1 M, 2 L, 3 XL (15x8), from the case item.
uint32_t CaseSize() {
    for (uint32_t i = 0; i < 4; ++i) {
        if (HasItem(static_cast<uint16_t>(0x7F - i))) return 3 - i;
    }
    return 0;
}

uint8_t PlType() {
    uint32_t pG = 0, word = 0;
    if (!Memory::TryRead32(Cheats::kPGAddress, pG) || !Cheats::GuestPointer(pG) ||
        !Memory::TryRead32(pG + Cheats::kPlTypeOffset, word)) {
        return 0xFF;
    }
    return static_cast<uint8_t>(word >> 24);
}

void CallItemMgr(CpuContext* ctx, void (*fn)(CpuContext*), uint32_t id, uint32_t num) {
    ctx->gpr[3] = kItemMgr;
    ctx->gpr[4] = id;
    ctx->gpr[5] = num;
    fn(ctx);
}
} // namespace



// Play As kit: every character plays from Leon's case (roomInit 0x80033604 sets m_char 0 for any pl_type
// but Ashley's). A switch to a stand-in during the story keeps the case as it is and adds the stand-in's
// own weapons (those of its Mercenaries start set, set_char 0x80032A58) with some of their ammo through the
// pickup add (0x8013FCC8), each one as soon as the case has room for it. A New Game started with Play As
// on gets the stand-in's whole start set instead of Leon's (Cheats_ItemGameInit_80033498). Items of the
// earlier per-character cases (cItem::type 2..5, Beta 14 tests) go back to Leon's set.
namespace {
int StandInPlaying();  // below, with Play As
bool InExtraGame();     // below: true in The Mercenaries, Separate Ways, Assignment Ada (or no pG)
constexpr uint32_t kItemSlotSize = 0x0Eu;

struct PlayAsWeapon {
    uint16_t weapon;
    uint16_t ammo;  // 0xFFFF: none
    uint16_t ammoNum;
};
// By pl_type - 2: the weapons of set_char's start sets, the ammo as there (or a few of its kind).
// Wesker also gets his handgun's silencer (item 0x3F), fitted in the case (his weapon file has the model).
constexpr uint32_t kKitSize = 4;
constexpr PlayAsWeapon kPlayAsKit[4][kKitSize] = {
    {{0x21, 0x04, 30}, {0x30, 0x20, 100}, {0x2F, 0x07, 5}, {0xFFFF, 0xFFFF, 0}},       // Ada: Punisher, TMP, rifle
    {{0x3E, 0x20, 50}, {0xFFFF, 0xFFFF, 0}, {0xFFFF, 0xFFFF, 0}, {0xFFFF, 0xFFFF, 0}},  // HUNK: TMP
    {{0x52, 0x72, 20}, {0xFFFF, 0xFFFF, 0}, {0xFFFF, 0xFFFF, 0}, {0xFFFF, 0xFFFF, 0}},  // Krauser: bow, arrows
    {{0x23, 0x04, 30}, {0x2A, 0x00, 10}, {0x2F, 0x07, 5}, {0x3F, 0xFFFF, 0}},          // Wesker: + silencer
};
constexpr uint16_t kSilencerItem = 0x3F;
int g_kitWho = 0;         // the stand-in whose weapons are being added
uint32_t g_kitLeft = 0;   // bit i: kPlayAsKit[g_kitWho - 2][i] still waits for room in the case
constexpr uint16_t kBowItem = 0x52, kArrowItem = 0x72;

extern "C" void func_800357B8(CpuContext* ctx);  // cItemMgr::arm(r4 item, 0 = bare hands)

void ArmBareHands(CpuContext* ctx) {
    const uint32_t r3 = ctx->gpr[3], r4 = ctx->gpr[4];
    ctx->gpr[3] = kItemMgr;
    ctx->gpr[4] = 0;
    func_800357B8(ctx);
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
}

// Only Krauser has a file for his bow (weapon 0x1C): anyone else who held it read an empty entry and
// crashed (Wesker after Krauser, 2026-11-18). The bow and its arrows leave the case while another plays.
void StripBow(CpuContext* ctx) {
    int gone = 0;
    for (const uint16_t id : {kBowItem, kArrowItem}) {
        for (int n = 0; n < 0x400 && HasItem(id); ++n) {
            uint32_t wep = 0;
            if (Memory::TryRead32(kItemMgr + 0x0Cu, wep) && wep == FindItem(id)) ArmBareHands(ctx);
            CallItemMgr(ctx, func_80034D98, id, 0);  // dump: one off the count, the slot erased at 0
            ++gone;
        }
    }
    if (gone != 0) RT_LOGF(RT_TAG_OS, "cheats: play as: Krauser's bow and arrows out of the case (%d)\n", gone);
}

// The pickup add of `num` of item `id` into the case's grid; true when it went in.
bool PutInCase(CpuContext* ctx, uint16_t id, uint16_t num) {
    ctx->gpr[3] = id;
    ctx->gpr[4] = num;
    ctx->gpr[5] = CaseSize();  // the pickup's grid must match the case (0 S .. 3 XL)
    func_8013FCC8(ctx);
    return HasItem(id);
}

void PlayAsKitRoom(CpuContext* ctx) {
    uint32_t items = 0, count = 0;
    // The extra games keep their own sets: StandInPlaying() is 0 there, so the bow strip below took
    // Krauser's bow and arrows from him in The Mercenaries (TECNO LJ8k, 1.0, room 404).
    if (InExtraGame()) return;
    if (!Memory::TryRead32(kItemMgr + 0x14u, items) || !Cheats::GuestPointer(items) ||
        !Memory::TryRead32(kItemMgr + 0x1Cu, count) || count > 0x400u) {
        return;
    }
    int moved = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t it = items + i * kItemSlotSize;
        const uint8_t type = Memory::Read8(it + 5u);
        if ((Memory::Read8(it + 4u) & 1u) != 0 && type >= 2 && type <= 5) {
            Memory::Write8(it + 5u, 0);
            ++moved;
        }
    }
    if (moved != 0) RT_LOGF(RT_TAG_OS, "cheats: play as: %d items of the old per-character cases back in Leon's\n", moved);
    if (Memory::Read8(kItemMgr + 0x13u) == 1) return;  // Ashley's part: no switch
    static int s_lastWho = -1;
    const int who = StandInPlaying();
    if (who != 4) StripBow(ctx);
    if (who != s_lastWho) {
        s_lastWho = who;
        g_kitWho = who >= 2 ? who : 0;
        g_kitLeft = 0;
        if (g_kitWho != 0) {
            for (uint32_t i = 0; i < kKitSize; ++i) {
                const uint16_t id = kPlayAsKit[who - 2][i].weapon;
                if (id != 0xFFFF && !HasItem(id)) g_kitLeft |= 1u << i;  // already held: left alone
            }
        }
    }
    if (g_kitWho < 2 || g_kitWho != who || g_kitLeft == 0) return;
    if (CaseSize() + HasItem(0x7C) == 0) return;  // no case yet
    for (uint32_t i = 0; i < kKitSize; ++i) {
        if ((g_kitLeft & (1u << i)) == 0) continue;
        const PlayAsWeapon& w = kPlayAsKit[who - 2][i];
        if (HasItem(w.weapon)) {
            g_kitLeft &= ~(1u << i);
            continue;
        }
        if (!PutInCase(ctx, w.weapon, 1)) continue;  // no room yet: tried again in the next room
        g_kitLeft &= ~(1u << i);
        const bool ammo = w.ammo == 0xFFFF || PutInCase(ctx, w.ammo, w.ammoNum);
        RT_LOGF(RT_TAG_OS, "cheats: play as: character %d's weapon %X put in the case (ammo %s)\n", who, w.weapon,
                ammo ? "too" : "no room");
    }
}
} // namespace

extern "C" void Cheats_ItemRoomInit_80033604(CpuContext* ctx) {
    func_80033604(ctx);
    PlayAsKitRoom(ctx);
#if !defined(__ANDROID__)
    // RE4_TEST_ARM=<item hex> (Windows tests): that weapon (plus rifle rounds 0x07) goes in the case at
    // every room start; Cheats::TestArmTick puts it in Leon's hands once play runs.
    if (const char* e = std::getenv("RE4_TEST_ARM"); e != nullptr && *e != '\0') {
        const auto id = static_cast<uint16_t>(std::strtoul(e, nullptr, 16));  // 0: no weapon, the log only
        const uint32_t r3 = ctx->gpr[3], r4 = ctx->gpr[4], r5 = ctx->gpr[5];
        if (id != 0 && !HasItem(id)) CallItemMgr(ctx, func_8003443C, id, 1);
        if (!HasItem(0x07)) CallItemMgr(ctx, func_8003443C, 0x07, 30);
        RT_LOGF(RT_TAG_OS, "cheats: test arm %X (%s)\n", id, FindItem(id) != 0 ? "in the case" : "missing");
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        ctx->gpr[5] = r5;
    }
#endif
    if (!CheatsAllowed()) return;
    static const bool s_envCase = Cheats::EnvOn("RE4_BIGGEST_CASE");
    const bool biggest = s_envCase || Cheats::g_biggestCase.load(std::memory_order_relaxed);
    const uint32_t r3 = ctx->gpr[3];
    // Repair (Beta 14 testers' saves): the first silencer was fitted to Leon's handgun 0x23 (part lv 1,
    // bullet = the gun's slot, weapon id 0x24), but Leon's weapon file has no silenced handgun model
    // (only Wesker's own file does), so the handgun vanished from the case. Detach it again.
    if (PlType() == 0 && Cheats::PlayAs() != 5) {  // Wesker's start set fits it (his own file has the model)
        const uint32_t part = FindItem(0x3F);
        uint16_t lv = 0, slot = 0, atId = 0;
        uint32_t items = 0;
        if (part != 0 && Cheats::Read16(part + 0x06u, lv) && lv == 1 && Cheats::Read16(part + 0x08u, slot) &&
            Memory::TryRead32(kItemMgr + 0x14u, items) && Cheats::GuestPointer(items) &&
            Cheats::Read16(items + slot * 0x0Eu, atId) && atId == 0x23) {
            Memory::Write16(part + 0x06u, 0);
            Memory::Write16(part + 0x08u, 0xFFFF);
            uint16_t wepId = 0;
            if (Cheats::Read16(kItemMgr + 0x10u, wepId) && wepId == 0x24) Memory::Write16(kItemMgr + 0x10u, 0x23);
            RT_LOGF(RT_TAG_OS, "cheats: silencer taken off Leon's handgun (slot %u)\n", slot);
        }
    }
    if (!biggest) return;
    if (biggest && !HasItem(0x7F) && (HasItem(0x7C) || HasItem(0x7D) || HasItem(0x7E))) {
        for (uint32_t id = 0x7C; id <= 0x7E; ++id) {
            if (HasItem(static_cast<uint16_t>(id))) CallItemMgr(ctx, func_80034D98, id, 0);
        }
        CallItemMgr(ctx, func_8003443C, 0x7F, 0);
        RT_LOGF(RT_TAG_OS, "cheats: attache case XL (now %s)\n", HasItem(0x7F) ? "in the case" : "missing");
    }
    ctx->gpr[3] = r3;
}
REGISTER_NATIVE_FUNCTION_AS(0x80033604, Cheats_ItemRoomInit_80033604, "Cheats_ItemRoomInit_80033604");

#if !defined(__ANDROID__)
extern "C" void func_80138F78(CpuContext* ctx);  // cPlayer::weaponLoad(r4 weapon no, r5 type)
extern "C" void func_80138F94(CpuContext* ctx);  // cPlayer::weaponInit
namespace Cheats {
// RE4_TEST_ARM, second half: 3 s into play, the equip the GC's r22c weapon switch does (weaponLoad,
// weaponInit, ItemMgr.arm), once a room, for any item in wep_info (rifle 0x2E, semi-auto 0x2F, handgun 0x23, shotgun 0x2C);
// 0 arms nothing and only logs.
void TestArmTick(CpuContext* ctx) {
    static const char* s_env = std::getenv("RE4_TEST_ARM");
    if (s_env == nullptr || *s_env == '\0') return;
    static uint16_t s_room = 0;
    static int s_frames = 0;
    const uint16_t room = GameTweaks::CurrentRoom();
    if (room != s_room) {
        s_room = room;
        s_frames = 0;
    }
    if (room == 0) return;
    ++s_frames;
    if (s_frames > 90 && s_frames % 30 == 0 && s_frames < 1800) {  // the scope camera and the player's routine
        uint32_t pG = 0, st0 = 0, pl = 0, rno = 0, rep = 0;
        if (Memory::TryRead32(kPGAddress, pG) && GuestPointer(pG) && Memory::TryRead32(pG + 0x501Cu, st0) &&
            Memory::TryRead32(0x8043DEA0u, pl) && GuestPointer(pl) && Memory::TryRead32(pl + 0xFCu, rno)) {
            Memory::TryRead32(0x8043E274u, rep);  // PlWepLockCtrl's repCtr (game_tweaks.cpp, mouse aim)
            float repF = 0.0f;
            std::memcpy(&repF, &rep, sizeof(repF));
            RT_LOGF(RT_TAG_OS,
                    "cheats: test arm: frame %d scope camera %d look through %d, player r_no %08X, aim repCtr %.0f\n",
                    s_frames, (st0 & 0x40u) ? 1 : 0, (st0 & (0x80000000u >> 16)) ? 1 : 0, rno, repF);
        }
    }
    if (s_frames != 90) return;
    const unsigned long armEnv = std::strtoul(s_env, nullptr, 16);
    if (armEnv == 0) return;
    // Item -> weapon number from the Wii's wep_info (0x802AF910, 43 rows x 22 bytes: item id u16, weapon no).
    const auto armId = static_cast<uint16_t>(armEnv);
    uint8_t wepNo = 0xFF;
    for (uint32_t row = 0; row < 43u && wepNo == 0xFF; ++row) {
        uint16_t rowId = 0;
        uint16_t no = 0;
        if (Read16(0x802AF910u + row * 22u, rowId) && rowId == armId && Read16(0x802AF910u + row * 22u + 2u, no)) {
            wepNo = static_cast<uint8_t>(no);
        }
    }
    const uint32_t item = FindItem(armId);
    uint32_t pl = 0;
    if (wepNo == 0xFF || item == 0 || !Memory::TryRead32(0x8043DEA0u, pl) || !GuestPointer(pl)) return;
    const CpuContext saved = *ctx;
    ctx->gpr[3] = pl;
    ctx->gpr[4] = wepNo;
    ctx->gpr[5] = 0;
    func_80138F78(ctx);
    ctx->gpr[3] = pl;
    func_80138F94(ctx);
    ctx->gpr[3] = kItemMgr;
    ctx->gpr[4] = item;
    func_800357B8(ctx);
    uint16_t bullet = 0;
    if (Read16(item + 0x08u, bullet)) Memory::Write16(item + 0x08u, (bullet & 0xE000u) | 1u);  // 1 round: reload after a shot
    *ctx = saved;
    RT_LOGF(RT_TAG_OS, "cheats: test arm: item %X in the player's hands (room %X)\n", armId, room);
}
} // namespace Cheats
#endif

extern "C" void OneShot_Damage_8009FAFC(CpuContext* ctx) {
    static const bool s_env = Cheats::EnvOn("RE4_ONE_SHOT");
    const uint32_t model = ctx->gpr[3];
    if ((s_env || Cheats::g_oneShotKills.load(std::memory_order_relaxed)) && CheatsAllowed() &&
        Cheats::GuestPointer(model) && Memory::Contains(model + 0x322u, 2)) {
        uint32_t idWord = 0;
        uint16_t life = 0;
        if (Memory::TryRead32(model + 0x100u, idWord) && Cheats::Read16(model + 0x322u, life)) {
            const uint32_t id = idWord >> 24;
            if (id >= 0x10u && id < 0x50u && static_cast<int16_t>(life) > 1) Memory::Write16(model + 0x322u, 1);
        }
    }
    func_8009FAFC(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009FAFC, OneShot_Damage_8009FAFC, "OneShot_Damage_8009FAFC");

// Steady Aim: 0x8013BDD0 updates the aim pointer. With r4 bit 0 set it stores the caller's position as
// the pointer (r31+40/44) and returns; otherwise it writes the stored pointer plus a random shake
// (0x80141C30, x and y) back to the caller's position. 1.0 forced bit 0 on every call (the Gecko code
// 0413BE20 60000000 does the same), so callers that read the pointer got their own input back: the
// laser looked right but the shot's ray missed small targets such as the blue medallions (1.0 report).
// Now every caller keeps its mode and only the shake is zeroed while this function runs.
namespace {
bool g_steadyAimActive = false;  // the guest thread only
}
extern "C" void func_8013BDD0(CpuContext* ctx);
extern "C" void SteadyAim_Pointer_8013BDD0(CpuContext* ctx) {
    static const bool s_env = Cheats::EnvOn("RE4_STEADY_AIM");
    const bool on = (s_env || Cheats::g_steadyAim.load(std::memory_order_relaxed)) && CheatsAllowed();
    const bool outer = g_steadyAimActive;
    g_steadyAimActive = on;
    func_8013BDD0(ctx);
    g_steadyAimActive = outer;
}
REGISTER_NATIVE_FUNCTION_AS(0x8013BDD0, SteadyAim_Pointer_8013BDD0, "SteadyAim_Pointer_8013BDD0");

// The shake's random number (a float in f1); called elsewhere too, so zeroed only inside 0x8013BDD0.
// It still runs, so the game's random sequence stays the same as without the cheat.
extern "C" void func_80141C30(CpuContext* ctx);
extern "C" void SteadyAim_Shake_80141C30(CpuContext* ctx) {
    func_80141C30(ctx);
    if (g_steadyAimActive) ctx->fpr[1].d = 0.0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80141C30, SteadyAim_Shake_80141C30, "SteadyAim_Shake_80141C30");

// Auto Aim lock. The game's lock-on (CFG_LOCK_ON, KeepAutoAim) picks its target in SearchLockEm
// (0x8013ABFC, r3 = from, r4 = the target to pass over) and turns the aim to it for 10 frames
// (lockMove 0x8013A448, r3 = cPlWep: m_LockTime u8 +0x40, PlWepAutoTrack 0x8013BF3C) at the enemy's lock
// point (lockOfs Vec +0x384 in the matrix of parts lockParts +0x390, low 3 bits). Its filter drops an
// enemy that is down or stunned (lock-off / inactive status), so the aim left it or went to another.
// With the cheat on: the target is the live enemy nearest the aim (down and stunned ones too), kept for
// the whole aim and replaced when it dies, at the game's own lock point (the body). Holding the aim
// stick for half a second cancels the lock until the next aim.
extern "C" void func_80119CC4(CpuContext* ctx);  // cModel::getPartsPtr(r3 model, r4 index)
extern "C" void func_800832B0(CpuContext* ctx);  // EatMgr hitCheck
namespace {
constexpr uint32_t kPPLAddr = 0x8043DEA0u;     // pPL (r13 - 0x6B80)
constexpr uint32_t kEmArrayPtr = 0x8032E274u;  // EmMgr pArray
constexpr uint32_t kEmCount = 60u, kEmStride = 0xE04u;
constexpr uint32_t kPlLockEm = 0x52Cu;         // cPlayer m_pEm
constexpr uint32_t kLockOfs = 0x384u, kLockParts = 0x390u;  // cEm lockOfs Vec, lockParts u8 (low 3 bits)
constexpr uint32_t kCampos = 0x118u;           // pG->Camera Campos

bool AutoAimLockOn() {
    static const bool s_env = Cheats::EnvOn("RE4_AUTO_AIM");
    return (s_env || Cheats::g_autoAim.load(std::memory_order_relaxed)) && CheatsAllowed();
}
float ReadF(uint32_t a, bool& ok) {
    uint32_t bits = 0;
    ok = ok && Memory::TryRead32(a, bits);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
void WriteF(uint32_t a, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    Memory::TryWrite32(a, bits);
}
float WrapAngle(float a) {
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}
// The world position of a parts: its matrix (cCoord +0x0C, 3x4) translation.
bool PartsPos(uint32_t parts, float out[3]) {
    bool ok = true;
    out[0] = ReadF(parts + 0x18u, ok);
    out[1] = ReadF(parts + 0x28u, ok);
    out[2] = ReadF(parts + 0x38u, ok);
    return ok && std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}
// Animals and traps the lock passes over: the village dog 0x21, crow 0x23, snake 0x24, cow 0x26, lake
// fish 0x27, chicken 0x28, traps 0x2A. The Colmillos (0x22) and the bats (0x29) stay lockable.
bool NotATarget(uint32_t id) {
    return id == 0x21u || id == 0x23u || id == 0x24u || id == 0x26u || id == 0x27u || id == 0x28u || id == 0x2Au;
}
// A live enemy (in use, id 0x10..0x4F but no animal, life above 0): its highest parts.
bool EnemyHead(uint32_t em, float head[3]) {
    uint32_t flags = 0, idWord = 0, parts = 0;
    uint16_t life = 0;
    if (!Cheats::GuestPointer(em) || !Memory::TryRead32(em, flags) || (flags & 1u) == 0 ||
        !Memory::TryRead32(em + 0x100u, idWord) || (idWord >> 24) < 0x10u || (idWord >> 24) >= 0x50u ||
        NotATarget(idWord >> 24) ||
        !Cheats::Read16(em + 0x322u, life) || static_cast<int16_t>(life) <= 0 ||
        !Memory::TryRead32(em + 0xF4u, parts)) {
        return false;
    }
    bool found = false;
    for (int n = 0; n < 64 && Cheats::GuestPointer(parts); ++n) {
        float p[3];
        if (PartsPos(parts, p) && (!found || p[1] > head[1])) {
            std::memcpy(head, p, sizeof(p));
            found = true;
        }
        if (!Memory::TryRead32(parts + 0xF4u, parts)) break;
    }
    return found;
}
// The parts `index` of `model`, through the game's getPartsPtr; the context is left as it was.
uint32_t PartsPtr(CpuContext* ctx, uint32_t model, uint32_t index) {
    const CpuContext saved = *ctx;
    ctx->gpr[3] = model;
    ctx->gpr[4] = index;
    func_80119CC4(ctx);
    const uint32_t parts = ctx->gpr[3];
    *ctx = saved;
    return Cheats::GuestPointer(parts) ? parts : 0u;
}
// The enemy's lock point in the world: the matrix of its lock parts applied to lockOfs.
bool LockPoint(CpuContext* ctx, uint32_t em, float out[3]) {
    uint32_t lockWord = 0;
    if (!Memory::TryRead32(em + kLockParts, lockWord)) return false;
    const uint32_t parts = PartsPtr(ctx, em, (lockWord >> 24) & 7u);
    if (parts == 0) return false;
    bool ok = true;
    float m[12], ofs[3];
    for (uint32_t i = 0; i < 12 && ok; ++i) m[i] = ReadF(parts + 0x0Cu + 4u * i, ok);
    for (uint32_t i = 0; i < 3 && ok; ++i) ofs[i] = ReadF(em + kLockOfs + 4u * i, ok);
    if (!ok) return false;
    for (uint32_t r = 0; r < 3; ++r) out[r] = m[r * 4] * ofs[0] + m[r * 4 + 1] * ofs[1] + m[r * 4 + 2] * ofs[2] + m[r * 4 + 3];
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}
// A wall between the guest Vec at `fromAddr` and `to`: the game's own lock check (lockEmCk 0x8013A4C4)
// asks EatMgr.hitCheck (0x800832B0, r3 &EatMgr 0x80328E98, r4 from, r5 to, r8 0x800) the same.
bool WallBetween(CpuContext* ctx, uint32_t fromAddr, const float to[3]) {
    const CpuContext saved = *ctx;
    const uint32_t sp = saved.gpr[1] - 0x40u, vec = sp + 0x10u;
    for (uint32_t i = 0; i < 3; ++i) WriteF(vec + 4u * i, to[i]);
    ctx->gpr[1] = sp;
    ctx->gpr[3] = 0x80328E98u;
    ctx->gpr[4] = fromAddr;
    ctx->gpr[5] = vec;
    ctx->gpr[6] = 0;
    ctx->gpr[7] = 0;
    ctx->gpr[8] = 0x800u;
    ctx->gpr[9] = 0;
    func_800832B0(ctx);
    const bool hit = ctx->gpr[3] != 0;
    *ctx = saved;
    return hit;
}
// The enemy to lock from the guest Vec at `fromAddr`: within 20 m and 100 degrees of the player's
// facing, its lock point not behind a wall, scored by its distance plus a penalty for the angle off
// the facing. 0 when none.
uint32_t PickLockTarget(CpuContext* ctx, uint32_t fromAddr) {
    bool okFrom = true;
    const float from[3] = {ReadF(fromAddr, okFrom), ReadF(fromAddr + 4u, okFrom), ReadF(fromAddr + 8u, okFrom)};
    if (!okFrom) return 0;
    uint32_t arr = 0, pl = 0;
    bool ok = Memory::TryRead32(kEmArrayPtr, arr) && Cheats::GuestPointer(arr) &&
              Memory::TryRead32(kPPLAddr, pl) && Cheats::GuestPointer(pl);
    const float yaw = ok ? ReadF(pl + 0xA4u, ok) : 0.0f;
    if (!ok) return 0;
    uint32_t best = 0;
    float bestScore = 1e30f;
    for (uint32_t i = 0; i < kEmCount; ++i) {
        const uint32_t em = arr + i * kEmStride;
        float head[3];
        if (!EnemyHead(em, head)) continue;
        const float dx = head[0] - from[0], dz = head[2] - from[2];
        const float flat = std::sqrt(dx * dx + dz * dz);
        const float off = std::fabs(WrapAngle(std::atan2(dx, dz) - yaw));
        if (flat > 20000.0f || off > 1.745f) continue;
        const float score = flat + off * 6000.0f;
        float point[3];
        if (score < bestScore && LockPoint(ctx, em, point) && !WallBetween(ctx, fromAddr, point)) {
            bestScore = score;
            best = em;
        }
    }
    return best;
}
bool g_lockCancelled = false;  // the aim stick dropped the lock in this aim
constexpr uint32_t kJoyOnAddr = 0x803324E0u;     // Joy[0].on
constexpr uint32_t kJoyMainStick = 0x000F0000u;  // JOY_SRIGHT..JOY_SUP (main stick >= 30 units)
constexpr uint32_t kStickCancelFrames = 15u;     // 0.5 s at 30 fps
uint32_t g_stickFrames = 0;                      // frames in a row the main stick is pushed
} // namespace

// Play As HUNK: the rifle scope (CameraScope ctor 0x80086D10), the binoculars (CameraBinocular ctor
// 0x80087D40) and the look down on an enemy (StartLookDownEm 0x8001E474) put the eye at the middle of the player's parts 0x20 and 0x21 (world +0x70), Leon's eyes. On
// HUNK's skeleton those are his chest and his root, so his scope looked from the hips (1.1 report, Mali-G610,
// screenshot 20-39-55: the reticle at Ashley's waist; CubeEvil4 measured 1132 and -61 above the feet against
// Leon's ~1640). For those two callers both parts become one point: where Leon's eyes sit in the head part 4
// (0, 76, 85 in its matrix, CubeEvil4's measurement; parts 0..25 are the same bones on both skeletons).
// getPartsPtr is a leaf the translator calls without setting lr, so the caller is read from its frame.
extern "C" void PlayAs_GetPartsPtr_80119CC4(CpuContext* ctx) {
    const uint32_t model = ctx->gpr[3], index = ctx->gpr[4], sp = ctx->gpr[1];
    func_80119CC4(ctx);
    if ((index != 0x20u && index != 0x21u) || StandInPlaying() != 3) return;
    const uint32_t owner = FrameOwner(sp);
    uint32_t pl = 0;
    const int camera = owner == 0x80086D10u ? 0 : owner == 0x80087D40u ? 1 : owner == 0x8001E474u ? 2 : -1;
    if (camera < 0 || !Memory::TryRead32(kPPLAddr, pl) || model != pl) return;
    const uint32_t head = PartsPtr(ctx, model, 4u);
    if (head == 0) return;
    bool ok = true;
    float m[12];
    for (uint32_t i = 0; i < 12 && ok; ++i) m[i] = ReadF(head + 0x0Cu + 4u * i, ok);
    if (!ok) return;
    constexpr float kEye[3] = {0.0f, 76.0f, 85.0f};
    constexpr uint32_t kEyeParts = 0x97848000u;  // a parts stand-in: only world (+0x70) is read
    for (uint32_t r = 0; r < 3; ++r) {
        const float v = m[r * 4] * kEye[0] + m[r * 4 + 1] * kEye[1] + m[r * 4 + 2] * kEye[2] + m[r * 4 + 3];
        if (!std::isfinite(v)) return;
        WriteF(kEyeParts + 0x70u + 4u * r, v);
    }
    ctx->gpr[3] = kEyeParts;
    static bool s_logged[3] = {};
    if (!s_logged[camera]) {
        s_logged[camera] = true;
        static const char* const kName[3] = {"scope", "binoculars", "look-down camera"};
        RT_LOGF(RT_TAG_OS, "cheats: play as: HUNK's %s looks from Leon's eye point\n", kName[camera]);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80119CC4, PlayAs_GetPartsPtr_80119CC4, "PlayAs_GetPartsPtr_80119CC4");

extern "C" void func_8013ABFC(CpuContext* ctx);
extern "C" void AutoAim_SearchLockEm_8013ABFC(CpuContext* ctx) {
    if (!AutoAimLockOn()) {
        func_8013ABFC(ctx);
        return;
    }
    const uint32_t from = ctx->gpr[3];
    if (!Memory::Contains(from, 12)) {
        func_8013ABFC(ctx);
        return;
    }
    // A rifle aims through its scope by hand: no target at all, also not the game's own 10-frame lock
    // (the rifle picks it when readied, before the scope's flags come on; moto g57, Final Beta).
    if (GameTweaks::RifleInHand()) {
        static bool s_loggedRifle = false;
        if (!s_loggedRifle) {
            s_loggedRifle = true;
            RT_LOGF(RT_TAG_OS, "cheats: auto aim leaves the rifle scope to the player\n");
        }
        ctx->gpr[3] = 0;
        return;
    }
    g_lockCancelled = false;
    g_stickFrames = 0;
    const uint32_t em = PickLockTarget(ctx, from);
    static uint32_t s_logged = 0;
    if (em != s_logged) {
        s_logged = em;
        uint32_t idWord = 0;
        if (em != 0) Memory::TryRead32(em + 0x100u, idWord);
        RT_LOGF(RT_TAG_OS, "cheats: auto aim locks 0x%08X (id %02X)\n", em, idWord >> 24);
    }
    ctx->gpr[3] = em;
}
REGISTER_NATIVE_FUNCTION_AS(0x8013ABFC, AutoAim_SearchLockEm_8013ABFC, "AutoAim_SearchLockEm_8013ABFC");

namespace {
// PlWepAutoTrack (0x8013BF3C) turns the aim toward the lock point by a capped step a frame, and not at all
// sideways within 400 units of the hand, so the sight trailed a moving enemy and drifted off a near one.
// Here the aim is set onto the lock point each frame: the body yaw (ang.y +0xA4, with the waist turn
// m_Fwork0 +0x424) and the aim pitch m3r (0x80327A34, atan2(dy, dist) / (pi/4), -1..1). m3r turns the
// camera and the shot; the laser sight and the arm follow their own animation and may point higher, so
// the laser is no measure of the shot (a correction steered by it sent the shots into the ground).
constexpr uint32_t kPlYaw = 0xA4u, kPlWaist = 0x424u, kAimPitch = 0x80327A34u;
constexpr uint32_t kHandParts = 10u;
void SnapAimTo(CpuContext* ctx, uint32_t pl, uint32_t em) {
    const uint32_t hand = PartsPtr(ctx, pl, kHandParts);
    bool ok = true;
    float t[3], h[3];
    const float waist = ReadF(pl + kPlWaist, ok);
    if (!ok || hand == 0 || !LockPoint(ctx, em, t) || !PartsPos(hand, h)) return;
    const float dx = t[0] - h[0], dy = t[1] - h[1], dz = t[2] - h[2];
    const float flat = std::sqrt(dx * dx + dz * dz);
    if (!std::isfinite(flat) || flat < 50.0f) return;  // on top of the hand: no sensible direction
    WriteF(pl + kPlYaw, WrapAngle(std::atan2(dx, dz) - waist));
    const float dist = std::sqrt(flat * flat + dy * dy);
    WriteF(kAimPitch, std::clamp(std::atan2(dy, dist) / 0.78539816f, -1.0f, 1.0f));
}
} // namespace

extern "C" void func_8013A448(CpuContext* ctx);
extern "C" void AutoAim_LockMove_8013A448(CpuContext* ctx) {
    uint32_t pl = 0, pg = 0;
    const uint32_t wep = ctx->gpr[3];
    if (!AutoAimLockOn() || GameTweaks::RifleInHand() || !Memory::TryRead32(kPPLAddr, pl) || !Cheats::GuestPointer(pl) ||
        !Memory::TryRead32(Cheats::kPGAddress, pg) || !Cheats::GuestPointer(pg) ||
        !Memory::Contains(wep + 0x40u, 1)) {
        func_8013A448(ctx);
        return;
    }
    uint32_t em = 0;
    Memory::TryRead32(pl + kPlLockEm, em);
    float head[3] = {};
    if (!g_lockCancelled && !EnemyHead(em, head)) {
        // The target died (or there was none): the next one, from the camera.
        em = PickLockTarget(ctx, pg + kCampos);
        if (em != 0 && EnemyHead(em, head)) {
            Memory::TryWrite32(pl + kPlLockEm, em);
            RT_LOGF(RT_TAG_OS, "cheats: auto aim moves on to 0x%08X\n", em);
        } else {
            em = 0;
        }
    }
    if (g_lockCancelled || em == 0) {
        func_8013A448(ctx);
        return;
    }
    // Locked for the whole aim. lockMove clears m_LockTime on any push of the main stick (Joy[0].on
    // 0xF0000, the aim stick with the Classic Controller), and a thumb resting on the touch stick pushes
    // it all the time; only a push held for kStickCancelFrames cancels, shorter ones are hidden from it.
    uint32_t lockTime = 0, joy = 0;
    if (Memory::TryRead32(wep + 0x40u, lockTime)) Memory::TryWrite32(wep + 0x40u, (lockTime & 0x00FFFFFFu) | 0x0A000000u);
    const bool haveJoy = Memory::TryRead32(kJoyOnAddr, joy);
    g_stickFrames = (haveJoy && (joy & kJoyMainStick) != 0u) ? g_stickFrames + 1u : 0u;
    const bool hide = g_stickFrames != 0u && g_stickFrames < kStickCancelFrames;
    if (hide) Memory::TryWrite32(kJoyOnAddr, joy & ~kJoyMainStick);
    func_8013A448(ctx);
    if (hide) Memory::TryWrite32(kJoyOnAddr, joy);
    if (Memory::TryRead32(wep + 0x40u, lockTime) && (lockTime >> 24) == 0u) {
        g_lockCancelled = true;
        RT_LOGF(RT_TAG_OS, "cheats: auto aim cancelled by the aim stick (held %u frames)\n", g_stickFrames);
        return;
    }
    SnapAimTo(ctx, pl, em);
}
REGISTER_NATIVE_FUNCTION_AS(0x8013A448, AutoAim_LockMove_8013A448, "AutoAim_LockMove_8013A448");

// Play As (the story only): ReadPlayerData (0x80141160, r3 pl_type, r4 pl_costume), called by gameRoomInit
// (0x801061E8) as each room starts, reads the player's archive and moveset for pl_type. Before it, Leon (0)
// becomes the chosen character, and a character this cheat gave (2..5, never in the story itself) goes
// back to Leon when it is turned off: pl_type set, the weapon module dropped (ReleaseWepData 0x8014153C,
// each character has its own weapon files), the costume picked again (PlSetCostume 0x80136BA0: the game
// costume for anyone but Leon) and pl_flag (pG +0x4FCE) bit 0 so the player data is read again. Ashley's
// part (1), Separate Ways and Assignment Ada (System_flg bits 0 and 1, the extra games) and The
// Mercenaries' own picks are left alone. Leon's case, life and money stay. The way CubeEvil4 does it
// (gameRoomInit before ReadPlayerData), written for the Wii's binary. Setting pl_type from the title
// instead, as the Gecko code does, crashed in room 120 (Beta 14 test: a null object from the player REL).
namespace {
constexpr uint32_t kSystemFlgOffset = 0x54u;
constexpr uint32_t kPlFlagOffset = 0x4FCEu;

// The extra games, which Play As leaves alone: System_flg bits 0 and 1, and Separate Ways (Stage 5, rooms
// 0x5xx), which on the Wii sets neither (System_flg 0x1884 in r51F). A stand-in loaded there drew the room
// grey (3 draws) and the merchant showed the wrong models (Xiaomi 25053PC47G, 2026-11-19).
bool ExtraGame(uint32_t pG, uint32_t sys) {
    uint32_t room = 0;
    return (sys & 0xC0000000u) != 0 || (Memory::TryRead32(pG + 0x4FACu, room) && (room >> 24) == 5u);
}

bool InExtraGame() {
    uint32_t pG = 0, sys = 0;
    return !Memory::TryRead32(Cheats::kPGAddress, pG) || !Cheats::GuestPointer(pG) ||
           !Memory::TryRead32(pG + kSystemFlgOffset, sys) || ExtraGame(pG, sys);
}

int PlayAsWanted() {
    static const bool s_env = [] {  // RE4_PLAY_AS: the start value; the Windows menu may change it later
        const char* e = std::getenv("RE4_PLAY_AS");
        if (e) Cheats::SetPlayAs(std::atoi(e));
        return true;
    }();
    (void) s_env;
    return Cheats::PlayAs();
}

// A New Game's start set (Cheats_ItemGameInit_80033498) is taken in hand when its stand-in takes over.
int g_newGameArmWho = 0;
uint16_t g_newGameArmItem = 0;
} // namespace

extern "C" void PlayAs_ReadPlayerData_80141160(CpuContext* ctx) {
    uint32_t pG = 0, sys = 0, word = 0;
    if (CheatsAllowed() && Memory::TryRead32(Cheats::kPGAddress, pG) && Cheats::GuestPointer(pG) &&
        Memory::TryRead32(pG + kSystemFlgOffset, sys) && !ExtraGame(pG, sys) &&
        Memory::TryRead32(pG + Cheats::kPlTypeOffset, word)) {
        const int type = static_cast<int>(word >> 24);
        // A new game starts with the opening drive (room 120) unless it is skipped: Leon sits in the car
        // there as the player itself, so a stand-in left the scene without anyone. The switch waits for
        // r100 (a character this cheat gave stays as it is).
        uint32_t roomWord = 0;
        // The title screen is room 120 as well: a stand-in's save loaded with Play As off kept the stand-in
        // for its first room, with no hook for Leon's weapons (Wesker's case froze on the Custom TMP,
        // 2026-11-18). Only the stand-in Play As still wants is kept.
        const bool drive = Memory::TryRead32(pG + 0x4FACu, roomWord) && (roomWord >> 16) == 0x120u;
        const int want = drive ? (type >= 2 && type <= 5 && type == PlayAsWanted() ? type : 0) : PlayAsWanted();
        int next = type;
        if (type == 0 && want != 0) {
            next = want;
        } else if (type >= 2 && type <= 5 && type != want) {
            next = want;
        }
        if (next != type) {
            Memory::TryWrite32(pG + Cheats::kPlTypeOffset, (word & 0x00FFFFFFu) | static_cast<uint32_t>(next) << 24);
            func_8014153C(ctx);
            func_80136BA0(ctx);
            if (next == g_newGameArmWho) {
                const uint32_t item = FindItem(g_newGameArmItem);
                if (item != 0) {
                    ctx->gpr[3] = kItemMgr;
                    ctx->gpr[4] = item;
                    func_800357B8(ctx);
                }
                g_newGameArmWho = 0;
            }
            uint16_t flag = 0;
            if (Cheats::Read16(pG + kPlFlagOffset, flag)) Memory::Write16(pG + kPlFlagOffset, flag | 1u);
            uint32_t now = 0;
            Memory::TryRead32(pG + Cheats::kPlTypeOffset, now);
            ctx->gpr[3] = now >> 24;
            ctx->gpr[4] = (now >> 16) & 0xFFu;  // pl_costume, as PlSetCostume left it
            RT_LOGF(RT_TAG_OS, "cheats: play as: pl_type %d -> %u, costume %u, room word %08X\n", type, ctx->gpr[3], ctx->gpr[4],
                    roomWord);
        }
    }
    func_80141160(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80141160, PlayAs_ReadPlayerData_80141160, "PlayAs_ReadPlayerData_80141160");

// Play As New Game: cItemMgr::gameInit (0x80033498, only for a New Game) gives Leon's start set in the
// story. With a stand-in chosen it is done again with the stand-in's own start set, as the extra games
// give it: clear (0x80032488), roomInit, set_char (0x80032A58, its weapons tuned and loaded, the small
// case) and the two items Leon's start adds (0xAC, 0xAD). pl_type is still Leon's here (the switch waits
// for r100), so the weapon set_char armed is put away (Leon has no file for some of them) and taken in
// hand when the stand-in takes over (PlayAs_ReadPlayerData_80141160). A loaded save is left alone.
extern "C" void func_80033498(CpuContext* ctx);
extern "C" void func_80032488(CpuContext* ctx);  // cItemMgr::clear
extern "C" void func_80032A58(CpuContext* ctx);  // cItemMgr::set_char(r4 pl_type)

extern "C" void Cheats_ItemGameInit_80033498(CpuContext* ctx) {
    func_80033498(ctx);
    uint32_t pG = 0, sys = 0;
    const int want = PlayAsWanted();
    if (want == 0 || !CheatsAllowed() || !Memory::TryRead32(Cheats::kPGAddress, pG) || !Cheats::GuestPointer(pG) ||
        !Memory::TryRead32(pG + kSystemFlgOffset, sys) || ExtraGame(pG, sys) || PlType() != 0) {
        return;
    }
    const uint32_t r3 = ctx->gpr[3], r4 = ctx->gpr[4], r5 = ctx->gpr[5];
    ctx->gpr[3] = kItemMgr;
    func_80032488(ctx);
    ctx->gpr[3] = kItemMgr;
    func_80033604(ctx);
    ctx->gpr[3] = kItemMgr;
    ctx->gpr[4] = static_cast<uint32_t>(want);
    func_80032A58(ctx);
    CallItemMgr(ctx, func_8003443C, 0xAC, 1);
    CallItemMgr(ctx, func_8003443C, 0xAD, 1);
    // Wesker's start set from set_char came without his silencer (tester, Beta 14): added here.
    if (want == 5 && !HasItem(kSilencerItem)) {
        CallItemMgr(ctx, func_8003443C, kSilencerItem, 1);
        RT_LOGF(RT_TAG_OS, "cheats: play as: Wesker's silencer added to the new game (%s)\n",
                HasItem(kSilencerItem) ? "in the case" : "missing");
    }
    uint32_t wep = 0;
    uint16_t id = 0;
    g_newGameArmWho = 0;
    if (Memory::TryRead32(kItemMgr + 0x0Cu, wep) && Cheats::GuestPointer(wep) && Cheats::Read16(wep, id)) {
        g_newGameArmWho = want;
        g_newGameArmItem = id;
        ArmBareHands(ctx);
    }
    RT_LOGF(RT_TAG_OS, "cheats: play as: new game with character %d's start set (in hand later: %X)\n", want, id);
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
}
REGISTER_NATIVE_FUNCTION_AS(0x80033498, Cheats_ItemGameInit_80033498, "Cheats_ItemGameInit_80033498");

// Another character chosen while the story runs takes over at once, as in CubeEvil4 (switchNow): as soon as
// the player is free (Rno0 3, no sub screen, merchant or typewriter, no event STA_EVENT 19 or movie
// STA_MOVIE_ON 3) the room starts again where the player stands (Stage Select's room jump into the same
// room, part and entry, at pPL pos +0x94 / ang.y +0xA4, the life kept). Its start reads the new player
// data (PlayAs_ReadPlayerData_80141160). Once per choice; the opening drive (r120) waits for r100.
namespace Cheats {
// The room starts again where the player stands, when free; false when not now.
bool RestartRoomHere(uint32_t pG, const char* why) {
    uint32_t rno = 0, room = 0, pl = 0, st0 = 0, x = 0, y = 0, z = 0, a = 0;
    if (!Memory::TryRead32(pG + 0x20u, rno) || (rno >> 24) != 3u || GameTweaks::MenuScreen() ||
        !Memory::TryRead32(pG + 0x501Cu, st0) || (st0 & (0x80000000u >> 19)) || (st0 & (0x80000000u >> 3)) ||
        !Memory::TryRead32(pG + 0x4FACu, room) || (room >> 16) == 0x120u || !Memory::TryRead32(0x8043DEA0u, pl) ||
        !GuestPointer(pl) || !Memory::TryRead32(pl + 0x94u, x) || !Memory::TryRead32(pl + 0x98u, y) ||
        !Memory::TryRead32(pl + 0x9Cu, z) || !Memory::TryRead32(pl + 0xA4u, a)) {
        return false;
    }
    float fx, fy, fz, fa;
    std::memcpy(&fx, &x, 4);
    std::memcpy(&fy, &y, 4);
    std::memcpy(&fz, &z, 4);
    std::memcpy(&fa, &a, 4);
    GameTweaks::RequestRoomJump(static_cast<uint16_t>(room >> 16), static_cast<uint8_t>(room & 0xFFu), fx, fy, fz, fa,
                                static_cast<uint8_t>((room >> 8) & 0xFFu), false);
    RT_LOGF(RT_TAG_OS, "cheats: %s: r%03X starts again at (%.0f %.0f %.0f)\n", why, room >> 16, fx, fy, fz);
    return true;
}

} // namespace Cheats
extern "C" void func_80142234(CpuContext* ctx);  // cRoomData::getRoomSavePtr
namespace Cheats {

// Replay cutscene (F10 > Cheats > Play As): plays the last cutscene again without restarting the game.
// While the player is free, the progress flags (Wii pG + 0x502C..0x52F8 = GC 0x501C..0x52E8: Em_flg,
// Item_flg, Scenario_flg, Key_flg, Frame_cnt, save_free_work) and the player's room and place are kept every
// half second (the last 8). When an event starts, the newest of them taken before its scenario flag went on
// (Scenario_flg, Wii +0x51CC) is kept for it. The replay puts those flags back and restarts the room at that
// place, so walking on starts the scene again.
namespace {
constexpr uint32_t kSnapFrom = 0x502Cu;
constexpr uint32_t kSnapSize = 0x52F8u - 0x502Cu;
constexpr uint32_t kSnapScenario = 0x51CCu - kSnapFrom;
// The room's own record too (cRoomData::getRoomSavePtr 0x80142234, RoomData 0x8034D904; ROOM_SAVE_DATA
// +0x04 save_flg .. +0xA8: the room save flags the room scripts keep their cutscenes in (r100: 3, 10, 12,
// 13), item flags, model flags). Without it r100s03 did not start again.
constexpr uint32_t kRoomSaveFrom = 0x04u;
constexpr uint32_t kRoomSaveSize = 0xA8u - 0x04u;
struct FlagSnap {
    bool valid = false;
    uint32_t room = 0;
    float x = 0, y = 0, z = 0, a = 0;
    uint8_t flags[kSnapSize] = {};
    uint32_t roomSave = 0;  // its record, 0 when the room has none
    uint8_t roomFlags[kRoomSaveSize] = {};
};
FlagSnap g_snaps[8];
int g_snapNext = 0;
FlagSnap g_replay;
std::mutex g_replayMutex;
std::string g_replayName;  // guarded by g_replayMutex
std::atomic<bool> g_replayRequest{false};

uint32_t RoomSaveRecord(uint32_t roomId) {
    GuestInterruptCallbackContext call;
    CpuContext* cpu = call.get();
    cpu->gpr[3] = 0x8034D904u;  // RoomData
    cpu->gpr[4] = roomId;
    func_80142234(cpu);
    return GuestPointer(cpu->gpr[3]) ? cpu->gpr[3] : 0;
}

// The player is free (as RestartRoomHere needs it): its room word and place.
bool FreePlayer(uint32_t pG, uint32_t& room, float& x, float& y, float& z, float& a) {
    uint32_t rno = 0, pl = 0, st0 = 0, sta2 = 0, w[4] = {};
    if (!Memory::TryRead32(pG + 0x20u, rno) || (rno >> 24) != 3u || GameTweaks::MenuScreen() ||
        !Memory::TryRead32(pG + 0x501Cu, st0) || (st0 & (0x80000000u >> 19)) || (st0 & (0x80000000u >> 3)) ||
        !Memory::TryRead32(pG + 0x501Cu + 8u, sta2) || (sta2 & 0x00080000u) ||
        !Memory::TryRead32(pG + 0x4FACu, room) || (room >> 16) == 0x120u || (room >> 16) == 0 ||
        !Memory::TryRead32(0x8043DEA0u, pl) || !GuestPointer(pl) || !Memory::TryRead32(pl + 0x94u, w[0]) ||
        !Memory::TryRead32(pl + 0x98u, w[1]) || !Memory::TryRead32(pl + 0x9Cu, w[2]) ||
        !Memory::TryRead32(pl + 0xA4u, w[3])) {
        return false;
    }
    std::memcpy(&x, &w[0], 4);
    std::memcpy(&y, &w[1], 4);
    std::memcpy(&z, &w[2], 4);
    std::memcpy(&a, &w[3], 4);
    return true;
}
} // namespace

void CutsceneReplayMark(const std::string& name) {
    uint32_t pG = 0;
    uint8_t scenario[0x20] = {};
    if (!Memory::TryRead32(kPGAddress, pG) || !GuestPointer(pG)) return;
    for (uint32_t i = 0; i < sizeof(scenario); ++i) scenario[i] = Memory::Read8(pG + kSnapFrom + kSnapScenario + i);
    const FlagSnap* pick = nullptr;
    const FlagSnap* newest = nullptr;
    for (int k = 1; k <= 8; ++k) {
        const FlagSnap& s = g_snaps[(g_snapNext - k + 8) % 8];
        if (!s.valid) break;
        if (!newest) newest = &s;
        if (std::memcmp(s.flags + kSnapScenario, scenario, sizeof(scenario)) != 0) {
            pick = &s;
            break;
        }
    }
    if (!pick) pick = newest;
    if (!pick) return;
    std::lock_guard<std::mutex> lock(g_replayMutex);
    g_replay = *pick;
    g_replayName = name;
}

std::string CutsceneReplayName() {
    std::lock_guard<std::mutex> lock(g_replayMutex);
    return g_replay.valid ? g_replayName : std::string();
}

void RequestCutsceneReplay() { g_replayRequest.store(true, std::memory_order_relaxed); }

void CutsceneReplayTick() {
    static uint32_t s_tick = 0;
    uint32_t pG = 0, room = 0;
    float x = 0, y = 0, z = 0, a = 0;
    if (!Memory::TryRead32(kPGAddress, pG) || !GuestPointer(pG) || !FreePlayer(pG, room, x, y, z, a)) return;
    if (g_replayRequest.exchange(false, std::memory_order_relaxed)) {
        if (!g_replay.valid || !CheatsAllowed()) return;
        for (uint32_t i = 0; i < kSnapSize; ++i) Memory::Write8(pG + kSnapFrom + i, g_replay.flags[i]);
        if (g_replay.roomSave != 0) {
            for (uint32_t i = 0; i < kRoomSaveSize; ++i) {
                Memory::Write8(g_replay.roomSave + kRoomSaveFrom + i, g_replay.roomFlags[i]);
            }
        }
        GameTweaks::RequestRoomJump(static_cast<uint16_t>(g_replay.room >> 16),
                                    static_cast<uint8_t>(g_replay.room & 0xFFu), g_replay.x, g_replay.y, g_replay.z,
                                    g_replay.a, static_cast<uint8_t>((g_replay.room >> 8) & 0xFFu), false);
        for (FlagSnap& s : g_snaps) s.valid = false;
        RT_LOGF(RT_TAG_OS, "cheats: replay cutscene %s: flags back, r%03X starts again at (%.0f %.0f %.0f)\n",
                CutsceneReplayName().c_str(), g_replay.room >> 16, g_replay.x, g_replay.y, g_replay.z);
        return;
    }
    if (++s_tick % 30u != 0) return;
    FlagSnap& s = g_snaps[g_snapNext];
    g_snapNext = (g_snapNext + 1) % 8;
    s.valid = true;
    s.room = room;
    s.x = x;
    s.y = y;
    s.z = z;
    s.a = a;
    for (uint32_t i = 0; i < kSnapSize; ++i) s.flags[i] = Memory::Read8(pG + kSnapFrom + i);
    s.roomSave = RoomSaveRecord(room >> 16);
    for (uint32_t i = 0; s.roomSave != 0 && i < kRoomSaveSize; ++i) {
        s.roomFlags[i] = Memory::Read8(s.roomSave + kRoomSaveFrom + i);
    }
}

void PlayAsSwitchNow() {
    static int s_tried = -1;
    uint32_t pG = 0, sys = 0, word = 0;
    if (!CheatsAllowed() || !Memory::TryRead32(kPGAddress, pG) || !GuestPointer(pG) ||
        !Memory::TryRead32(pG + kSystemFlgOffset, sys) || ExtraGame(pG, sys) ||
        !Memory::TryRead32(pG + kPlTypeOffset, word)) {
        return;
    }
    if (g_plTypeSwapDepth > 0) return;  // a hook shows the game Leon for one call (PlTypeSwapScope)
    const int type = static_cast<int>(word >> 24);
    const int want = PlayAsWanted();
    const bool change = (type == 0 && want != 0) || (type >= 2 && type <= 5 && type != want);
    if (!change) {
        s_tried = -1;
        return;
    }
    if (s_tried == want) return;
    // Not during a cutscene (Status_flg STA_EVENT_SYSYTEM, pG + 0x501C word 2 0x80000): the room restart
    // cut it, and its scenario flag was already set, so the scene was lost. The switch waits for its end.
    uint32_t sta2 = 0;
    if (!Memory::TryRead32(pG + 0x501Cu + 8u, sta2) || (sta2 & 0x00080000u) != 0) return;
    char why[48];
    std::snprintf(why, sizeof(why), "play as: pl_type %d -> %d now", type, want);
    if (RestartRoomHere(pG, why)) s_tried = want;
}
} // namespace Cheats

// Play As weapons: ReadWepData (0x80141590, r3 weapon no, r4 variant) picks the weapon module from the
// character's table (49 entries of 8 bytes: Leon 0x802DDCE0, Ada +0x188, HUNK +0x310, Wesker +0x498,
// Krauser +0x620). The stand-ins' tables hold only their Mercenaries weapons, so Leon's starting Handgun
// had no module, the weapon object stayed empty and the first frame called through it (Ada, 0x80138180).
// A weapon the character has no file for is read from Leon's table (pl_type 0 for the call). Krauser
// always: his own files go to the Mercenaries' extra buffer, which a story game never sets up.
extern "C" void func_80141590(CpuContext* ctx);
namespace {
// True when stand-in `who` (2..5) holds Leon's weapon `no` (type `type`, 0xFF unknown) as a weapon of its
// own: the game's number, before ReadWepData's per-character remap. A raw table entry is not enough:
// Wesker's entries 4 and 5 are where his grenades and his rifle (0x0A) land, so Leon's Blacktail (4) and
// Broken Butterfly (5) read Wesker's grenade and rifle files. From the Wii tables (2026-11-17):
// - all but Krauser: knife 0x00 and the throwables (0x13 after the remap above);
// - Ada: Punisher 1, Blacktail 4, shotgun 7, semi-auto rifle 0x0A, TMP 0x0B, Chicago 0x0C, 0x30
//   (Separate Ways' set);
// - HUNK and Wesker: the TMP 0x0B (HUNK's wep29) only without the stock: its stock models are a
//   placeholder (CubeEvil4 0206); Wesker also his handgun 2, Killer7 6 and rifle 0x0A;
// - Krauser: his bow 0x1C (wep28) only. The GC build read his files to the Mercenaries' extra buffer; the
//   Wii's ReadWepData reads every character's to 0x80634000 (2026-11-18), so the story can load it.
bool StandInOwnsWeapon(int who, uint32_t no, uint32_t type = 0xFFu) {
    if (who == 4) return no == 0x1C;
    if (who < 2 || who > 5) return false;
    if (no == 0x00 || no == 0x13) return true;
    switch (who) {
    case 2:
        return no == 0x01 || no == 0x04 || no == 0x07 || no == 0x0A || no == 0x0B || no == 0x0C || no == 0x30;
    case 3:
        return no == 0x0B && (type == 0 || type == 0xFFu);
    case 5:
        return no == 0x02 || no == 0x06 || no == 0x0A || (no == 0x0B && (type == 0 || type == 0xFFu));
    default:
        return false;
    }
}

// The weapon ReadWepData read last came from Leon's table (the stand-in has no file for it).
bool g_standInHoldsLeonWeapon = false;
uint32_t g_standInWeaponNo = 0;  // that weapon's number

// The stand-in Play As has made the story's player (2..5), 0 otherwise.
int StandInPlaying() {
    uint32_t pG = 0, sys = 0, word = 0;
    const int want = PlayAsWanted();
    if (want == 0 || !CheatsAllowed() || !Memory::TryRead32(Cheats::kPGAddress, pG) || !Cheats::GuestPointer(pG) ||
        !Memory::TryRead32(pG + kSystemFlgOffset, sys) || ExtraGame(pG, sys) ||
        !Memory::TryRead32(pG + Cheats::kPlTypeOffset, word) || static_cast<int>(word >> 24) != want) {
        return 0;
    }
    return want;
}
} // namespace

void UpdateStandInProps();  // below, with Leon's archive
void PlayAs_ReadWepData_Inner(CpuContext* ctx);

namespace {
// Ada's reloads. Her own weapon files (Punisher wep38, Blacktail wep54, semi-auto rifle wep40, TMP wep39,
// Chicago wep49) are her Mercenaries files. On the Wii disc each holds her reload motion only at reload
// level 1 (handgun 0x2E, rifle 0x1D, TMP 0x23; Chicago 0x23 is Leon's); the level 0 and 2 entries are
// empty (compared on the disc, 2026-11-22). The reload state picks the entry by weapon_lv_reload (pG
// +0x4FCA; wep39.rel 0x96E76D04: 0 -> 0x1E, 1 -> 0x23, 2 -> 0x24, as the GC's pl_machine.cpp), and an empty
// entry reads the data of the entry after it: another motion. That was the TMP reloading with a wrong
// motion and the camera at her back (1.0/1.1 reports); the old fix pointed levels 1 and 2 at level 0, the
// empty one. Her weapon object's reload (wep38.rel and the rest) is made for level 1 too. While she holds
// one of them the reload level stays 1, the level her Mercenaries plays at: her motions, level 1's end
// frame. Equipping any weapon sets the level again from the item (ItemMgr's weapon set).
void AdaOwnReloadLevel() {
    uint32_t pG = 0, word = 0, wep = 0;
    if (StandInPlaying() != 2 || g_standInHoldsLeonWeapon) return;
    const uint32_t no = g_standInWeaponNo;
    if ((no != 0x01 && no != 0x04 && no != 0x0A && no != 0x0B && no != 0x0C) ||
        !Memory::TryRead32(Cheats::kPGAddress, pG) || !Cheats::GuestPointer(pG) ||
        !Memory::TryRead32(pG + 0x4FC0u, wep) || (wep >> 24) != no ||
        !Memory::TryRead32(pG + Cheats::kPlTypeOffset, word) || ((word >> 8) & 0xFFu) == 1u) {
        return;
    }
    Memory::Write8(pG + Cheats::kPlTypeOffset + 2u, 1u);  // one byte: pl_type beside it may be swapped
    static uint32_t s_logged = 0;
    if ((s_logged & (1u << (no & 31))) == 0) {
        s_logged |= 1u << (no & 31);
        RT_LOGF(RT_TAG_OS, "cheats: play as: Ada's weapon %u reloads at level 1 (her own motion), was %u\n", no,
                (word >> 8) & 0xFFu);
    }
}
} // namespace

extern "C" void PlayAs_ReadWepData_80141590(CpuContext* ctx) {
    PlayAs_ReadWepData_Inner(ctx);
    AdaOwnReloadLevel();
    UpdateStandInProps();
}

void PlayAs_ReadWepData_Inner(CpuContext* ctx) {
    uint32_t pG = 0, sys = 0, word = 0;
    // Krauser's bow read by anyone else (the room start after a switch from Krauser, Leon included): the
    // entry is empty in every other table. Empty hands instead (weapon 0, as the bare-hand item 0x800);
    // the room's item init then takes the bow out of the case.
    if ((ctx->gpr[3] & 0xFFu) == 0x1C && Memory::TryRead32(Cheats::kPGAddress, pG) && Cheats::GuestPointer(pG) &&
        Memory::TryRead32(pG + Cheats::kPlTypeOffset, word) && (word >> 24) != 4u) {
        uint32_t wepWord = 0;
        if (Memory::TryRead32(pG + 0x4FC0u, wepWord) && (wepWord >> 24) == 0x1Cu) {
            Memory::TryWrite32(pG + 0x4FC0u, wepWord & 0x0000FFFFu);
        }
        uint16_t wepId = 0;
        if (Cheats::Read16(kItemMgr + 0x10u, wepId) && wepId == 0x52u) ArmBareHands(ctx);
        ctx->gpr[3] = 0;
        ctx->gpr[4] = 0;
        RT_LOGF(RT_TAG_OS, "cheats: play as: pl_type %u has no bow file: empty hands\n", word >> 24);
    }
    const int want = PlayAsWanted();
    if (want == 0 || !CheatsAllowed() || !Memory::TryRead32(Cheats::kPGAddress, pG) || !Cheats::GuestPointer(pG) ||
        !Memory::TryRead32(pG + kSystemFlgOffset, sys) || ExtraGame(pG, sys) ||
        !Memory::TryRead32(pG + Cheats::kPlTypeOffset, word) || static_cast<int>(word >> 24) != want) {
        func_80141590(ctx);
        return;
    }
    uint32_t no = ctx->gpr[3];
    if (no == 0x16 || no == 0x17 || no == 0x19 || no == 0x1F || no == 0x20) no = 0x13;  // the stand-ins' grenades
    // Leon's handguns (2, 3, 5, 6, 0x0F, 0x12) are read from his own files too, as CubeEvil4 0195 does: the
    // file's sound part fills the weapon sound block (SndCall block 2). With Ada's Punisher file (wep38) in
    // their place every one of them fired, cocked and reloaded with the Punisher's sounds (1.1 report).
    // Her grips stay hers (setRightHand/setLeftHand below); her slide while aiming was Oba (see below).
    g_standInHoldsLeonWeapon = !StandInOwnsWeapon(want, no, ctx->gpr[4] & 0xFFu);
    g_standInWeaponNo = no;
    if (!g_standInHoldsLeonWeapon) {
        func_80141590(ctx);
        return;
    }
    const PlTypeSwapScope plTypeSwap;
    Memory::TryWrite32(pG + Cheats::kPlTypeOffset, word & 0x00FFFFFFu);
    func_80141590(ctx);
    uint32_t now = 0;
    if (Memory::TryRead32(pG + Cheats::kPlTypeOffset, now)) {
        Memory::TryWrite32(pG + Cheats::kPlTypeOffset, (now & 0x00FFFFFFu) | (word & 0xFF000000u));
    }
    static uint32_t s_logged = 0;
    if ((s_logged & (1u << (no & 31))) == 0) {
        s_logged |= 1u << (no & 31);
        RT_LOGF(RT_TAG_OS, "cheats: play as: weapon %u read from Leon's table\n", no);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80141590, PlayAs_ReadWepData_80141590, "PlayAs_ReadWepData_80141590");

// Play As case screen: the inventory module (Sscrn REL, module 81) shows the character beside the case
// holding the equipped weapon. It first queues that weapon's case file (0x96DA2458: r3 path buffer, r4
// weapon no; SS/cmn/ss_wepNN.dat by the character's weapon), then builds the model with the character's
// own ModelInit(r3 weapon no, r4 type): Ada 0x96DA2EAC, Krauser 0x96DA45D4, HUNK 0x96DA4EA4, Wesker
// 0x96DA56B8; Leon's is 0x96DA627C. Each picks a per-weapon init by number (jump table); the stand-ins
// have inits only for their own weapons (the others read entry 4 as the pose: a T-pose, or no file at all,
// which aborted the case: DVDReadPrio, no published FST extent).
// A case file is an archive: entry 4 or 5 the gun's TPL (magic 0x0020AF30) and the other the gun's model,
// both handed to 0x80119884; the hands (0x8011B624, the player's TPL) and the pose (0x8011C310, the last
// entry) follow. A stand-in holding one of Leon's weapons gets the case file and init of its own weapon of
// the same kind (CaseClass: the hands and the pose), and for the build that file's gun entries point at
// Leon's case file of the equipped weapon (read to kCaseLeonAddr): its gun in the stand-in's hands. Leon's
// file comes from the case's own path builder run as Leon. Leon's files without a TPL beside the model
// (the rocket launcher) keep the stand-in's own gun.
// (Registrations written out: the shard emitter reads the addresses from this source text.)
namespace {
// The stand-in's own weapon whose case file and init show Leon's weapon `no`.
uint32_t CaseClass(int who, uint32_t no) {
    const bool handgun = no == 1 || no == 2 || no == 3 || no == 4 || no == 5 || no == 6 || no == 0x0F || no == 0x12;
    switch (who) {
    case 2:
        if (handgun) return 1;  // her Punisher's
        return no == 0x11 ? 0x0B : 7;  // the Matilda as her TMP, the long guns as her shotgun
    case 3:
        return 0x0B;  // HUNK: his TMP
    case 4:
        return 0x1C;  // Krauser: his bow (his only case weapon with a gun model)
    case 5:
        // Wesker: his Killer7, his rifle. The TMP (0x0B) takes the Killer7's pose too: his own init for
        // 0x0B has no pose (Windows: T-pose; phones: wild reads, black screen with the TMP, 1.1 report
        // 19-59). The case path now loads the Killer7's file for it (CaseOwnsWeapon below).
        if (no == 0x0B) return 6;
        return handgun ? 6 : 0x0A;
    default:
        return 0;
    }
}

// The case screen's view of a stand-in's own weapon: Wesker's TMP plays as his own in the game (his weapon
// table has it), but his case init for it builds no pose, so the case shows it as one of Leon's.
bool CaseOwnsWeapon(int who, uint32_t no, uint32_t type = 0xFFu) {
    if (who == 5 && no == 0x0B) return false;
    return StandInOwnsWeapon(who, no, type);
}

constexpr uint32_t kCaseWeaponArc = 0x80328664u;  // casework 0x80328458 + 0x20C: the loaded case file
constexpr uint32_t kCaseLeonAddr = 0x97D80000u;    // Leon's case file (up to 0x97DC0000)
constexpr size_t kCaseLeonMax = 0x40000u;
constexpr uint32_t kCaseLeonPath = 0x97D7FF00u;   // its path, built by the game's path builder
constexpr uint32_t kTplMagic = 0x0020AF30u;

struct CaseLeon {
    uint32_t no = 0xFFFFFFFFu;  // the weapon whose file is at kCaseLeonAddr
    size_t size = 0;
};
CaseLeon g_caseLeon;

struct CasePatch {
    uint32_t arc = 0;
    uint32_t original[2] = {};
    uint32_t written[2] = {};
};
CasePatch g_casePatch;

bool IsTpl(uint32_t addr) {
    uint32_t magic = 0;
    return addr != 0 && Memory::TryRead32(addr, magic) && magic == kTplMagic;
}

// Guest address of entry `n` of the archive at `arc`, 0 when missing or past `size` (0: unchecked).
uint32_t CaseEntry(uint32_t arc, uint32_t n, size_t size) {
    uint32_t count = 0, ofs = 0;
    if (!Memory::TryRead32(arc, count) || n >= count + 4u || !Memory::TryRead32(arc + n * 4u, ofs) || ofs == 0 ||
        (size != 0 && ofs >= size)) {
        return 0;
    }
    return arc + ofs;
}

// Leon's gun in his case file of weapon `no`, type `type`: its TPL and model (the variant his init picks
// by type, as his per-weapon inits at 0x96DA697C.. read them). False without both.
bool LeonCaseGun(uint32_t no, uint32_t type, uint32_t& tpl, uint32_t& model) {
    const uint32_t e4 = CaseEntry(kCaseLeonAddr, 4, g_caseLeon.size);
    const uint32_t e5 = CaseEntry(kCaseLeonAddr, 5, g_caseLeon.size);
    if (IsTpl(e4) && e5 != 0 && !IsTpl(e5)) {
        tpl = e4;
        model = e5;
    } else if (IsTpl(e5) && e4 != 0 && !IsTpl(e4)) {
        tpl = e5;
        model = e4;
    } else {
        return false;
    }
    uint32_t alt = 0;
    if ((type == 1 && (no == 1 || no == 2 || no == 4 || no == 6 || no == 9 || (no >= 0x12 && no <= 0x2D))) ||
        (type == 2 && no == 3)) {
        alt = 6;
    } else if (type == 2 && no == 9) {
        alt = 7;
    }
    if (alt != 0) {
        const uint32_t m = CaseEntry(kCaseLeonAddr, alt, g_caseLeon.size);
        if (m != 0 && !IsTpl(m)) model = m;
    }
    return true;
}

// Points the loaded case file's gun entries (4 and 5) at Leon's gun; false when nothing was changed.
bool CaseLeonGunBegin(int who, uint32_t no, uint32_t type) {
    uint32_t arc = 0, tpl = 0, model = 0, ofs4 = 0, ofs5 = 0;
    if (g_caseLeon.no != no || !Memory::TryRead32(kCaseWeaponArc, arc) || !Cheats::GuestPointer(arc) ||
        !LeonCaseGun(no, type, tpl, model) || !Memory::TryRead32(arc + 16u, ofs4) || !Memory::TryRead32(arc + 20u, ofs5)) {
        return false;
    }
    const bool tplAt4 = IsTpl(CaseEntry(arc, 4, 0));
    if (!tplAt4 && !IsTpl(CaseEntry(arc, 5, 0))) return false;
    // Leon's shotgun-built files (model at 4, TPL at 5: shotgun, Striker) face the other way in a pose
    // built for a TPL-first file (HUNK's TMP: the stock forward); those keep the stand-in's own gun.
    if (tplAt4 && model == CaseEntry(kCaseLeonAddr, 4, g_caseLeon.size)) return false;
    // The pose (the file's last entry) animates as many gun bones as the stand-in's own gun model has
    // (byte 0x19 of a model header, nParts; 0x18 is its weight palette size: both must match). Leon's gun with another count read past the pose: garbage matrices,
    // the gun floating or across the body, unmapped reads at 0x80151F98 (Wesker with the Broken Butterfly:
    // 15 bones against his Killer7's 3, 2026-11-18). Only a gun with the same count is shown, Ada's too
    // (Leon's weapon 15 as her 1 crashed there, Mali-G57, 1.0).
    (void) who;
    uint32_t ownWord = 0, leonWord = 0;
    const uint32_t own = CaseEntry(arc, tplAt4 ? 5 : 4, 0);
    if (own == 0 || !Memory::TryRead32(own + 0x18u, ownWord) || !Memory::TryRead32(model + 0x18u, leonWord) ||
        (ownWord >> 16) != (leonWord >> 16)) {
        return false;
    }
    g_casePatch.arc = arc;
    g_casePatch.original[0] = ofs4;
    g_casePatch.original[1] = ofs5;
    g_casePatch.written[0] = (tplAt4 ? tpl : model) - arc;
    g_casePatch.written[1] = (tplAt4 ? model : tpl) - arc;
    Memory::TryWrite32(arc + 16u, g_casePatch.written[0]);
    Memory::TryWrite32(arc + 20u, g_casePatch.written[1]);
    return true;
}

void CaseLeonGunEnd() {
    for (uint32_t k = 0; k < 2; ++k) {
        uint32_t now = 0;
        const uint32_t at = g_casePatch.arc + 16u + 4u * k;
        if (Memory::TryRead32(at, now) && now == g_casePatch.written[k]) Memory::TryWrite32(at, g_casePatch.original[k]);
    }
    g_casePatch = CasePatch{};
}

void CaseModel(int who, CpuContext* ctx, void (*init)(CpuContext*)) {
    uint32_t no = ctx->gpr[3] & 0xFFu;
    if (no == 0x16 || no == 0x17 || no == 0x19 || no == 0x1F || no == 0x20) no = 0x13;  // the stand-ins' grenades: their own
    const uint32_t type = ctx->gpr[4] & 0xFFu;
    bool patched = false;
    if (StandInPlaying() == who && !CaseOwnsWeapon(who, no, type)) {
        patched = CaseLeonGunBegin(who, no, type);
        ctx->gpr[3] = CaseClass(who, no);
        ctx->gpr[4] = 0;
        static uint32_t s_logged[6] = {};
        if ((s_logged[who] & (1u << (no & 31))) == 0) {
            s_logged[who] |= 1u << (no & 31);
            RT_LOGF(RT_TAG_OS, "cheats: play as: case: character %d, Leon's weapon %u as own %u, Leon's gun %s\n", who, no,
                    ctx->gpr[3], patched ? "shown" : "not shown (own gun)");
        }
    }
    init(ctx);
    if (patched) CaseLeonGunEnd();
}
} // namespace

extern "C" void func_96DA2EAC(CpuContext* ctx);
extern "C" void func_96DA45D4(CpuContext* ctx);
extern "C" void func_96DA4EA4(CpuContext* ctx);
extern "C" void func_96DA56B8(CpuContext* ctx);
extern "C" void PlayAs_CaseModel_96DA2EAC(CpuContext* ctx) { CaseModel(2, ctx, func_96DA2EAC); }
extern "C" void PlayAs_CaseModel_96DA45D4(CpuContext* ctx) { CaseModel(4, ctx, func_96DA45D4); }
extern "C" void PlayAs_CaseModel_96DA4EA4(CpuContext* ctx) { CaseModel(3, ctx, func_96DA4EA4); }
extern "C" void PlayAs_CaseModel_96DA56B8(CpuContext* ctx) { CaseModel(5, ctx, func_96DA56B8); }
REGISTER_NATIVE_FUNCTION_AS(0x96DA2EAC, PlayAs_CaseModel_96DA2EAC, "PlayAs_CaseModel_96DA2EAC");
REGISTER_NATIVE_FUNCTION_AS(0x96DA45D4, PlayAs_CaseModel_96DA45D4, "PlayAs_CaseModel_96DA45D4");
REGISTER_NATIVE_FUNCTION_AS(0x96DA4EA4, PlayAs_CaseModel_96DA4EA4, "PlayAs_CaseModel_96DA4EA4");
REGISTER_NATIVE_FUNCTION_AS(0x96DA56B8, PlayAs_CaseModel_96DA56B8, "PlayAs_CaseModel_96DA56B8");

// Play As partner command: the X key that tells Ashley to wait or follow lives in Leon's class only
// (vtable slot +0x30, 0x80066FB0: Ashley following, the command timer at player +0x523, the call SE and
// the sub character's mode; it also raises the status bit that lets X call her out of a hiding place).
// The stand-ins' own slot +0x30 returns 0 (Ada pl02 0x96DD0F80, HUNK pl06 0x96DD39E4, Wesker pl0d
// 0x96DDAC64) or starts Krauser's arm attack (pl0a 0x96DD5540, same key). While a stand-in plays the
// story, Leon's command runs first on the stand-in (it reads only cPlayer fields and globals); with no
// Ashley following it returns 0 and the stand-in's own handler runs as before.
extern "C" void func_80066FB0(CpuContext* ctx);
extern "C" void func_96DD0F80(CpuContext* ctx);
extern "C" void func_96DD39E4(CpuContext* ctx);
extern "C" void func_96DD5540(CpuContext* ctx);
extern "C" void func_96DDAC64(CpuContext* ctx);
namespace {
void PartnerCommand(int who, CpuContext* ctx, void (*own)(CpuContext*)) {
    if (StandInPlaying() == who) {
        const uint32_t self = ctx->gpr[3];
        func_80066FB0(ctx);
        if (ctx->gpr[3] != 0) {
            static bool s_logged = false;
            if (!s_logged) {
                s_logged = true;
                RT_LOGF(RT_TAG_OS, "cheats: play as: character %d gave Ashley a command (Leon's X key)\n", who);
            }
            return;
        }
        ctx->gpr[3] = self;
        // Krauser's arm attack in a rifle scope left the scope camera on for good (OPPO Adreno 610,
        // 1.1, semi-auto; Windows repro: X in the scope, routine 0x0A, Status_flg scope bit stays): his
        // Mercenaries moves never meet a scope, so nothing in them ends it. In a scope or look-through
        // view (Status_flg STA_LOOK_THROUGH 16, STA_SCOPE_CAMERA 25) the key does nothing, as for Leon.
        uint32_t pG = 0, st0 = 0;
        if (Memory::TryRead32(Cheats::kPGAddress, pG) && Cheats::GuestPointer(pG) && Memory::TryRead32(pG + 0x501Cu, st0) &&
            (st0 & ((0x80000000u >> 16) | (0x80000000u >> 25))) != 0) {
            static bool s_loggedScope = false;
            if (!s_loggedScope) {
                s_loggedScope = true;
                RT_LOGF(RT_TAG_OS, "cheats: play as: character %d's X key skipped in the scope\n", who);
            }
            ctx->gpr[3] = 0;
            return;
        }
    }
    own(ctx);
}
} // namespace
extern "C" void PlayAs_PartnerCmd_96DD0F80(CpuContext* ctx) { PartnerCommand(2, ctx, func_96DD0F80); }
extern "C" void PlayAs_PartnerCmd_96DD39E4(CpuContext* ctx) { PartnerCommand(3, ctx, func_96DD39E4); }
extern "C" void PlayAs_PartnerCmd_96DD5540(CpuContext* ctx) { PartnerCommand(4, ctx, func_96DD5540); }
extern "C" void PlayAs_PartnerCmd_96DDAC64(CpuContext* ctx) { PartnerCommand(5, ctx, func_96DDAC64); }
REGISTER_NATIVE_FUNCTION_AS(0x96DD0F80, PlayAs_PartnerCmd_96DD0F80, "PlayAs_PartnerCmd_96DD0F80");
REGISTER_NATIVE_FUNCTION_AS(0x96DD39E4, PlayAs_PartnerCmd_96DD39E4, "PlayAs_PartnerCmd_96DD39E4");
REGISTER_NATIVE_FUNCTION_AS(0x96DD5540, PlayAs_PartnerCmd_96DD5540, "PlayAs_PartnerCmd_96DD5540");
REGISTER_NATIVE_FUNCTION_AS(0x96DDAC64, PlayAs_PartnerCmd_96DDAC64, "PlayAs_PartnerCmd_96DDAC64");

// Play As Ada in Leon's story: her face went missing in the Del Lago "CUT" QTE (OPPO CPH2565, 1.0).
// cPlAda::setFace (0x96DD0A40, r3 the player, r4 the face: 0 neutral, 1 / 2 blend the player archive's
// shapes 0x62 / 0x63 onto Body +0x7B8's face model +0x10). As a stand-in her face model may come from
// another archive than those shapes (her outfits do), so the blend can wreck it. Not proven; the face
// stays neutral instead (1 / 2 skipped, 0 still ends any blend) and the log says so once per scene.
extern "C" void func_96DD0A40(CpuContext* ctx);
extern "C" void PlayAs_AdaFace_96DD0A40(CpuContext* ctx) {
    static int s_loggedRoom = -1;
    const uint32_t face = ctx->gpr[4];
    if ((face == 1u || face == 2u) && StandInPlaying() != 0) {
        const int room = GameTweaks::CurrentRoom();
        if (room != s_loggedRoom) {
            s_loggedRoom = room;
            RT_LOGF(RT_TAG_OS, "cheats: play as: Ada's face shape %u skipped (room %X)\n", face, room);
        }
        return;
    }
    func_96DD0A40(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x96DD0A40, PlayAs_AdaFace_96DD0A40, "PlayAs_AdaFace_96DD0A40");

// Play As Wesker: cPlWesker::setFace (0x96DDA8E4, pl0d.rel; r3 the player, r4 the face) blends the player
// archive's shapes 0x62 / 0x63 onto his head (Body +0x7B8 -> +0x10, archive entry 8) for faces 1 / 2. His
// head has no shape table (entry 8's shape offset is 0 on the disc) while 0x62 / 0x63 are Leon-style shapes
// (2026-11-22, generated/play-as-audit/tools/face_tables.py), so the blend would move his vertices by
// whatever lies at the head's start: Ada's face went missing that way in a QTE. As for her, the face stays
// neutral while he stands in (1 / 2 skipped, 0 still ends any blend).
extern "C" void func_96DDA8E4(CpuContext* ctx);
extern "C" void PlayAs_WeskerFace_96DDA8E4(CpuContext* ctx) {
    static int s_loggedRoom = -1;
    const uint32_t face = ctx->gpr[4];
    if ((face == 1u || face == 2u) && StandInPlaying() != 0) {
        const int room = GameTweaks::CurrentRoom();
        if (room != s_loggedRoom) {
            s_loggedRoom = room;
            RT_LOGF(RT_TAG_OS, "cheats: play as: Wesker's face shape %u skipped (room %X)\n", face, room);
        }
        return;
    }
    func_96DDA8E4(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x96DDA8E4, PlayAs_WeskerFace_96DDA8E4, "PlayAs_WeskerFace_96DDA8E4");

// Play As Krauser: endless arrows, as in The Mercenaries. cItemMgr::trigger(cItem*) (0x80035D58, r3 the
// item manager, r4 the armed item, id u16 at +0) spends one shot; for the bow (0x52) that is one arrow
// (item 0x72) dumped from the case. While Krauser plays the story the bow's shot is allowed without it.
// The bow's aim key still needs an arrow in the case (Krauser's kit brings 20).
extern "C" void func_80035D58(CpuContext* ctx);
// Infinite Ammo takes the same way for the bow in any hands (Ada's bowgun in Separate Ways, OPPO
// CPH1933 1.1: arrows still ran out). KeepArrows raised the stacks once a frame, after the shot had
// already spent the arrow; skipping the dump keeps them as they are.
extern "C" void PlayAs_Trigger_80035D58(CpuContext* ctx) {
    uint16_t id = 0;
    if (Cheats::GuestPointer(ctx->gpr[4]) && Cheats::Read16(ctx->gpr[4], id) && id == kBowItem) {
        const bool krauser = StandInPlaying() == 4;
        const bool infinite = Cheats::Available() && Cheats::InfiniteAmmo() && !GameTweaks::SpeedrunMode();
        if (krauser || infinite) {
            static bool s_logged[2] = {};
            if (!s_logged[krauser ? 0 : 1]) {
                s_logged[krauser ? 0 : 1] = true;
                if (krauser) {
                    RT_LOGF(RT_TAG_OS, "cheats: play as: Krauser's bow shot, arrow kept\n");
                } else {
                    RT_LOGF(RT_TAG_OS, "cheats: infinite ammo: bow shot, arrow kept\n");
                }
            }
            ctx->gpr[3] = 1;
            return;
        }
    }
    func_80035D58(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80035D58, PlayAs_Trigger_80035D58, "PlayAs_Trigger_80035D58");

// The case path: a stand-in's own weapon as it is; one of Leon's: first Leon's case file of it (the
// builder run as Leon into kCaseLeonPath, the file read to kCaseLeonAddr), then the stand-in's file of
// CaseClass, which the case loads itself.
bool DvdReadWholeFile(const std::string& dvdPath, std::vector<uint8_t>& out);  // hle/storage/dvd.cpp
extern "C" void GxNotifyGuestRamDmaWrite(uint32_t addr, uint32_t size);           // hle/gx/gx_objects.cpp

extern "C" void func_96DA2458(CpuContext* ctx);
extern "C" void PlayAs_CaseWeaponPath_96DA2458(CpuContext* ctx) {
    const int who = StandInPlaying();
    const uint32_t no = ctx->gpr[4] & 0xFFu;
    // The grenades as CaseModel sees them (0x13: the stand-in's own). Without it the case loaded the own
    // TMP's file (CaseClass) while the init built the grenade: HUNK's case froze on the incendiary grenade
    // (unmapped reads at 0x80151F98 under commonScreenMatSub, 2026-11-19).
    const bool grenade = no == 0x16 || no == 0x17 || no == 0x19 || no == 0x1F || no == 0x20;
    uint32_t pG = 0, word = 0;
    if (who == 0 || CaseOwnsWeapon(who, grenade ? 0x13u : no) || !Memory::TryRead32(Cheats::kPGAddress, pG) ||
        !Memory::TryRead32(pG + Cheats::kPlTypeOffset, word)) {
        func_96DA2458(ctx);
        return;
    }
    const uint32_t buffer = ctx->gpr[3];
    if (g_caseLeon.no != no) {
        g_caseLeon = CaseLeon{};
        Memory::Write8(kCaseLeonPath, 0);
        const PlTypeSwapScope plTypeSwap;
        Memory::TryWrite32(pG + Cheats::kPlTypeOffset, word & 0x00FFFFFFu);
        ctx->gpr[3] = kCaseLeonPath;
        ctx->gpr[4] = no;
        func_96DA2458(ctx);
        uint32_t now = 0;
        if (Memory::TryRead32(pG + Cheats::kPlTypeOffset, now)) {
            Memory::TryWrite32(pG + Cheats::kPlTypeOffset, (now & 0x00FFFFFFu) | (word & 0xFF000000u));
        }
        std::string path;
        if (const uint8_t* text = Memory::GetPointer(kCaseLeonPath, 0xF0u)) {
            for (uint32_t i = 0; i < 0xF0u && text[i] != 0; ++i) path.push_back(static_cast<char>(text[i]));
        }
        std::vector<uint8_t> bytes;
        uint8_t* dst = nullptr;
        if (!path.empty() && DvdReadWholeFile(path[0] == '/' ? path : "/" + path, bytes) && bytes.size() >= 0x20 &&
            bytes.size() <= kCaseLeonMax && (dst = Memory::GetPointer(kCaseLeonAddr, bytes.size())) != nullptr) {
            std::memcpy(dst, bytes.data(), bytes.size());
            GxNotifyGuestRamDmaWrite(kCaseLeonAddr, static_cast<uint32_t>(bytes.size()));  // same address, new texture
            g_caseLeon.no = no;
            g_caseLeon.size = bytes.size();
        }
        RT_LOGF(RT_TAG_OS, "cheats: play as: case: Leon's file of weapon %u: %s %s (%zu bytes)\n", no, path.c_str(),
                g_caseLeon.no == no ? "read" : "missing", g_caseLeon.size);
    }
    ctx->gpr[3] = buffer;
    ctx->gpr[4] = CaseClass(who, no);
    func_96DA2458(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x96DA2458, PlayAs_CaseWeaponPath_96DA2458, "PlayAs_CaseWeaponPath_96DA2458");

// Play As case items: the sub screen's common data (0x8007B634) loads SS/<lang>/ss_pzzl.dat for Leon and
// Ashley (pl_type 0/1) and omk_pzzl.dat (the extra games' set) for anyone else, at room start and again
// each time the case closes (the case's work area overwrites it). The extra games' set lacks Leon's
// weapons: with them in the case (bought, or the All Weapons test) the second case opening showed them
// as yellow untextured models (Chicago Typewriter, launchers) or crashed in their skinning (0x80162E28).
// A stand-in in the story reads Leon's set: both loaders (0x8007B634, and 0x8007C774 as the case closes)
// pass the name to the sub screen path builder 0x8007B8FC (r3 owner, r4 name; makes SS/<lang>/<name>).
// Only the name is swapped: the whole loaders seen as Leon also gave the case Leon's weapon (ss_wep02).
extern "C" void func_8007B8FC(CpuContext* ctx);
extern "C" void PlayAs_SubScreenName_8007B8FC(CpuContext* ctx) {
    constexpr uint32_t kSsPzzl = 0x802B2284u;   // "ss_pzzl.dat"
    constexpr uint32_t kOmkPzzl = 0x802B2290u;  // "omk_pzzl.dat"
    if (ctx->gpr[4] == kOmkPzzl && StandInPlaying() != 0) {
        ctx->gpr[4] = kSsPzzl;
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            RT_LOGF(RT_TAG_OS, "cheats: play as: case items from Leon's set (ss_pzzl.dat)\n");
        }
    }
    func_8007B8FC(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007B8FC, PlayAs_SubScreenName_8007B8FC, "PlayAs_SubScreenName_8007B8FC");

// Play As hands for Ada. Leon's weapon files bring hand models made for his skeleton: the grip hand
// (initWepHand, Body->pWepHand) that setRightHand(1) shows, and for his handguns a support hand passed to
// setLeftHand as a pointer. On Ada's smaller skeleton the grip stood beside the weapon as a third hand
// (a Leon hand left behind). While she holds one of Leon's weapons the right hand is her own open hand
// (0), as for HUNK, Krauser and Wesker, and the left one of her archive for the kind of weapon (4, 1, 2).
// Her grips copied from her own weapon files (wep38/39/40 entries 0xA/0xC/0xD) drew white: New Game as
// Ada holds Leon's handgun, which since 1.1 is read from Leon's file (1.1 test, 2026-11-22).
// A .drs file starts with a 0x400 header: part 0 (the archive, loaded at the destination) has its size at
// +0x24 and its file offset at +0x2C. An archive entry n sits at archive + be32(archive + 4n).
bool DvdReadWholeFile(const std::string& dvdPath, std::vector<uint8_t>& out);  // hle/storage/dvd.cpp

namespace {
uint32_t Be32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | p[3];
}

// Copies part 0 (the archive) of the .drs file `path` to guest `addr`; its size, 0 when it failed.
size_t LoadDrsArchive(const char* path, uint32_t addr, size_t max) {
    std::vector<uint8_t> bytes;
    if (!DvdReadWholeFile(path, bytes) || bytes.size() < 0x400) return 0;
    const uint32_t size = Be32(&bytes[0x24]);
    const uint32_t offset = Be32(&bytes[0x2C]);
    uint8_t* dst = nullptr;
    if (size == 0 || size > max || offset < 0x400 || offset > bytes.size() || size > bytes.size() - offset ||
        (dst = Memory::GetPointer(addr, size)) == nullptr) {
        return 0;
    }
    std::memcpy(dst, bytes.data() + offset, size);
    GxNotifyGuestRamDmaWrite(addr, size);  // the GX texture cache keys by address
    return size;
}

constexpr uint32_t kAdaLeftHand[3] = {4, 1, 2};

// Which of Ada's grips fits Leon's weapon `no`: 1 the TMP's, 2 the rifle's (shotguns, rifles, the
// Chicago Typewriter, the launchers), 0 the handgun's (handguns, magnums, the rest).
int AdaGripKind(uint32_t no) {
    switch (no) {
    case 0x0B: case 0x14:
        return 1;
    case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0C: case 0x0D: case 0x0E: case 0x15: case 0x18:
    case 0x1E: case 0x1F: case 0x20: case 0x21:
        return 2;
    default:
        return 0;
    }
}

} // namespace

// True when `obj` is the player in the room (pPL, r13 - 0x6B80). The case screen builds a second Ada
// holding her own stand-in weapon (see CaseWeapon): her own hands are right there, and Leon's weapon in
// the room must not change them. With Leon's shotgun or TMP held, the case model took the room's grip
// too and opening the case again crashed in the model's skinning (0x80162E28, wild copies).
static bool IsRoomPlayer(uint32_t obj) {
    uint32_t pl = 0;
    return Memory::TryRead32(0x8043DEA0u, pl) && pl != 0 && pl == obj;
}

// Ada's setRightHand(r3 this, r4 no) (module 82 pl02, 0x96DD0778): 1 = Body(+0x7B8)->pWepHand(+0xC),
// 0 and 2..9 her open hand (archive entry 0x11), 10 entry 0x19, 12 and up a model pointer.
extern "C" void func_96DD0778(CpuContext* ctx);
extern "C" void PlayAs_AdaRightHand_96DD0778(CpuContext* ctx) {
    if (ctx->gpr[4] == 1 && g_standInHoldsLeonWeapon && StandInPlaying() == 2 && IsRoomPlayer(ctx->gpr[3])) {
        ctx->gpr[4] = 0;
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            RT_LOGF(RT_TAG_OS, "cheats: play as: Ada's grip of Leon's weapon %u -> her open hand\n", g_standInWeaponNo);
        }
    }
    func_96DD0778(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x96DD0778, PlayAs_AdaRightHand_96DD0778, "PlayAs_AdaRightHand_96DD0778");

// Ada's setLeftHand(r3 this, r4 no; 0x63 restores the previous one) (0x96DD089C) knows 0, 1, 2 and 4..8;
// any other number is taken as the part itself. Leon's support hands (past 5, his handguns pass a model
// pointer) become her own left hand for the kind of weapon; with her own weapons Leon's hand 3 (the TMP
// grip) becomes her grip 1 and a value past her table her hand 4. Each number is logged once.
extern "C" void func_96DD089C(CpuContext* ctx);
extern "C" void PlayAs_AdaLeftHand_96DD089C(CpuContext* ctx) {
    const uint32_t no = ctx->gpr[4];
    if (StandInPlaying() == 2 && no != 0x63u) {
        uint32_t mapped = no;
        if (g_standInHoldsLeonWeapon && no > 5 && IsRoomPlayer(ctx->gpr[3])) {
            mapped = kAdaLeftHand[AdaGripKind(g_standInWeaponNo)];
        } else if (no == 3) {
            mapped = 1;
        } else if (no > 8) {
            mapped = 4;
        }
        static uint32_t s_seen = 0;
        const uint32_t bit = 1u << (no > 8 ? 9 : no);
        if ((s_seen & bit) == 0) {
            s_seen |= bit;
            RT_LOGF(RT_TAG_OS, "cheats: play as: Ada left hand %X -> %u\n", no, mapped);
        }
        ctx->gpr[4] = mapped;
    }
    func_96DD089C(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x96DD089C, PlayAs_AdaLeftHand_96DD089C, "PlayAs_AdaLeftHand_96DD089C");

// HUNK's setLeftHand (pl06, 0x96DD353C) knows 0, 1 and 3, Wesker's (pl0d, 0x96DDA7B8) 0, 2 and 4; any
// other value is taken as the part itself (Krauser's, 0x96DD58F8, knows 0..9). Leon's weapons ask for
// hands they lack (the shotgun module wep07 asks 2: HUNK read a part at address 2 and crashed), which
// become their gripping hand (HUNK 1, Wesker 2); part pointers and 0x63 (restore) pass.
namespace {
void StandInLeftHand(int who, CpuContext* ctx, void (*fn)(CpuContext*)) {
    const uint32_t no = ctx->gpr[4];
    if (no != 0x63u && no < 0x80000000u && StandInPlaying() == who) {
        uint32_t mapped = no;
        if (who == 3 && no != 0 && no != 1 && no != 3) mapped = 1;
        if (who == 5 && no != 0 && no != 2 && no != 4) mapped = 2;
        static uint32_t s_seen[6] = {};
        const uint32_t bit = 1u << (no > 30 ? 31 : no);
        if (mapped != no && (s_seen[who] & bit) == 0) {
            s_seen[who] |= bit;
            RT_LOGF(RT_TAG_OS, "cheats: play as: character %d left hand %X -> %u\n", who, no, mapped);
        }
        ctx->gpr[4] = mapped;
    }
    fn(ctx);
}
// Their setRightHand (HUNK 0x96DD3464, Krauser 0x96DD5810, Wesker 0x96DDA6E0): 1 = Body(+0x7B8)->pWepHand
// (+0xC), the grip the weapon file brings. Leon's grips are made for his skeleton (on Ada a third hand);
// HUNK switched in holding Leon's Striker crashed in the skinning (0x80162E28). While one of Leon's weapons
// is held the room's player keeps the open hand (0).
void StandInRightHand(int who, CpuContext* ctx, void (*fn)(CpuContext*)) {
    if (ctx->gpr[4] == 1 && g_standInHoldsLeonWeapon && StandInPlaying() == who && IsRoomPlayer(ctx->gpr[3])) {
        ctx->gpr[4] = 0;
        static bool s_logged[6] = {};
        if (!s_logged[who]) {
            s_logged[who] = true;
            RT_LOGF(RT_TAG_OS, "cheats: play as: character %d grip of Leon's weapon %u -> open hand\n", who,
                    g_standInWeaponNo);
        }
    }
    fn(ctx);
}
} // namespace
extern "C" void func_96DD3464(CpuContext* ctx);
extern "C" void func_96DD5810(CpuContext* ctx);
extern "C" void func_96DDA6E0(CpuContext* ctx);
extern "C" void PlayAs_HunkRightHand_96DD3464(CpuContext* ctx) { StandInRightHand(3, ctx, func_96DD3464); }
extern "C" void PlayAs_KrauserRightHand_96DD5810(CpuContext* ctx) { StandInRightHand(4, ctx, func_96DD5810); }
extern "C" void PlayAs_WeskerRightHand_96DDA6E0(CpuContext* ctx) { StandInRightHand(5, ctx, func_96DDA6E0); }
REGISTER_NATIVE_FUNCTION_AS(0x96DD3464, PlayAs_HunkRightHand_96DD3464, "PlayAs_HunkRightHand_96DD3464");
REGISTER_NATIVE_FUNCTION_AS(0x96DD5810, PlayAs_KrauserRightHand_96DD5810, "PlayAs_KrauserRightHand_96DD5810");
REGISTER_NATIVE_FUNCTION_AS(0x96DDA6E0, PlayAs_WeskerRightHand_96DDA6E0, "PlayAs_WeskerRightHand_96DDA6E0");
extern "C" void func_96DD353C(CpuContext* ctx);
extern "C" void func_96DDA7B8(CpuContext* ctx);
extern "C" void PlayAs_HunkLeftHand_96DD353C(CpuContext* ctx) { StandInLeftHand(3, ctx, func_96DD353C); }
extern "C" void PlayAs_WeskerLeftHand_96DDA7B8(CpuContext* ctx) { StandInLeftHand(5, ctx, func_96DDA7B8); }
REGISTER_NATIVE_FUNCTION_AS(0x96DD353C, PlayAs_HunkLeftHand_96DD353C, "PlayAs_HunkLeftHand_96DD353C");
REGISTER_NATIVE_FUNCTION_AS(0x96DDA7B8, PlayAs_WeskerLeftHand_96DDA7B8, "PlayAs_WeskerLeftHand_96DDA7B8");

// Play As room cameras: CameraControl::areaHitCheck (0x8001B4EC) picks a room's camera areas (their cuts
// and light sets) by a character bit: 2 while Ashley follows, else by pl_type (Leon 1, Ashley 4, Ada,
// HUNK, Wesker, Krauser their own). The story's rooms only mark Leon's areas, so a stand-in found none and
// ran on the default camera: every aim blended from that camera into the shoulder camera and the
// character slid across the screen first (and the default light was used). The call sees Leon.
extern "C" void func_8001B4EC(CpuContext* ctx);
bool g_areaSeesLeon = false;      // inside areaHitCheck with pl_type 0
uint32_t g_areaRealPlType = 0;   // the real pl_type byte (top byte of the word) meanwhile
extern "C" void PlayAs_AreaHitCheck_8001B4EC(CpuContext* ctx) {
    uint32_t pG = 0, word = 0;
    if (StandInPlaying() == 0 || !Memory::TryRead32(Cheats::kPGAddress, pG) ||
        !Memory::TryRead32(pG + Cheats::kPlTypeOffset, word)) {
        func_8001B4EC(ctx);
        return;
    }
    const PlTypeSwapScope plTypeSwap;
    Memory::TryWrite32(pG + Cheats::kPlTypeOffset, word & 0x00FFFFFFu);
    g_areaRealPlType = word & 0xFF000000u;
    g_areaSeesLeon = true;
    func_8001B4EC(ctx);
    g_areaSeesLeon = false;
    uint32_t now = 0;
    if (Memory::TryRead32(pG + Cheats::kPlTypeOffset, now)) {
        Memory::TryWrite32(pG + Cheats::kPlTypeOffset, (now & 0x00FFFFFFu) | (word & 0xFF000000u));
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x8001B4EC, PlayAs_AreaHitCheck_8001B4EC, "PlayAs_AreaHitCheck_8001B4EC");

// Ada's slide before each aim with Leon's handgun (weapon 2): the handgun routine's ready step (Leon's
// wep02 0x96E14188 and 0x96E14F74; Ada's wep38 has the same pair) turns on the gun object's character
// collision ("Oba", cAtariInfo flag 0x200 at Wep(+0x7AC)->m_pWep(+0x34)->atari(+0x2B4)+0x1A) for weapon
// number 2 only, from either file. EmAtCheck then pushes the player against the gun: sized for Leon, it
// pushed Ada aside. Her Punisher (1) never turns it on and did not slide (user test 2026-11-17). While Ada
// stands in, the ready step leaves it off.
namespace {
void AdaHandgunObaOff() {
    uint32_t pl = 0, wep = 0, obj = 0;
    uint16_t flag = 0;
    if (StandInPlaying() != 2 || !Memory::TryRead32(0x8043DEA0u, pl) || !Cheats::GuestPointer(pl) ||
        !Memory::TryRead32(pl + 0x7ACu, wep) || !Cheats::GuestPointer(wep) || !Memory::TryRead32(wep + 0x34u, obj) ||
        !Cheats::GuestPointer(obj) || !Cheats::Read16(obj + 0x2B4u + 0x1Au, flag) || (flag & 0x200u) == 0) {
        return;
    }
    Memory::Write16(obj + 0x2B4u + 0x1Au, static_cast<uint16_t>(flag & ~0x200u));
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        RT_LOGF(RT_TAG_OS, "cheats: play as: Ada's handgun collision (Oba) left off for weapon 2\n");
    }
}
} // namespace

// Both ready steps of Leon's wep02 that turn Oba on (0x96E14188 and 0x96E14F74).
extern "C" void func_96E14188(CpuContext* ctx);
extern "C" void PlayAs_AdaHandgunReady_96E14188(CpuContext* ctx) {
    func_96E14188(ctx);
    AdaHandgunObaOff();
}
REGISTER_NATIVE_FUNCTION_AS(0x96E14188, PlayAs_AdaHandgunReady_96E14188, "PlayAs_AdaHandgunReady_96E14188");
extern "C" void func_96E14F74(CpuContext* ctx);
extern "C" void PlayAs_AdaHandgunReady2_96E14F74(CpuContext* ctx) {
    func_96E14F74(ctx);
    AdaHandgunObaOff();
}
REGISTER_NATIVE_FUNCTION_AS(0x96E14F74, PlayAs_AdaHandgunReady2_96E14F74, "PlayAs_AdaHandgunReady2_96E14F74");


// The shoulder camera's type (CameraQuasiFPS::checkCameraType 0x8008A498, r3 camera: +0x1B4 by pl_type, Ada 3,
// Leon 0) is checked again inside areaHitCheck while it sees Leon: the camera flipped to Leon's tables and
// blended back to Ada's, and she slid across the screen before each aim. Inside that call it sees the real
// pl_type.
extern "C" void func_8008A498(CpuContext* ctx);
extern "C" void PlayAs_CameraType_8008A498(CpuContext* ctx) {
    uint32_t pG = 0, now = 0;
    if (!g_areaSeesLeon || !Memory::TryRead32(Cheats::kPGAddress, pG) ||
        !Memory::TryRead32(pG + Cheats::kPlTypeOffset, now)) {
        func_8008A498(ctx);
        return;
    }
    Memory::TryWrite32(pG + Cheats::kPlTypeOffset, (now & 0x00FFFFFFu) | g_areaRealPlType);
    func_8008A498(ctx);
    if (Memory::TryRead32(pG + Cheats::kPlTypeOffset, now)) {
        Memory::TryWrite32(pG + Cheats::kPlTypeOffset, now & 0x00FFFFFFu);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x8008A498, PlayAs_CameraType_8008A498, "PlayAs_CameraType_8008A498");


// Play As enemies: the enemy/NPC loader (0x80140BA0, r4 enemy no) picks its file table by pl_type (80
// entries of 8 bytes: Leon 0x802DD2E0, Ada 0x802DD560, HUNK/Wesker 0x802DD7E0, Krauser 0x802DDA60). Ada's
// holds the Separate Ways variants (em/ema3, em/ema5) with low models (em/ema3low, em/ema5low) the Wii disc
// does not have: the open failed and the read of the empty file info aborted on opening the case in r100.
// Leon's story keeps Leon's enemies, so the loader sees pl_type 0 for the call.
extern "C" void func_80140BA0(CpuContext* ctx);
void ReadStandInGrabs(int who);  // below, with the Ganados' moves
extern "C" void PlayAs_EnemyData_80140BA0(CpuContext* ctx) {
    uint32_t pG = 0, word = 0;
    if (StandInPlaying() == 0 || !Memory::TryRead32(Cheats::kPGAddress, pG) ||
        !Memory::TryRead32(pG + Cheats::kPlTypeOffset, word)) {
        func_80140BA0(ctx);
        return;
    }
    // Ada's and Krauser's grabs for the story Ganados (StandInTick points the archives at the copy).
    ReadStandInGrabs(static_cast<int>(word >> 24));
    const PlTypeSwapScope plTypeSwap;
    Memory::TryWrite32(pG + Cheats::kPlTypeOffset, word & 0x00FFFFFFu);
    func_80140BA0(ctx);
    uint32_t now = 0;
    if (Memory::TryRead32(pG + Cheats::kPlTypeOffset, now)) {
        Memory::TryWrite32(pG + Cheats::kPlTypeOffset, (now & 0x00FFFFFFu) | (word & 0xFF000000u));
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80140BA0, PlayAs_EnemyData_80140BA0, "PlayAs_EnemyData_80140BA0");

// Play As cutscenes: the event start (0x800F4F2C) registers Leon's body bins for the scene's models only
// when pl_type is 0, from the player archive (pG->pPlayer, pG + 0x50; costume byte 4 uses another
// archive). The scenes' motions and parts are Leon's, so with a stand-in the scene had no body and the
// r100 police scene aborted. Leon's archive (Em/pl00.drs, read whole, no compression) is kept in free
// MEM2 between the REL homes (to 0x97000000) and the FST reservation, and the event start sees Leon
// with that archive for the call: the cutscenes show Leon. Only part 0 of the .drs (the archive itself,
// LoadDrsArchive above) goes there: a copy with the file's 0x400 header left the scenes without Leon.
extern "C" void func_800F4F2C(CpuContext* ctx);

namespace {
constexpr uint32_t kLeonArcAddr = 0x97000000u;
constexpr uint32_t kLeonArcMax = 0x00800000u;  // pl00.drs part 0 is ~1 MB; em10's finishers at 0x97800000
constexpr uint32_t kPlayerArcOffset = 0x50u;

bool LeonArchiveReady() {
    static int s_state = 0;  // 0 not read yet, 1 in place, -1 failed
    if (s_state == 0) {
        const size_t size = LoadDrsArchive("/Em/pl00.drs", kLeonArcAddr, kLeonArcMax);
        s_state = size != 0 ? 1 : -1;
        RT_LOGF(RT_TAG_OS, "cheats: play as: Leon's archive for the cutscenes %s (%zu bytes)\n",
                s_state == 1 ? "in place" : "missing", size);
    }
    return s_state == 1;
}
} // namespace

// Play As cutscenes with the stand-in's own model. The event start registers Leon's files by name from the
// player archive (em/pl00/pl000a.bin = entry 4, pl000a.tpl = 5, ...). For the call the copy of Leon's
// archive points 4 and 5 at the stand-in's body; its other parts (as its setModel in pl06/pl02/pl0a/pl0d.rel
// makes them: model entry, texture entry) are registered after it under names of their own (EvtMgr.SetBin
// 0x800F7850, EvtMgr 0x8032F83C) and hang where the event hangs Leon's parts (see SetPartsSub below).
// The stand-ins' first bones (body, arms, legs; Ada's, Krauser's and Wesker's fingers) are Leon's, so his
// motions move them, but some of their vertices hang on bones Leon has for other things (HUNK's thighs on
// 28/29 and forearms on 98/99, Leon's eyes and chest; Wesker's coat on 64..87, Leon's face): the motion
// threw HUNK's hands and legs away. For the event those weights go to the nearest bone up the stand-in's
// tree that is Leon's too, and come back when it ends.
namespace {
// The stand-in's parts are the ones its game model has now (cModel::pModelInfo +0x15C of pPL 0x8043DEA0,
// cModelInfo: model +0x0C, texture +0x10, next +0x14): head, face, hands of the outfit it wears (Ada's
// outfits come from other archives with other entries; a fixed table hung wrong parts and crashed).
constexpr uint32_t kPlayerPtr = 0x8043DEA0u;
// Leon's parts on the player's body pl0000 in every story event. They stay off; at the first of them all the
// stand-in's parts are hung under names of their own (ev0002's name would move the body's bones to Leon's
// proportions). Wesker has 7, Krauser more.
const char* const kLeonPartSlots[] = {"pl000d", "ev0001", "ev0003", "ev000e", "ev0002"};
constexpr int kMaxOwnParts = 24;
bool g_ownBodyScene = false;  // the running event shows the stand-in's own body
int g_ownPartCount = 0;       // parts registered for it
uint32_t g_ownModel[24] = {}; // their model and texture, named in the bin table only while hung
uint32_t g_ownTex[24] = {};
bool g_ownPartsHung = false;  // hung on the body already
constexpr uint32_t kOwnNames = 0x97840000u;  // "px/b0".. (models), "px/t0".. (textures), "pxprt0".. (parts)
uint32_t OwnBinName(int i) { return kOwnNames + 8u * static_cast<uint32_t>(i); }
uint32_t OwnTplName(int i) { return kOwnNames + 0x100u + 8u * static_cast<uint32_t>(i); }
uint32_t OwnPartName(int i) { return kOwnNames + 0x200u + 8u * static_cast<uint32_t>(i); }
std::vector<std::pair<uint32_t, uint8_t>> g_weightUndo;  // weight bone bytes changed for the event

uint32_t ArcEntry(uint32_t arc, uint32_t no) {  // 0 when empty
    uint32_t ofs = 0, next = 0;
    if (!Memory::TryRead32(arc + no * 4u, ofs) || !Memory::TryRead32(arc + (no + 1u) * 4u, next) || ofs == 0 ||
        next <= ofs) {
        return 0;
    }
    return arc + ofs;
}
bool IsBin(uint32_t p) {
    uint32_t v = 0;
    return p != 0 && Memory::TryRead32(p + 0x3Cu, v) && (v == 0x20010801u || v == 0x20030817u || v == 0x20030818u);
}
uint32_t BinField(uint32_t bin, uint32_t off) {  // a file offset, or a pointer once relocated
    const uint32_t v = Memory::Read32(bin + off);
    return v >= 0x80000000u ? v : bin + v;
}
std::vector<uint8_t> BinParents(uint32_t bin) {
    std::vector<uint8_t> out(Memory::Read8(bin + 0x19u));
    const uint32_t head = BinField(bin, 0x00u);
    for (size_t i = 0; i < out.size(); ++i) out[i] = Memory::Read8(head + 16u * static_cast<uint32_t>(i) + 1u);
    return out;
}

void RestoreSceneWeights() {
    for (auto it = g_weightUndo.rbegin(); it != g_weightUndo.rend(); ++it) Memory::Write8(it->first, it->second);
    g_weightUndo.clear();
}

// Moves the weights of `bin` (skinned on the stand-in's skeleton `own`) off the bones that are not Leon's.
int RetargetWeights(uint32_t bin, const std::vector<uint8_t>& own, const std::vector<uint8_t>& leon) {
    std::vector<int8_t> ok(own.size(), -1);  // -1 not known yet, 0 no, 1 Leon's bone too (and its parents)
    std::function<bool(size_t, int)> isOk = [&](size_t i, int depth) -> bool {
        if (ok[i] >= 0) return ok[i] == 1;
        const bool v = i < leon.size() && own[i] == leon[i] && depth < 64 &&
                       (own[i] == 0xFF || (own[i] < own.size() && isOk(own[i], depth + 1)));
        ok[i] = v ? 1 : 0;
        return v;
    };
    auto target = [&](uint8_t id) -> uint8_t {
        for (int depth = 0; depth < 64 && id < own.size() && !isOk(id, 0); ++depth) id = own[id];
        return id < own.size() ? id : 0;
    };
    const uint32_t weights = BinField(bin, 0x14u);
    const uint32_t ext = Memory::Read16(bin + 0x2Au);
    const uint32_t count = ext > 0xFFu ? ext : Memory::Read8(bin + 0x18u);
    int moved = 0;
    for (uint32_t j = 0; j < count; ++j) {
        const uint32_t e = weights + j * 8u;
        const uint32_t n = std::min<uint32_t>(Memory::Read8(e + 3u), 3u);
        for (uint32_t k = 0; k < n; ++k) {
            const uint8_t id = Memory::Read8(e + k);
            if (id >= own.size() || isOk(id, 0)) continue;
            g_weightUndo.emplace_back(e + k, id);
            Memory::Write8(e + k, target(id));
            ++moved;
        }
    }
    return moved;
}

// The event's bin table (EvtMgr.BinTbl, a DatTbl at EvtMgr 0x8032F83C + 0x140: count, work; entries of 0x3C:
// name[0x30], flag u8 +0x30 (bit 0 in use), type +0x31, count u16 +0x32, dat +0x34, dat2 +0x38) holds 0x140
// names. s20bs00 and r20bs00 bring 310 bins of their own and Leon's 8 come on top, so of the stand-in's parts
// (two names each, registered at the event start) only the first fitted: the others were hung from a failed
// lookup with model 0, and the skinning walked off MEM1 (HUNK s20bs00, 1.2 test, Mali-G610, dump
// 2026-11-22_21-49-13: the scene body's part infos 3 and 4 with model 0; r20bs00 crashed every stand-in the
// same way). Each part now borrows two free slots only for its own SetParts call and gives them back; a
// part that finds none is left off instead.
constexpr uint32_t kEvtBinTbl = 0x8032F83Cu + 0x140u;
constexpr uint32_t kDatWorkSize = 0x3Cu;
void ClearEvtBin(uint32_t entry) {
    for (uint32_t k = 0; k < kDatWorkSize; k += 4) Memory::TryWrite32(entry + k, 0);
}
// A free slot holding guest string `name` -> `data`; 0 when the table has none.
uint32_t TempEvtBin(uint32_t name, uint32_t data) {
    uint32_t num = 0, work = 0;
    if (data == 0 || !Memory::TryRead32(kEvtBinTbl, num) || !Memory::TryRead32(kEvtBinTbl + 4u, work) ||
        !Cheats::GuestPointer(work) || num == 0 || num > 0x400u) {
        return 0;
    }
    for (uint32_t i = 0; i < num; ++i) {
        const uint32_t entry = work + i * kDatWorkSize;
        if ((Memory::Read8(entry + 0x30u) & 1u) != 0) continue;
        ClearEvtBin(entry);
        for (uint32_t c = 0; c < 0x2Fu; ++c) {
            const uint8_t ch = Memory::Read8(name + c);
            if (ch == 0) break;
            Memory::Write8(entry + c, ch);
        }
        Memory::Write8(entry + 0x30u, 1);
        Memory::Write8(entry + 0x31u, 7);
        Memory::Write16(entry + 0x32u, 1);
        Memory::TryWrite32(entry + 0x34u, data);
        return entry;
    }
    return 0;
}

// The stand-in's parts (model, texture) on its game model now, past the body `body`; none when the list
// does not hold that body. With `glow`, a texture-render part (Krauser's arm glow) comes too and *glow is its
// info, whose material the scene copies every frame (CopyGlow).
std::vector<std::pair<uint32_t, uint32_t>> LiveParts(uint32_t body, uint32_t* glow = nullptr) {
    if (glow) *glow = 0;
    std::vector<std::pair<uint32_t, uint32_t>> live;
    uint32_t pl = 0, info = 0;
    bool bodySeen = false;
    if (Memory::TryRead32(kPlayerPtr, pl) && Cheats::GuestPointer(pl) && Memory::TryRead32(pl + 0x15Cu, info)) {
        // The knife (cPlBody::m_pKnife +0x2C, Body at pPL + 0x7B8 on the Wii) stays off: Ada's was shown.
        uint32_t plBody = 0, knife = 0;
        if (Memory::TryRead32(pl + 0x7B8u, plBody) && Cheats::GuestPointer(plBody)) {
            Memory::TryRead32(plBody + 0x2Cu, knife);
        }
        for (int n = 0; n < 24 && Cheats::GuestPointer(info); ++n) {
            const uint32_t model = Memory::Read32(info + 0x0Cu), tex = Memory::Read32(info + 0x10u);
            // be_flag (+0x00) bit 3 draws it: Krauser keeps his mutant arm (0xA/0xB) and 0x18 hidden that way,
            // and the scene showed the arm stretched across the room (r100s03, 2026-11-18).
            // A texture-render material (texBlendTbl +0xE4, flagsDC +0xDC bit 2) only draws with its render
            // target: Krauser's arm glow (0x18, texture 0x19 a 16x8 blue) drew a blue sleeve on his arm.
            const bool texRender = Memory::Read32(info + 0xE4u) != 0 || (Memory::Read16(info + 0xDCu) & 4u) != 0;
            const bool drawn = (Memory::Read32(info) & 8u) != 0 && !texRender;
            if (model == body) {
                bodySeen = true;
            } else if (info != knife && IsBin(model) && IsTpl(tex)) {
                if (drawn) {
                    live.emplace_back(model, tex);
                } else if (texRender && glow && *glow == 0) {
                    *glow = info;  // added last, below
                }
            }
            info = Memory::Read32(info + 0x14u);
        }
    }
    if (!bodySeen) {
        live.clear();
        if (glow) *glow = 0;
    }
    // The glow draws with its own material colour (be_flag 0x20, the pulsing alpha): the parts after it in the
    // chain took that alpha, and Krauser's hands (added after it) showed see-through (r100s03, 2026-11-18).
    // Last in the chain, nothing follows it.
    if (glow && *glow != 0) live.emplace_back(Memory::Read32(*glow + 0x0Cu), Memory::Read32(*glow + 0x10u));
    return live;
}

void WriteGuestString(uint32_t at, const std::string& text) {
    for (size_t c = 0; c <= text.size(); ++c) {
        Memory::Write8(at + static_cast<uint32_t>(c), c < text.size() ? static_cast<uint8_t>(text[c]) : 0);
    }
}

// Krauser's arm glow in a scene: the event's copy of the part (caught in cModel::addModel while the parts are
// hung) takes the game model's material every frame: draw / own colour bits (be_flag 0x08, 0x20), colour
// with the pulsing alpha (+0x8C), invisible_factor (+0xD8), flagsDC bit 2 (+0xDC), blend ratio (+0xDE),
// blend type (+0xE1) and the blend table (+0xE4) naming Krauser's render texture.
uint32_t g_glowLive = 0, g_glowModel = 0, g_glowScene = 0;
bool g_glowCatch = false;

void CopyGlow() {
    if (g_glowLive == 0 || g_glowScene == 0 || Memory::Read32(g_glowLive + 0x0Cu) != g_glowModel ||
        Memory::Read32(g_glowScene + 0x0Cu) != g_glowModel) {
        return;
    }
    const uint32_t flag = (Memory::Read32(g_glowScene) & ~0x28u) | (Memory::Read32(g_glowLive) & 0x28u);
    Memory::Write32(g_glowScene, flag);
    Memory::Write32(g_glowScene + 0x8Cu, Memory::Read32(g_glowLive + 0x8Cu));
    Memory::Write32(g_glowScene + 0xD8u, Memory::Read32(g_glowLive + 0xD8u));
    Memory::Write16(g_glowScene + 0xDCu,
                    (Memory::Read16(g_glowScene + 0xDCu) & ~4u) | (Memory::Read16(g_glowLive + 0xDCu) & 4u));
    Memory::Write16(g_glowScene + 0xDEu, Memory::Read16(g_glowLive + 0xDEu));
    Memory::Write8(g_glowScene + 0xE1u, Memory::Read8(g_glowLive + 0xE1u));
    Memory::Write32(g_glowScene + 0xE4u, Memory::Read32(g_glowLive + 0xE4u));
}
} // namespace

void EndOwnBodyScene() {
    g_ownBodyScene = false;
    g_glowLive = g_glowModel = g_glowScene = 0;
    g_glowCatch = false;
    RestoreSceneWeights();
}

// cModel::addModel 0x8011A628 (r3 the model, r4 the info): catches the scene's copy of the glow part.
extern "C" void func_8011A628(CpuContext* ctx);
extern "C" void PlayAs_AddModel_8011A628(CpuContext* ctx) {
    if (g_glowCatch && g_glowModel != 0 && Cheats::GuestPointer(ctx->gpr[4]) &&
        Memory::Read32(ctx->gpr[4] + 0x0Cu) == g_glowModel) {
        g_glowScene = ctx->gpr[4];
        g_glowCatch = false;
    }
    func_8011A628(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8011A628, PlayAs_AddModel_8011A628, "PlayAs_AddModel_8011A628");

// The event's name: Event::pData (+0x38) starts with its path, "event/evd/r100s03.evd".
std::string EventName(uint32_t evt) {
    uint32_t data = 0;
    std::string name;
    if (!Memory::TryRead32(evt + 0x38u, data) || !Cheats::GuestPointer(data)) return "?";
    for (uint32_t i = 0; i < 40; ++i) {
        const uint8_t c = Memory::Read8(data + i);
        if (c < 0x20 || c > 0x7E) break;
        name.push_back(static_cast<char>(c));
    }
    return name.empty() ? "?" : name;
}

extern "C" void PlayAs_ExeBeginEvt_800F4F2C(CpuContext* ctx) {
    uint32_t pG = 0, sys = 0, word = 0, arc = 0;
    const int want = PlayAsWanted();
    const bool swap = want != 0 && CheatsAllowed() && Memory::TryRead32(Cheats::kPGAddress, pG) &&
                      Cheats::GuestPointer(pG) && Memory::TryRead32(pG + kSystemFlgOffset, sys) &&
                      !ExtraGame(pG, sys) && Memory::TryRead32(pG + Cheats::kPlTypeOffset, word) &&
                      static_cast<int>(word >> 24) == want && Memory::TryRead32(pG + kPlayerArcOffset, arc) &&
                      LeonArchiveReady();
    const std::string eventName = EventName(ctx->gpr[3]);
    Cheats::CutsceneReplayMark(eventName);
    if (want != 0) {
        RT_LOGF(RT_TAG_OS, "cheats: play as: event %s starts, pl_type %u, System_flg %08X, %s\n",
                eventName.c_str(), word >> 24, sys, swap ? "Leon's files for it" : "no swap");
    }
    EndOwnBodyScene();
    if (!swap) {
        func_800F4F2C(ctx);
        return;
    }
    const uint32_t body = ArcEntry(arc, 4), bodyTex = ArcEntry(arc, 5), leonBody = ArcEntry(kLeonArcAddr, 4);
    // Krauser's own body crashed r20bs00 (Ada's fight; the skinning 0x80162E28 read past MEM1 right after the
    // parts were hung, "arm glow not found", crash folder 2026-11-19_13-29-01; no memory dump came back).
    // That scene keeps Leon's model. Ada (pl_type 2) crashed there the same way in 1.0 (Mali-G610, crash
    // 2026-11-20_12-44-51: 0x80162E28 walked 16511 unmapped 64 KiB blocks right after "character 2's own
    // model"), so it is every Play As character there, not only Krauser. s117s00 (Ashley freed in the
    // church) crashed the same way for HUNK (1.1, Mali-G610, crash 2026-11-22_14-12-21: 16511 unmapped
    // blocks from 0x80162E28 as the scene was skipped after 840 frames of "character 3's own model").
    // r209s00 (castle) crashed the same way for HUNK (1.2 test, Mali-G610, crash 2026-11-22_20-25-06: 16511
    // unmapped blocks from 0x80162E28 as the scene was skipped 32 frames in). It was also the only one of
    // his 14 logged scenes with 2 live parts instead of 3; r20bs00 was missing Krauser's arm glow. So a
    // stand-in whose live parts are fewer than it had in an earlier scene keeps Leon's model there as well.
    const bool leonBodyScene = eventName == "event/evd/r20bs00.evd" || eventName == "event/evd/s117s00.evd" ||
                               eventName == "event/evd/r209s00.evd";
    static std::array<int, 16> s_mostParts{};
    const uint32_t who = (word >> 24) & 0x0Fu;
    const int liveParts = IsBin(body) ? static_cast<int>(LiveParts(body).size()) : 0;
    const bool partsShort = !leonBodyScene && liveParts < s_mostParts[who];
    s_mostParts[who] = std::max(s_mostParts[who], liveParts);
    // The P.R.L. 412 (weapon 46) in the stand-in's hands: HUNK's skips of r209s00 (three times) and s117s00
    // crashed with it, scenes with few bins (146, 82), "2 parts" logged each time; with other weapons the
    // same scenes ran (1.2 test, Mali-G610). Not proven; such a scene shows Leon's model.
    uint32_t wepWord = 0;
    const bool prlScene = !leonBodyScene && Memory::TryRead32(pG + 0x4FC0u, wepWord) && (wepWord >> 24) == 46u;
    if (prlScene) RT_LOGF(RT_TAG_OS, "cheats: play as: %s keeps Leon's model (the P.R.L. in hand)\n", eventName.c_str());
    const bool ownBody =
        !leonBodyScene && !partsShort && !prlScene && IsBin(body) && IsTpl(bodyTex) && IsBin(leonBody);
    if (leonBodyScene) RT_LOGF(RT_TAG_OS, "cheats: play as: %s keeps Leon's model\n", eventName.c_str());
    if (partsShort) {
        RT_LOGF(RT_TAG_OS, "cheats: play as: %s keeps Leon's model (%d live parts, %d before)\n", eventName.c_str(),
                liveParts, s_mostParts[who]);
    }
    uint32_t leon4 = 0, leon5 = 0;
    if (ownBody) {
        leon4 = Memory::Read32(kLeonArcAddr + 4u * 4u);
        leon5 = Memory::Read32(kLeonArcAddr + 5u * 4u);
        Memory::TryWrite32(kLeonArcAddr + 4u * 4u, body - kLeonArcAddr);
        Memory::TryWrite32(kLeonArcAddr + 5u * 4u, bodyTex - kLeonArcAddr);
    }
    const PlTypeSwapScope plTypeSwap;
    Memory::TryWrite32(pG + Cheats::kPlTypeOffset, word & 0x0000FFFFu);  // pl_type 0, pl_costume 0
    Memory::TryWrite32(pG + kPlayerArcOffset, kLeonArcAddr);
    func_800F4F2C(ctx);
    uint32_t now = 0;
    if (Memory::TryRead32(pG + Cheats::kPlTypeOffset, now)) {
        Memory::TryWrite32(pG + Cheats::kPlTypeOffset, (now & 0x0000FFFFu) | (word & 0xFFFF0000u));
    }
    Memory::TryWrite32(pG + kPlayerArcOffset, arc);
    if (!ownBody) return;
    Memory::TryWrite32(kLeonArcAddr + 4u * 4u, leon4);
    Memory::TryWrite32(kLeonArcAddr + 5u * 4u, leon5);
    g_ownBodyScene = true;
    const std::vector<uint8_t> leonTree = BinParents(leonBody), ownTree = BinParents(body);
    int moved = RetargetWeights(body, ownTree, leonTree);
    uint32_t glow = 0;
    const std::vector<std::pair<uint32_t, uint32_t>> live = LiveParts(body, &glow);
    g_glowLive = glow;
    g_glowModel = glow != 0 ? Memory::Read32(glow + 0x0Cu) : 0;
    g_ownPartsHung = false;
    g_ownPartCount = 0;
    for (int i = 0; i < kMaxOwnParts && static_cast<size_t>(i) < live.size(); ++i) {
        const uint32_t model = live[i].first, tex = live[i].second;
        WriteGuestString(OwnBinName(i), "px/b" + std::to_string(i));
        WriteGuestString(OwnTplName(i), "px/t" + std::to_string(i));
        WriteGuestString(OwnPartName(i), "pxprt" + std::to_string(i));
        g_ownModel[i] = model;
        g_ownTex[i] = tex;
        // A part skinned on the whole skeleton (Krauser's head and arms, Wesker's coat, Ada's other body) has
        // the same bones in its own list; the small ones (hands, faces) have bones of their own.
        const std::vector<uint8_t> tree = BinParents(model);
        if (tree.size() >= 26 && std::equal(tree.begin(), tree.begin() + 26, leonTree.begin())) {
            moved += RetargetWeights(model, tree, leonTree);
        }
        ++g_ownPartCount;
    }
    RT_LOGF(RT_TAG_OS, "cheats: play as: the scene shows character %u's own model (%d parts, %d weights moved)\n",
            word >> 24, g_ownPartCount, moved);
}
REGISTER_NATIVE_FUNCTION_AS(0x800F4F2C, PlayAs_ExeBeginEvt_800F4F2C, "PlayAs_ExeBeginEvt_800F4F2C");

// Event::ExePacket_SetPartsSub 0x800F3D94 (r4 the part's name, r5/r6 its model/texture names, r7 its parent;
// it returns 1 either way). On a stand-in's own body Leon's parts are left off and its own take their places.
extern "C" void func_800F3D94(CpuContext* ctx);
extern "C" void PlayAs_EvtSetParts_800F3D94(CpuContext* ctx) {
    auto read = [](uint32_t p, char (&out)[8]) {
        for (uint32_t i = 0; i < 7 && Cheats::GuestPointer(p); ++i) {
            out[i] = static_cast<char>(Memory::Read8(p + i));
            if (out[i] == 0) break;
        }
    };
    char nm[8] = {}, oya[8] = {};
    read(ctx->gpr[4], nm);
    read(ctx->gpr[7], oya);
    if (!g_ownBodyScene || StandInPlaying() == 0 || std::strcmp(oya, "pl0000") != 0) {
        func_800F3D94(ctx);
        return;
    }
    bool leonPart = false;
    for (const char* slot : kLeonPartSlots) leonPart = leonPart || std::strcmp(nm, slot) == 0;
    if (leonPart && !g_ownPartsHung) {
        g_ownPartsHung = true;
        const uint32_t evt = ctx->gpr[3], parent = ctx->gpr[7];
        g_glowCatch = g_glowModel != 0;
        int left = 0;
        for (int i = 0; i < g_ownPartCount; ++i) {
            const uint32_t b = TempEvtBin(OwnBinName(i), g_ownModel[i]);
            const uint32_t t = b != 0 ? TempEvtBin(OwnTplName(i), g_ownTex[i]) : 0u;
            if (b != 0 && t != 0) {
                ctx->gpr[3] = evt;
                ctx->gpr[4] = OwnPartName(i);
                ctx->gpr[5] = OwnBinName(i);
                ctx->gpr[6] = OwnTplName(i);
                ctx->gpr[7] = parent;
                func_800F3D94(ctx);
            } else {
                ++left;
            }
            if (t != 0) ClearEvtBin(t);
            if (b != 0) ClearEvtBin(b);
        }
        if (left != 0) {
            RT_LOGF(RT_TAG_OS, "cheats: play as: %d of %d parts left off (the scene's bin table is full)\n", left,
                    g_ownPartCount);
        }
        g_glowCatch = false;
        if (g_glowModel != 0) {
            RT_LOGF(RT_TAG_OS, "cheats: play as: the arm glow %s in the scene\n", g_glowScene ? "follows" : "not found");
        }
    }
    ctx->gpr[3] = 1;
}
REGISTER_NATIVE_FUNCTION_AS(0x800F3D94, PlayAs_EvtSetParts_800F3D94, "PlayAs_EvtSetParts_800F3D94");

// SetObj18 0x8012869C (r3 bin, r4 tpl, r5 pos, r6 rot, r7 type; returns the obj): a scene's body "pl0000"
// (type 1, Leon's) gets light mask 1, the player's lights, only while pl_type is 0 (Leon); otherwise 0x40,
// its parent's lights only. With a stand-in playing the story the body (the stand-in's own or Leon's) lost
// the scene's lights: Krauser black under the syringe in r104s00 (OPPO Adreno 610 report, Windows trace:
// lm01 with one black light vs Leon's 4 lights, 2026-11-19). The mask goes back to 1; pl_type stays, so
// Leon's cloth (hair, jacket) is still not set on a body that is not his.
extern "C" void func_8012869C(CpuContext* ctx);
extern "C" void PlayAs_SetObj18_8012869C(CpuContext* ctx) {
    const uint32_t type = ctx->gpr[7];
    func_8012869C(ctx);
    const uint32_t obj = ctx->gpr[3];
    if (type != 1u || !Cheats::GuestPointer(obj) || StandInPlaying() == 0) return;
    constexpr uint32_t kLightEnableMask = 0x164u + 0x50u;  // cModel::LightInfo (+0x164), EnableMask (+0x50)
    if (Memory::Read8(obj + kLightEnableMask) == 0x40u) {
        Memory::Write8(obj + kLightEnableMask, 0x01u);
        RT_LOGF(RT_TAG_OS, "cheats: play as: the scene's body takes the player's lights (character %d)\n",
                StandInPlaying());
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x8012869C, PlayAs_SetObj18_8012869C, "PlayAs_SetObj18_8012869C");

// Play As radio: the radio screen's player model (tel00ModelInit, Sscrn module 81 at 0x96DA2768) is Leon's,
// built from the player archive's entries 4, 5, 0xA, 5, 9, 7 with his face parts from ss_term.dat. Ada's
// entry 0xA is a texture: read as a model, the radio after r100s20 crashed in the skinning (0x80162E28 from
// 0x96D977C0, 2026-11-18). For a stand-in the call sees the copy of Leon's archive, so the radio shows Leon
// (as CubeEvil4 does).
// In a radio call (OpeSetOpenTerm, below, which waits until the screen closes) the stand-in shows instead,
// as in the cutscenes: the call builds the model on the stand-in's body (Leon's entries 4/5 point at it), Leon's
// head parts after the body (archive 0xA, ss_term.dat 10 and 12, 9: skinned on his 119 bones) are cut off the
// chain (cModel::pModelInfo +0x15C, cModelInfo next +0x14), and the stand-in's live parts are created
// (ssModInfoMgr.create 0x8011B624, the manager at 0x96DCF9E4 in module 81) and added (cModel::addModel
// 0x8011A628). The radio pose is Leon's motion: the weights on bones that are not Leon's go to the nearest
// one that is until the call ends.
extern "C" void func_96DA2768(CpuContext* ctx);
extern "C" void func_8011B624(CpuContext* ctx);
extern "C" void func_8011A628(CpuContext* ctx);
namespace {
bool g_inRadioCall = false;
std::vector<std::pair<uint32_t, uint8_t>> g_radioUndo;  // weight bone bytes changed for the radio call
void RestoreRadioWeights() {
    for (auto it = g_radioUndo.rbegin(); it != g_radioUndo.rend(); ++it) Memory::Write8(it->first, it->second);
    g_radioUndo.clear();
}
} // namespace

extern "C" void PlayAs_RadioModel_96DA2768(CpuContext* ctx) {
    uint32_t pG = 0, arc = 0;
    if (StandInPlaying() == 0 || !Memory::TryRead32(Cheats::kPGAddress, pG) || !Cheats::GuestPointer(pG) ||
        !Memory::TryRead32(pG + kPlayerArcOffset, arc) || !LeonArchiveReady()) {
        func_96DA2768(ctx);
        return;
    }
    RestoreRadioWeights();
    const uint32_t model = ctx->gpr[3];
    const uint32_t body = ArcEntry(arc, 4), bodyTex = ArcEntry(arc, 5), leonBody = ArcEntry(kLeonArcAddr, 4);
    const bool own = g_inRadioCall && g_weightUndo.empty() && Cheats::GuestPointer(model) && IsBin(body) &&
                     IsTpl(bodyTex) && IsBin(leonBody);
    const uint32_t leon4 = Memory::Read32(kLeonArcAddr + 4u * 4u), leon5 = Memory::Read32(kLeonArcAddr + 5u * 4u);
    if (own) {
        Memory::TryWrite32(kLeonArcAddr + 4u * 4u, body - kLeonArcAddr);
        Memory::TryWrite32(kLeonArcAddr + 5u * 4u, bodyTex - kLeonArcAddr);
    }
    Memory::TryWrite32(pG + kPlayerArcOffset, kLeonArcAddr);
    func_96DA2768(ctx);
    Memory::TryWrite32(pG + kPlayerArcOffset, arc);
    if (!own) {
        RT_LOGF(RT_TAG_OS, "cheats: play as: the radio shows Leon from his archive\n");
        return;
    }
    Memory::TryWrite32(kLeonArcAddr + 4u * 4u, leon4);
    Memory::TryWrite32(kLeonArcAddr + 5u * 4u, leon5);
    uint32_t first = 0;
    if (!Memory::TryRead32(model + 0x15Cu, first) || !Cheats::GuestPointer(first) ||
        Memory::Read32(first + 0x0Cu) != body) {
        RT_LOGF(RT_TAG_OS, "cheats: play as: the radio model is not on the stand-in's body, Leon's parts kept\n");
        return;
    }
    const std::vector<uint8_t> leonTree = BinParents(leonBody);
    int moved = RetargetWeights(body, BinParents(body), leonTree);
    const std::vector<std::pair<uint32_t, uint32_t>> live = LiveParts(body);
    for (const auto& part : live) {
        const std::vector<uint8_t> tree = BinParents(part.first);
        if (tree.size() >= 26 && std::equal(tree.begin(), tree.begin() + 26, leonTree.begin())) {
            moved += RetargetWeights(part.first, tree, leonTree);
        }
    }
    g_radioUndo.swap(g_weightUndo);  // no event runs: the event watch would put g_weightUndo back at once
    Memory::TryWrite32(first + 0x14u, 0);  // Leon's parts off
    int added = 0;
    const uint32_t saved3 = ctx->gpr[3], saved4 = ctx->gpr[4], saved5 = ctx->gpr[5];
    for (const auto& part : live) {
        ctx->gpr[3] = 0x96DCF9E4u;  // ssModInfoMgr
        ctx->gpr[4] = part.first;
        ctx->gpr[5] = part.second;
        func_8011B624(ctx);
        const uint32_t info = ctx->gpr[3];
        if (!Cheats::GuestPointer(info)) continue;
        ctx->gpr[3] = model;
        ctx->gpr[4] = info;
        func_8011A628(ctx);
        ++added;
    }
    ctx->gpr[3] = saved3;
    ctx->gpr[4] = saved4;
    ctx->gpr[5] = saved5;
    RT_LOGF(RT_TAG_OS, "cheats: play as: the radio shows the stand-in (%d of %zu parts, %d weights moved)\n", added,
            live.size(), moved);
}
REGISTER_NATIVE_FUNCTION_AS(0x96DA2768, PlayAs_RadioModel_96DA2768, "PlayAs_RadioModel_96DA2768");

// Play As radio call: before the radio screen, OpeSetOpenTerm (0x8007CECC; GameCube 0x80062508) plays the
// "take out the radio" motion (player archive entry 0x79) with the radio model (0x77, texture 0x78) in the
// left hand, and waits for the motion to end. Ada's archive (pl02) leaves 0x77..0x79 empty: the motion ended
// at once and the radio screen opened without the scene (r100, 2026-11-18). For the call, a stand-in's
// empty entries point at Leon's (the cutscenes' copy); the ones the call did not change go back after it.
extern "C" void func_8007CECC(CpuContext* ctx);
extern "C" void PlayAs_OpenTerm_8007CECC(CpuContext* ctx) {
    uint32_t pG = 0, arc = 0, count = 0, leonCount = 0;
    if (StandInPlaying() == 0 || !Memory::TryRead32(Cheats::kPGAddress, pG) || !Cheats::GuestPointer(pG) ||
        !Memory::TryRead32(pG + kPlayerArcOffset, arc) || !Cheats::GuestPointer(arc) || arc == kLeonArcAddr ||
        !Memory::TryRead32(arc, count) || !LeonArchiveReady() || !Memory::TryRead32(kLeonArcAddr, leonCount)) {
        func_8007CECC(ctx);
        return;
    }
    constexpr uint32_t kFirst = 0x77u, kLast = 0x79u;
    uint32_t original[kLast - kFirst + 1] = {}, written[kLast - kFirst + 1] = {};
    int patched = 0;
    for (uint32_t n = kFirst; n <= kLast; ++n) {
        uint32_t own = 0, next = 0, leon = 0, leonNext = 0;
        if (n + 1u >= count + 4u || n + 1u >= leonCount + 4u || !Memory::TryRead32(arc + n * 4u, own) ||
            !Memory::TryRead32(arc + (n + 1u) * 4u, next) || own != next ||
            !Memory::TryRead32(kLeonArcAddr + n * 4u, leon) ||
            !Memory::TryRead32(kLeonArcAddr + (n + 1u) * 4u, leonNext) || leon == 0 || leon == leonNext) {
            continue;  // the stand-in has its own, or Leon's is missing
        }
        original[n - kFirst] = own;
        written[n - kFirst] = kLeonArcAddr + leon - arc;
        Memory::TryWrite32(arc + n * 4u, written[n - kFirst]);
        ++patched;
    }
    if (patched != 0) {
        RT_LOGF(RT_TAG_OS, "cheats: play as: radio call takes %d of Leon's radio entries\n", patched);
    }
    g_inRadioCall = true;
    func_8007CECC(ctx);
    g_inRadioCall = false;
    RestoreRadioWeights();
    for (uint32_t n = kFirst; n <= kLast; ++n) {
        uint32_t now = 0;
        if (written[n - kFirst] != 0 && Memory::TryRead32(arc + n * 4u, now) && now == written[n - kFirst]) {
            Memory::TryWrite32(arc + n * 4u, original[n - kFirst]);
        }
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x8007CECC, PlayAs_OpenTerm_8007CECC, "PlayAs_OpenTerm_8007CECC");

// Play As crouch and jump-over: the player routines take their motions straight from the player archive.
// Crouch behind cover (pl_R1_Crouch, Wii 0x800748C8) uses 0x5A..0x5C, 0x68 and 0x69; jump over a wall
// (0x80074D6C) 0x5E..0x61. Ada's pl02 leaves 0x5B, 0x5C and 0x69 empty and Krauser's pl0a all of them
// (HUNK's and Wesker's have Leon's set): with no motion the body stayed in its bind pose, half under
// the floor, through the whole crouch (Ada, r107 crawl box, 2026-11-18 tester video). While a stand-in
// runs one of them and misses any entry of its set, the whole set points at Leon's copy for the call
// (one set, so the moves fit each other); the entries go back after it. The motion keeps its pointer
// into Leon's copy, which stays in place.
extern "C" void func_800748C8(CpuContext* ctx);
extern "C" void func_80074D6C(CpuContext* ctx);
namespace {
void WithLeonMotions(CpuContext* ctx, void (*routine)(CpuContext*), const uint32_t* set, int n, bool& logged,
                     const char* what) {
    constexpr int kMax = 8;
    uint32_t pG = 0, arc = 0, count = 0, leonCount = 0;
    if (n > kMax || StandInPlaying() == 0 || !Memory::TryRead32(Cheats::kPGAddress, pG) ||
        !Cheats::GuestPointer(pG) || !Memory::TryRead32(pG + kPlayerArcOffset, arc) || !Cheats::GuestPointer(arc) ||
        arc == kLeonArcAddr || !Memory::TryRead32(arc, count) || !LeonArchiveReady() ||
        !Memory::TryRead32(kLeonArcAddr, leonCount)) {
        routine(ctx);
        return;
    }
    bool missing = false, leonHasAll = true;
    for (int i = 0; i < n; ++i) {
        const uint32_t e = set[i];
        uint32_t own = 0, next = 0, leon = 0, leonNext = 0;
        if (e + 1u > count || e + 1u > leonCount || !Memory::TryRead32(arc + e * 4u, own) ||
            !Memory::TryRead32(arc + (e + 1u) * 4u, next) || !Memory::TryRead32(kLeonArcAddr + e * 4u, leon) ||
            !Memory::TryRead32(kLeonArcAddr + (e + 1u) * 4u, leonNext) || leon == 0 || leon == leonNext) {
            leonHasAll = false;
            break;
        }
        missing = missing || own == next;
    }
    if (!missing || !leonHasAll) {
        routine(ctx);
        return;
    }
    uint32_t original[kMax] = {}, written[kMax] = {};
    for (int i = 0; i < n; ++i) {
        const uint32_t e = set[i];
        uint32_t leon = 0;
        Memory::TryRead32(arc + e * 4u, original[i]);
        Memory::TryRead32(kLeonArcAddr + e * 4u, leon);
        written[i] = kLeonArcAddr + leon - arc;
        Memory::TryWrite32(arc + e * 4u, written[i]);
    }
    if (!logged) {
        logged = true;
        RT_LOGF(RT_TAG_OS, "cheats: play as: character %d %s with Leon's motions\n", StandInPlaying(), what);
    }
    routine(ctx);
    for (int i = 0; i < n; ++i) {
        uint32_t now = 0;
        if (Memory::TryRead32(arc + set[i] * 4u, now) && now == written[i]) {
            Memory::TryWrite32(arc + set[i] * 4u, original[i]);
        }
    }
}
} // namespace

extern "C" void PlayAs_Crouch_800748C8(CpuContext* ctx) {
    static const uint32_t kSet[] = {0x5A, 0x5B, 0x5C, 0x68, 0x69};
    static bool s_logged = false;
    WithLeonMotions(ctx, func_800748C8, kSet, 5, s_logged, "crouches");
}
REGISTER_NATIVE_FUNCTION_AS(0x800748C8, PlayAs_Crouch_800748C8, "PlayAs_Crouch_800748C8");

extern "C" void PlayAs_JumpOver_80074D6C(CpuContext* ctx) {
    static const uint32_t kSet[] = {0x5E, 0x5F, 0x60, 0x61};
    static bool s_logged = false;
    WithLeonMotions(ctx, func_80074D6C, kSet, 4, s_logged, "jumps over");
}
REGISTER_NATIVE_FUNCTION_AS(0x80074D6C, PlayAs_JumpOver_80074D6C, "PlayAs_JumpOver_80074D6C");


// Play As finishers on story Ganados: the stand-ins' finishers take their motions from the Ganado's archive
// (em->subArc, +0x37C on the Wii): Wesker's palm strike 0x2B4..0x2B6 and heel kick 0x2B7/0x2B8, HUNK's neck
// breaker 0x2B9 (his) and 0x2BA/0x2BB (the Ganado's). Only Em/em10.drs (the Mercenaries' Ganados) has them;
// the story's em11..em15 archives leave 0x2B4..0x2BC empty (one offset), so the motion ended at once and the
// Ganado died standing (HUNK, r100's first Ganado, 2026-11-18). When the action prompt (cActionButton::set
// 0x8007DF3C, r7 = the enemy) comes up for a stand-in, an archive without them gets entries 0x2B4..0x2BB
// pointing at a copy of em10's (0x15760 bytes, read once to free MEM2 after Leon's archive).
extern "C" void func_8007DF3C(CpuContext* ctx);
namespace {
constexpr uint32_t kFinisherAddr = 0x97800000u;
constexpr uint32_t kFinisherMax = 0x40000u;
constexpr uint32_t kFinisherFirst = 0x2B4u;
constexpr uint32_t kFinisherLast = 0x2BBu;
uint32_t g_finisherOfs[kFinisherLast - kFinisherFirst + 1];  // each motion's offset in the copy

bool FinishersReady() {
    static int s_state = 0;  // 0 not read yet, 1 in place, -1 failed
    if (s_state != 0) return s_state == 1;
    s_state = -1;
    std::vector<uint8_t> bytes;
    if (DvdReadWholeFile("/Em/em10.drs", bytes) && bytes.size() >= 0x400) {
        const uint32_t size = Be32(&bytes[0x24]);
        const uint32_t offset = Be32(&bytes[0x2C]);
        if (offset >= 0x400 && offset <= bytes.size() && size <= bytes.size() - offset &&
            size >= (kFinisherLast + 2u) * 4u) {
            const uint8_t* arc = bytes.data() + offset;
            const uint32_t from = Be32(arc + kFinisherFirst * 4u);
            const uint32_t to = Be32(arc + (kFinisherLast + 1u) * 4u);
            uint8_t* dst = nullptr;
            if (from < to && to <= size && to - from <= kFinisherMax &&
                (dst = Memory::GetPointer(kFinisherAddr, to - from)) != nullptr) {
                std::memcpy(dst, arc + from, to - from);
                GxNotifyGuestRamDmaWrite(kFinisherAddr, to - from);
                for (uint32_t i = 0; i <= kFinisherLast - kFinisherFirst; ++i) {
                    g_finisherOfs[i] = Be32(arc + (kFinisherFirst + i) * 4u) - from;
                }
                s_state = 1;
            }
        }
    }
    RT_LOGF(RT_TAG_OS, "cheats: play as: em10's finisher motions %s\n", s_state == 1 ? "in place" : "missing");
    return s_state == 1;
}
} // namespace

// Ada's and Krauser's own moves at a Ganado. The story's enemy files are Leon's (PlayAs_EnemyData_80140BA0),
// whose entries 0x297..0x29E hold Leon's kick at a stunned Ganado and the Ganado's answer: Ada and Krauser
// kicked with Leon's kick (1.0 report "kick of the wrong character"), and Krauser got em10's finishers,
// Wesker's palm strike for his knee strike (CubeEvil4 0.98.3 found the same: "Krauser plays an animation of
// Wesker"). Their Mercenaries Ganados (type 0x20: em50 Ada, em70 Krauser) hold their own: Ada's fan kick,
// Krauser's double kick 0x29D/0x29E, his knee strike 0x2B4 and its answer 0x2B7/0x2B8 (compared on the disc:
// sizes differ from em11/em12 only there). A copy of those entries' data (read once) is pointed at from the
// story archive, as em10's finishers are for HUNK and Wesker.
namespace {
struct OwnMoves {
    const char* file;
    uint32_t first, last;  // entries, inclusive
    uint32_t addr, max;    // where the copy goes (free MEM2 below Ada's grips at 0x97A00000)
    int state = 0;         // 0 not read yet, 1 in place, -1 failed
    uint32_t ofs[16] = {};
};
OwnMoves g_adaKick{"/Em/em50.drs", 0x297u, 0x29Eu, 0x97850000u, 0x20000u};
OwnMoves g_krauserKick{"/Em/em70.drs", 0x297u, 0x29Eu, 0x97870000u, 0x20000u};
OwnMoves g_krauserKnee{"/Em/em70.drs", 0x2B4u, 0x2B8u, 0x97890000u, 0x10000u};

bool OwnMovesReady(OwnMoves& m) {
    if (m.state != 0) return m.state == 1;
    m.state = -1;
    std::vector<uint8_t> bytes;
    if (DvdReadWholeFile(m.file, bytes) && bytes.size() >= 0x400) {
        const uint32_t size = Be32(&bytes[0x24]);
        const uint32_t offset = Be32(&bytes[0x2C]);
        if (offset >= 0x400 && offset <= bytes.size() && size <= bytes.size() - offset &&
            size >= (m.last + 2u) * 4u && Be32(bytes.data() + offset) >= m.last + 1u) {
            const uint8_t* arc = bytes.data() + offset;
            const uint32_t from = Be32(arc + m.first * 4u);
            const uint32_t to = Be32(arc + (m.last + 1u) * 4u);
            uint8_t* dst = nullptr;
            if (from < to && to <= size && to - from <= m.max &&
                (dst = Memory::GetPointer(m.addr, to - from)) != nullptr) {
                std::memcpy(dst, arc + from, to - from);
                GxNotifyGuestRamDmaWrite(m.addr, to - from);
                for (uint32_t i = 0; i <= m.last - m.first; ++i) {
                    m.ofs[i] = Be32(arc + (m.first + i) * 4u) - from;
                }
                m.state = 1;
            }
        }
    }
    RT_LOGF(RT_TAG_OS, "cheats: play as: %s entries 0x%X..0x%X %s\n", m.file, m.first, m.last,
            m.state == 1 ? "in place" : "missing");
    return m.state == 1;
}

// Ada's and Krauser's own grabs (a Ganado's hold and the escape) in their Mercenaries Ganados, compared entry
// by entry on the disc (2026-11-22, the same list as CubeEvil4's loadStandInSet): they differ from every story
// Ganado (em10..em20) only in these. Ada (em50): 0xFB, 0x100, 0x101, 0x103 (holds; only some Ganado types
// have them), 0x17A/0x17B, 0x1A7 (the stun rod's shock), 0x28F, 0x290, 0x292. Krauser (em70): 0x101, 0x103,
// 0x17A/0x17B, 0x1A7. In the story the stand-ins played Leon's.
struct OwnEntries {
    const char* file;
    std::vector<uint32_t> list;
    uint32_t addr, max;           // where the copy goes
    std::atomic<int> state{0};    // 0 not read yet, 1 in place, -1 failed (read on the game thread only)
    std::vector<uint32_t> ofs;    // filled before state turns 1
};
OwnEntries g_adaGrabs{"/Em/em50.drs", {0xFB, 0x100, 0x101, 0x103, 0x17A, 0x17B, 0x1A7, 0x28F, 0x290, 0x292},
                      0x978A0000u, 0x40000u};
OwnEntries g_krauserGrabs{"/Em/em70.drs", {0x101, 0x103, 0x17A, 0x17B, 0x1A7}, 0x978E0000u, 0x20000u};

bool OwnEntriesReady(OwnEntries& m) {
    if (m.state.load() != 0) return m.state.load() == 1;
    m.state = -1;
    std::vector<uint8_t> bytes;
    if (DvdReadWholeFile(m.file, bytes) && bytes.size() >= 0x400) {
        const uint32_t size = Be32(&bytes[0x24]);
        const uint32_t offset = Be32(&bytes[0x2C]);
        if (offset >= 0x400 && offset <= bytes.size() && size <= bytes.size() - offset && size >= 4u) {
            const uint8_t* arc = bytes.data() + offset;
            const uint32_t count = Be32(arc);
            std::vector<uint8_t> copy;
            bool ok = size >= (count + 1u) * 4u;
            for (uint32_t e : m.list) {
                const uint32_t from = e < count ? Be32(arc + e * 4u) : 0u;
                const uint32_t to = e + 1u < count ? Be32(arc + (e + 1u) * 4u) : size;
                if (!ok || from == 0 || from >= to || to > size) {
                    ok = false;
                    break;
                }
                m.ofs.push_back(static_cast<uint32_t>(copy.size()));
                copy.insert(copy.end(), arc + from, arc + to);
                while (copy.size() % 32u != 0) copy.push_back(0);
            }
            uint8_t* dst = nullptr;
            if (ok && copy.size() <= m.max && (dst = Memory::GetPointer(m.addr, copy.size())) != nullptr) {
                std::memcpy(dst, copy.data(), copy.size());
                GxNotifyGuestRamDmaWrite(m.addr, static_cast<uint32_t>(copy.size()));
                m.state = 1;
            }
        }
    }
    RT_LOGF(RT_TAG_OS, "cheats: play as: %s grab entries %s\n", m.file, m.state.load() == 1 ? "in place" : "missing");
    return m.state.load() == 1;
}

// Points the story archive's listed entries at the copy, where the archive has them (a Ganado type without a
// hold keeps the entry empty). Empty entries just before one share its data: they follow it.
// `mayRead`: on the game thread the copy is read from the disc first if needed; the retrace only uses it.
bool PointAtOwnEntries(uint32_t arc, uint32_t count, OwnEntries& m, bool mayRead) {
    if (mayRead ? !OwnEntriesReady(m) : m.state.load() != 1) return false;
    bool changed = false;
    for (size_t k = 0; k < m.list.size(); ++k) {
        const uint32_t e = m.list[k], want = m.addr + m.ofs[k] - arc;
        uint32_t now = 0, next = 0;
        if (e + 1u >= count || !Memory::TryRead32(arc + e * 4u, now) || !Memory::TryRead32(arc + (e + 1u) * 4u, next) ||
            now == want || now == 0 || now >= next) {
            continue;
        }
        Memory::TryWrite32(arc + e * 4u, want);
        for (uint32_t j = e - 1u; j > 0 && Memory::Read32(arc + j * 4u) == now; --j) Memory::TryWrite32(arc + j * 4u, want);
        changed = true;
    }
    return changed;
}

} // namespace

void ReadStandInGrabs(int who) {
    if (who == 2) OwnEntriesReady(g_adaGrabs);
    if (who == 4) OwnEntriesReady(g_krauserGrabs);
}

namespace {
// Points the story archive's entries at the copy; false when they already were.
bool PointAtOwnMoves(uint32_t arc, OwnMoves& m) {
    uint32_t now = 0;
    if (!OwnMovesReady(m) || !Memory::TryRead32(arc + m.first * 4u, now) || now == m.addr + m.ofs[0] - arc) {
        return false;
    }
    for (uint32_t i = 0; i <= m.last - m.first; ++i) {
        Memory::TryWrite32(arc + (m.first + i) * 4u, m.addr + m.ofs[i] - arc);
    }
    return true;
}
} // namespace

namespace {
// The stand-in's own moves into a story Ganado's archive (em +0x37C): kicks, knee, grabs, finishers.
void StandInGanadoMoves(uint32_t em, int who, bool gameThread) {
    // em44 has one more entry (0x2BB) with the same layout. em43 is another body (Leon's own finishers
    // 0x28E.. are empty there too): left alone. em66..em70 keep part of their own set: left alone too.
    uint32_t arc = 0, count = 0, first = 0, hunk = 0, last = 0, leon = 0, leonEnd = 0;
    if (who == 0 || !Cheats::GuestPointer(em) || !Memory::TryRead32(em + 0x37Cu, arc) ||
        !Cheats::GuestPointer(arc) || !Memory::TryRead32(arc, count) || (count != 0x2BAu && count != 0x2BBu) ||
        !Memory::TryRead32(arc + 0x28Eu * 4u, leon) || !Memory::TryRead32(arc + 0x28Fu * 4u, leonEnd) ||
        leon == leonEnd) {
        return;  // not an enemy, Leon, or not a story body with Leon's moves
    }
    if ((who == 2 || who == 4) && PointAtOwnEntries(arc, count, who == 2 ? g_adaGrabs : g_krauserGrabs, gameThread)) {
        RT_LOGF(RT_TAG_OS, "cheats: play as: Ganado archive %08X: %s own grabs\n", arc,
                who == 2 ? "Ada's" : "Krauser's");
    }
    if (!gameThread) return;  // kicks and finishers: their copies are read on the game thread (ActBtnSet)
    if (who == 2 || who == 4) {
        bool changed = PointAtOwnMoves(arc, who == 2 ? g_adaKick : g_krauserKick);
        if (who == 4) {
            changed = PointAtOwnMoves(arc, g_krauserKnee) || changed;
        }
        if (changed) {
            RT_LOGF(RT_TAG_OS, "cheats: play as: Ganado archive %08X: %s own kick%s\n", arc,
                    who == 2 ? "Ada's" : "Krauser's", who == 4 ? " and knee strike" : "");
        }
        if (who == 4) return;  // em10's finishers are HUNK's and Wesker's, not his
    }
    if (!Memory::TryRead32(arc + kFinisherFirst * 4u, first) || !Memory::TryRead32(arc + 0x2B9u * 4u, hunk) ||
        !Memory::TryRead32(arc + kFinisherLast * 4u, last) || first != hunk || hunk != last || !FinishersReady()) {
        return;  // the archive has them (em10's, or already pointed at the copy)
    }
    for (uint32_t i = 0; i <= kFinisherLast - kFinisherFirst; ++i) {
        Memory::TryWrite32(arc + (kFinisherFirst + i) * 4u, kFinisherAddr + g_finisherOfs[i] - arc);
    }
    RT_LOGF(RT_TAG_OS, "cheats: play as: Ganado archive %08X: finisher motions 0x2B4..0x2BB from em10's\n", arc);
}
} // namespace

extern "C" void PlayAs_ActBtnSet_8007DF3C(CpuContext* ctx) {
    const uint32_t em = ctx->gpr[7];
    func_8007DF3C(ctx);
    StandInGanadoMoves(em, StandInPlaying(), true);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007DF3C, PlayAs_ActBtnSet_8007DF3C, "PlayAs_ActBtnSet_8007DF3C");

namespace Cheats {
// Once a retrace (Apply): a Ganado grabs without the kick prompt (ActBtnSet) ever showing, so every live
// enemy's archive gets the stand-in's grabs here too (their copy is read on the game thread with the room's
// enemy data, PlayAs_EnemyData_80140BA0); an archive done already costs a few reads. Ada's reload
// level is held here as well (equipping sets it from the item).
void StandInTick() {
    const int who = StandInPlaying();
    if (who == 0) return;
    AdaOwnReloadLevel();
    uint32_t arr = 0;
    if (!Memory::TryRead32(kEmArrayPtr, arr) || !Cheats::GuestPointer(arr)) return;
    for (uint32_t i = 0; i < kEmCount; ++i) {
        const uint32_t em = arr + i * kEmStride;
        uint32_t flag = 0;
        if (Memory::TryRead32(em, flag) && (flag & 1u) != 0) StandInGanadoMoves(em, who, false);  // be_flag bit 0: in use
    }
}
} // namespace Cheats

namespace Cheats {
// Play As: logs when a cutscene runs and ends (Status_flg STA_EVENT_SYSYTEM, bit 76: word 2 0x80000) and
// when the game cancels one (STA_EVENT_CANCEL, bit 103: word 3 0x01000000). Status_flg is at pG + 0x501C
// on the Wii (the GameCube's 0x500C + 0x10, as pl_type). Called once a retrace by Apply().
void PlayAsEventWatch() {
    static uint32_t s_last = 0;
    static uint32_t s_frames = 0;
    uint32_t pG = 0, sta2 = 0, sta3 = 0, sys = 0;
    if (!Memory::TryRead32(kPGAddress, pG) || !GuestPointer(pG) ||
        !Memory::TryRead32(pG + 0x501Cu + 8u, sta2) || !Memory::TryRead32(pG + 0x501Cu + 12u, sta3) ||
        !Memory::TryRead32(pG + kSystemFlgOffset, sys)) {
        return;
    }
    if ((sta2 & 0x00080000u) == 0) {
        EndOwnBodyScene();  // the stand-in's weights back after the event
    } else if (g_ownBodyScene) {
        CopyGlow();
    }
    if (PlayAsWanted() == 0) return;
    const uint32_t now = (sta2 & 0x00080000u) | (sta3 & 0x01000000u);
    ++s_frames;
    if (now == s_last) return;
    RT_LOGF(RT_TAG_OS, "cheats: play as: event %s%s after %u frames, pl_type %u, System_flg %08X\n",
            (now & 0x00080000u) ? "running" : "ended", (now & 0x01000000u) ? " (cancel set)" : "", s_frames,
            Memory::Read8(pG + kPlTypeOffset), sys);
    s_last = now;
    s_frames = 0;
}
} // namespace Cheats

// Play As props: Leon's rocket launcher takes its tube (model 0x76, texture 0x75), the rocket (0x70/0x71)
// and the rocket's motion (0x74) from the PLAYER archive (pG + 0x50; objRocket.cpp), as do his mine thrower,
// radio and whistle (0x70..0x7C). The stand-ins' archives hold other things there (Ada's: a small model;
// Krauser's: his arrow): the launcher showed only a rocket on a stick. While a stand-in holds a weapon from
// Leon's table, those entries point at Leon's archive (kLeonArcAddr, the cutscenes' copy), as CubeEvil4
// does (re4_arc_stand_in); each weapon read puts them back first. Entries past either table (its first
// word + 4, CubeEvil4's bound) are left alone.
// Leon's weapon modules also hang small parts from the player archive's entries 16..26 (Leon's: models
// of 1 or 2 bones; the Striker takes entry 22). HUNK's archive has models only up to 21, Wesker's up to
// 24: past them the entries are empty, and the part read a heap block as a model (HUNK switched in with
// the Striker crashed in the skinning 0x80162E28, model 0x80559AE0 = HUNK's empty entry 22, 2026-11-18).
// For HUNK, Krauser and Wesker the empty ones (no model header 0x40/0x60) point at Leon's models too;
// Ada's set, tested as it is, stays.
namespace {
constexpr uint32_t kPropFirst = 0x10u, kPropLast = 0x7Cu;
constexpr uint32_t kPartFirst = 0x10u, kPartLast = 0x1Au;
// Word 0 is the header's offset (0x40/0x60) until calcModelAddr (model.cpp) first uses the
// model and turns it into a pointer (addr + 0x40/0x60). Leon's models in kLeonArcAddr are used by the
// cutscenes: read as offsets only, his relocated parts looked empty, Krauser's entries with other bone
// counts stayed under Leon's Striker and the skinning walked off MEM1 (0x80162E28, 1.0 report
// 2026-11-20_20-50-07, r229: 5 of the 9 parts the files differ in were replaced, 7 in r206).
bool IsModelAt(uint32_t addr) {
    uint32_t head = 0;
    return Memory::TryRead32(addr, head) &&
           (head == 0x40u || head == 0x60u || head == addr + 0x40u || head == addr + 0x60u);
}
// Bytes 0x18/0x19 of a model header: weight palette size and parts (bone) count. Krauser's archive has models of its own in some of 16..26
// (8 of the 11 are empty): his entry under the Striker's part kept its own bones and the skinning read
// garbage (0x80162E28 after the case, crash folders 2026-11-19_15-07-02 and 15-32-50). A model with
// another count than Leon's is replaced as an empty one is.
bool SameBoneCount(uint32_t a, uint32_t b) {
    uint32_t wa = 0, wb = 0;  // bytes 0x18 and 0x19 (weight_palette_num, nParts) both
    return Memory::TryRead32(a + 0x18u, wa) && Memory::TryRead32(b + 0x18u, wb) && (wa >> 16) == (wb >> 16);
}
struct PropPatch {
    uint32_t arc = 0;
    bool on = false;
    uint32_t original[kPropLast - kPropFirst + 1] = {};
    uint32_t written[kPropLast - kPropFirst + 1] = {};
};
PropPatch g_props;
} // namespace

void UpdateStandInProps() {
    if (g_props.on) {
        for (uint32_t n = kPropFirst; n <= kPropLast; ++n) {
            uint32_t now = 0;
            const uint32_t k = n - kPropFirst;
            if (g_props.written[k] != 0 && Memory::TryRead32(g_props.arc + n * 4u, now) && now == g_props.written[k]) {
                Memory::TryWrite32(g_props.arc + n * 4u, g_props.original[k]);
            }
        }
        g_props.on = false;
    }
    uint32_t pG = 0, arc = 0, count = 0, leonCount = 0;
    if (StandInPlaying() == 0 || !g_standInHoldsLeonWeapon || !Memory::TryRead32(Cheats::kPGAddress, pG) ||
        !Memory::TryRead32(pG + kPlayerArcOffset, arc) || !Cheats::GuestPointer(arc) || arc == kLeonArcAddr ||
        !Memory::TryRead32(arc, count) || !LeonArchiveReady() || !Memory::TryRead32(kLeonArcAddr, leonCount)) {
        return;
    }
    g_props.arc = arc;
    g_props.on = true;
    int patched = 0;
    const int who = StandInPlaying();
    int parts = 0;
    for (uint32_t n = kPropFirst; n <= kPropLast; ++n) {
        const uint32_t k = n - kPropFirst;
        g_props.written[k] = 0;
        const bool part = n >= kPartFirst && n <= kPartLast;
        if (!part && n < 0x70u) continue;
        uint32_t own = 0, leonOfs = 0;
        if (n >= count + 4u || n >= leonCount + 4u || !Memory::TryRead32(arc + n * 4u, own) ||
            !Memory::TryRead32(kLeonArcAddr + n * 4u, leonOfs) || leonOfs == 0) {
            continue;
        }
        if (part && (who == 2 || !IsModelAt(kLeonArcAddr + leonOfs) ||
                     (IsModelAt(arc + own) && SameBoneCount(arc + own, kLeonArcAddr + leonOfs)))) {
            continue;
        }
        if (part) ++parts;
        g_props.original[k] = own;
        g_props.written[k] = kLeonArcAddr + leonOfs - arc;
        Memory::TryWrite32(arc + n * 4u, g_props.written[k]);
        ++patched;
    }
    static uint32_t s_loggedWeapon = 0xFFFFFFFFu;
    const uint32_t key = g_standInWeaponNo | static_cast<uint32_t>(who) << 8;
    if (s_loggedWeapon != key) {
        s_loggedWeapon = key;
        RT_LOGF(RT_TAG_OS,
                "cheats: play as: weapon %u: %d player archive entries from Leon's (%d parts 0x10..0x1A, count %u/%u)\n",
                g_standInWeaponNo, patched, parts, count, leonCount);
    }
}


