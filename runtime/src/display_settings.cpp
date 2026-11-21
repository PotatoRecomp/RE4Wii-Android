#include "det_clock.h"
#include "display_settings.h"
#include "game_tweaks.h"
#include "hle_stubs.h"

#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "thermal.h"

#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#if defined(__ANDROID__)
#include <SDL3/SDL.h>
#include <android/native_window.h>
#include <dlfcn.h>
#endif

namespace DisplaySettings {
namespace {

// Only the host side (the renderer and aurora's 3D projection) follows the aspect; the game's own
// canvas is left alone (RE4 draws 16:9 itself when SCGetAspectRatio reports wide).
constexpr float kLinesPerScale = 480.0f;
constexpr float kRatio16x9 = 16.0f / 9.0f;

std::mutex g_mutex;
Settings g_current;
Settings g_pending;
bool g_hasPending = false;
bool g_rendererDirty = true;
// 4:3 Full Screen (1.0 suggestion: a 4:3 picture over the whole screen, not cropped, for speedruns):
// the game keeps its 4:3 view and the picture is stretched to the surface. RE4_STRETCH_43=1 at boot,
// SetStretch43 from the Display menu.
std::atomic<bool> g_stretch43{[] {
    const char* v = std::getenv("RE4_STRETCH_43");
    return v != nullptr && *v == '1';
}()};
uint32_t g_surfaceWidth = 0;
uint32_t g_surfaceHeight = 0;

// Dynamic resolution: a factor on the player's render lines, lowered while the renderer holds the
// game back and raised again once it has room. The player's choice stays the ceiling.
constexpr float kDynSteps[] = {1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f};
constexpr int kDynStepCount = static_cast<int>(sizeof(kDynSteps) / sizeof(kDynSteps[0]));
// Floor 240 lines (0.5x), as in 1.1. 1.2 raised it to 480 for a sharper picture on Mali, and that
// left dynamic resolution with nothing to do at 1x, the most common setting on weak phones: ultimates
// and cutscenes that 1.1 carried at 240-360 lines stayed at 480 and fell to 5-20 fps (1.2 reports,
// Adreno 610/618, Mali-G52/G57). Coarse for a moment beats a slide show; the steps climb back.
constexpr float kDynMinLines = 240.0f;
// Emergency only: a GPU far past its budget (an ultimate's full-screen effects took a Mali-G52 MC2 to
// 46-64 ms a frame at 480 lines under the 30 FPS Lock) may drop below the floor to this many lines,
// in one jump, and climbs back to 480 the usual way once the GPU has room again.
constexpr float kDynEmergencyLines = 336.0f;
constexpr double kDynEmergencyGpuMs = 22.0;   // per 60 fps frame; doubled by the 30 fps budget
constexpr uint64_t kDynEmergencyDrops = 6;    // presenter drops in one window (a tenth of the frames)
// Under 480 lines already (Ultra Low: 360) the emergency floor is 70% of the player's lines, never
// below 240: 336 sat above 360 x 0.9, so dynamic resolution could not move there at all.
constexpr float kDynEmergencyMinLines = 240.0f;
bool g_dynEmergency = false;

float EmergencyLines(float lines) {
    return std::min(kDynEmergencyLines, std::max(kDynEmergencyMinLines, lines * 0.7f));
}
constexpr uint32_t kDynWindowFrames = 60;     // one decision a second at 60 fps
constexpr double kDynDownBusyMs = 15.0;       // worker this busy a cycle: it is the limit
constexpr double kDynDownWaitMs = 2.0;        // ...or the game waits this long a frame for it
constexpr double kDynDownWaitBusyMs = 13.0;  //    while the worker is at least this busy
constexpr double kDynUpBusyMs = 11.5;         // predicted worker time at the next step up
// Vulkan: the GPU's own time a presented frame (timestamps). The worker there hands the GPU its
// work and goes on, so a GPU that fills the frame barely shows in the worker's time.
constexpr double kDynDownGpuMs = 15.5;        // GPU this busy a frame: it is the limit
constexpr double kDynUpGpuMs = 14.0;          // predicted GPU time at the next step up
// GPU timestamps alone are not load: Adreno's governor clocks a lightly loaded GPU down until the
// work fills the frame, so 'gpu ms' tracks the budget instead of the load. Redmi (Adreno 618, 1.1,
// village): 64 draws in one 1067x480 pass read 11 ms, and 17-21 ms "a picture" at 56 pictures a
// second (over 100% of the GPU) while every picture was shown, the worker took 2 ms and the game
// waited under 1 ms; dynamic resolution still walked 100% -> 50%. A GPU that is the limit makes the
// worker wait in prepare for an in-flight frame (or the presenter drop pictures), so a GPU step
// down needs that pressure as well.
constexpr double kDynGpuPressurePrepareMs = 2.0;  // prepare waits this long a cycle: GPU frames behind
constexpr int kDynUpWindows = 3;              // calm windows in a row before stepping up
constexpr int kDynUpWindowsMax = 24;          // ...raised up to this while steps up keep bouncing back
constexpr int kDynBounceWindows = 5;          // a step down this soon after a step up is a bounce
constexpr int kDynMinGapWindows = 5;          // windows between two dynamic steps (each one rebuilds the targets)
int g_dynSinceChange = 1 << 20;
// While the phone runs hot (Thermal::Hot, ten seconds ahead), at least this step: the GPU shares the
// SoC's heat budget with the CPU, so fewer pixels keep the CPU clocks up in a long battle.
constexpr int kDynHotStep = 2;                // 80%
// ...but only while the GPU does a real share of the frame: a CPU-bound game gains nothing from it.
constexpr double kDynHotBusyMs = 8.0;
std::atomic<bool> g_dynEnabled{false};
int g_dynStep = 0;
int g_dynCalm = 0;
int g_dynUpNeed = kDynUpWindows;              // calm windows needed now (grows on bounces)
int g_dynSinceUp = 1 << 20;                   // windows since the last step up
uint32_t g_dynFrames = 0;
bool g_dynSkipWindow = true;                  // the window after a resize carries its stall
AuroraFrameWorkerTotals g_dynLast{};
// Per room, after CubeEvil4's AutoLevel: a step up that bounced back down in a room is not tried there
// again for kDynRoomForget (a 60 FPS Full log, Adreno 618 in the village: 90 -> 50 -> 60 -> 50 % in the
// same place). Other rooms try it after the usual wait. A new room and the screens without play (title,
// Load, typewriter, logos) are not judged: the first windows of a room carry its loading, and the title
// says nothing about the GPU at play.
constexpr double kDynRoomForget = 600.0;      // seconds
constexpr int kDynRoomQuiet = 2;              // windows not judged after a new room
struct DynRoomBlock {
    uint16_t room = 0;
    int step = 0;                             // this step and every higher one did not hold here
    std::chrono::steady_clock::time_point at{};
};
DynRoomBlock g_dynRoomBlocks[32];
int g_dynRoomNext = 0;
uint16_t g_dynRoom = 0;
int g_dynQuiet = 0;
uint16_t g_dynHeldLogged = 0xFFFF;            // the room whose hold was logged last

void DynRememberBounce(uint16_t room, int step) {
    if (room == 0) return;
    const auto now = std::chrono::steady_clock::now();
    for (DynRoomBlock& b : g_dynRoomBlocks) {
        if (b.room == room) {
            b.step = std::max(b.step, step);
            b.at = now;
            return;
        }
    }
    g_dynRoomBlocks[g_dynRoomNext] = {room, step, now};
    g_dynRoomNext = (g_dynRoomNext + 1) % 32;
}

// The step this room did not hold, or -1.
int DynBlockedStep(uint16_t room) {
    if (room == 0) return -1;
    const auto now = std::chrono::steady_clock::now();
    for (const DynRoomBlock& b : g_dynRoomBlocks) {
        if (b.room == room && std::chrono::duration<double>(now - b.at).count() < kDynRoomForget) {
            return b.step;
        }
    }
    return -1;
}

// Every aspect applies while playing: widescreen is the host's (aurora's wide projection). Fill
// shows the boot logos and the typewriter's save screen at 16:9 with side bars instead: the logos'
// white fade filled the whole screen (tester video, Beta 4). The title's main menu is cropped and
// filled like play since the 1.0 reports (players asked for a full-screen main menu).
int32_t EffectiveAspect(int32_t aspect) {
    return aspect == kAspectFill && GameTweaks::FramedBars() ? kAspect16x9 : aspect;
}

bool SurfaceWiderThan169() {
    return g_surfaceWidth != 0 && g_surfaceHeight != 0 &&
           static_cast<float>(g_surfaceWidth) / static_cast<float>(g_surfaceHeight) > kRatio16x9;
}

// Render scale 0 ("match screen resolution"): exactly as many lines as the picture is tall on the
// surface, so it renders 1:1 (a 2560x1080 surface in Fill renders 2560x1080, with the game's view
// widened to match), not the window size with the EFB's workspace rows squeezed in.
float RenderLines(float renderScale, int32_t aspect) {
    if (renderScale > 0.0f || g_surfaceWidth == 0 || g_surfaceHeight == 0) {
        return LinesForScale(renderScale);
    }
    const float w = static_cast<float>(g_surfaceWidth), h = static_cast<float>(g_surfaceHeight);
    const bool fillsSurface = aspect == kAspectFill || (aspect == kAspect4x3 && g_stretch43.load(std::memory_order_relaxed));
    const float pictureAspect = aspect == kAspect4x3 ? 4.0f / 3.0f : kRatio16x9;
    return fillsSurface ? h : std::min(h, w / pictureAspect);
}

void ApplyRenderer(const Settings& settings) {
    const int32_t aspect = EffectiveAspect(settings.aspect);
    if (aspect == kAspect4x3) {
        AuroraSetViewportPolicy(g_stretch43.load(std::memory_order_relaxed) ? AURORA_VIEWPORT_STRETCH : AURORA_VIEWPORT_FIT);
        VILockAspectRatio(4, 3);
    } else if (aspect == kAspect16x9) {
        VIUnlockAspectRatio();
        AuroraSetViewportPolicy(AURORA_VIEWPORT_16_9);
    } else {
        // The framebuffer takes the surface's aspect; the game draws the wider view into it.
        VIUnlockAspectRatio();
        AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
    }
    const float lines = RenderLines(settings.renderScale, aspect);
    const float factor = kDynSteps[g_dynStep];  // 0 (100%) unless dynamic resolution or the heat lowered it
    const float floorLines = g_dynEmergency ? std::min(EmergencyLines(lines), kDynMinLines) : kDynMinLines;
    const float dynLines = factor < 1.0f && lines > 0.0f ? std::max(std::min(lines, floorLines), lines * factor)
                                                         : lines;
    VISetFrameBufferLines(dynLines);
    RT_LOGF(RT_TAG_VI, "display: aspect %d%s, render %.0f lines (%.2gx, dynamic %.0f%%), surface %ux%u\n", aspect,
            aspect != settings.aspect ? " (game booted in 4:3)" : "", dynLines, settings.renderScale,
            factor * 100.0f, g_surfaceWidth, g_surfaceHeight);
}

// Once a window: steps the dynamic factor from the frame worker's time. Returns true when it changed.
bool DynamicResolutionTick() {
    const bool dynamic = g_dynEnabled.load(std::memory_order_relaxed);
    const bool hot = Thermal::Hot();
    if (!dynamic && !hot && g_dynStep == 0) {
        g_dynFrames = 0;
        g_dynSkipWindow = true;  // the worker totals since now are stale when this wakes up
        return false;
    }
    if (!dynamic && !hot) {
        // Neither the player's dynamic resolution nor the heat: back to full, a step a window.
        if (++g_dynFrames < kDynWindowFrames) {
            return false;
        }
        g_dynFrames = 0;
        RT_LOGF(RT_TAG_VI, "dynamic resolution: %.0f%% -> %.0f%% (cool again)\n", kDynSteps[g_dynStep] * 100.0f,
                kDynSteps[g_dynStep - 1] * 100.0f);
        g_dynStep -= 1;
        g_dynCalm = 0;
        if (g_dynStep == 0) g_dynEmergency = false;
        return true;
    }
    if (++g_dynFrames < kDynWindowFrames) {
        return false;
    }
    AuroraFrameWorkerTotals now{};
    aurora_get_frame_worker_totals(&now);
    const uint64_t cycles = now.cycles - g_dynLast.cycles;
    const double busyMs = cycles ? (now.busyNanos - g_dynLast.busyNanos) / 1e6 / cycles : 0.0;
    const double waitMs = (now.producerWaitNanos - g_dynLast.producerWaitNanos) / 1e6 / g_dynFrames;
    const uint64_t gpuFrames = now.gpuFrames - g_dynLast.gpuFrames;
    // Per frame the worker rendered, not per present: the GPU time of frames the presenter dropped
    // lands on the next present, and a Mali-G57 MP1 then read 80-140 ms "a frame" and went to 50%.
    const uint64_t gpuDivisor = std::max<uint64_t>(gpuFrames, cycles);
    const double gpuMs = gpuFrames ? (now.gpuNanos - g_dynLast.gpuNanos) / 1e6 / gpuDivisor : 0.0;
    // Signals that need no GPU timestamps (stock Adreno drivers give implausible ones, so 1.2 never
    // saw its ultimates as a GPU spike): the worker held in prepare by a GPU that is frames behind
    // (the in-flight bound waits there), and frames the presenter threw away.
    const double prepareMs = cycles ? (now.prepareNanos - g_dynLast.prepareNanos) / 1e6 / cycles : 0.0;
    const uint64_t dropped = now.droppedPresents - g_dynLast.droppedPresents;
    g_dynLast = now;
    g_dynFrames = 0;
    if (g_dynSinceUp < (1 << 20)) {
        ++g_dynSinceUp;
        if (g_dynSinceUp % 30 == 0 && g_dynUpNeed > kDynUpWindows) {
            g_dynUpNeed = std::max(g_dynUpNeed / 2, kDynUpWindows);  // the steps up hold again
        }
    }
    if (g_dynSinceChange < (1 << 20)) {
        ++g_dynSinceChange;
    }
    if (g_dynSkipWindow || cycles == 0) {
        g_dynSkipWindow = false;
        return false;
    }
    const uint16_t room = GameTweaks::CurrentRoom();
    if (room != g_dynRoom) {
        g_dynRoom = room;
        g_dynQuiet = kDynRoomQuiet;
        g_dynCalm = 0;
        g_dynHeldLogged = 0xFFFF;
    }
    if (dynamic && (room == 0 || g_dynQuiet > 0)) {
        if (g_dynQuiet > 0) --g_dynQuiet;
        return false;
    }

    int step = g_dynStep;
    const char* reason = "";
    // The thresholds are for a 60 fps picture; RE4 plays at 30 (menus at 60), so a frame has twice
    // the time. At 1.0 a 15 ms worker on the Redmi (Adreno 618, 18 fps, game thread-bound) walked
    // the picture down to 50% without gaining a frame.
    const double budget = 2.0;
    // GPU time is per presented picture. With frame interpolation (60 FPS) every game frame presents
    // two, so a picture has a 60 fps frame's time again (Redmi Beta 8: 15 ms a picture, Present
    // blocked 28-31 ms and only 33-43 pictures a second, while the 30 fps budget stepped up to 100%).
    const double gpuBudget = aurora_get_frame_interpolation_fps() != 0 ? 1.0 : budget;
    static const bool s_cycleTest = [] {
        const char* v = std::getenv("RE4_DYNRES");
        return v && *v == '2';
    }();
    const float curLines = RenderLines(g_current.renderScale, EffectiveAspect(g_current.aspect));
    const bool gpuPressure = prepareMs > kDynGpuPressurePrepareMs || dropped > 0;
    const bool gpuOver = (gpuMs > kDynEmergencyGpuMs * gpuBudget && gpuPressure) ||
                         prepareMs > kDynEmergencyGpuMs * budget;
    // One window over the line is a moment (a room load, a menu, a burst of effects): 1.0 jumped from
    // 80% to 50% on one 34 ms second at 60 FPS and stayed coarse (Samsung M15, Mali-G57, 2026-11-20).
    // The jump needs two windows in a row, or dropped pictures; a single one steps down as usual.
    static int s_gpuOverWindows = 0;
    s_gpuOverWindows = gpuOver ? s_gpuOverWindows + 1 : 0;
    const bool gpuSpike = (gpuOver && s_gpuOverWindows >= 2) || dropped >= kDynEmergencyDrops;
    if (dynamic && !s_cycleTest && curLines > 0.0f && gpuSpike) {
        // GPU spike: jump straight to the deepest step that keeps the emergency lines.
        const float emergencyLines = EmergencyLines(curLines);
        int target = g_dynStep;
        while (target + 1 < kDynStepCount && curLines * kDynSteps[target + 1] >= emergencyLines - 0.5f) {
            ++target;
        }
        if (target > g_dynStep && (!g_dynEmergency || curLines * kDynSteps[g_dynStep] > emergencyLines)) {
            if (g_dynSinceUp <= kDynBounceWindows) DynRememberBounce(room, g_dynStep);
            g_dynEmergency = true;
            g_dynCalm = 0;
            g_dynSinceChange = 0;
            RT_LOGF(RT_TAG_VI,
                    "dynamic resolution: %.0f%% -> %.0f%% (gpu spike: gpu %.2f ms, prepare %.2f ms a frame, %llu "
                    "dropped, emergency %.0f lines)\n",
                    kDynSteps[g_dynStep] * 100.0f, kDynSteps[target] * 100.0f, gpuMs, prepareMs,
                    static_cast<unsigned long long>(dropped), curLines * kDynSteps[target]);
            g_dynStep = target;
            g_dynSkipWindow = true;
            return true;
        }
    }
    if (s_cycleTest) {
        // RE4_DYNRES=2 (test): walk down every step and back up, one a window.
        static int s_dir = 1;
        if (step + s_dir < 0 || step + s_dir >= kDynStepCount) s_dir = -s_dir;
        step += s_dir;
    } else if (dynamic && (busyMs > kDynDownBusyMs * budget || (gpuMs > kDynDownGpuMs * gpuBudget && gpuPressure) ||
                           (waitMs > kDynDownWaitMs && busyMs > kDynDownWaitBusyMs * budget))) {
        g_dynCalm = 0;
        step = std::min(step + 1, kDynStepCount - 1);
        if (step != g_dynStep && g_dynSinceUp <= kDynBounceWindows) {
            g_dynUpNeed = std::min(g_dynUpNeed * 2, kDynUpWindowsMax);  // the last step up did not hold
            DynRememberBounce(room, g_dynStep);
        }
    } else if (hot && step < kDynHotStep && (busyMs >= kDynHotBusyMs * budget || gpuMs >= kDynHotBusyMs * gpuBudget)) {
        g_dynCalm = 0;
        step += 1;
        reason = ", running hot";
    } else if (hot && step <= kDynHotStep) {
        g_dynCalm = 0;  // hold: no step up while hot (CPU-bound: stays where it is, often 100%)
    } else if (!dynamic) {
        step -= 1;  // hot only: below the heat's step from an earlier dynamic run, back up to it
    } else if (step > 0) {
        // Predicted worker time one step up. Measured on Mali (G81, ~9,000 draws) the worker grows about
        // linearly with the line factor, not with the pixel count: most of it is per draw, not per pixel
        // (50% 8 ms, 100% 16.7 ms). The square kept it at 50% for good (8.2 ms -> 11.8 > 11.5).
        const float ratio = kDynSteps[step - 1] / kDynSteps[step];
        const double predicted = busyMs * ratio;
        // Linear like the worker's: on the Redmi (Adreno 618) a calm battle took 12.8 ms at 100% and still
        // 10.5 ms at 60%, most of it in passes at the display's size. The square kept it at 60% for good.
        const double predictedGpu = gpuMs * ratio;
        // Without pressure the GPU keeps up whatever its timestamps say; a step up that brings
        // pressure steps back down and is remembered as a bounce.
        const bool fits = predicted < kDynUpBusyMs * budget &&
                          (!gpuPressure || predictedGpu < kDynUpGpuMs * gpuBudget);
        const int blocked = DynBlockedStep(room);
        if (fits && blocked >= step - 1) {
            g_dynCalm = 0;  // the step up did not hold in this room: stay
            if (g_dynHeldLogged != room) {
                g_dynHeldLogged = room;
                RT_LOGF(RT_TAG_VI, "dynamic resolution: stays at %.0f%% in room %X (%.0f%% did not hold here)\n",
                        kDynSteps[step] * 100.0f, room, kDynSteps[blocked] * 100.0f);
            }
        } else if (fits && ++g_dynCalm >= g_dynUpNeed) {
            g_dynCalm = 0;
            step -= 1;
            g_dynSinceUp = 0;
        } else if (!fits) {
            g_dynCalm = 0;
        }
    }
    // At the 480-line floor a step down changes nothing on screen: hold instead of stepping in the
    // dark (a step up from there would then wait out windows that bought nothing).
    if (step > g_dynStep && !s_cycleTest) {
        const float floorLines = g_dynEmergency ? std::min(EmergencyLines(curLines), kDynMinLines) : kDynMinLines;
        if (curLines > 0.0f && curLines * kDynSteps[g_dynStep] <= floorLines) {
            return false;
        }
    }
    if (step == g_dynStep) {
        return false;
    }
    if (dynamic && !s_cycleTest && g_dynSinceChange < kDynMinGapWindows) {
        return false;  // too soon after the last step: wait (the calm count is kept)
    }
    g_dynSinceChange = 0;
    RT_LOGF(RT_TAG_VI,
            "dynamic resolution: %.0f%% -> %.0f%% (worker %.2f ms a cycle, gpu %.2f ms a frame, prepare %.2f ms, "
            "%llu dropped, game waited %.2f ms a frame%s)\n",
            kDynSteps[g_dynStep] * 100.0f, kDynSteps[step] * 100.0f, busyMs, gpuMs, prepareMs,
            static_cast<unsigned long long>(dropped), waitMs, reason);
    g_dynStep = step;
    g_dynSkipWindow = true;
    // Back at or above the normal floor: the emergency is over.
    if (g_dynEmergency && (step == 0 || curLines * kDynSteps[step] >= kDynMinLines)) {
        g_dynEmergency = false;
    }
    return true;
}

// The game's picture, 3D and 2D alike, is stretched to the canvas. Real Widescreen (Hor+, the 3D
// camera widened to the screen) was removed after Beta 4: RE4 draws HUD icons, gauges, menu panels
// and extra world passes with their own cameras, and widening some but not others moved them off
// the 2D text and doubled models.
void ApplyGameCanvas(int32_t aspect) {
    const bool fitted = aspect == kAspectFill && g_surfaceWidth != 0 && g_surfaceHeight != 0;
    // RE4_FILL_2D=1 (test): 2D (HUD, text, menus) keeps its 16:9 shape in the middle instead of
    // stretching to the edges. Off: players found the narrowed HUD and the bars in menus worse.
    static const bool s_fill2D = [] {
        const char* v = std::getenv("RE4_FILL_2D");
        return v && *v == '1';
    }();
    AuroraSetFill2D(fitted && s_fill2D);
}

} // namespace

float LinesForScale(float renderScale) {
    return renderScale > 0.0f ? kLinesPerScale * renderScale : 0.0f;
}

// Deterministic mode (network play) keeps every device on the same 16:9 canvas: how wide the game
// draws decides which enemies it wakes, so Fill's screen-dependent width would desync the session.
Settings Lockstep(Settings settings) {
    if (DetClock::Enabled()) {
        settings.aspect = kAspect16x9;
    }
    return settings;
}

void Initialize(const Settings& requested) {
    const Settings settings = Lockstep(requested);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_current = settings;
    g_rendererDirty = true;
}

void Request(const Settings& settings) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending = Lockstep(settings);
    g_hasPending = true;
}

#if defined(__ANDROID__)
// On 90/120 Hz panels the compositor and the panel otherwise run at the high rate for a 60 fps game.
// ANativeWindow_setFrameRate (API 30) asks for 60; with frame interpolation on (120-240 fps
// output) the preference is cleared so the panel can go high. Checked once a second because the
// window behind the SDL window changes when the surface is recreated. RE4_SURFACE_FPS=0: off.
void ApplySurfaceFrameRate() {
    static int s_frames = 0;
    if (++s_frames < 60) {
        return;
    }
    s_frames = 0;
    using SetFrameRateFn = int32_t (*)(ANativeWindow*, float, int8_t);
    static const SetFrameRateFn s_set = [] {
        const char* env = std::getenv("RE4_SURFACE_FPS");
        if (env != nullptr && std::strcmp(env, "0") == 0) {
            return static_cast<SetFrameRateFn>(nullptr);
        }
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        return lib ? reinterpret_cast<SetFrameRateFn>(dlsym(lib, "ANativeWindow_setFrameRate")) : nullptr;
    }();
    if (s_set == nullptr) {
        return;
    }
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_Window* sdlWindow = windows != nullptr && count > 0 ? windows[0] : nullptr;
    SDL_free(windows);
    auto* window = sdlWindow != nullptr ? static_cast<ANativeWindow*>(SDL_GetPointerProperty(
                                              SDL_GetWindowProperties(sdlWindow),
                                              SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr))
                                        : nullptr;
    const float want = aurora_get_frame_interpolation_fps() > 60 ? 0.0f : 60.0f;
    static ANativeWindow* s_window = nullptr;
    static float s_want = -1.0f;
    if (window == nullptr || (window == s_window && want == s_want)) {
        return;
    }
    const int32_t result = s_set(window, want, 1 /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE */);
    s_window = window;
    s_want = want;
    RT_LOGF(RT_TAG_VI, "display: surface frame rate %s (%d)\n", want > 0.0f ? "60 Hz" : "free", result);
}
#endif

// Adaptive 60 FPS (1.0 reports: on mid-range phones 30 felt better than a forced 60). 60 FPS Full
// draws every 3D draw again for the in-between picture, about twice the GPU work: a Mali-G57 MC2 at
// its 50% dynamic floor still spent 15-16 ms a picture (worst 40) and showed 40-55 new pictures a
// second, an uneven mix of 60 and 30 that reads as stutter. When two windows in a row show fewer
// than 52 new pictures a second, the in-between pictures are held off and the game runs a steady
// 30; 60 is tried again after 5 s, then 10, 20, 40 and 60 s while the area stays too heavy, and the
// wait is back at 5 s once 60 has held for a minute. Menus and framed screens are not judged (they
// are held to 30 anyway). RE4_60FPS_ADAPTIVE=0 turns it off.
std::atomic<bool> g_sixtyHeld{false};

void AdaptiveSixtyTick() {
    // RE4_60FPS_ADAPTIVE=0 off, =2 (test) judges every window too slow, to exercise the hold.
    static const char* s_env = std::getenv("RE4_60FPS_ADAPTIVE");
    static const bool s_enabled = s_env == nullptr || *s_env != '0';
    static const double s_minRate = s_env != nullptr && *s_env == '2' ? 61.0 : 52.0;
    using SteadyClock = std::chrono::steady_clock;
    static SteadyClock::time_point s_windowStart{}, s_holdUntil{}, s_goodSince{};
    static uint64_t s_motionBase = 0;
    static int s_badWindows = 0;
    static int s_holdSeconds = 5;
    const bool on = s_enabled && (aurora_get_frame_interpolation_fps() != 0 || aurora_get_frame_gen_lite());
    const auto now = SteadyClock::now();
    AuroraPresentTiming timing{};
    aurora_get_present_timing(&timing);
    const auto restartWindow = [&] {
        s_windowStart = now;
        s_motionBase = timing.totalNewMotionCount;
    };
    if (!on) {
        if (g_sixtyHeld.exchange(false, std::memory_order_relaxed)) {
            RT_LOGF(RT_TAG_VI, "adaptive 60: off\n");
        }
        s_badWindows = 0;
        restartWindow();
        return;
    }
    if (g_sixtyHeld.load(std::memory_order_relaxed)) {
        if (now >= s_holdUntil) {
            g_sixtyHeld.store(false, std::memory_order_relaxed);
            s_badWindows = 0;
            s_goodSince = now;
            restartWindow();
            RT_LOGF(RT_TAG_VI, "adaptive 60: trying 60 again\n");
        }
        return;
    }
    if (GameTweaks::MenuScreen() || GameTweaks::FramedScreen() || s_windowStart == SteadyClock::time_point{}) {
        restartWindow();
        return;
    }
    const double span = std::chrono::duration<double>(now - s_windowStart).count();
    if (span < 1.0) {
        return;
    }
    // A longer window spans a pause or a load: no verdict.
    const double rate = span > 1.5 ? 60.0 : (timing.totalNewMotionCount - s_motionBase) / span;
    restartWindow();
    if (rate >= s_minRate) {
        s_badWindows = 0;
        if (s_goodSince == SteadyClock::time_point{}) s_goodSince = now;
        if (now - s_goodSince >= std::chrono::seconds(60)) s_holdSeconds = 5;
        return;
    }
    s_goodSince = {};
    if (++s_badWindows < 2) {
        return;
    }
    s_badWindows = 0;
    g_sixtyHeld.store(true, std::memory_order_relaxed);
    s_holdUntil = now + std::chrono::seconds(s_holdSeconds);
    RT_LOGF(RT_TAG_VI, "adaptive 60: %.1f new pictures/s, steady 30 for %d s\n", rate, s_holdSeconds);
    s_holdSeconds = std::min(s_holdSeconds * 2, 60);
}

bool SixtyHeldForPerformance() {
    return g_sixtyHeld.load(std::memory_order_relaxed);
}

void OnFrameBoundary(uint32_t surfaceWidth, uint32_t surfaceHeight) {
#if defined(__ANDROID__)
    ApplySurfaceFrameRate();
#endif
    AdaptiveSixtyTick();
    // RE4_TEX_FILTER_FILE=path (test): the digit in that file (0-2) is the Texture Detail mode,
    // read every 10 frames, so one scene can be compared with and without it.
    static const char* s_texFilterFile = std::getenv("RE4_TEX_FILTER_FILE");
    static uint32_t s_texFilterFrames = 0;
    if (s_texFilterFile && *s_texFilterFile && ++s_texFilterFrames % 10 == 0) {
        if (FILE* f = std::fopen(s_texFilterFile, "rb")) {
            const int c = std::fgetc(f);
            std::fclose(f);
            if (c >= '0' && c <= '2' && aurora_get_texture_filter() != c - '0') {
                aurora_set_texture_filter(c - '0');
                RT_LOGF(RT_TAG_VI, "texture filter test: mode %d\n", c - '0');
            }
        }
    }
    Settings settings;
    bool renderer = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_hasPending) {
            g_current = g_pending;
            g_hasPending = false;
            g_rendererDirty = true;
        }
        if (surfaceWidth != 0 && surfaceHeight != 0 &&
            (surfaceWidth != g_surfaceWidth || surfaceHeight != g_surfaceHeight)) {
            g_surfaceWidth = surfaceWidth;
            g_surfaceHeight = surfaceHeight;
            // A "match screen" render follows every size.
            if (g_current.renderScale <= 0.0f) {
                g_rendererDirty = true;
            }
        }
        if (DynamicResolutionTick()) {
            g_rendererDirty = true;
        }
        // A framed screen opening or closing changes the effective aspect (EffectiveAspect).
        static int32_t s_lastEffective = -1;
        const int32_t effective = EffectiveAspect(g_current.aspect);
        if (effective != s_lastEffective) {
            s_lastEffective = effective;
            g_rendererDirty = true;
        }
        settings = g_current;
        renderer = g_rendererDirty;
        g_rendererDirty = false;
    }
    if (renderer) {
        ApplyRenderer(settings);
    }
    ApplyGameCanvas(EffectiveAspect(settings.aspect));
}

void SetDynamicResolution(bool enabled) {
    if (g_dynEnabled.exchange(enabled, std::memory_order_relaxed) != enabled) {
        RT_LOGF(RT_TAG_VI, "dynamic resolution %s\n", enabled ? "on" : "off");
    }
}

void SetStretch43(bool enabled) {
    if (g_stretch43.exchange(enabled, std::memory_order_relaxed) != enabled) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_rendererDirty = true;
        RT_LOGF(RT_TAG_VI, "display: 4:3 full screen %s\n", enabled ? "on" : "off");
    }
}

bool DynamicResolutionEnabled() {
    return g_dynEnabled.load(std::memory_order_relaxed);
}

Settings Current() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_current;
}

Settings Latest() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_hasPending ? g_pending : g_current;
}

bool GameWidescreen() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return (g_hasPending ? g_pending : g_current).aspect != kAspect4x3;
}

bool SurfaceWide() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return SurfaceWiderThan169();
}

} // namespace DisplaySettings
