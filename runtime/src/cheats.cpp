#include "cheats.h"
#include "game_tweaks.h"

#include "abi_bridge.h"
#include "ppc_runtime.h"

#include "memory.h"
#include "netplay_session.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
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
std::atomic<bool> g_itemStack{false};

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

void KeepAmmo() {
    uint32_t slot = 0, wepWord = 0, head = 0, flagsWord = 0, bulletWord = 0;
    if (Memory::TryRead32(kItemMgrAddr + 0x10u, wepWord) && IsGrenade(static_cast<uint16_t>(wepWord >> 16))) {
        KeepGrenades(static_cast<uint16_t>(wepWord >> 16), static_cast<uint8_t>(wepWord & 0xFFu));
        g_ammo = {};
        return;
    }
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
                     bool autoAim, bool autoQte, bool itemStack) {
    g_itemStack = itemStack;
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

extern "C" void ItemStack_ItemInfo_8003376C(CpuContext* ctx) {
    const uint32_t lr = ctx->lr;
    const uint32_t info = ctx->gpr[4];
    const uint32_t id = ctx->gpr[3] & 0xFFFFu;
    func_8003376C(ctx);
    if ((lr == 0x80036CD4u || lr == 0x8013FD08u) && Cheats::ItemStackOn() && Memory::Contains(info + 4u, 2)) {
        Memory::Write16(info + 4u, kItemStackMax);
        static bool s_logged[2] = {};
        if (!s_logged[lr == 0x8013FD08u]) {
            s_logged[lr == 0x8013FD08u] = true;
            RT_LOGF(RT_TAG_OS, "cheats: item stack limit 31999 (%s, item %04X)\n",
                    lr == 0x8013FD08u ? "pickup" : "combine", id);
        }
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
    func_800350C8(ctx);
    g_combineSameItem = saved;
}
REGISTER_NATIVE_FUNCTION_AS(0x800350C8, ItemStack_Combine_800350C8, "ItemStack_Combine_800350C8");

