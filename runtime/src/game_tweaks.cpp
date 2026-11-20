#include "game_tweaks.h"

#include "hle_stubs.h"
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include "det_clock.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "minimap.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "timebase_contract.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace GameTweaks {
namespace {

// pRK (RESET_KEEP_WORK, sbss, GC 0x80315AD4): the only r13 slot main.dol reads +0x10 (reset_flag) and
// +0x17 (logo_skip_enable) from. The title sets logo_skip_enable once its menu has been shown, so a soft
// reset skips the Nintendo logo, the health warning and the Capcom logo; titleWait reads it once
// title.dat is loaded. Skip Boot keeps it at 1 from the first retrace pRK is set.
constexpr uint32_t kPRKAddr = 0x8043E19Cu;
constexpr uint32_t kLogoSkipWordOffset = 0x14u;  // language .. logo_skip_enable (its low byte)

// The Wii Remote strap screen ("Wii/strap_eng.tpl", "Wii/wpad_xxx.tpl"), a task of the Wii build's
// tv_mode.cpp: 0x8016AE34 creates it and keeps it at r13-0x6610; its step 0x8016B070 runs a state byte
// at +1 and a counter at +4. State 1 waits 900 steps (0x384) and only reads A once the counter is
// below 0x30C; at 0 it moves on to state 2, the fade out. Skip Boot sets the counter to 1 once A would
// be read: earlier, the fade out starts over the 30-step fade in and the title menu ignores input.
constexpr uint32_t kStrapTaskPtr = 0x8043E410u;
constexpr uint32_t kStrapWaitState = 1u;
constexpr uint32_t kStrapReadsA = 0x30Cu;

// pSys (SYSTEM_SAVE_WORK, sdata next to pG): u8 brightness at +0x0A, the third byte of the word at
// +0x08 (language, eff_country, brightness, pad_type); main.dol reads and writes +0x0A off it as GC
// does. Render_done hands it to Bg_brightness_set every frame: the display copy's vertical filter is
// scaled by brightness / 64. The Options menu keeps it within 64 - 30 .. 64 + 50.
constexpr uint32_t kPSysAddr = 0x8043D584u;
constexpr uint32_t kBrightnessWordOffset = 0x08u;
constexpr int kBrightnessDefault = 0x40;

std::atomic<int> g_brightness{kBrightnessDefault};

bool GuestPointer(uint32_t p) {
    // Only MEM1 and MEM2 (128 MB here; RELs live at 0x96...). A stale word read during a room load
    // can point into the unmapped gap between them (0x88F757D7, OPPO CPH1933 crash in room 300),
    // and Read32 faults there instead of failing.
    return (p >= Memory::kMem1CachedBase && p < Memory::kMem1CachedBase + Memory::kMem1Size) ||
           (p >= Memory::kMem2CachedBase && p < Memory::kMem2CachedEnd);
}

// Once, while the strap screen waits and would take A: the counter left at 1, as an early A press.
// The task pointer stays set after the task ends, so this only runs until it has done its write or the
// boot is long over.
void SkipStrapScreen() {
    static bool done = false;
    static uint32_t retraces = 0;
    if (done || ++retraces > 60u * 120u) {
        return;
    }
    uint32_t task = 0, head = 0, counter = 0;
    if (!Memory::TryRead32(kStrapTaskPtr, task) || !GuestPointer(task) || !Memory::TryRead32(task, head) ||
        ((head >> 16) & 0xFFu) != kStrapWaitState || !Memory::TryRead32(task + 4u, counter) ||
        counter <= 1u || counter >= kStrapReadsA) {
        return;
    }
    if (Memory::TryWrite32(task + 4u, 1u)) {
        done = true;
        RT_LOGF(RT_TAG_OS, "skip boot: strap screen skipped (task %08X, %u steps left)\n", task, counter);
    }
}

// The title intro (title task states 3 and 4, 0x8015CBA8 / 0x8015CD00) skips a part on A, but only in
// set frame windows. From the end of the strap screen until the title menu runs (its handler 0x8015EB30
// sets pRK logo_skip_enable every step), Skip Boot presses A for 4 retraces out of 8, so each press is a
// new one and lands in every window. Only A: the windows read A or START, and A never reaches the menu.
constexpr uint32_t kIntroPressRetraces = 60u * 60u;
std::atomic<bool> g_pressA{false};

void SkipTitleIntro() {
    static bool strapSeen = false, done = false;
    static uint32_t retraces = 0, pressing = 0;
    if (done) return;
    if (++retraces > kIntroPressRetraces) {
        done = true;
        g_pressA.store(false, std::memory_order_relaxed);
        return;
    }
    uint32_t task = 0, head = 0, rk = 0, word = 0;
    if (!strapSeen || pressing == 0) {
        if (!Memory::TryRead32(kStrapTaskPtr, task) || !GuestPointer(task) || !Memory::TryRead32(task, head)) {
            return;
        }
        const uint32_t state = (head >> 16) & 0xFFu;
        if (state != 0u) {
            strapSeen = true;
            return;
        }
        if (!strapSeen) return;  // the strap screen has not started yet
    }
    if (Memory::TryRead32(kPRKAddr, rk) && GuestPointer(rk) && Memory::TryRead32(rk + kLogoSkipWordOffset, word) &&
        (word & 0xFFu) != 0u) {
        done = true;
        g_pressA.store(false, std::memory_order_relaxed);
        RT_LOGF(RT_TAG_OS, "skip boot: title intro skipped (%u retraces of A presses)\n", pressing);
        return;
    }
    ++pressing;
    g_pressA.store((pressing & 7u) < 4u, std::memory_order_relaxed);
}

// Which parts Skip Boot skips: bit 0 the logos (pRK logo_skip_enable), bit 1 the strap screen, bit 2 the
// title intro (A presses). logo_skip_enable is the title's soft-reset path: it jumps to title state 5, past
// state 4 where the menu's objects are made (0x800779AC at frame 0x190), so on a fresh boot the menu takes
// no input; it is left out by default. RE4_SKIP_BOOT_PARTS (env.txt) overrides the parts for testing.
constexpr unsigned kSkipLogos = 1u, kSkipStrap = 2u, kSkipIntro = 4u;

unsigned SkipBootParts() {
    if (!RuntimeConfigFile::SkipBoot()) return 0u;
    if (const char* env = std::getenv("RE4_SKIP_BOOT_PARTS"); env && *env) {
        return static_cast<unsigned>(std::strtoul(env, nullptr, 0)) & (kSkipLogos | kSkipStrap | kSkipIntro);
    }
    return kSkipStrap | kSkipIntro;
}

void ApplySkipBoot() {
    static const unsigned parts = SkipBootParts();
    static bool logged = false;
    if (parts & kSkipStrap) {
        SkipStrapScreen();
    }
    if ((parts & (kSkipIntro | kSkipLogos)) == kSkipIntro) {
        SkipTitleIntro();
    }
    uint32_t rk = 0, word = 0;
    if (!(parts & kSkipLogos) || !Memory::TryRead32(kPRKAddr, rk) || !GuestPointer(rk) ||
        !Memory::TryRead32(rk + kLogoSkipWordOffset, word) || (word & 0xFFu) != 0) {
        return;
    }
    if (Memory::TryWrite32(rk + kLogoSkipWordOffset, (word & ~0xFFu) | 1u) && !logged) {
        logged = true;
        RT_LOGF(RT_TAG_OS, "skip boot: logos skipped (pRK %08X)\n", rk);
    }
}

// Research aid: RE4_MEM_DUMP_AT=<retrace> writes MEM1 (24 MiB from 0x80000000, big-endian words as the
// guest sees them) to mem1_dump.bin in the working directory at that retrace.
void DumpMem1() {
    static const uint32_t at = [] {
        const char* e = std::getenv("RE4_MEM_DUMP_AT");
        return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0u;
    }();
    static uint32_t retraces = 0;
    if (at == 0u || ++retraces != at) return;
    if (FILE* f = std::fopen("mem1_dump.bin", "wb")) {
        for (uint32_t a = 0x80000000u; a < 0x81800000u; a += 4) {
            uint32_t v = 0;
            Memory::TryRead32(a, v);
            const uint8_t be[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
            std::fwrite(be, 1, 4, f);
        }
        std::fclose(f);
        RT_LOGF(RT_TAG_OS, "mem1 dumped at retrace %u\n", retraces);
    }
}

void ApplyBrightness() {
    uint32_t sys = 0, word = 0;
    if (!Memory::TryRead32(kPSysAddr, sys) || !GuestPointer(sys) ||
        !Memory::TryRead32(sys + kBrightnessWordOffset, word)) {
        return;
    }
    // 0 (the default) leaves the game's own Options > Brightness alone: forcing 0x40 every frame
    // undid it (Beta 13 report). Moving our slider back to 0 restores the default once.
    static bool s_forced = false;
    const auto level = static_cast<uint32_t>(g_brightness.load(std::memory_order_relaxed));
    if (level == kBrightnessDefault) {
        if (!s_forced) return;
        s_forced = false;
    } else {
        s_forced = true;
    }
    if (((word >> 8) & 0xFFu) != level) {
        Memory::TryWrite32(sys + kBrightnessWordOffset, (word & 0xFFFF00FFu) | level << 8);
    }
}

// Reduce Shadows: pG->Disp_flg (+0x58) bit DPF_SHADOW (6, MSB first) makes ShadowTrans (0x80153BA0)
// return at once, so no shadow light is set up: no cast / self shadows and their render-to-texture
// passes. Foot shadows are drawn by the model pass and stay. Cleared once when the option goes off.
constexpr uint32_t kPGAddr = 0x8043D580u;
constexpr uint32_t kDpfShadow = 0x02000000u;
std::atomic<bool> g_reduceShadows{false};

void ApplyReduceShadows() {
    static bool applied = false;
    const bool on = g_reduceShadows.load(std::memory_order_relaxed);
    if (!on && !applied) return;
    uint32_t pg = 0, flags = 0;
    if (!Memory::TryRead32(kPGAddr, pg) || !GuestPointer(pg) || !Memory::TryRead32(pg + 0x58u, flags)) return;
    const uint32_t want = on ? (flags | kDpfShadow) : (flags & ~kDpfShadow);
    if (want != flags) Memory::TryWrite32(pg + 0x58u, want);
    applied = on;
}

bool IsPuzzleRoom(uint16_t room);

// Effects off: every Esp effect (sparks, blood, smoke, fog sheets, muzzle flashes) is skipped where the
// game queues it. EspTrans (0x800C4E50) queues EspTransTbl[m_Id] (0x8032E3C8, 255 guest function
// pointers) for each active effect; while an id is off, its entry points at an empty guest function
// (0x8007E13C, a lone blr) and the game's pointer is kept here, put back once the id is on again.
// Settings: Reduce Effects turns every id off (not in puzzle rooms, whose symbols and switches are
// effects); Disable Fog turns off the fog sheet ids (kFogEspIds).
// RE4_ESP_LOG=1 (research): every 5 s, the active effect ids and their counts.
constexpr uint32_t kEspTransTbl = 0x8032E3C8u;
constexpr uint32_t kEmptyGuestFn = 0x8007E13Cu;
constexpr uint32_t kEspSysPtr = 0x8043DE90u;  // g_pEspSys, r13 - 0x6B90
constexpr int kEspIds = 255;
uint32_t g_espSaved[kEspIds] = {};
bool g_espAnySaved = false;

// Fog and weather effects: 0x04 screen rain / dust sheet, 0x08 scrolling fog sheet, 0x15 the haze, rain,
// snow and dust particles kept in a box around the camera (r100's drifting fog: 60 of them, about 18
// screens of blending a frame), 0x47 full-screen scrolling rain / dust overlay.
bool FogEspId(unsigned id) {
    return id == 0x04 || id == 0x08 || id == 0x15 || id == 0x47;
}

// The laser sight is an additive Esp effect too, but the player aims with it. EspTransTbl is indexed
// by the effect class (cEsp::m_Id, the record's cEspSeqTbl::Id at +1), not by the record number: the
// line is core record 3 (class 0x19, cEsp19), the dot core record 0x50. While the player aims (pPL
// r_no 0/6), Disable Effects keeps both classes and the GX filter (aurora) pauses.
uint32_t CoreEffectRecord(uint16_t id);

int CoreEffectClass(uint16_t record) {
    const uint32_t rec = CoreEffectRecord(record);
    uint32_t w = 0;
    if (rec == 0 || !Memory::TryRead32(rec, w)) return -1;
    return static_cast<int>((w >> 16) & 0xFFu);
}

bool PlayerAiming() {
    uint32_t pl = 0, rno = 0;
    if (!Memory::TryRead32(0x8043DEA0u, pl) || !GuestPointer(pl) || !Memory::TryRead32(pl + 0xFCu, rno)) return false;
    return (rno >> 24) == 0u && ((rno >> 16) & 0xFFu) == 6u;
}

// On the boat (pPL r_no 0/0x0F, pl_R1_Boat) the lake room's own move function runs, harpoon aiming
// included, so r_no 0/6 never shows it. Disable Effects stood down for the whole ride: the harpoon
// laser was gone in the Del Lago fight (tester video, 2026-11-18).
bool PlayerOnBoat() {
    uint32_t pl = 0, rno = 0;
    if (!Memory::TryRead32(0x8043DEA0u, pl) || !GuestPointer(pl) || !Memory::TryRead32(pl + 0xFCu, rno)) return false;
    return (rno >> 24) == 0u && ((rno >> 16) & 0xFFu) == 0x0Fu;
}

void LogActiveEsp() {
    static const bool on = std::getenv("RE4_ESP_LOG") != nullptr;
    static uint32_t s_tick = 0;
    if (!on || ++s_tick % 300 != 0) return;
    uint32_t sys = 0, arr = 0, n = 0;
    if (!Memory::TryRead32(kEspSysPtr, sys) || !GuestPointer(sys) || !Memory::TryRead32(sys + 0xC5E0u, arr) ||
        !GuestPointer(arr) || !Memory::TryRead32(sys + 0xC5E8u, n) || n > 4096) {
        RT_LOGF(RT_TAG_RUNTIME, "esp active: no pool (sys %08X, array %08X, slots %u)\n", sys, arr, n);
        return;
    }
    uint16_t count[256] = {};
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t w = 0;
        if (!Memory::TryRead32(arr + i * 0x150u + 0x10u, w)) break;
        if ((w >> 24) & 1u) ++count[(w >> 16) & 0xFFu];  // m_Be_flg bit 0 (+0x10), m_Id (+0x11)
    }
    char line[1024];
    int len = 0;
    for (int id = 0; id < 256 && len < 1000; ++id) {
        if (count[id]) len += std::snprintf(line + len, sizeof(line) - len, " %02X:%u", id, count[id]);
    }
    RT_LOGF(RT_TAG_RUNTIME, "esp active (%u slots):%s\n", n, len ? line : " none");
}

void ApplyEspOff() {
    LogActiveEsp();
    uint32_t pg = 0, word = 0;
    uint16_t room = 0;
    if (Memory::TryRead32(kPGAddr, pg) && GuestPointer(pg) && Memory::TryRead32(pg + 0x4FACu, word)) {
        room = static_cast<uint16_t>(word >> 16);
    }
    static const bool s_envAll = std::getenv("RE4_EFFECTS_OFF") != nullptr;  // Windows tests
    const bool puzzle = IsPuzzleRoom(room);
    const bool aiming = PlayerAiming();
    const bool boat = PlayerOnBoat();
    static bool s_boatLogged = false;
    if (boat != s_boatLogged) {
        s_boatLogged = boat;
        RT_LOGF(RT_TAG_RUNTIME, "effects: %s the boat, Disable Effects %s\n", boat ? "on" : "off",
                boat ? "held" : "back");
    }
    aurora_set_reduce_effects_paused(puzzle || aiming || boat);
    int laserLine = -1, laserDot = -1;
    if (aiming) {
        laserLine = CoreEffectClass(0x03);
        laserDot = CoreEffectClass(0x50);
        static int s_logged = -2;
        if ((laserLine << 8 | (laserDot & 0xFF)) != s_logged) {
            s_logged = laserLine << 8 | (laserDot & 0xFF);
            RT_LOGF(RT_TAG_RUNTIME, "effects: laser classes line %d, dot %d kept while aiming\n", laserLine, laserDot);
        }
    }
    const bool allOff = (aurora_get_reduce_effects() || s_envAll) && !puzzle && !boat && !MenuScreen() &&
                        !FramedScreen() && room != 0x120;
    static const bool s_envFog = [] {  // Windows tests
        const bool on = std::getenv("RE4_DISABLE_FOG") != nullptr;
        if (on) aurora_set_disable_fog(true);
        return on;
    }();
    (void)s_envFog;
    const bool fogOff = aurora_get_disable_fog();
    if (!allOff && !fogOff && !g_espAnySaved) return;
    bool anySaved = false;
    for (int id = 0; id < kEspIds; ++id) {
        const uint32_t slot = kEspTransTbl + 4u * static_cast<uint32_t>(id);
        uint32_t fn = 0;
        if (!Memory::TryRead32(slot, fn)) return;
        const bool off = (allOff && id != laserLine && id != laserDot) ||
                         (fogOff && FogEspId(static_cast<unsigned>(id)));
        if (off && fn != kEmptyGuestFn && fn != 0) {
            g_espSaved[id] = fn;
            Memory::TryWrite32(slot, kEmptyGuestFn);
        } else if (!off && g_espSaved[id] != 0) {
            if (fn == kEmptyGuestFn) Memory::TryWrite32(slot, g_espSaved[id]);
            g_espSaved[id] = 0;
        }
        anySaved |= g_espSaved[id] != 0;
    }
    g_espAnySaved = anySaved;
}

// Draw Distance: RE4 culls every model and effect against the view frustum (AddOtWorldPosRadius and
// the others), whose far plane cLightMgr::setFog (0x80112034, LightMgr 0x8033232C) sets every frame to
// Fog.End (+0x30) * (1 - far_play_ratio (+0x64)) + 1 in rooms with fog (Fog.Type +0x28 != 0). The
// ratio is scaled here so (1 - ratio) shrinks by the setting: the far models are dropped before they
// are drawn, and the GX fog and the projection stay as they were. VIEW::setFarPlane (0x8016B498) is
// inlined by the translator, so it cannot be overridden, and View._zfar itself is rewritten each
// frame before the frustum is built. A ratio that is still the one written here is left alone, so
// the scale never compounds. RE4_DRAW_DISTANCE=<scale> (Windows tests), RE4_LOG_FAR=1 logs it.
float FloatOf(uint32_t bits);
uint32_t BitsOf(float f);
constexpr uint32_t kLightMgr = 0x8033232Cu;
constexpr uint32_t kViewZFar = 0x803528B8u + 0x10u;
std::atomic<uint32_t> g_drawDistanceBits{0x3F800000u};  // 1.0f

void ApplyDrawDistance() {
    static uint32_t s_written = 0, s_game = 0;
    static const bool s_env = [] {
        if (const char* e = std::getenv("RE4_DRAW_DISTANCE")) {
            g_drawDistanceBits.store(BitsOf(std::clamp(std::strtof(e, nullptr), 0.1f, 1.0f)));
        }
        return true;
    }();
    (void)s_env;
    const float scale = FloatOf(g_drawDistanceBits.load(std::memory_order_relaxed));
    uint32_t bits = 0;
    if (!Memory::TryRead32(kLightMgr + 0x64u, bits)) return;
    if (bits != s_written || s_written == 0) s_game = bits;  // the game's own ratio (a new room)
    const float game = FloatOf(s_game);
    const float want = scale >= 1.0f ? game : 1.0f - (1.0f - game) * scale;
    static const bool s_log = std::getenv("RE4_LOG_FAR") != nullptr;
    static uint32_t s_logTick = 0;
    if (s_log && ++s_logTick % 300 == 0) {
        uint32_t zfar = 0, end = 0;
        Memory::TryRead32(kViewZFar, zfar);
        Memory::TryRead32(kLightMgr + 0x30u, end);
        RT_LOGF(RT_TAG_RUNTIME, "far plane %.0f: fog end %.0f, ratio game %.3f used %.3f\n", FloatOf(zfar),
                FloatOf(end), game, want);
    }
    if (scale >= 1.0f) {
        if (s_written != 0 && bits == s_written) Memory::TryWrite32(kLightMgr + 0x64u, s_game);
        s_written = 0;
        return;
    }
    s_written = BitsOf(want);
    if (s_written == 0) s_written = 1;  // keep 0 free as "nothing written"
    Memory::TryWrite32(kLightMgr + 0x64u, s_written);
}

// Fill Screen without bars. RE4 draws its 3D into rows 56-392 of the 448-line frame (a letterbox
// viewport; the cinema-scope quads and a scissor cover the rest). The Wii build has its own mode for
// this: with *pSys word 0 bit 1 set, the display copy (0x80116DF8) takes only those 336 rows and
// scales them to the full height, so the bars never reach the screen. On everywhere but the framed
// screens (FramedScreen: logos, title, save screen), which show at 16:9 and keep their full frame.
// Cleared once when the bars return. A free-gameplay-only test (Beta 2-4) never let it on in play.
constexpr uint32_t kPSysCropBars = 0x40000000u;
std::atomic<bool> g_removeBars{false};
std::atomic<bool> g_cropActive{false};  // the crop is on this frame

// Status_flg (+0x501C) bit, MSB first.
bool StatusBit(uint32_t pg, int bit) {
    uint32_t w = 0;
    return Memory::TryRead32(pg + 0x501Cu + 4u * (bit >> 5), w) && (w & (0x80000000u >> (bit & 31)));
}

// The sub screen (inventory, map, files: SUB_SCRN 13), card access (STA_CARD_ACCESS 14), the
// merchant (its shop is a sub screen too) and the typewriter's save screen (System_flg SYS_TYPEWRITER 19). Screen
// filters stay on in them (FilterTrans): the merchant's selection glow is one.
std::atomic<bool> g_menuScreen{false};
std::atomic<bool> g_framedScreen{true};

void UpdateMenuScreen() {
    uint32_t pg = 0;
    uint32_t sys = 0, room = 0;
    const bool game = Memory::TryRead32(kPGAddr, pg) && GuestPointer(pg);
    // Separate Ways (Stage 5, rooms 0x5xx) keeps SYS_TYPEWRITER (System_flg 19) on for the whole mode
    // (Windows dump 2026-11-14: 0x1844 from the chapter card through play; Leon's New Game 0x100804),
    // so there it says nothing about the typewriter: the mode stayed framed and counted as a menu.
    const bool roomRead = game && Memory::TryRead32(pg + 0x4FACu, room);
    const bool separateWays = roomRead && (room >> 24) == 5u;
    const bool typewriter = game && Memory::TryRead32(pg + 0x54u, sys) && (sys & (0x80000000u >> 19)) && !separateWays;
    // Not INTO_SHOP (77): the shop sets it at the merchant's greeting and leaves it on after the shop
    // closes (ss_shop / stage.cpp), so 60 FPS Full stayed held at 30 for the rest of the room (Infinix
    // X6871, 2026-11-19). The shop itself runs as a sub screen (SubScreenExec sets SUB_SCRN 13).
    const bool menu = game && (StatusBit(pg, 13) || StatusBit(pg, 14) || typewriter);
    g_menuScreen.store(menu, std::memory_order_relaxed);
    // Framed (FramedScreen): no pG or no room yet (boot, logos), TITLE (Status_flg 80), or the
    // typewriter.
    // TITLE counts only outside free play (pG->Rno0 == 3, the byte at +0x20): Separate Ways (a Wii
    // addition, no GC code) showed framed with its bars in play (Beta 8 report, moto g57), the title's
    // flag still set there as far as can be told.
    uint32_t rno = 0;
    const bool play = game && Memory::TryRead32(pg + 0x20u, rno) && (rno >> 24) == 3u;
    const bool title = game && StatusBit(pg, 80) && !play;
    bool framed = !roomRead || (room >> 16) == 0 || title || typewriter;
    // A load from the title's Load screen starts play (Rno0 3) while the Load screen still fades out:
    // going full there cropped and stretched the fading menu to the whole screen (phone recording,
    // Beta 13). The title's framing is kept until play starts (Rno0 3, the room set up in the black after
    // the fade), at most 5 s: a fixed 0.7 s was too short on a phone at 17 fps (Redmi, Beta 13).
    const auto now = std::chrono::steady_clock::now();
    static bool s_titleFramed = false;
    static std::chrono::steady_clock::time_point s_titleHold{};
    if (title) {
        s_titleFramed = true;
    } else if (!framed && s_titleFramed) {
        s_titleFramed = false;
        s_titleHold = now + std::chrono::seconds(5);
    }
    if (!framed && !play && now < s_titleHold) framed = true;
    if (play) s_titleHold = {};
    g_framedScreen.store(framed, std::memory_order_relaxed);
    // 60 FPS Lite warps a 3D scene; menus and the title / Load screen get the full interpolation.
    // Also the 3 s after them: a loaded room fades in while it streams, and Lite's half-way redraws
    // of Ashley and Leon stacked into copies there (Windows recording, 60 FPS off was clean).
    static std::chrono::steady_clock::time_point s_liteFree{};
    if (menu || framed) {
      s_liteFree = now + std::chrono::seconds(3);
    }
    aurora_set_frame_gen_lite_held(menu || framed || now < s_liteFree);
    aurora_set_frame_interpolation_held(menu);
    // Fill Screen reports (Separate Ways kept its bars): why a screen counts as framed, once a change.
    static int s_framed = -1;
    if (static_cast<int>(framed) != s_framed) {
        s_framed = framed;
        RT_LOGF(RT_TAG_RUNTIME, "screen: %s (room %X, title flag %d, play %d, typewriter %d, System_flg %08X)\n",
                framed ? "framed" : "full", roomRead ? room >> 16 : 0u, game && StatusBit(pg, 80) ? 1 : 0,
                play ? 1 : 0, typewriter ? 1 : 0, sys);
    }
}

void ApplyBars() {
    static bool applied = false;
    const bool remove = g_removeBars.load(std::memory_order_relaxed) && !g_framedScreen.load(std::memory_order_relaxed);
    if (!remove && !applied) {
        g_cropActive.store(false, std::memory_order_relaxed);
        return;
    }
    uint32_t sys = 0, word = 0;
    if (!Memory::TryRead32(kPSysAddr, sys) || !GuestPointer(sys) || !Memory::TryRead32(sys, word)) return;
    const uint32_t want = remove ? (word | kPSysCropBars) : (word & ~kPSysCropBars);
    if (want != word) Memory::TryWrite32(sys, want);
    applied = remove;
    g_cropActive.store(remove, std::memory_order_relaxed);
}

// RE4 loads room by room, so new models and effects (and the pipelines they need) arrive at each
// door, not once a stage. For the first seconds of a room, while it fades in from the load, new
// pipelines compile on every core instead of the play-time two. The game is not held for them: Beta 3
// waited here (up to 4 s a room), inside the VI retrace, and the frame pacing catching up after it
// dropped frames badly in play.
// Reduce Effects drops additive draws, and puzzles, switches and laser traps are often made of them:
// the church's symbol dial (1-08) lights each symbol with an Esp glow (Beta 4 report). Every room with
// a puzzle, terminal or laser in the GC room code draws all effects instead.
bool IsPuzzleRoom(uint16_t room) {
    static constexpr uint16_t kRooms[] = {
        0x105, 0x108, 0x118, 0x10F, 0x117, 0x11C,                // village: church dial, eye, light, gate
        0x201, 0x209, 0x20C, 0x20D, 0x20E, 0x211, 0x212, 0x217,  // castle: gems, panels, crests, statue
        0x21D, 0x225,                                            // castle: laser seal, crank
        0x300, 0x307, 0x30B, 0x311, 0x318, 0x327,                // island: lasers, pieces, crane, lamps
        0x40D,                                                   // terminal
    };
    for (const uint16_t r : kRooms) {
        if (r == room) return true;
    }
    return false;
}

void RoomShaderWarmup() {
    constexpr uint32_t kWindowRetraces = 120;  // ~2 s from the room change
    static uint16_t s_room = 0;
    static uint32_t s_left = 0;
    uint32_t pg = 0, word = 0;
    if (DetClock::Enabled() || !Memory::TryRead32(kPGAddr, pg) || !GuestPointer(pg) ||
        !Memory::TryRead32(pg + 0x4FACu, word)) {
        return;
    }
    const auto room = static_cast<uint16_t>(word >> 16);
    if (room != s_room) {
        s_room = room;
        if (room != 0) {
            s_left = kWindowRetraces;
            aurora_set_pipeline_loading_boost(true);
        }
    }
    if (s_left != 0 && --s_left == 0) {
        aurora_set_pipeline_loading_boost(false);
    }
}

// Laser Color. The laser sight's line (EspDrawLaserLine, effect 3) and dot (drawPoint, effect 0x50)
// of the core effect file (owner 0) take their colour from record 0's Col_start RGBA (+0x9C of the
// 0x12C-byte record), the bytes the GameCube Gecko codes changed. EspGetEstAddr (0x800978E8) is
// followed here without calling it: the effect system (SDA 0x8043DE90) holds per owner (0xC apart) the
// file at +0x6800, its id table at +0x6804 (u32 count, then 8-byte rows with the u16 id at +4) and 0xD6
// at +0x6808 while the owner is not loaded; an id's block is file + file[1 + row], records from +0x30.
constexpr uint32_t kEffectSystemPtr = 0x8043DE90u;
struct LaserPreset {
    const char* name;
    uint32_t line, dot;  // 0xRRGGBB00; 0 = the game's own
};
constexpr LaserPreset kLaserPresets[] = {
    {"red", 0u, 0u},
    {"blue", 0x0000D800u, 0x321EFA00u},
    {"yellow", 0xA8A80000u, 0xFAFA3200u},
    {"green", 0x00A80000u, 0x1EFA3200u},
};
std::atomic<int> g_laserColor{0};

uint32_t CoreEffectRecord(uint16_t id) {
    uint32_t sys = 0, unloaded = 0, table = 0, count = 0, file = 0;
    if (!Memory::TryRead32(kEffectSystemPtr, sys) || !GuestPointer(sys) ||
        !Memory::TryRead32(sys + 0x6808u, unloaded) || unloaded == 0xD6u ||
        !Memory::TryRead32(sys + 0x6804u, table) || !GuestPointer(table) || !Memory::TryRead32(table, count) ||
        count > 0x400u || !Memory::TryRead32(sys + 0x6800u, file) || !GuestPointer(file)) {
        return 0;
    }
    for (uint32_t row = 0; row < count; ++row) {
        uint32_t idWord = 0, offset = 0, num = 0;
        if (!Memory::TryRead32(table + 4u + 8u * row, idWord)) return 0;
        if ((idWord >> 16) != id) continue;
        if (!Memory::TryRead32(file + 4u + 4u * row, offset)) return 0;
        const uint32_t head = file + offset;
        if (!GuestPointer(head) || !Memory::TryRead32(head, num) || (num >> 16) == 0) return 0;
        return head + 0x30u;
    }
    return 0;
}

// Keeps one record's colour at `rgb` (0: the colour the file came with). A record whose bytes are not
// the ones written last (first look, or the file loaded again) gives a new original.
struct LaserRecord {
    uint32_t addr = 0, original = 0, written = 0;
};
bool KeepLaserRecord(LaserRecord& r, uint16_t id, uint32_t rgb) {
    const uint32_t rec = CoreEffectRecord(id);
    uint32_t now = 0;
    if (rec == 0 || !Memory::TryRead32(rec + 0x9Cu, now)) {
        r = {};
        return false;
    }
    if (rec != r.addr || now != r.written) {
        r.addr = rec;
        r.original = now;
    }
    const uint32_t want = rgb == 0u ? r.original : (rgb & 0xFFFFFF00u) | (now & 0xFFu);
    if (want != now) Memory::TryWrite32(rec + 0x9Cu, want);
    r.written = want;
    return true;
}

void ApplyLaserColor() {
    static uint32_t s_tick = 0;
    if (++s_tick % 30u != 1u) return;
    static LaserRecord s_line, s_dot;
    static int s_logged = -1;
    const int preset = std::clamp(g_laserColor.load(std::memory_order_relaxed), 0, 3);
    const LaserPreset& p = kLaserPresets[preset];
    const bool line = KeepLaserRecord(s_line, 3, p.line);
    const bool dot = KeepLaserRecord(s_dot, 0x50, p.dot);
    if (line && dot && preset != s_logged) {
        s_logged = preset;
        RT_LOGF(RT_TAG_RUNTIME, "laser color: %s (line %08X, dot %08X; the game's %08X, %08X)\n", p.name,
                s_line.written, s_dot.written, s_line.original, s_dot.original);
    }
}

// Camera Distance (the Sengoku Basara port's, as a wider view). CameraMove builds the game camera's
// projection with C_MTXPerspective (0x801825C4) at 0x8008C620: r3 = pG + 0xD8 (Camera.ProjMat),
// f1 = pG->Camera.param.Fovy (pG + 0x134). The view widens as if the eye stood k times farther back,
// tan(fov'/2) = k * tan(fov/2); the eye stays where the game's camera collision put it. The wider Fovy
// is written to pG->Camera too, as the effects that project onto the scene (C_MTXLightPerspective with
// Camera.param.Fovy) must match the projection. CameraMove copies the camera controller's camera over
// pG->Camera every frame of play; on a frame it doesn't, the Fovy written last is taken back to the
// game's. Free play only: cutscenes, events, the scope, menus and the title keep the game's view.
std::atomic<float> g_cameraDistance{1.0f};
constexpr uint32_t kCameraMoveProjectionReturn = 0x8008C624u;
// CameraMove's C_MTXLookAt (0x80182214) call at 0x8008C644, v_mat from Campos / Up / Target.
constexpr uint32_t kCameraMoveLookAtReturn = 0x8008C648u;
// The factor CameraFovy widened this frame's view by (1 = not widened): the look-at that follows
// raises the eye with it (CameraLookAt).
float g_cameraWidened = 1.0f;
constexpr uint32_t kCameraFovyOffset = 0x134u;
constexpr uint32_t kCameraProjOffset = 0xD8u;

float FloatOf(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
uint32_t BitsOf(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

float CameraFovy(uint32_t proj, float fov) {
    static uint32_t s_written = 0, s_original = 0;
    uint32_t pg = 0, rno = 0;
    if (!Memory::TryRead32(kPGAddr, pg) || !GuestPointer(pg) || proj != pg + kCameraProjOffset) {
        return fov;
    }
    // A Fovy this hook wrote and nobody replaced: the game's is the one it came from.
    if (s_written != 0u && BitsOf(fov) == s_written) fov = FloatOf(s_original);
    const float k = g_cameraDistance.load(std::memory_order_relaxed);
    // Movie (3, 5), sub screen (13), first-person view (STA_LOOK_THROUGH 16: the rifle scope and the
    // binoculars), event (19), binoculars (21), scope camera (25), event system (76), title (80); not the
    // cinema scope (7), which the Wii build keeps on in all of free play (its letterbox). `why`: the first
    // that holds, 0 in free play (-1: Rno0 is not 3).
    static constexpr int kNotPlay[] = {3, 5, 13, 16, 19, 21, 25, 76, 80};
    int why = Memory::TryRead32(pg + 0x20u, rno) && (rno >> 24) == 3u ? 0 : -1;
    // Separate Ways (stage 5, room_id at pG + 0x4FAC = stage << 8 | room): its scenes overload the GPU
    // once the view widens (Redmi: 200-490 ms frames, then VK_ERROR_DEVICE_LOST in room 500; turned on
    // again in Beta 13 and the Redmi lost its GPU there at 2x the same way), so the game's view is kept (-5).
    uint32_t roomWord = 0;
    if (why == 0 && Memory::TryRead32(pg + 0x4FACu, roomWord) && (roomWord >> 24) == 5u) why = -5;
    for (int bit : kNotPlay) {
        if (why == 0 && StatusBit(pg, bit)) why = bit;
    }
    // A rifle aimed counts as the scope (logged as bit 16) before and after the game's own flag.
    if (why == 0 && RifleAimed()) why = 16;
    const bool play = why == 0;
    static int s_why = -2;
    if (k > 1.0f && why != s_why) {
        s_why = why;
        RT_LOGF(RT_TAG_RUNTIME, "camera distance: %s (fov %.1f, Rno0 %u, Status_flg bit %d)\n",
                play ? "on" : "off", fov, rno >> 24, why);
    }
    float out = fov;
    if (k > 1.0f && play && fov > 1.0f && fov < 170.0f) {
        constexpr float kDegToRad = 0.01745329252f;
        out = std::min(2.0f * std::atan(std::tan(fov * 0.5f * kDegToRad) * k) / kDegToRad, 120.0f);
        static float s_loggedK = 0.0f;
        if (k != s_loggedK) {
            s_loggedK = k;
            RT_LOGF(RT_TAG_RUNTIME, "camera distance: %.2fx widens the view %.1f -> %.1f degrees\n", k, fov, out);
        }
    }
    g_cameraWidened = out != fov ? k : 1.0f;
    if (out != fov) {
        Memory::TryWrite32(pg + kCameraFovyOffset, BitsOf(out));
        s_written = BitsOf(out);
        s_original = BitsOf(fov);
    } else {
        if (s_written != 0u) Memory::TryWrite32(pg + kCameraFovyOffset, BitsOf(fov));
        s_written = 0;
    }
    return out;
}

// Speedrun Mode (Settings > Display): the game's own play clock (IGT) and real time since New Game (RTA),
// split at each chapter end, shown by the overlay (settings_overlay.cpp). IGT is the game's GetGameTime:
// play_time + OSGetTime seconds - game_start_time. chapter counts the chapters ended (SceChapterEnd).
// Offsets carried over from the GameCube layout (the save block sits 0x10 further on the Wii, as room_id
// does); the log line below checks them against the game's own clock (save and results screens).
// Splits go to console.log and speedrun.csv beside Config.toml. Cheats and Stage Select are off while it
// is on (cheats.cpp, RequestRoomJump).
std::atomic<bool> g_speedrun{false};
constexpr uint32_t kGameStartTimeOffset = 0x5Cu;
constexpr uint32_t kChapterOffset = 0x4F9Au;
constexpr uint32_t kPlayTimeOffset = 0x4FA4u;
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr int kMainChapters[] = {3, 3, 4, 4, 4};  // 1-1 .. 5-4
constexpr uint32_t kMainChapterCount = 18;

std::atomic<uint32_t> g_srIgt{kNone};
std::atomic<uint32_t> g_srChapter{kNone};
std::atomic<int64_t> g_srRtaStartMs{0};  // 0: no run (a loaded save, not a New Game)
std::atomic<int64_t> g_srRtaEndMs{0};    // 0: running
std::atomic<uint32_t> g_srLastSplit{kNone};
std::atomic<uint32_t> g_srLastChapter{kNone};

int64_t SteadyMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint32_t GuestSeconds() {
    uint32_t hi = 0, lo = 0, again = 0;
    do {
        hi = PPC_Mftbu();
        lo = PPC_Mftb();
        again = PPC_Mftbu();
    } while (hi != again);
    return static_cast<uint32_t>(((static_cast<uint64_t>(hi) << 32) | lo) / TimeBaseContract::kTicksPerSecond);
}

// "2-1" for the chapter after `ended` chapters of the main game, "end" after 5-4.
void ChapterLabel(uint32_t ended, char* out, size_t size) {
    if (ended >= kMainChapterCount) {
        std::snprintf(out, size, "end");
        return;
    }
    uint32_t left = ended;
    for (int c = 0; c < 5; ++c) {
        if (left < static_cast<uint32_t>(kMainChapters[c])) {
            std::snprintf(out, size, "%d-%u", c + 1, left + 1);
            return;
        }
        left -= kMainChapters[c];
    }
}

void FormatTime(uint32_t s, char* out, size_t size) {
    if (s >= 3600u) std::snprintf(out, size, "%u:%02u:%02u", s / 3600u, s / 60u % 60u, s % 60u);
    else std::snprintf(out, size, "%u:%02u", s / 60u, s % 60u);
}

void AppendSplitCsv(const char* line) {
    const std::filesystem::path path = RuntimeConfigFile::ResolveConfigPath().parent_path() / "speedrun.csv";
    std::error_code ec;
    const bool fresh = !std::filesystem::exists(path, ec);
    if (FILE* f = std::fopen(RuntimeConfigFile::PathToUtf8(path).c_str(), "a")) {
        if (fresh) std::fputs("time,event,chapter,igt_s,rta_s,split_igt_s\n", f);
        std::fputs(line, f);
        std::fclose(f);
    }
}

void CsvLine(const char* event, uint32_t chapter, uint32_t igt, int64_t rtaMs, uint32_t split) {
    const std::time_t now = std::time(nullptr);
    char stamp[24] = "";
    if (const std::tm* t = std::localtime(&now)) std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", t);
    char label[8];
    ChapterLabel(chapter, label, sizeof(label));
    char line[160];
    std::snprintf(line, sizeof(line), "%s,%s,%s,%u,%.1f,%s\n", stamp, event, label, igt,
                  rtaMs >= 0 ? rtaMs / 1000.0 : -1.0, split == kNone ? "" : std::to_string(split).c_str());
    AppendSplitCsv(line);
}

void UpdateSpeedrun() {
    static bool s_was = false;
    const bool on = g_speedrun.load(std::memory_order_relaxed);
    if (!on) {
        if (s_was) {
            g_srIgt = kNone;
            g_srChapter = kNone;
            g_srRtaStartMs = 0;
            g_srRtaEndMs = 0;
            g_srLastSplit = kNone;
            g_srLastChapter = kNone;
        }
        s_was = false;
        return;
    }
    s_was = true;
    uint32_t pg = 0, rno = 0, start = 0, played = 0, chapterWord = 0;
    if (!Memory::TryRead32(kPGAddr, pg) || !GuestPointer(pg) || !Memory::TryRead32(pg + 0x20u, rno) ||
        (rno >> 24) != 3u || !Memory::TryRead32(pg + kGameStartTimeOffset, start) ||
        !Memory::TryRead32(pg + kPlayTimeOffset, played) || !Memory::TryRead32(pg + (kChapterOffset & ~3u), chapterWord)) {
        return;  // not in free play: the numbers hold
    }
    const uint32_t now = GuestSeconds();
    const uint32_t igt = now - start + played;
    const uint32_t chapter = (chapterWord >> (8u * (3u - (kChapterOffset & 3u)))) & 0xFFu;
    static uint32_t s_lastIgt = kNone, s_splitIgt = 0, s_logCount = 0;
    static uint32_t s_chapter = kNone;
    const int64_t nowMs = SteadyMs();
    // Time went back (New Game or a loaded save): the run ends; a New Game (IGT under 10 s) starts one.
    if (s_lastIgt != kNone && igt + 5u < s_lastIgt) {
        if (g_srRtaStartMs.load() != 0 && g_srRtaEndMs.load() == 0) {
            CsvLine("reset", s_chapter, s_lastIgt, nowMs - g_srRtaStartMs.load(), kNone);
        }
        g_srRtaStartMs = 0;
        g_srRtaEndMs = 0;
        g_srLastSplit = kNone;
        g_srLastChapter = kNone;
        s_chapter = kNone;
    }
    if (g_srRtaStartMs.load() == 0 && s_chapter == kNone && igt < 10u && chapter == 0u) {
        g_srRtaStartMs = nowMs - static_cast<int64_t>(igt) * 1000;
        s_splitIgt = 0;
        CsvLine("start", 0, igt, static_cast<int64_t>(igt) * 1000, kNone);
        RT_LOGF(RT_TAG_RUNTIME, "speedrun: run started (igt %u)\n", igt);
    }
    if (s_chapter == kNone) {
        s_chapter = chapter;
        if (g_srRtaStartMs.load() == 0) s_splitIgt = igt;
    } else if (chapter == s_chapter + 1u) {
        const uint32_t split = igt - s_splitIgt;
        const int64_t startMs = g_srRtaStartMs.load();
        const int64_t rtaMs = startMs != 0 ? nowMs - startMs : -1;
        CsvLine("split", s_chapter, igt, rtaMs, split);
        char label[8];
        ChapterLabel(s_chapter, label, sizeof(label));
        RT_LOGF(RT_TAG_RUNTIME, "speedrun: %s done in %u s (igt %u, rta %.1f s)\n", label, split, igt,
                rtaMs / 1000.0);
        g_srLastSplit = split;
        g_srLastChapter = s_chapter;
        s_splitIgt = igt;
        s_chapter = chapter;
        if (chapter >= kMainChapterCount && startMs != 0 && g_srRtaEndMs.load() == 0) {
            g_srRtaEndMs = nowMs;
            CsvLine("finish", chapter, igt, rtaMs, kNone);
        }
    } else if (chapter != s_chapter) {
        s_chapter = chapter;  // a loaded save
        s_splitIgt = igt;
    }
    s_lastIgt = igt;
    g_srIgt = igt;
    g_srChapter = chapter;
    // Every minute: the raw fields, to check the offsets against the game's own clock.
    if (++s_logCount % 3600u == 1u) {
        RT_LOGF(RT_TAG_RUNTIME, "speedrun: igt %u (play_time %u, game_start_time %u, now %u), chapter %u\n", igt,
                played, start, now, chapter);
    }
}

} // namespace

} // namespace GameTweaks

// C_MTXPerspective (SDK mtx44.c), done natively so the game camera's call can be widened above.
extern "C" void MtxPerspective_HLE_801825c4(CpuContext* ctx) {
    const uint32_t m = ctx->gpr[3];
    float fovY = static_cast<float>(ctx->fpr[1].d);
    const float aspect = static_cast<float>(ctx->fpr[2].d);
    const float n = static_cast<float>(ctx->fpr[3].d);
    const float f = static_cast<float>(ctx->fpr[4].d);
    if (ctx->lr == GameTweaks::kCameraMoveProjectionReturn) fovY = GameTweaks::CameraFovy(m, fovY);
    const float cot = 1.0f / std::tan(fovY * 0.5f * 0.01745329252f);
    const float tmp = 1.0f / (f - n);
    const float v[16] = {cot / aspect, 0.0f, 0.0f, 0.0f, 0.0f, cot, 0.0f, 0.0f,
                         0.0f, 0.0f, -n * tmp, -(f * n) * tmp, 0.0f, 0.0f, -1.0f, 0.0f};
    for (int i = 0; i < 16; ++i) Memory::Write32(m + 4u * i, GameTweaks::BitsOf(v[i]));
}
PPC_NATIVE_OVERRIDE_VOID(801825C4, MtxPerspective_HLE_801825c4, (CpuContext* ctx), (ctx));

// C_MTXLookAt (SDK mtx.c), done natively so the game camera's view can be raised: with Camera Distance
// widening the view, the eye goes up by a share of its distance to the target and looks down at it
// (a view from higher behind Leon instead of a wide one from his shoulder). Only the drawn view
// changes; the game's Campos, which aiming and the camera logic use, stays.
extern "C" void MtxLookAt_HLE_80182214(CpuContext* ctx) {
    const auto readVec = [](uint32_t a, float v[3]) {
        for (int i = 0; i < 3; ++i) {
            const uint32_t bits = Memory::Read32(a + 4u * i);
            std::memcpy(&v[i], &bits, sizeof(float));
        }
    };
    const uint32_t m = ctx->gpr[3];
    float eye[3], up[3], target[3];
    readVec(ctx->gpr[4], eye);
    readVec(ctx->gpr[5], up);
    readVec(ctx->gpr[6], target);
    // The first-person views (scope, binoculars: Status_flg 16, 21, 25) are checked again here, in case
    // their frame's projection did not come through CameraFovy and left the last widening behind.
    uint32_t pg = 0;
    const bool firstPerson = Memory::TryRead32(GameTweaks::kPGAddr, pg) && GameTweaks::GuestPointer(pg) &&
                             (GameTweaks::StatusBit(pg, 16) || GameTweaks::StatusBit(pg, 21) ||
                              GameTweaks::StatusBit(pg, 25) || GameTweaks::RifleAimed());
    if (ctx->lr == GameTweaks::kCameraMoveLookAtReturn && GameTweaks::g_cameraWidened > 1.0f && !firstPerson) {
        const float dx = eye[0] - target[0], dy = eye[1] - target[1], dz = eye[2] - target[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        eye[1] += dist * (GameTweaks::g_cameraWidened - 1.0f) * 0.15f;
    }
    const auto normalize = [](float v[3]) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 0.0f) {
            for (int i = 0; i < 3; ++i) v[i] /= l;
        }
    };
    float look[3] = {eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]};
    normalize(look);
    float right[3] = {up[1] * look[2] - up[2] * look[1], up[2] * look[0] - up[0] * look[2],
                      up[0] * look[1] - up[1] * look[0]};
    normalize(right);
    const float vup[3] = {look[1] * right[2] - look[2] * right[1], look[2] * right[0] - look[0] * right[2],
                          look[0] * right[1] - look[1] * right[0]};
    const float* rows[3] = {right, vup, look};
    float view[12];
    for (int r = 0; r < 3; ++r) {
        const float* v = rows[r];
        const float w = -(eye[0] * v[0] + eye[1] * v[1] + eye[2] * v[2]);
        const float out[4] = {v[0], v[1], v[2], w};
        for (int c = 0; c < 4; ++c) {
            Memory::Write32(m + 16u * r + 4u * c, GameTweaks::BitsOf(out[c]));
            view[r * 4 + c] = out[c];
        }
    }
    // 60 FPS Lite reprojects the frame's picture with the game camera's view (CameraMove's call).
    if (ctx->lr == GameTweaks::kCameraMoveLookAtReturn) aurora_set_scene_view(view);
}
PPC_NATIVE_OVERRIDE_VOID(80182214, MtxLookAt_HLE_80182214, (CpuContext* ctx), (ctx));

namespace GameTweaks {

void SetLaserColor(int preset) {
    g_laserColor.store(std::clamp(preset, 0, 3), std::memory_order_relaxed);
}

void SetCameraDistance(float k) {
    g_cameraDistance.store(std::isfinite(k) ? std::clamp(k, 1.0f, 2.0f) : 1.0f, std::memory_order_relaxed);
}

int LaserColor() {
    return g_laserColor.load(std::memory_order_relaxed);
}

float CameraDistance() {
    return g_cameraDistance.load(std::memory_order_relaxed);
}

void SetSpeedrunMode(bool on) {
    g_speedrun.store(on, std::memory_order_relaxed);
}

bool SpeedrunMode() {
    return g_speedrun.load(std::memory_order_relaxed);
}

namespace {
std::atomic<int> g_bootTurbo{[] {
    const char* e = std::getenv("RE4_FAST_BOOT");
    return e ? std::clamp(std::atoi(e), 1, 8) : 1;
}()};
} // namespace

int BootTurbo() {
    return g_bootTurbo.load(std::memory_order_relaxed);
}

void EndBootTurbo() {
    if (g_bootTurbo.exchange(1, std::memory_order_relaxed) > 1) {
        RT_LOGF(RT_TAG_RUNTIME, "fast boot: title menu reached, normal speed\n");
    }
}

bool SpeedrunLines(char* top, size_t topSize, char* bottom, size_t bottomSize) {
    if (!g_speedrun.load(std::memory_order_relaxed)) return false;
    const uint32_t igt = g_srIgt.load();
    const uint32_t chapter = g_srChapter.load();
    const int64_t start = g_srRtaStartMs.load();
    const int64_t end = g_srRtaEndMs.load();
    char igtText[16] = "--:--", rtaText[16] = "--:--";
    if (igt != kNone) FormatTime(igt, igtText, sizeof(igtText));
    if (start != 0) FormatTime(static_cast<uint32_t>(((end != 0 ? end : SteadyMs()) - start) / 1000), rtaText,
                               sizeof(rtaText));
    std::snprintf(top, topSize, "IGT %s  RTA %s", igtText, rtaText);
    char label[8] = "--";
    if (chapter != kNone) ChapterLabel(chapter, label, sizeof(label));
    const uint32_t split = g_srLastSplit.load();
    const uint32_t splitChapter = g_srLastChapter.load();
    if (split != kNone && splitChapter != kNone) {
        char splitLabel[8], splitText[16];
        ChapterLabel(splitChapter, splitLabel, sizeof(splitLabel));
        FormatTime(split, splitText, sizeof(splitText));
        std::snprintf(bottom, bottomSize, "Ch %s   %s %s", label, splitLabel, splitText);
    } else {
        std::snprintf(bottom, bottomSize, "Ch %s", label);
    }
    return true;
}

void SetRemoveBars(bool on) {
    g_removeBars.store(on, std::memory_order_relaxed);
}

bool MenuScreen() {
    return g_menuScreen.load(std::memory_order_relaxed);
}

bool FramedScreen() {
    return g_framedScreen.load(std::memory_order_relaxed);
}

namespace {
std::atomic<bool> g_autoRun{false};
std::atomic<float> g_aimSensitivity{1.0f};
} // namespace

void SetAimSensitivity(float scale) {
    g_aimSensitivity.store(std::isfinite(scale) ? std::clamp(scale, 0.25f, 3.0f) : 1.0f, std::memory_order_relaxed);
}

float AimStickScale() {
    // RE4_AIM_SENSITIVITY=<scale> (Windows tests).
    static const float s_env = [] {
        const char* e = std::getenv("RE4_AIM_SENSITIVITY");
        return e != nullptr ? std::strtof(e, nullptr) : 0.0f;
    }();
    const float scale = s_env > 0.0f ? s_env : g_aimSensitivity.load(std::memory_order_relaxed);
    return scale != 1.0f && PlayerAiming() ? scale : 1.0f;
}

void SetAutoRun(bool on) {
    g_autoRun.store(on, std::memory_order_relaxed);
}

// Free play only (pG->Rno0 3: not the title, not the pause menu's Rno0 6), outside the sub screens and
// events (Status_flg 13, 14, 19, as MenuScreen plus STA_EVENT), with the player in routine 0 walking
// freely (r_no_1 footwork 0 .. turn 4, as the touch camera). B is cancel in every menu, so it must
// never be held there.
bool AutoRunActive() {
    // RE4_AUTO_RUN=1 (Windows tests).
    static const bool s_env = std::getenv("RE4_AUTO_RUN") != nullptr;
    if (!(s_env || g_autoRun.load(std::memory_order_relaxed)) || MenuScreen()) return false;
    uint32_t pg = 0, gameRno = 0, pl = 0, rno = 0;
    if (!Memory::TryRead32(kPGAddr, pg) || !GuestPointer(pg) || !Memory::TryRead32(pg + 0x20u, gameRno) ||
        (gameRno >> 24) != 3u || StatusBit(pg, 13) || StatusBit(pg, 14) || StatusBit(pg, 19)) {
        return false;
    }
    if (!Memory::TryRead32(0x8043DEA0u, pl) || !GuestPointer(pl) || !Memory::TryRead32(pl + 0xFCu, rno)) return false;
    const bool active = (rno >> 24) == 0u && ((rno >> 16) & 0xFFu) <= 4u;
    // Logged on each change of routine / footwork, the first 16 (a walk 1 turning into a run 3 shows it works).
    static std::atomic<uint32_t> s_last{~0u};
    static std::atomic<int> s_lines{0};
    const uint32_t state = rno >> 16;
    if (s_last.exchange(state, std::memory_order_relaxed) != state &&
        s_lines.fetch_add(1, std::memory_order_relaxed) < 16) {
        RT_LOGF(RT_TAG_RUNTIME, "auto run: %s (routine %u/%u)\n", active ? "on" : "off", rno >> 24,
                (rno >> 16) & 0xFFu);
    }
    return active;
}

void SetReduceShadows(bool on) {
    g_reduceShadows.store(on, std::memory_order_relaxed);
}

void SetDrawDistance(float scale) {
    g_drawDistanceBits.store(BitsOf(std::clamp(scale, 0.1f, 1.0f)), std::memory_order_relaxed);
}

bool SkipBootPressesA() {
    return g_pressA.load(std::memory_order_relaxed);
}

void SetBrightness(int offset) {
    g_brightness.store(kBrightnessDefault + std::clamp(offset, -50, 50), std::memory_order_relaxed);
}

namespace {
void ApplyRoomJump();  // below, beside RequestRoomJump
} // namespace

void OnRetrace() {
    Minimap::OnRetrace();
    // The game's Aim option in the log, for touch / gyro aim reports.
    static int s_aimReversed = -1;
    static uint32_t s_aimCheck = 0;
    if (++s_aimCheck % 60 == 0) {
        const int reversed = AimReversed() ? 1 : 0;
        if (reversed != s_aimReversed) {
            s_aimReversed = reversed;
            RT_LOGF(RT_TAG_RUNTIME, "game aim option: %s\n", reversed ? "reverse" : "normal");
        }
    }
    DumpMem1();
    ApplySkipBoot();
    ApplyBrightness();
    ApplyReduceShadows();
    ApplyEspOff();
    ApplyDrawDistance();
    UpdateSpeedrun();
    // RE4_LASER_COLOR=0..3, RE4_CAMERA_DISTANCE=1..2 and RE4_SPEEDRUN=0/1 (Windows tests): the Display options.
    static const bool s_displayEnv = [] {
        if (const char* e = std::getenv("RE4_LASER_COLOR")) SetLaserColor(std::atoi(e));
        if (const char* e = std::getenv("RE4_CAMERA_DISTANCE")) SetCameraDistance(std::strtof(e, nullptr));
        if (const char* e = std::getenv("RE4_SPEEDRUN")) SetSpeedrunMode(std::atoi(e) != 0);
        return true;
    }();
    (void)s_displayEnv;
    ApplyLaserColor();
    // RE4_NO_BARS=1 (Windows tests): Fill Screen without the letterbox.
    static const bool s_noBarsEnv = [] {
        const char* e = std::getenv("RE4_NO_BARS");
        return e && *e == '1';
    }();
    if (s_noBarsEnv) g_removeBars.store(true, std::memory_order_relaxed);
    ApplyBars();
    UpdateMenuScreen();
    RoomShaderWarmup();
    // RE4_ROOM_JUMP=<room hex>,<point>,<x>,<y>,<z>,<angle> (Windows tests): Stage Select once, at the
    // first free play (values from debug/roominfo.dat).
    static bool s_envJump = [] {
        const char* e = std::getenv("RE4_ROOM_JUMP");
        unsigned room = 0, point = 0;
        float x = 0, y = 0, z = 0, a = 0;
        if (e && std::sscanf(e, "%x,%u,%f,%f,%f,%f", &room, &point, &x, &y, &z, &a) == 6) {
            RequestRoomJump(static_cast<uint16_t>(room), static_cast<uint8_t>(point), x, y, z, a);
        }
        return true;
    }();
    (void)s_envJump;
    ApplyRoomJump();
    // RE4_DRAW_TRACE_AT=<retrace> (research, with AURORA_PVR_DIAG=1): aurora's one-frame draw trace.
    static const uint32_t traceAt = [] {
        const char* e = std::getenv("RE4_DRAW_TRACE_AT");
        return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 10)) : 0u;
    }();
    static uint32_t s_traceRetrace = 0;
    if (traceAt != 0 && ++s_traceRetrace == traceAt) {
        aurora_request_draw_trace(0);
    }
    // RE4_TEST_BLUR=<rate> (research): filter00's blur feedback alpha (blur_rate, r13-0x7738) every
    // retrace, so the room-light blur of Del Lago can be looked at in r100.
    static const int testBlur = [] {
        const char* e = std::getenv("RE4_TEST_BLUR");
        return e ? static_cast<int>(std::strtol(e, nullptr, 0)) : -1;
    }();
    if (testBlur >= 0) {
        uint32_t word = 0;
        if (Memory::TryRead32(0x8043D2E8u, word)) {
            Memory::TryWrite32(0x8043D2E8u, (word & 0x00FFFFFFu) | (static_cast<uint32_t>(testBlur & 0xFF) << 24));
        }
    }
    // RE4_DISP_FLG=<hex> (research): bits OR'd into pG->Disp_flg (+0x58, the GameCube's DPF_* display
    // switches, MSB first) every retrace.
    static const uint32_t dispFlg = [] {
        const char* e = std::getenv("RE4_DISP_FLG");
        return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0u;
    }();
    // RE4_CAPTURE_AT=<retrace>[,<retrace>...] (research): the game's own frame to capture_<retrace>.bmp in
    // the working folder (aurora's frame capture, not a screen grab).
    static uint32_t retrace = 0;
    static const char* captureAt = std::getenv("RE4_CAPTURE_AT");
    ++retrace;
    if (captureAt != nullptr) {
        for (const char* c = captureAt; *c;) {
            char* end = nullptr;
            const unsigned long at = std::strtoul(c, &end, 10);
            if (end == c) break;
            if (at == retrace) {
                static std::string path;
                path = "capture_" + std::to_string(retrace) + ".bmp";
                aurora_request_frame_capture(0, path.c_str());
            }
            c = (*end == ',') ? end + 1 : end;
        }
    }
    // RE4_CAPTURE_RANGE=<from>:<to>[:<step>] (research): every step-th frame in that retrace range.
    static const auto captureRange = [] {
        struct R { unsigned long from = 0, to = 0, step = 1; } r;
        if (const char* e = std::getenv("RE4_CAPTURE_RANGE")) {
            std::sscanf(e, "%lu:%lu:%lu", &r.from, &r.to, &r.step);
            if (r.step == 0) r.step = 1;
        }
        return r;
    }();
    if (captureRange.to != 0 && retrace >= captureRange.from && retrace <= captureRange.to &&
        (retrace - captureRange.from) % captureRange.step == 0) {
        static std::string rangePath;
        rangePath = "capture_" + std::to_string(retrace) + ".bmp";
        aurora_request_frame_capture(0, rangePath.c_str());
    }
    // RE4_LOG_STATUS=1 (research): the retrace at which movie / event / cinesco / sub screen / title
    // Status_flg bits and the crop change, to place a capture range on a transition.
    static const bool logStatus = std::getenv("RE4_LOG_STATUS") != nullptr;
    if (logStatus) {
        uint32_t pg = 0, w0 = 0, w2 = 0;
        static uint32_t last = 0xFFFFFFFFu;
        if (Memory::TryRead32(0x8043D580u, pg) && GuestPointer(pg) && Memory::TryRead32(pg + 0x501Cu, w0) &&
            Memory::TryRead32(pg + 0x501Cu + 8u, w2)) {
            const uint32_t bits = (w0 & 0x15141000u) | ((w2 & 0x00088000u) >> 8) | (g_cropActive.load() ? 1u : 0u);
            if (bits != last) {
                RT_LOGF(RT_TAG_VI, "status: retrace %u Status_flg[0] %08X [2] %08X crop %d\n", retrace, w0, w2,
                        g_cropActive.load() ? 1 : 0);
                last = bits;
            }
        }
    }
    // RE4_DISP_FLG_AT=<retrace>: RE4_DISP_FLG only from that retrace on (a before / after pair in one run).
    static const unsigned long dispFrom = [] {
        const char* e = std::getenv("RE4_DISP_FLG_AT");
        return e ? std::strtoul(e, nullptr, 10) : 0ul;
    }();
    if (dispFlg != 0 && retrace >= dispFrom) {
        uint32_t pg = 0;
        if (Memory::TryRead32(0x8043D580u, pg) && (pg & 0xFE000000u) == 0x80000000u) {
            Memory::TryWrite32(pg + 0x58u, Memory::Read32(pg + 0x58u) | dispFlg);
        }
    }
}

namespace {
struct RoomJump {
    uint16_t room;
    uint8_t point;
    float x, y, z, angle;
    uint8_t part;
    bool refillLife;
};
std::mutex g_roomJumpMutex;
std::optional<RoomJump> g_roomJump;

uint32_t FloatBits(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    return bits;
}

// The GameCube's roomJumpExec + CRoomInfo::setNextPos + roomJumpExit, on the Wii's pG (fields below
// 0x60 as on the GameCube, the save block from 0x4F90 = the GameCube's + 0x10). Message windows are
// not cleared (cMes); the room change resets them.
void ApplyRoomJump() {
    std::optional<RoomJump> jump;
    {
        std::lock_guard<std::mutex> lock(g_roomJumpMutex);
        if (!g_roomJump) return;
        jump = g_roomJump;
    }
    uint32_t pg = 0, rno = 0, ids = 0, life = 0, sys = 0, dbg = 0;
    if (!Memory::TryRead32(kPGAddr, pg) || !GuestPointer(pg) || !Memory::TryRead32(pg + 0x20u, rno) ||
        (rno >> 24) != 3u || g_menuScreen.load(std::memory_order_relaxed)) {
        return;  // not in free play yet: keep it for a later retrace
    }
    {
        std::lock_guard<std::mutex> lock(g_roomJumpMutex);
        g_roomJump.reset();
    }
    Memory::TryWrite32(pg + 0x2Cu, FloatBits(jump->x));
    Memory::TryWrite32(pg + 0x30u, FloatBits(jump->y));
    Memory::TryWrite32(pg + 0x34u, FloatBits(jump->z));
    Memory::TryWrite32(pg + 0x38u, FloatBits(jump->angle));
    // RoomNo_next (+0x28) = room, Part_next (+0x2A) = part, keeping +0x2B.
    uint32_t next = 0;
    Memory::TryRead32(pg + 0x28u, next);
    Memory::TryWrite32(pg + 0x28u, (static_cast<uint32_t>(jump->room) << 16) |
                                       (static_cast<uint32_t>(jump->part) << 8) | (next & 0xFFu));
    // room_id (+0x4FAC) .. Part (+0x4FAE), JumpPoint (+0x4FAF); room_id_prev (+0x4FB0), Part_old (+0x4FB2).
    if (Memory::TryRead32(pg + 0x4FACu, ids)) {
        Memory::TryWrite32(pg + 0x4FACu, (ids & 0xFFFFFF00u) | jump->point);
        uint32_t prev = 0;
        Memory::TryRead32(pg + 0x4FB0u, prev);
        Memory::TryWrite32(pg + 0x4FB0u, (ids & 0xFFFF0000u) | (((ids >> 8) & 0xFFu) << 8) | (prev & 0xFFu));
    }
    // pl_life = pl_life_max (Stage Select; Play As keeps the life).
    if (jump->refillLife && Memory::TryRead32(pg + 0x4FB4u, life)) Memory::TryWrite32(pg + 0x4FB4u, (life & 0xFFFFu) | (life << 16));
    // Debug_flg[2] DBG_ROOMJMP (bit 64 = word 2, MSB), System_flg SYS_START_EVT_SKIP (25) off.
    if (Memory::TryRead32(pg + 0x68u, dbg)) Memory::TryWrite32(pg + 0x68u, dbg | 0x80000000u);
    if (Memory::TryRead32(pg + 0x54u, sys)) Memory::TryWrite32(pg + 0x54u, sys & ~(0x80000000u >> 25));
    // Rno0 = 4 (room change), Rno1..3 = 0.
    Memory::TryWrite32(pg + 0x20u, 0x04000000u);
    RT_LOGF(RT_TAG_OS, "stage select: room %03X point %u at (%.0f, %.0f, %.0f) angle %.2f\n", jump->room,
            jump->point, jump->x, jump->y, jump->z, jump->angle);
}
} // namespace

void RequestRoomJump(uint16_t room, uint8_t point, float x, float y, float z, float angle, uint8_t part,
                     bool refillLife) {
    if (SpeedrunMode()) {
        RT_LOGF(RT_TAG_RUNTIME, "stage select: off in Speedrun Mode\n");
        return;
    }
    std::lock_guard<std::mutex> lock(g_roomJumpMutex);
    g_roomJump = RoomJump{room, point, x, y, z, angle, part, refillLife};
}

uint32_t RetracesPerFrame() {
    // system_vcnt, the last byte of this word: SetSystemVcnt (0x8011558C) stores it at r13-0x687D.
    uint32_t word = 0;
    if (!Memory::TryRead32(0x8043E1A0u, word)) {
        return 2;
    }
    return std::clamp<uint32_t>(word & 0xFFu, 1u, 3u);
}

bool AimReversed() {
    // pSys (sdata 0x8043D584) points at the system save; Config_flg at +0x00, CFG_AIM_REVERSE is
    // flag 0 (0x80000000 >> 0), as cam_qfps / pl_wep test it.
    uint32_t pSys = 0, flags = 0;
    if (!Memory::TryRead32(0x8043D584u, pSys) || (pSys & 0xF0000000u) != 0x80000000u
        || !Memory::TryRead32(pSys, flags)) {
        return false;
    }
    return (flags & 0x80000000u) != 0;
}

bool BinocularView() {
    uint32_t pg = 0;
    return Memory::TryRead32(kPGAddr, pg) && GuestPointer(pg) && StatusBit(pg, 21);
}

bool RifleInHand() {
    uint32_t pg = 0, wep = 0;
    if (!Memory::TryRead32(kPGAddr, pg) || !GuestPointer(pg) || !Memory::TryRead32(pg + 0x4FC0u, wep)) return false;
    const uint32_t no = wep >> 24;
    return no == 0x09u || no == 0x0Au || no == 0x28u || no == 0x2Fu;
}

bool RifleAimed() {
    constexpr uint32_t kPlayerPtrAddr = 0x8043DEA0u;  // pPL (r13 - 0x6B80)
    uint32_t pl = 0, rno = 0;
    return RifleInHand() && Memory::TryRead32(kPlayerPtrAddr, pl) && GuestPointer(pl) &&
           Memory::TryRead32(pl + 0xFCu, rno) && (rno >> 24) == 0u && ((rno >> 16) & 0xFFu) == 6u;
}

// Touch camera, Swipe (Settings > Controls): the finger drives the camera the game itself places.
// CameraQuasiFPS::calcDepressionRatio (Wii 0x80089E8C, r3 = CamCtrl.m_QuasiFPS) sets the shoulder
// camera's pitch ratio (+0x204, -1..1) from the sub stick (Key 0x80332E80 +3, / 59; flipped by the
// Aim option) and its yaw (+0x208, +-60 degrees) from +2, easing both back by 0.9 a frame when the
// stick is let go, so on its own the camera only peeks and swings back behind Leon. A horizontal swipe
// turns Leon instead (pPL ang.y, +0xA4), which turns the camera with him, and the finger's height holds
// the pitch, set after the game's own computation, and the camera is handed his new matrix so it
// swings behind him even when he turns on the spot. Only while he walks freely: routine 0 (move) with
// r_no_1 footwork 0, walk 1, back 2, run 3 or turn 4 (player.cpp's move table); aiming (6), the quick
// turn, ladders, events and the rest keep the game's camera.
namespace {
std::atomic<float> g_touchYaw{0.0f};
std::atomic<float> g_touchLookUp{0.0f};
std::atomic<bool> g_touchLookHeld{false};
constexpr uint32_t kPPLAddr = 0x8043DEA0u;  // pPL (r13 - 0x6B80)
constexpr float kPi = 3.14159265f;
} // namespace

void TouchLook(float yawDelta, float lookUp, bool held) {
    if (std::isfinite(yawDelta) && yawDelta != 0.0f) {
        float cur = g_touchYaw.load(std::memory_order_relaxed);
        while (!g_touchYaw.compare_exchange_weak(cur, cur + yawDelta, std::memory_order_relaxed)) {
        }
    }
    g_touchLookUp.store(std::isfinite(lookUp) ? std::clamp(lookUp, -1.0f, 1.0f) : 0.0f, std::memory_order_relaxed);
    g_touchLookHeld.store(held, std::memory_order_relaxed);
}

void ApplyTouchLook(uint32_t qfps) {
    // RE4_TOUCH_LOOK_TEST=1 (Windows tests): a steady swipe, turning 0.02 rad a frame, looking half up.
    static const bool s_test = std::getenv("RE4_TOUCH_LOOK_TEST") != nullptr;
    if (s_test) TouchLook(0.02f, 0.5f, true);
    const float yaw = g_touchYaw.exchange(0.0f, std::memory_order_relaxed);
    const bool held = g_touchLookHeld.load(std::memory_order_relaxed);
    static int s_sync = 0;  // frames left to hand Leon's matrix to the camera (below)
    if (yaw == 0.0f && !held && s_sync == 0) return;
    uint32_t pl = 0, rno = 0;
    if (!Memory::TryRead32(kPPLAddr, pl) || !GuestPointer(pl) || !Memory::TryRead32(pl + 0xFCu, rno)) return;
    const bool free = (rno >> 24) == 0u && ((rno >> 16) & 0xFFu) <= 4u;
    static int s_free = -1;
    if (static_cast<int>(free) != s_free) {
        s_free = free;
        RT_LOGF(RT_TAG_RUNTIME, "touch look: %s (routine %u/%u)\n", free ? "on" : "off", rno >> 24,
                (rno >> 16) & 0xFFu);
    }
    if (!free) return;
    uint32_t bits = 0;
    if (yaw != 0.0f && Memory::TryRead32(pl + 0xA4u, bits)) {
        float a = FloatOf(bits) + yaw;
        if (std::isfinite(a)) {
            a = std::remainder(a, 2.0f * kPi);
            Memory::TryWrite32(pl + 0xA4u, BitsOf(a));
            s_sync = 3;
        }
    }
    // The camera stays behind Leon: cPlayer::move hands its matrix to the camera
    // (setPlayerLocation 0x8008A060, QFPS m_pl_mat +0x178) only when his position changed, so a
    // turn on the spot left the camera looking the old way. Copied here (pPL mat +0xC, 3x4 floats)
    // for a few frames after each turn; calcBaseMatrix reads it on the next frame.
    if (s_sync > 0) {
        --s_sync;
        for (uint32_t i = 0; i < 12; ++i) {
            uint32_t w = 0;
            if (!Memory::TryRead32(pl + 0xCu + 4u * i, w) || !Memory::TryWrite32(qfps + 0x178u + 4u * i, w)) break;
        }
    }
    if (held && Memory::Contains(qfps + 0x204u, 4)) {
        const float up = g_touchLookUp.load(std::memory_order_relaxed);
        Memory::TryWrite32(qfps + 0x204u, BitsOf(AimReversed() ? -up : up));
    }
}

} // namespace GameTweaks

extern "C" void func_80089E8C(CpuContext* ctx);
extern "C" void TouchLook_CalcDepressionRatio_80089E8C(CpuContext* ctx) {
    const uint32_t qfps = ctx->gpr[3];
    func_80089E8C(ctx);
    GameTweaks::ApplyTouchLook(qfps);
}
REGISTER_NATIVE_FUNCTION_AS(0x80089E8C, TouchLook_CalcDepressionRatio_80089E8C, "TouchLook_CalcDepressionRatio_80089E8C");

// Fill Screen crop and the game's display copy setup. 0x80116DF8 (no arguments) reads the crop bit
// (*pSys word 0, kPSysCropBars) and sets the copy source and Y scale from it, but the game only calls it
// on screen changes (the GameCube's ScreenReSize: card, sub screen, movie, DVD load, options), not every
// frame. A save loaded from the title ran it while the screen still counted as framed (crop off) and
// nothing ran it again in play: the bars stayed until the next menu (Redmi, 2026-11-18). Draw_cinesco
// (0x8008CBC8, every frame of the main loop's draw, on the game thread between draws) runs the setup
// again when the bit no longer matches the one it last used.
namespace {
int g_copySetupCrop = -1;  // the crop bit 0x80116DF8 last used, -1 before its first call

int CropBitNow() {
    uint32_t sys = 0, word = 0;
    if (!Memory::TryRead32(GameTweaks::kPSysAddr, sys) || !GameTweaks::GuestPointer(sys) ||
        !Memory::TryRead32(sys, word)) {
        return -1;
    }
    return (word & GameTweaks::kPSysCropBars) != 0 ? 1 : 0;
}
} // namespace

extern "C" void func_80116DF8(CpuContext* ctx);
extern "C" void DispCopySetup_80116DF8(CpuContext* ctx) {
    g_copySetupCrop = CropBitNow();
    func_80116DF8(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80116DF8, DispCopySetup_80116DF8, "DispCopySetup_80116DF8");

extern "C" void func_8008CBC8(CpuContext* ctx);
extern "C" void DrawCinesco_8008CBC8(CpuContext* ctx) {
    const int crop = CropBitNow();
    if (g_copySetupCrop >= 0 && crop >= 0 && crop != g_copySetupCrop) {
        RT_LOGF(RT_TAG_RUNTIME, "fill screen: display copy set up again (crop %d -> %d)\n", g_copySetupCrop, crop);
        const CpuContext saved = *ctx;
        DispCopySetup_80116DF8(ctx);
        *ctx = saved;
    }
    func_8008CBC8(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008CBC8, DrawCinesco_8008CBC8, "DrawCinesco_8008CBC8");

// RE4's "wait n frames" helper (0x8015145C) sleeps the task only when 0x801515F4(0x8034DDC8) returns
// nonzero; otherwise it returns at once and its caller busy-polls. The codec in r100 (module 81, Hunnigan's call)
// waits like that for a voice to end (0x800F9094). On the console interrupts keep running under the
// loop; here the sound stream's DVD callback waits for a retrace (dvd.cpp) and retraces only come with a
// sleep or OSRestoreInterrupts, so the voice never ended and the game froze. The poll now pumps them.
extern "C" void func_801515F4(CpuContext* ctx);
extern "C" void func_8015209C(CpuContext* ctx);

extern "C" void TaskWait_8015145C(CpuContext* ctx) {
    const uint32_t frames = ctx->gpr[3];
    ctx->gpr[3] = 0x8034DDC8u;
    func_801515F4(ctx);
    if (ctx->gpr[3] != 0) {
        ctx->gpr[3] = frames;
        func_8015209C(ctx);
        return;
    }
    OS_HLE_ServicePendingInterrupts();
}
PPC_NATIVE_OVERRIDE_VOID(8015145C, TaskWait_8015145C, (CpuContext* ctx), (ctx));

// FilterTrans (0x800FB8AC), the game's post-process filters, every frame: while the render mode
// (pRender at r13-0x74A0, +0x20) is 3, 4 or 6, the screen is 512 x 448 (Screen 0x80332F54 +8 / +0xC
// against the sdata2 constants at r2-0x5788 / r2-0x5784) and the pause filter (filter09's bUse, r13-0x6994)
// is off, it runs filters 00, 01, 08, 02, 03, 04, 06, 07, 0A, 0B in that order (05 is empty). The same
// here, leaving out the ones Settings > Performance > Reduce Screen Filters turns off: each copies the
// frame (EFB copy) and draws it back, which costs a phone GPU a render pass each.
namespace {
std::atomic<unsigned> g_skipFilters{0};

using GuestFn = void (*)(CpuContext*);
extern "C" void func_800FB9F0(CpuContext*);
extern "C" void func_800FC8E0(CpuContext*);
extern "C" void func_80101BAC(CpuContext*);
extern "C" void func_800FD774(CpuContext*);
extern "C" void func_800FED98(CpuContext*);
extern "C" void func_800FF878(CpuContext*);
extern "C" void func_801009FC(CpuContext*);
extern "C" void func_8010105C(CpuContext*);
extern "C" void func_80103770(CpuContext*);
extern "C" void func_80104240(CpuContext*);
} // namespace

namespace GameTweaks {
void SetSkippedScreenFilters(unsigned mask) {
    g_skipFilters.store(mask, std::memory_order_relaxed);
}
} // namespace GameTweaks

extern "C" void FilterTrans_800FB8AC(CpuContext* ctx) {
    static const unsigned envMask = [] {
        const char* e = std::getenv("RE4_FILTER_SKIP");  // hex mask, Windows tests
        return e ? static_cast<unsigned>(std::strtoul(e, nullptr, 16)) : 0u;
    }();
    const uint32_t r13 = ctx->gpr[13], r2 = ctx->gpr[2];
    const uint8_t mode = static_cast<uint8_t>(Memory::Read32(Memory::Read32(r13 - 0x74A0u) + 0x20u) >> 24);
    if (mode != 3 && mode != 4 && mode != 6) return;
    if (Memory::Read32(0x80332F54u + 8u) != Memory::Read32(r2 - 0x5788u) ||
        Memory::Read32(0x80332F54u + 12u) != Memory::Read32(r2 - 0x5784u)) {
        return;
    }
    if (Memory::Read32(r13 - 0x6994u) != 0) return;
    static constexpr struct {
        unsigned id;
        GuestFn fn;
    } kFilters[] = {{0x0, func_800FB9F0}, {0x1, func_800FC8E0}, {0x8, func_80101BAC}, {0x2, func_800FD774},
                    {0x3, func_800FED98}, {0x4, func_800FF878}, {0x6, func_801009FC}, {0x7, func_8010105C},
                    {0xA, func_80103770}, {0xB, func_80104240}};
    // Menus (sub screen, merchant, typewriter) keep every filter: the merchant's selection glow went
    // missing with Reduce Effects on, leaving no way to see which item was picked (Beta 3).
    const unsigned skip =
        (GameTweaks::MenuScreen() ? 0u : g_skipFilters.load(std::memory_order_relaxed)) | envMask;
    const uint32_t sp = ctx->gpr[1];
    Memory::Write32(sp - 16u, sp);  // the frame the original makes: callees save their LR at its +4
    ctx->gpr[1] = sp - 16u;
    for (const auto& f : kFilters) {
        if (!(skip & (1u << f.id))) f.fn(ctx);
    }
    ctx->gpr[1] = sp;
}
PPC_NATIVE_OVERRIDE_VOID(800FB8AC, FilterTrans_800FB8AC, (CpuContext* ctx), (ctx));

