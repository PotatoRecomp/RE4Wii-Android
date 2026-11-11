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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
    return (p & 0xF0000000u) == 0x80000000u || (p & 0xF0000000u) == 0x90000000u;
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
    const auto level = static_cast<uint32_t>(g_brightness.load(std::memory_order_relaxed));
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

// Fill Screen without bars. RE4 draws its 3D into rows 56-392 of the 448-line frame (a letterbox
// viewport; the cinema-scope quads and a scissor cover the rest). The Wii build has its own mode for
// this: with *pSys word 0 bit 1 set, the display copy (0x80116DF8) takes only those 336 rows and
// scales them to the full height, so the bars never reach the screen. DisplaySettings narrows the
// Hor+ widening to match (the visible view is 336/448 as tall). Cleared once when the bars return.
constexpr uint32_t kPSysCropBars = 0x40000000u;
std::atomic<bool> g_removeBars{false};
std::atomic<bool> g_cropActive{false};  // the crop is on this frame (Hor+ follows it)

// Free gameplay only. Movies, events (their cinema bars and subtitles sit in the cropped rows), the
// sub screen, the die demo, door demos and the title draw a full 4:3 frame: cropping them cut off
// subtitles and flashed a white frame where the logos change (Windows recording, 2026-11-12).
// Status_flg (+0x501C) bits, MSB first: MOVIE 3, MOVIE2 5, CINESCO 7, DIEDEMO 11, SUB_SCRN 13,
// EVENT 19, EVENT_SYSTEM 76, INTO_SHOP 77, TITLE 80; System_flg (+0x54) SYS_DOORDEMO 10.
bool StatusBit(uint32_t pg, int bit) {
    uint32_t w = 0;
    return Memory::TryRead32(pg + 0x501Cu + 4u * (bit >> 5), w) && (w & (0x80000000u >> (bit & 31)));
}

bool FreeGameplay() {
    uint32_t pg = 0;
    if (!Memory::TryRead32(kPGAddr, pg) || !GuestPointer(pg)) return false;
    static constexpr int kStatusBits[] = {3, 5, 7, 11, 13, 19, 76, 77, 80};
    for (const int bit : kStatusBits) {
        uint32_t w = 0;
        if (!Memory::TryRead32(pg + 0x501Cu + 4u * (bit >> 5), w) || (w & (0x80000000u >> (bit & 31)))) return false;
    }
    uint32_t sys = 0;
    if (!Memory::TryRead32(pg + 0x54u, sys) || (sys & (0x80000000u >> 10))) return false;
    // No room yet (boot, logos): pG exists before the first room but Status_flg is still clear.
    uint32_t room = 0;
    return Memory::TryRead32(pg + 0x4FACu, room) && (room >> 16) != 0;
}

// The sub screen (inventory, map, files) and the merchant draw their panels and the attache case as
// 3D models under a 4:3 camera, but the text over them in 2D. Hor+ narrowed the models into the
// middle and showed the panels parked off screen while the text stayed stretched (merchant,
// 2026-11-12), so Hor+ is held off while either is open.
std::atomic<bool> g_menuScreen{false};

void UpdateMenuScreen() {
    uint32_t pg = 0;
    const bool menu = Memory::TryRead32(kPGAddr, pg) && GuestPointer(pg) && (StatusBit(pg, 13) || StatusBit(pg, 77));
    g_menuScreen.store(menu, std::memory_order_relaxed);
}

void ApplyBars() {
    static bool applied = false;
    const bool remove = g_removeBars.load(std::memory_order_relaxed) && FreeGameplay();
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
// pipelines compile on every core and the game waits for them, so the compile lands in the loading
// pause instead of stuttering the play that follows. Capped so a slow phone never hangs on a door.
void RoomShaderWarmup() {
    using Clock = std::chrono::steady_clock;
    constexpr uint32_t kWindowRetraces = 120;                  // ~2 s from the room change
    constexpr auto kRoomBudget = std::chrono::milliseconds(4000);  // waiting, at most, per room
    static uint16_t s_room = 0;
    static uint32_t s_left = 0;
    static Clock::duration s_waited{};
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
            s_waited = {};
            aurora_set_pipeline_loading_boost(true);
        }
    }
    if (s_left == 0) return;
    if (--s_left == 0) {
        aurora_set_pipeline_loading_boost(false);
        if (s_waited > Clock::duration::zero()) {
            RT_LOGF(RT_TAG_VI, "[shaders] room %03X: waited %lld ms for new shaders\n", room,
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(s_waited).count()));
        }
        return;
    }
    if (aurora_get_queued_pipeline_count() == 0 || s_waited >= kRoomBudget) return;
    const Clock::time_point start = Clock::now();
    while (aurora_get_queued_pipeline_count() != 0 && s_waited + (Clock::now() - start) < kRoomBudget) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    s_waited += Clock::now() - start;
    if (s_waited >= kRoomBudget) {
        RT_LOGF(RT_TAG_VI, "[shaders] room %03X: stopped waiting after %lld ms, %u still compiling\n", room,
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(s_waited).count()),
                aurora_get_queued_pipeline_count());
    }
}

} // namespace

void SetRemoveBars(bool on) {
    g_removeBars.store(on, std::memory_order_relaxed);
}

bool BarsRemoved() {
    return g_cropActive.load(std::memory_order_relaxed);
}

bool MenuScreen() {
    return g_menuScreen.load(std::memory_order_relaxed);
}

void SetReduceShadows(bool on) {
    g_reduceShadows.store(on, std::memory_order_relaxed);
}

bool SkipBootPressesA() {
    return g_pressA.load(std::memory_order_relaxed);
}

void SetBrightness(int offset) {
    g_brightness.store(kBrightnessDefault + std::clamp(offset, -50, 50), std::memory_order_relaxed);
}

void OnRetrace() {
    Minimap::OnRetrace();
    DumpMem1();
    ApplySkipBoot();
    ApplyBrightness();
    ApplyReduceShadows();
    // RE4_NO_BARS=1 (Windows tests): Fill Screen without the letterbox.
    static const bool s_noBarsEnv = [] {
        const char* e = std::getenv("RE4_NO_BARS");
        return e && *e == '1';
    }();
    if (s_noBarsEnv) g_removeBars.store(true, std::memory_order_relaxed);
    ApplyBars();
    UpdateMenuScreen();
    RoomShaderWarmup();
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

} // namespace GameTweaks

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
    const unsigned skip = g_skipFilters.load(std::memory_order_relaxed) | envMask;
    const uint32_t sp = ctx->gpr[1];
    Memory::Write32(sp - 16u, sp);  // the frame the original makes: callees save their LR at its +4
    ctx->gpr[1] = sp - 16u;
    for (const auto& f : kFilters) {
        if (!(skip & (1u << f.id))) f.fn(ctx);
    }
    ctx->gpr[1] = sp;
}
PPC_NATIVE_OVERRIDE_VOID(800FB8AC, FilterTrans_800FB8AC, (CpuContext* ctx), (ctx));
