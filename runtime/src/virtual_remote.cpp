#include "virtual_remote.h"

#include "cheats.h"
#include "game_tweaks.h"

#include "input_bindings.h"
#include "keybinds.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <dolphin/pad.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <utility>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace VirtualRemote {
namespace {

// WPAD_BUTTON_* bits (KPADStatus.hold): the Wii Remote the Classic Controller plugs into.
constexpr uint32_t kWpadLeft = 0x0001, kWpadRight = 0x0002, kWpadDown = 0x0004, kWpadUp = 0x0008,
                   kWpadPlus = 0x0010, kWpadB = 0x0400, kWpadA = 0x0800, kWpadMinus = 0x1000, kWpadHome = 0x8000;

// WPAD_CL_BUTTON_* bits (KPADStatus.ex_status.cl.hold), as in wii_remote_input.cpp.
constexpr uint32_t kClUp = 0x0001, kClLeft = 0x0002, kClZR = 0x0004, kClX = 0x0008, kClA = 0x0010, kClY = 0x0020,
                   kClB = 0x0040, kClZL = 0x0080, kClR = 0x0200, kClPlus = 0x0400, kClHome = 0x0800,
                   kClMinus = 0x1000, kClL = 0x2000, kClDown = 0x4000, kClRight = 0x8000;


constexpr int kTriggerThreshold = 128;         // of 255
// aurora's PADRead polls SDL; the game probes and reads all four channels each frame.
constexpr uint64_t kPadRefreshNs = 4'000'000;

struct Intent {
    uint32_t cl = 0;                  // WPAD_CL_BUTTON_* bits
    float lStick[2] = {};             // -1..1, +y up
    float rStick[2] = {};
    uint8_t triggerL = 0;             // 0..255
    uint8_t triggerR = 0;
    float aimStick[2] = {};           // Alternate Controls: the gamepad's right stick, aiming while L / R holds
};

// Written by the Android UI thread (SetTouchState), read on the guest thread. The *SinceRead
// latches keep a press shorter than one 60 Hz input read (a quick tap) visible for one read.
std::atomic<uint32_t> g_touchButtons{0};
std::atomic<uint32_t> g_touchButtonsSinceRead{0};
// RE4 reads input more than once a frame (KPADRead, then KPADGetUnifiedWpadStatus for the Classic
// Controller), so a latched tap stays until every read of that frame saw it: bits first seen at
// g_touchSeenNs are cleared from the latch by the first read 8 ms or more later. Clearing it at the
// first read lost taps whose press and release Android delivered together, which it does when a busy
// GPU (60 FPS) delays the UI thread: the menu cursor then did not move (Infinix, Beta 8).
uint32_t g_touchSeen = 0;
uint64_t g_touchSeenNs = 0;
// Input log (beta builds): presses arriving from the touch controls, and what reached player 1.
std::atomic<uint32_t> g_touchPresses{0};
std::array<std::atomic<float>, 4> g_touchSticks{};  // left x, y, right x, y

// SetGamepadMap: one source per Classic button, in kMapButtons order. -1 = none, 0.. = an
// SDL_GamepadButton, kAxisSource + n = SDL_GamepadAxis n (a trigger). Off = PadIntent's own layout.
struct MapButton {
    const char* name;
    uint32_t cl;
};
constexpr MapButton kMapButtons[] = {
    {"a", kClA}, {"b", kClB}, {"x", kClX}, {"y", kClY}, {"l", kClL}, {"r", kClR}, {"zl", kClZL},
    {"zr", kClZR}, {"plus", kClPlus}, {"minus", kClMinus}, {"home", kClHome}, {"up", kClUp},
    {"down", kClDown}, {"left", kClLeft}, {"right", kClRight},
};
constexpr size_t kMapCount = sizeof(kMapButtons) / sizeof(kMapButtons[0]);
constexpr int kAxisSource = 100;
// kJoySource + n = raw joystick button n: generic USB pads send BUTTON_1..16, which SDL's Android
// joystick numbers 20..35 and its automatic gamepad mapping leaves out (GitHub issue #7).
constexpr int kJoySource = 200;
std::array<std::atomic<int>, kMapCount> g_map{};
std::atomic<bool> g_mapOn{false};

// Settings > Controls (SetPadOptions, SetToggleRun, SetMouseAim, TapQte, KnifeSwipe).
std::atomic<bool> g_altControls{false};
std::atomic<bool> g_swapFace{false};
std::atomic<int> g_deadZone{0};  // percent of the stick's way
std::atomic<bool> g_swapSticks{false};
std::atomic<bool> g_invertRightY{false};
std::atomic<int> g_rightAxisX{-1};  // SDL joystick axis numbers of the right stick, -1 = the gamepad mapping's
std::atomic<int> g_rightAxisY{-1};
std::atomic<int> g_range{100};
std::atomic<bool> g_toggleRun{false};
std::atomic<bool> g_mouseAim{false};
std::atomic<float> g_mouseSensitivity{1.0f};
std::atomic<uint64_t> g_tapQteNs{0};
std::atomic<uint32_t> g_tapCount{0};
std::atomic<uint64_t> g_knifeNs{0};
std::atomic<int> g_knifeDir{0};
constexpr uint64_t kTapPressNs = 60'000'000;
constexpr uint64_t kKnifeReadyNs = 160'000'000, kKnifeSlashNs = 300'000'000, kKnifeEndNs = 450'000'000;
// The Reload key (Keybinds::kReload): RE4 reloads on B pressed while R is held, so the key holds R
// for as long as it is down (at least kReloadEndNs) and presses B from kReloadBNs on.
constexpr uint64_t kReloadBNs = 120'000'000, kReloadEndNs = 300'000'000;
bool g_vpadReload = false;  // "RK" in generated/b/vpad.txt (Windows tests): the Reload key held
// Mouse wheel notches (up positive), added on SDL's thread, taken by the guest's pad read.
std::atomic<int> g_wheelNotches{0};

std::mutex g_mutex;
std::array<PADStatus, PAD_MAX_CONTROLLERS> g_pads{};
uint64_t g_padsReadNs = 0;
bool g_keyboardOnAPort = false;
std::array<bool, PAD_MAX_CONTROLLERS> g_keyboardPort{};  // F10 "Keyboard and mouse" on this port
std::array<bool, PAD_MAX_CONTROLLERS> g_announced{};

// Buttons-only pad (GameActivity.dispatchKeyEvent, Android): a clip-on pad Android lists as a keyboard
// (GameSir X3 on an Infinix X6871: "gamepads: none", no gamepad in RE4_ANDROID_PADS, 75 key presses to
// player 1). Its gamepad keys set g_buttonsPadMask; a virtual SDL gamepad carries them to the pad code,
// so Gamepad Buttons, Alternate Controls and the rest apply as to any gamepad.
std::atomic<uint32_t> g_buttonsPadMask{0};
std::atomic<bool> g_buttonsPadWanted{false};
std::mutex g_buttonsPadNameMutex;
std::string g_buttonsPadName;  // guarded by g_buttonsPadNameMutex

void PumpButtonsPad() {
    if (!g_buttonsPadWanted.load(std::memory_order_relaxed)) {
        return;
    }
    static SDL_Joystick* joystick = nullptr;
    static bool attached = false;
    if (!attached) {
        attached = true;
        std::string name;
        {
            std::lock_guard<std::mutex> lock(g_buttonsPadNameMutex);
            name = g_buttonsPadName.empty() ? "Android buttons-only pad" : g_buttonsPadName;
        }
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = name.c_str();  // SDL copies it
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        joystick = id != 0 ? SDL_OpenJoystick(id) : nullptr;
        RT_LOGF(RT_TAG_RUNTIME, "buttons-only pad: %s as a virtual gamepad (%s)\n", name.c_str(),
                joystick != nullptr ? "opened" : SDL_GetError());
    }
    if (joystick == nullptr) {
        return;
    }
    const uint32_t mask = g_buttonsPadMask.load(std::memory_order_relaxed);
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT && i < 30; ++i) {
        SDL_SetJoystickVirtualButton(joystick, i, (mask >> i) & 1u);
    }
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, (mask >> 30) & 1u ? SDL_JOYSTICK_AXIS_MAX : 0);
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, (mask >> 31) & 1u ? SDL_JOYSTICK_AXIS_MAX : 0);
}

// Test hook for checking the gamepad mapping without hardware: RE4_VIRTUAL_GAMEPAD=1 attaches an
// SDL virtual gamepad driven by generated/b/vgamepad.txt ("B=hex [A=lx,ly,rx,ry,lt,rt]", SDL's button
// order and axes from -1 to 1).
void PumpVirtualGamepad() {
    static const bool enabled = std::getenv("RE4_VIRTUAL_GAMEPAD") != nullptr;
    if (!enabled) {
        return;
    }
    static SDL_Joystick* joystick = nullptr;
    static bool attached = false;
    if (!attached) {
        attached = true;
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "Virtual Test Gamepad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        joystick = id != 0 ? SDL_OpenJoystick(id) : nullptr;
        RT_LOGF(RT_TAG_RUNTIME, "virtual gamepad: id=%u opened=%d %s\n", static_cast<unsigned>(id),
                joystick != nullptr ? 1 : 0, joystick != nullptr ? "" : SDL_GetError());
    }
    static uint64_t lastCheck = 0;
    const uint64_t now = SDL_GetTicksNS();
    if (joystick == nullptr || now - lastCheck < 15000000) {
        return;
    }
    lastCheck = now;
    FILE* fp = fopen("generated/b/vgamepad.txt", "r");
    if (fp == nullptr) {
        return;
    }
    char buf[96]{};
    const bool read = fgets(buf, sizeof(buf), fp) != nullptr;
    fclose(fp);
    uint32_t buttons = 0;
    float axes[6] = {0.0f, 0.0f, 0.0f, 0.0f, -1.0f, -1.0f};
    if (read) {
        sscanf(buf, "B=%x", &buttons);
        if (const char* a = strstr(buf, "A=")) {
            sscanf(a + 2, "%f,%f,%f,%f,%f,%f", &axes[0], &axes[1], &axes[2], &axes[3], &axes[4], &axes[5]);
        }
    }
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
        SDL_SetJoystickVirtualButton(joystick, i, (buttons >> i) & 1u);
    }
    for (int i = 0; i < 6; ++i) {
        SDL_SetJoystickVirtualAxis(joystick, i, static_cast<Sint16>(axes[i] * 32767.0f));
    }
}

// Logs every joystick SDL sees (name, USB ids, gamepad or not, game port) when the set changes,
// checked once a second: a tester's "the gamepad does nothing" is then readable from console.log.
void LogGamepadsLocked(uint64_t now) {
    static uint64_t s_checkedNs = 0;
    static std::string s_last;
    if (s_checkedNs != 0 && now - s_checkedNs < 1'000'000'000ull) {
        return;
    }
    s_checkedNs = now;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    std::string text;
    for (int i = 0; i < count; ++i) {
        const SDL_JoystickID id = ids[i];
        int port = -1;
        for (uint32_t p = 0; p < PAD_MAX_CONTROLLERS && port < 0; ++p) {
            const s32 index = PADGetIndexForPort(p);
            SDL_Gamepad* gamepad = index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr;
            if (gamepad != nullptr && SDL_GetGamepadID(gamepad) == id) port = static_cast<int>(p);
        }
        const char* name = SDL_GetJoystickNameForID(id);
        char portText[16] = "no port";
        if (port >= 0) std::snprintf(portText, sizeof(portText), "port %d", port + 1);
        char line[256];
        std::snprintf(line, sizeof(line), "%s%s (%04x:%04x, %s, %s)", text.empty() ? "" : "; ",
                      name != nullptr ? name : "unnamed", SDL_GetJoystickVendorForID(id),
                      SDL_GetJoystickProductForID(id), SDL_IsGamepad(id) ? "gamepad" : "joystick only", portText);
        text += line;
    }
    SDL_free(ids);
    if (text.empty()) {
        text = "none";
    }
#if defined(__ANDROID__)
    // What Android itself lists (GameActivity, at launch) and the HIDAPI switch (aurora window.cpp): a pad
    // Android has but SDL dropped shows up here.
    if (const char* pads = std::getenv("RE4_ANDROID_PADS"); pads != nullptr && pads[0] != '\0') {
        const char* hidapi = SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI);
        text += " [Android at launch: ";
        text += pads;
        text += "; hidapi ";
        text += hidapi != nullptr ? hidapi : "default";
        text += "]";
    }
#endif
    if (text != s_last) {
        s_last = text;
        RT_LOGF(RT_TAG_RUNTIME, "gamepads: %s\n", text.c_str());
    }
}

void RefreshPadsLocked() {
    const uint64_t now = SDL_GetTicksNS();
    if (g_padsReadNs != 0 && now - g_padsReadNs < kPadRefreshNs) {
        return;
    }
    g_padsReadNs = now;
    PumpVirtualGamepad();
    PumpButtonsPad();
    PADRead(g_pads.data());
    LogGamepadsLocked(now);
    InputBindings::Apply(g_pads.data());
    g_keyboardOnAPort = false;
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        uint32_t count = 0;
        g_keyboardPort[port] = PADGetKeyButtonBindings(port, &count) != nullptr;
        if (g_keyboardPort[port]) {
            g_keyboardOnAPort = true;
        }
    }
}

// One stick from four keys, keeping a diagonal on the unit circle.
void KeyStick(float* stick, bool up, bool down, bool left, bool right) {
    const float x = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);
    const float y = (up ? 1.0f : 0.0f) - (down ? 1.0f : 0.0f);
    const float scale = x != 0.0f && y != 0.0f ? 0.7071068f : 1.0f;
    stick[0] = x * scale;
    stick[1] = y * scale;
}

// The built-in layout for port 1, used while no port has aurora's keyboard switched on.
Intent KeyboardIntent() {
    Intent intent;
    if (SDL_GetKeyboardFocus() == nullptr) {
        return intent;
    }
    // The keys come from the F10 bar (Config.toml [keyboard]); none count while one is being
    // picked by a capture, or until the key picked is let go.
    if (Keybinds::Suppressed()) {
        return intent;
    }
    using namespace Keybinds;
    const auto bit = [&intent](int action, uint32_t cl) {
        if (Held(action)) intent.cl |= cl;
    };
    bit(kUp, kClUp);
    bit(kDown, kClDown);
    bit(kLeft, kClLeft);
    bit(kRight, kClRight);
    bit(Keybinds::kA, kClA);
    bit(Keybinds::kB, kClB);
    bit(Keybinds::kX, kClX);
    bit(Keybinds::kY, kClY);
    bit(Keybinds::kZL, kClZL);
    bit(Keybinds::kZR, kClZR);
    bit(kPlus, kClPlus);
    bit(kMinus, kClMinus);
    bit(kHome, kClHome);
    if (Held(Keybinds::kL)) {
        intent.cl |= kClL;
        intent.triggerL = 255;
    }
    if (Held(Keybinds::kR)) {
        intent.cl |= kClR;
        intent.triggerR = 255;
    }
    KeyStick(intent.lStick, Held(kLStickUp), Held(kLStickDown), Held(kLStickLeft), Held(kLStickRight));
    KeyStick(intent.rStick, Held(kRStickUp), Held(kRStickDown), Held(kRStickLeft), Held(kRStickRight));
    return intent;
}

// The on-screen Classic Controller set through SetTouchState and SetTouchSticks.
Intent TouchIntent() {
    Intent intent;
    const uint64_t now = SDL_GetTicksNS();
    if (g_touchSeen != 0 && now - g_touchSeenNs >= 8'000'000ull) {
        g_touchButtonsSinceRead.fetch_and(~g_touchSeen, std::memory_order_relaxed);
        g_touchSeen = 0;
    }
    const uint32_t latched = g_touchButtonsSinceRead.load(std::memory_order_relaxed);
    if ((latched & ~g_touchSeen) != 0) {
        if (g_touchSeen == 0) g_touchSeenNs = now;
        g_touchSeen |= latched;
    }
    const uint32_t touch = g_touchButtons.load(std::memory_order_relaxed) | latched;
    const auto bit = [&](uint32_t button, uint32_t cl) {
        if (touch & button) intent.cl |= cl;
    };
    bit(kTouchLeft, kClLeft);
    bit(kTouchRight, kClRight);
    bit(kTouchUp, kClUp);
    bit(kTouchDown, kClDown);
    bit(kTouchA, kClA);
    bit(kTouchB, kClB);
    bit(kTouchX, kClX);
    bit(kTouchY, kClY);
    bit(kTouchZL, kClZL);
    bit(kTouchZR, kClZR);
    bit(kTouchPlus, kClPlus);
    bit(kTouchMinus, kClMinus);
    bit(kTouchHome, kClHome);
    if (touch & kTouchL) {
        intent.cl |= kClL;
        intent.triggerL = 255;
    }
    if (touch & kTouchR) {
        intent.cl |= kClR;
        intent.triggerR = 255;
    }
    intent.lStick[0] = g_touchSticks[0].load(std::memory_order_relaxed);
    intent.lStick[1] = g_touchSticks[1].load(std::memory_order_relaxed);
    intent.rStick[0] = g_touchSticks[2].load(std::memory_order_relaxed);
    intent.rStick[1] = g_touchSticks[3].load(std::memory_order_relaxed);
    return intent;
}

// File-based virtual pad automation (works without window focus). A line of words, or
// "CL=hex [LS=x,y]" for raw Classic bits and a left stick.
Intent AutomationIntent() {
    static uint64_t lastCheck = 0;
    static Intent file;
    const uint64_t now = SDL_GetTicksNS();
    if (now - lastCheck > 15000000) {
        lastCheck = now;
        file = {};
        g_vpadReload = false;
        FILE* fp = fopen("generated/b/vpad.txt", "r");
        if (fp != nullptr) {
            char buf[64]{};
            if (fgets(buf, sizeof(buf), fp) != nullptr) {
                for (char* p = buf; *p; ++p) *p = static_cast<char>(toupper(*p));
                if (strncmp(buf, "CL=", 3) == 0) {
                    file.cl = static_cast<uint32_t>(std::strtoul(buf + 3, nullptr, 16));
                    if (const char* ls = strstr(buf, "LS=")) {
                        sscanf(ls + 3, "%f,%f", &file.lStick[0], &file.lStick[1]);
                    }
                    if (const char* rs = strstr(buf, "RS=")) {
                        sscanf(rs + 3, "%f,%f", &file.rStick[0], &file.rStick[1]);
                    }
                    g_vpadReload = strstr(buf, "RK") != nullptr;
                } else {
                    if (strstr(buf, "X") || strstr(buf, "2") || strstr(buf, "A")) file.cl |= kClA;
                    if (strstr(buf, "START") || strstr(buf, "PLUS") || strstr(buf, "ENTER")) file.cl |= kClPlus;
                    if (strstr(buf, "UP")) file.cl |= kClUp;
                    if (strstr(buf, "DOWN")) file.cl |= kClDown;
                    if (strstr(buf, "LEFT")) file.cl |= kClLeft;
                    if (strstr(buf, "RIGHT")) file.cl |= kClRight;
                    if (strstr(buf, "1") || strstr(buf, "B") || strstr(buf, "RUN")) file.cl |= kClB;
                }
                // L and R are pulled all the way, as a Classic trigger reports them.
                if (file.cl & kClL) file.triggerL = 255;
                if (file.cl & kClR) file.triggerR = 255;
            }
            fclose(fp);
        }
    }
    return file;
}

// The player's own gamepad buttons (SetGamepadMap) in place of the layout below; the sticks stay.
// L and R carry how far their source is pulled, as the Classic triggers do.
void ApplyGamepadMap(SDL_Gamepad* gamepad, Intent& intent) {
    intent.cl = 0;
    intent.triggerL = 0;
    intent.triggerR = 0;
    for (size_t i = 0; i < kMapCount; ++i) {
        const int source = g_map[i].load(std::memory_order_relaxed);
        if (source < 0) {
            continue;
        }
        uint8_t level = 0;
        if (source >= kJoySource) {
            SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
            if (joystick != nullptr && SDL_GetJoystickButton(joystick, source - kJoySource)) {
                level = 255;
            }
        } else if (source >= kAxisSource) {
            const int value = SDL_GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(source - kAxisSource));
            level = static_cast<uint8_t>(std::clamp(value * 255 / 32767, 0, 255));
        } else if (SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(source))) {
            level = 255;
        }
        const uint32_t cl = kMapButtons[i].cl;
        if (cl == kClL) {
            intent.triggerL = std::max(intent.triggerL, level);
        } else if (cl == kClR) {
            intent.triggerR = std::max(intent.triggerR, level);
        } else if (level >= kTriggerThreshold) {
            intent.cl |= cl;
        }
    }
    if (intent.triggerL >= kTriggerThreshold) intent.cl |= kClL;
    if (intent.triggerR >= kTriggerThreshold) intent.cl |= kClR;
}

// Stick dead zone and range (SetPadOptions): inside the dead zone the stick rests (round, so a diagonal
// keeps its direction); from there to the range its way is stretched to the full way, past it it counts
// as fully pushed, for a worn stick that no longer reaches its edge.
void ShapeStick(float* stick) {
    const int dead = g_deadZone.load(std::memory_order_relaxed);
    const int range = g_range.load(std::memory_order_relaxed);
    if (dead <= 0 && range >= 100) return;
    const float m = std::hypot(stick[0], stick[1]);
    const float lo = static_cast<float>(dead) / 100.0f;
    const float hi = std::max(lo + 0.05f, static_cast<float>(range) / 100.0f);
    if (m <= lo) {
        stick[0] = stick[1] = 0.0f;
        return;
    }
    const float scale = std::min(1.0f, (m - lo) / (hi - lo)) / m;
    stick[0] *= scale;
    stick[1] *= scale;
}

// Settings > Controls (SetStickOptions): the right stick read from the axes the player picked (generic
// pads report it on Z/RZ or RX/RY, and SDL's guessed mapping can take the other pair), then the two
// sticks swapped, then the camera stick's up and down turned around.
void ApplyStickOptions(uint32_t chan, Intent& intent) {
    const int ax = g_rightAxisX.load(std::memory_order_relaxed);
    const int ay = g_rightAxisY.load(std::memory_order_relaxed);
    if (ax >= 0 && ay >= 0) {
        const s32 index = PADGetIndexForPort(chan);
        SDL_Gamepad* gamepad = index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr;
        SDL_Joystick* joystick = gamepad != nullptr ? SDL_GetGamepadJoystick(gamepad) : nullptr;
        const int count = joystick != nullptr ? SDL_GetNumJoystickAxes(joystick) : 0;
        if (ax < count && ay < count) {
            intent.rStick[0] = std::clamp(static_cast<float>(SDL_GetJoystickAxis(joystick, ax)) / 32767.0f, -1.0f, 1.0f);
            intent.rStick[1] = std::clamp(-static_cast<float>(SDL_GetJoystickAxis(joystick, ay)) / 32767.0f, -1.0f, 1.0f);
        }
    }
    if (g_swapSticks.load(std::memory_order_relaxed)) {
        std::swap(intent.lStick[0], intent.rStick[0]);
        std::swap(intent.lStick[1], intent.rStick[1]);
    }
    if (g_invertRightY.load(std::memory_order_relaxed)) {
        intent.rStick[1] = -intent.rStick[1];
    }
}

// A gamepad (after the user's remapping) as the Classic Controller, by position as in Dolphin's
// Classic layout: A east, B south, X north, Y west, the triggers as L and R, the shoulders as ZL
// and ZR. PADStatus has no left shoulder or guide button, so those two come from SDL directly.
Intent PadIntent(uint32_t chan, const PADStatus& pad) {
    Intent intent;
    const uint32_t b = pad.button;
    const auto bit = [&](uint32_t button, uint32_t cl) {
        if (b & button) intent.cl |= cl;
    };
    bit(PAD_BUTTON_LEFT, kClLeft);
    bit(PAD_BUTTON_RIGHT, kClRight);
    bit(PAD_BUTTON_UP, kClUp);
    bit(PAD_BUTTON_DOWN, kClDown);
    // A gamepad goes by position (its south button is the Classic's B). F10's Keyboard and mouse
    // goes by name: the player picked "A" for a key and means the Classic's A (1.1 test report:
    // Mouse left -> A gave B, E -> B gave A, so the left click fired only by luck and E never reloaded).
    const bool byName = chan < PAD_MAX_CONTROLLERS && g_keyboardPort[chan];
    bit(PAD_BUTTON_B, byName ? kClB : kClA);
    bit(PAD_BUTTON_A, byName ? kClA : kClB);
    bit(PAD_BUTTON_Y, byName ? kClY : kClX);
    bit(PAD_BUTTON_X, byName ? kClX : kClY);
    bit(PAD_BUTTON_START, kClPlus);
    bit(PAD_TRIGGER_Z, kClZR);
    if (pad.extButton & PAD_BUTTON_BACK) intent.cl |= kClMinus;
    const s32 index = PADGetIndexForPort(chan);
    if (SDL_Gamepad* gamepad = index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr) {
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) intent.cl |= kClZL;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_GUIDE)) intent.cl |= kClHome;
    }
    const bool own = !g_mapOn.load(std::memory_order_relaxed);  // the default layout, not the player's map
    if (own && g_swapFace.load(std::memory_order_relaxed)) {
        // By label: the south button (Xbox A, PlayStation Cross) is A, the west one (Xbox X) is X.
        const uint32_t face = intent.cl;
        intent.cl &= ~(kClA | kClB | kClX | kClY);
        if (face & kClA) intent.cl |= kClB;
        if (face & kClB) intent.cl |= kClA;
        if (face & kClX) intent.cl |= kClY;
        if (face & kClY) intent.cl |= kClX;
    }
    const uint8_t lt = (b & PAD_TRIGGER_L) != 0 ? 255 : pad.triggerLeft;
    const uint8_t rt = (b & PAD_TRIGGER_R) != 0 ? 255 : pad.triggerRight;
    const bool alternate = own && g_altControls.load(std::memory_order_relaxed);
    if (alternate) {
        // LT aims (R, analog), RT fires (A), LB readies the knife (L) in place of ZL; RB stays the map.
        const bool lb = (intent.cl & kClZL) != 0;
        intent.cl &= ~kClZL;
        intent.triggerR = lt;
        intent.triggerL = lb ? 255 : 0;
        if (rt >= kTriggerThreshold) intent.cl |= kClA;
    } else {
        intent.triggerL = lt;
        intent.triggerR = rt;
    }
    if (intent.triggerL >= kTriggerThreshold) intent.cl |= kClL;
    if (intent.triggerR >= kTriggerThreshold) intent.cl |= kClR;
    intent.lStick[0] = std::clamp(pad.stickX / 127.0f, -1.0f, 1.0f);
    intent.lStick[1] = std::clamp(pad.stickY / 127.0f, -1.0f, 1.0f);
    intent.rStick[0] = std::clamp(pad.substickX / 127.0f, -1.0f, 1.0f);
    intent.rStick[1] = std::clamp(pad.substickY / 127.0f, -1.0f, 1.0f);
    ApplyStickOptions(chan, intent);
    ShapeStick(intent.lStick);
    ShapeStick(intent.rStick);
    if (alternate) {
        intent.aimStick[0] = intent.rStick[0];
        intent.aimStick[1] = intent.rStick[1];
    }
    if (!own) {
        if (SDL_Gamepad* gamepad = index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr) {
            ApplyGamepadMap(gamepad, intent);
        }
    }
    return intent;
}

// Every 10 s with input: touch presses that arrived, Classic presses player 1 got, and the buttons
// held now. A stuck button or presses that never reach the game show here (menu cursor reports).
void LogInputLocked(const Intent& intent) {
    const uint32_t cl = intent.cl;
    static uint32_t s_prev = 0, s_presses = 0;
    static uint64_t s_windowNs = 0;
    s_presses += static_cast<uint32_t>(__builtin_popcount(cl & ~s_prev));
    s_prev = cl;
    const uint64_t now = SDL_GetTicksNS();
    if (s_windowNs == 0) s_windowNs = now;
    if (now - s_windowNs < 10'000'000'000ull) return;
    s_windowNs = now;
    const uint32_t touch = g_touchPresses.exchange(0, std::memory_order_relaxed);
    if (touch == 0 && s_presses == 0 && cl == 0 && intent.lStick[0] == 0.0f && intent.lStick[1] == 0.0f) return;
    RT_LOGF(RT_TAG_RUNTIME, "input 10 s: %u touch presses, %u presses to player 1, held 0x%04x, left stick %.2f,%.2f\n", touch, s_presses, cl,
            intent.lStick[0], intent.lStick[1]);
    s_presses = 0;
}

void AnnounceLocked(uint32_t chan, bool pad) {
    if (g_announced[chan]) {
        return;
    }
    g_announced[chan] = true;
    const char* source = "keyboard";
    if (pad) {
        const s32 index = PADGetIndexForPort(chan);
        SDL_Gamepad* gamepad = index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr;
        const char* name = gamepad != nullptr ? SDL_GetGamepadName(gamepad) : nullptr;
        source = name != nullptr ? name : g_keyboardOnAPort ? "keyboard (settings bindings)" : "controller";
    }
    RT_LOGF(RT_TAG_RUNTIME, "player %u: %s as a Wii Remote + Classic Controller\n", chan + 1, source);
}

// Toggle Aim (Settings > Controls, off by default; CubeEvil4's "Latch L and R"): a press of R (aim)
// or L (knife), on the touch controls, a gamepad or the keyboard, holds it fully until the next
// press, so one thumb is free for A (fire) and the stick. A press of the other one switches over (the
// knife lowers the weapon). Where the latch may not hold (GameTweaks::AimLatchAllowed: menus, the
// pause, events and the L / R prompts) it lets go and the buttons pass as pressed; a button already
// held when it may hold again counts once it is pressed anew.
std::atomic<uint32_t> g_aimLatched{0};  // kClL / kClR
uint32_t g_aimRawPrev = 0;

void ApplyAimLatch(Intent& intent) {
    const uint32_t raw = intent.cl & (kClL | kClR);
    uint32_t latched = g_aimLatched.load(std::memory_order_relaxed);
    if (!GameTweaks::AimLatchAllowed()) {
        latched = 0;
    } else {
        const uint32_t pressed = raw & ~g_aimRawPrev;
        if (pressed == (kClL | kClR)) {
            latched ^= pressed;
        } else if (pressed != 0) {
            latched = (latched & pressed) != 0 ? 0u : pressed;
        }
        intent.cl &= ~(kClL | kClR);
        intent.cl |= latched;
        intent.triggerL = (latched & kClL) != 0 ? 255 : 0;
        intent.triggerR = (latched & kClR) != 0 ? 255 : 0;
    }
    g_aimRawPrev = raw;
    if (g_aimLatched.exchange(latched, std::memory_order_relaxed) != latched) {
        static std::atomic<int> s_lines{0};
        if (s_lines.fetch_add(1, std::memory_order_relaxed) < 16) {
            RT_LOGF(RT_TAG_RUNTIME, "toggle aim: %s\n",
                    (latched & kClR) ? "R held" : (latched & kClL) ? "L held" : "let go");
        }
    }
}

// Toggle Run (CubeEvil4's "Latch B"): a press of B holds it until the next press, so Leon keeps
// running. Only in free play and with no prompt that asks for B (4 A+B, 5 / 13 B, 11 the gacha): there,
// and in every menu where B cancels, B passes as pressed and the latch lets go.
std::atomic<bool> g_runLatched{false};
bool g_runRawPrev = false;

void ApplyRunLatch(Intent& intent) {
    static const bool s_env = std::getenv("RE4_TOGGLE_RUN") != nullptr;  // Windows tests
    const bool raw = (intent.cl & kClB) != 0;
    bool latched = g_runLatched.load(std::memory_order_relaxed);
    uint8_t prompt = 0;
    if (!(s_env || g_toggleRun.load(std::memory_order_relaxed)) || !GameTweaks::FreePlay(&prompt) || prompt == 4u ||
        prompt == 5u || prompt == 11u || prompt == 13u) {
        latched = false;
    } else {
        if (raw && !g_runRawPrev) latched = !latched;
        intent.cl = (intent.cl & ~kClB) | (latched ? kClB : 0u);
    }
    g_runRawPrev = raw;
    if (g_runLatched.exchange(latched, std::memory_order_relaxed) != latched) {
        static int s_lines = 0;
        if (s_lines++ < 16) RT_LOGF(RT_TAG_RUNTIME, "toggle run: %s\n", latched ? "B held" : "let go");
    }
}

// The mouse (SetMouseAim). A click in the game window captures it (SDL relative mode, pointer capture
// on Android); the settings overlay, losing focus or the option turned off let it go. Relative mode is
// switched on SDL's main thread, the deltas are read here on the guest thread.
struct MouseState {
    bool captured = false;
    bool swallowLeft = false;  // the click that captured the mouse is not an A press
    uint64_t lastNs = 0;
    uint64_t movedNs = 0;
    uint64_t pitchNs = 0;
    float stick[2] = {};
    float lookUp = 0.0f;
    bool lookHeld = false;
};
MouseState g_mouse;
std::atomic<SDL_Window*> g_mouseWindow{nullptr};
std::atomic<bool> g_mouseAiming{false};  // MouseAiming()

void SetRelativeMouse(SDL_Window* window, bool on) {
    g_mouseWindow.store(window, std::memory_order_relaxed);
    SDL_RunOnMainThread(
        [](void* on) {
            if (SDL_Window* w = g_mouseWindow.load(std::memory_order_relaxed)) {
                SDL_SetWindowRelativeMouseMode(w, on != nullptr);
            }
        },
        on ? reinterpret_cast<void*>(1) : nullptr, false);
    RT_LOGF(RT_TAG_RUNTIME, "mouse: %s\n", on ? "captured (aim / turn)" : "released");
}

void ReleaseMouseLocked() {
    if (g_mouse.captured) {
        SetRelativeMouse(g_mouseWindow.load(std::memory_order_relaxed), false);
    }
    if (g_mouse.lookHeld) GameTweaks::TouchLook(0.0f, 0.0f, false);
    g_mouse = {};
    g_mouseAiming.store(false, std::memory_order_relaxed);
}

// A mouse is there once one sent an event of its own. SDL_HasMouse() alone is never true on Android:
// SDL adds no mouse device there and sends a real mouse's events as SDL_DEFAULT_MOUSE_ID, so Mouse Aim
// never captured (1.1 report, Xiaomi 23127PN0CG: the pointer arrow moved, clicks did nothing).
// Touches SDL turns into mouse events (SDL_TOUCH_MOUSEID) do not count.
std::atomic<bool> g_mouseSeen{false};

bool MouseEventWatch(void*, SDL_Event* e) {
    SDL_MouseID which = 0;
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION: which = e->motion.which; break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: which = e->button.which; break;
    case SDL_EVENT_MOUSE_WHEEL: {
        which = e->wheel.which;
        static float s_rest = 0.0f;  // a smooth wheel's parts of a notch
        s_rest += e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
        const int notches = static_cast<int>(s_rest);
        if (notches != 0) {
            s_rest -= static_cast<float>(notches);
            g_wheelNotches.fetch_add(notches, std::memory_order_relaxed);
        }
        break;
    }
    default: return true;
    }
    if (which != SDL_TOUCH_MOUSEID) g_mouseSeen.store(true, std::memory_order_relaxed);
    return true;
}

bool MousePresent() {
    static const bool s_watching = SDL_AddEventWatch(MouseEventWatch, nullptr);
    (void)s_watching;
    return SDL_HasMouse() || g_mouseSeen.load(std::memory_order_relaxed);
}

// Each change of the mouse buttons, the first 40: a click that does something else than A / R / L
// (1.1 report: left and right click opened the game's Options) is then readable from console.log.
// `sent`: what the click gave player 1 (1.1 test report: clicks read and captured, yet they did nothing).
void LogMouseButtons(SDL_MouseButtonFlags buttons, const char* sent) {
    static SDL_MouseButtonFlags s_last = 0;
    static int s_lines = 0;
    if (buttons == s_last) return;
    s_last = buttons;
    if (s_lines++ < 40) {
        RT_LOGF(RT_TAG_RUNTIME, "mouse buttons: 0x%x (%s)%s\n", static_cast<unsigned>(buttons),
                g_mouse.captured ? "captured" : "not captured", sent);
    }
}

// The mouse buttons, merged before the latches (the right button is R, so Toggle Aim applies to it).
Intent MouseButtonsLocked() {
    Intent intent;
    static const bool s_env = std::getenv("RE4_MOUSE_AIM") != nullptr;  // Windows tests
    SDL_Window* focus = SDL_GetKeyboardFocus();
    if (!(s_env || g_mouseAim.load(std::memory_order_relaxed)) || focus == nullptr || !MousePresent() ||
        InputBindings::InputBlocked()) {
        ReleaseMouseLocked();
        Keybinds::SetMouseButtons(0);
        return intent;
    }
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
    if (!g_mouse.captured) {
        LogMouseButtons(buttons, "");
        Keybinds::SetMouseButtons(0);
        if ((buttons & SDL_BUTTON_LMASK) != 0 && SDL_GetMouseFocus() == focus) {
            g_mouse.captured = true;
            g_mouse.swallowLeft = true;
            g_mouse.lastNs = SDL_GetTicksNS();
            SetRelativeMouse(focus, true);
        }
        return intent;
    }
    if (g_mouse.lastNs != 0 && SDL_GetTicksNS() - g_mouse.lastNs > 500'000'000ull &&
        !SDL_GetWindowRelativeMouseMode(focus)) {
        ReleaseMouseLocked();  // the system took it back (Alt+Tab, Android dropped the pointer capture)
        return intent;
    }
    if ((buttons & SDL_BUTTON_LMASK) == 0) g_mouse.swallowLeft = false;
    // Keyboard Keys can take the mouse buttons ("Mouse Left" ...); a button bound there leaves its
    // built-in role (left A, right R, middle L) below.
    const uint32_t left = (buttons & SDL_BUTTON_LMASK) != 0 && !g_mouse.swallowLeft ? 1u : 0u;
    Keybinds::SetMouseButtons(left | ((buttons & SDL_BUTTON_RMASK) != 0 ? 2u : 0u) |
                              ((buttons & SDL_BUTTON_MMASK) != 0 ? 4u : 0u) |
                              ((buttons & SDL_BUTTON_X1MASK) != 0 ? 8u : 0u) |
                              ((buttons & SDL_BUTTON_X2MASK) != 0 ? 16u : 0u));
    if (left != 0 && !Keybinds::MouseButtonBound(0)) intent.cl |= kClA;
    if ((buttons & SDL_BUTTON_RMASK) != 0 && !Keybinds::MouseButtonBound(1)) {
        intent.cl |= kClR;
        intent.triggerR = 255;
    }
    if ((buttons & SDL_BUTTON_MMASK) != 0 && !Keybinds::MouseButtonBound(2)) {
        intent.cl |= kClL;
        intent.triggerL = 255;
    }
    const bool bound = (left != 0 && Keybinds::MouseButtonBound(0)) ||
                       ((buttons & SDL_BUTTON_RMASK) != 0 && Keybinds::MouseButtonBound(1)) ||
                       ((buttons & SDL_BUTTON_MMASK) != 0 && Keybinds::MouseButtonBound(2));
    LogMouseButtons(buttons, bound                     ? " -> Keyboard Keys"
                             : (intent.cl & kClR) != 0 ? " -> R"
                             : (intent.cl & kClA) != 0 ? " -> A"
                             : (intent.cl & kClL) != 0 ? " -> L"
                             : buttons != 0            ? " -> nothing"
                                                       : "");
    return intent;
}

// The mouse's movement, after the latches: while L / R aims its speed is the sight's tilt (RE4's sight is
// a rate control), otherwise sideways turns Leon and up / down tilts the camera (GameTweaks::TouchLook,
// the touch camera's Locked mode).
void ApplyMouseMotionLocked(Intent& intent) {
    g_mouseAiming.store(g_mouse.captured && (intent.cl & (kClL | kClR)) != 0, std::memory_order_relaxed);
    if (!g_mouse.captured) return;
    float dx = 0.0f, dy = 0.0f;
    SDL_GetRelativeMouseState(&dx, &dy);
    const uint64_t now = SDL_GetTicksNS();
    const float dt = std::clamp(static_cast<float>(now - g_mouse.lastNs) * 1e-9f, 0.001f, 0.1f);
    g_mouse.lastNs = now;
    const float sens = g_mouseSensitivity.load(std::memory_order_relaxed);
    const bool moved = dx != 0.0f || dy != 0.0f;
    if (moved) g_mouse.movedNs = now;
    if ((intent.cl & (kClL | kClR)) != 0) {
        if (g_mouse.lookHeld) {
            GameTweaks::TouchLook(0.0f, 0.0f, false);
            g_mouse.lookHeld = false;
            g_mouse.lookUp = 0.0f;
        }
        if (moved) {
            // 1500 px/s at sensitivity 1 tilts fully; past the game's small dead zone at once.
            const auto axis = [sens](float v) {
                const float a = std::min(1.0f, std::fabs(v) * sens / 1500.0f);
                if (a < 0.005f) return 0.0f;
                const float out = 0.15f + 0.85f * a;
                return v < 0.0f ? -out : out;
            };
            g_mouse.stick[0] = axis(dx / dt);
            g_mouse.stick[1] = axis(-dy / dt);  // +y up: the mouse pushed away aims up
            if (GameTweaks::AimReversed()) g_mouse.stick[1] = -g_mouse.stick[1];
        } else if (now - g_mouse.movedNs > 30'000'000ull) {
            g_mouse.stick[0] = g_mouse.stick[1] = 0.0f;  // reads between the mouse's reports keep the last
        }
        if (g_mouse.stick[0] != 0.0f || g_mouse.stick[1] != 0.0f) {
            intent.lStick[0] = g_mouse.stick[0];
            intent.lStick[1] = g_mouse.stick[1];
        }
        return;
    }
    g_mouse.stick[0] = g_mouse.stick[1] = 0.0f;
    if (dy != 0.0f) {
        g_mouse.lookUp = std::clamp(g_mouse.lookUp - dy * 0.004f * sens, -1.0f, 1.0f);
        g_mouse.pitchNs = now;
    } else if (now - g_mouse.pitchNs > 1'500'000'000ull) {
        g_mouse.lookUp *= 0.9f;  // the camera drifts back level once the mouse rests
        if (std::fabs(g_mouse.lookUp) < 0.02f) g_mouse.lookUp = 0.0f;
    }
    const bool held = g_mouse.lookUp != 0.0f;
    if (dx != 0.0f || held || g_mouse.lookHeld) {
        GameTweaks::TouchLook(-dx * 0.0025f * sens, g_mouse.lookUp, held);
        g_mouse.lookHeld = held;
    }
}

// TapQte and KnifeSwipe: short sequences pressed on top of everything else, after the latches.
void ApplyTouchGesturesLocked(Intent& intent, uint64_t now) {
    const uint64_t tap = g_tapQteNs.load(std::memory_order_relaxed);
    if (tap != 0 && now - tap < kTapPressNs) {
        if (const uint32_t qte = Cheats::QtePromptButtons(); qte != 0) {
            intent.cl |= ((qte & Cheats::kQteA) ? kClA : 0) | ((qte & Cheats::kQteB) ? kClB : 0) |
                         ((qte & Cheats::kQteL) ? kClL : 0) | ((qte & Cheats::kQteR) ? kClR : 0) |
                         ((qte & Cheats::kQteY) ? kClY : 0);
            if ((qte & Cheats::kQteL) != 0) intent.triggerL = 255;
            if ((qte & Cheats::kQteR) != 0) intent.triggerR = 255;
            if ((qte & Cheats::kQteStickL) != 0) {
                // The gacha counts each new stick direction: every tap flicks the other way.
                intent.lStick[0] = (g_tapCount.load(std::memory_order_relaxed) & 1u) != 0 ? -1.0f : 1.0f;
                intent.lStick[1] = 0.0f;
            }
        }
    }
    const uint64_t knife = g_knifeNs.load(std::memory_order_relaxed);
    if (knife == 0 || now - knife >= kKnifeEndNs) return;
    uint8_t prompt = 0;
    if (!GameTweaks::FreePlay(&prompt) || prompt > 1u) return;  // no QTE answered by a slash
    const uint64_t t = now - knife;
    intent.cl = (intent.cl & ~(kClR | kClA)) | kClL;
    intent.triggerL = 255;
    intent.triggerR = 0;
    if (const int dir = g_knifeDir.load(std::memory_order_relaxed); dir != 0) {
        intent.lStick[0] = 0.0f;
        intent.lStick[1] = (dir > 0 ? 0.9f : -0.9f) * (GameTweaks::AimReversed() ? -1.0f : 1.0f);
    }
    if (t >= kKnifeReadyNs && t < kKnifeSlashNs) intent.cl |= kClA;
}

// The Reload key (1.1 report, keyboard and mouse: no way to reload by hand). Free play only, after the
// latches (Toggle Run must not take its B). Not with F10's keyboard port, whose keys are a gamepad.
void ApplyReloadKeyLocked(Intent& intent, uint64_t now) {
    static uint64_t s_pressNs = 0;
    static bool s_prev = false;
    const bool held = g_vpadReload || (!g_keyboardOnAPort && SDL_GetKeyboardFocus() != nullptr &&
                                       !Keybinds::Suppressed() && Keybinds::Held(Keybinds::kReload));
    if (held && !s_prev) {
        s_pressNs = now;
        static int s_lines = 0;
        if (s_lines++ < 8) RT_LOGF(RT_TAG_RUNTIME, "reload key: R held, B pressed\n");
    }
    s_prev = held;
    if (s_pressNs == 0) return;
    const uint64_t t = now - s_pressNs;
    if (!held && t >= kReloadEndNs) {
        s_pressNs = 0;
        return;
    }
    uint8_t prompt = 0;
    if (!GameTweaks::FreePlay(&prompt) || prompt > 1u) return;
    intent.cl |= kClR;
    intent.triggerR = 255;
    if (t >= kReloadBNs && t < kReloadEndNs) intent.cl |= kClB;
}

// The mouse wheel zooms the rifle scope and the binoculars (tester wish, 1.1): the game zooms both by
// the right stick's Y, up in; a notch pushes it fully for kWheelPolls pad reads. A notch the other
// way drops those still queued; anywhere else the notches are dropped.
void ApplyWheelZoomLocked(Intent& intent) {
    constexpr int kWheelPolls = 2, kWheelMax = 16;
    static int s_push = 0;
    const int notches = g_wheelNotches.exchange(0, std::memory_order_relaxed);
    if (!g_mouse.captured || !GameTweaks::LookThroughView()) {
        s_push = 0;
        return;
    }
    if (notches != 0) {
        if (s_push != 0 && (notches > 0) != (s_push > 0)) s_push = 0;
        s_push = std::clamp(s_push + notches * kWheelPolls, -kWheelMax, kWheelMax);
    }
    if (s_push == 0 || intent.rStick[1] != 0.0f) return;
    intent.rStick[1] = s_push > 0 ? 1.0f : -1.0f;
    s_push += s_push > 0 ? -1 : 1;
}

} // namespace

uint32_t AimLatched() {
    const uint32_t latched = g_aimLatched.load(std::memory_order_relaxed);
    return ((latched & kClL) ? 1u : 0u) | ((latched & kClR) ? 2u : 0u) |
           (g_runLatched.load(std::memory_order_relaxed) ? 4u : 0u);
}

void SetPadOptions(bool alternate, bool swapFace, int deadZonePercent, int rangePercent) {
    g_altControls.store(alternate, std::memory_order_relaxed);
    g_swapFace.store(swapFace, std::memory_order_relaxed);
    g_deadZone.store(std::clamp(deadZonePercent, 0, 30), std::memory_order_relaxed);
    g_range.store(std::clamp(rangePercent, 50, 100), std::memory_order_relaxed);
}

void SetStickOptions(bool swapSticks, bool invertRightY, int rightAxisX, int rightAxisY) {
    g_swapSticks.store(swapSticks, std::memory_order_relaxed);
    g_invertRightY.store(invertRightY, std::memory_order_relaxed);
    const bool pair = rightAxisX >= 0 && rightAxisY >= 0 && rightAxisX < 64 && rightAxisY < 64;
    g_rightAxisX.store(pair ? rightAxisX : -1, std::memory_order_relaxed);
    g_rightAxisY.store(pair ? rightAxisY : -1, std::memory_order_relaxed);
}

void SetToggleRun(bool on) {
    g_toggleRun.store(on, std::memory_order_relaxed);
}

void SetMouseAim(bool on, float sensitivity) {
    g_mouseAim.store(on, std::memory_order_relaxed);
    g_mouseSensitivity.store(std::isfinite(sensitivity) ? std::clamp(sensitivity, 0.25f, 3.0f) : 1.0f,
                             std::memory_order_relaxed);
}

bool MouseAiming() { return g_mouseAiming.load(std::memory_order_relaxed); }

void TapQte() {
    g_tapCount.fetch_add(1, std::memory_order_relaxed);
    g_tapQteNs.store(SDL_GetTicksNS(), std::memory_order_relaxed);
}

void KnifeSwipe(int dir) {
    g_knifeDir.store(std::clamp(dir, -1, 1), std::memory_order_relaxed);
    g_knifeNs.store(SDL_GetTicksNS(), std::memory_order_relaxed);
    static std::atomic<int> s_lines{0};
    if (s_lines.fetch_add(1, std::memory_order_relaxed) < 8) {
        RT_LOGF(RT_TAG_RUNTIME, "knife swipe: %s\n", dir > 0 ? "high" : dir < 0 ? "low" : "straight");
    }
}

void SetTouchState(uint32_t touchButtons) {
    const uint32_t before = g_touchButtons.exchange(touchButtons, std::memory_order_relaxed);
    g_touchPresses.fetch_add(static_cast<uint32_t>(__builtin_popcount(touchButtons & ~before)),
                             std::memory_order_relaxed);
    g_touchButtonsSinceRead.fetch_or(touchButtons, std::memory_order_relaxed);
}

void SetTouchSticks(float leftX, float leftY, float rightX, float rightY) {
    g_touchSticks[0].store(leftX, std::memory_order_relaxed);
    g_touchSticks[1].store(leftY, std::memory_order_relaxed);
    g_touchSticks[2].store(rightX, std::memory_order_relaxed);
    g_touchSticks[3].store(rightY, std::memory_order_relaxed);
}

void SetGamepadMap(const char* map) {
    std::array<int, kMapCount> sources;
    sources.fill(-1);
    size_t found = 0;
    const std::string text = map != nullptr ? map : "";
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos) end = text.size();
        const std::string entry = text.substr(start, end - start);
        start = end + 1;
        const size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const auto trim = [](std::string s) {
            const size_t a = s.find_first_not_of(" \t");
            const size_t b = s.find_last_not_of(" \t");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        const std::string key = trim(entry.substr(0, eq));
        const std::string value = trim(entry.substr(eq + 1));
        for (size_t i = 0; i < kMapCount; ++i) {
            if (key != kMapButtons[i].name) {
                continue;
            }
            const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(value.c_str());
            const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(value.c_str());
            if (value.size() > 3 && value.compare(0, 3, "joy") == 0 &&
                value.find_first_not_of("0123456789", 3) == std::string::npos && value.size() <= 6) {
                sources[i] = kJoySource + std::stoi(value.substr(3));
            } else if (button != SDL_GAMEPAD_BUTTON_INVALID) {
                sources[i] = static_cast<int>(button);
            } else if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
                sources[i] = kAxisSource + static_cast<int>(axis);
            } else {
                break;
            }
            ++found;
            break;
        }
    }
    for (size_t i = 0; i < kMapCount; ++i) {
        g_map[i].store(sources[i], std::memory_order_relaxed);
    }
    const bool on = !text.empty();
    if (g_mapOn.exchange(on) != on || on) {
        static std::string s_last;
        static std::mutex s_logMutex;
        std::lock_guard<std::mutex> lock(s_logMutex);
        if (s_last != text) {
            s_last = text;
            if (on) {
                RT_LOGF(RT_TAG_RUNTIME, "gamepad buttons: %zu set by the player (%s)\n", found, text.c_str());
            } else {
                RT_LOGF(RT_TAG_RUNTIME, "gamepad buttons: default layout\n");
            }
        }
    }
}

bool Present(uint32_t chan) {
    if (chan >= PAD_MAX_CONTROLLERS) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    RefreshPadsLocked();
    return chan == 0 || g_pads[chan].err == PAD_ERR_NONE;
}

bool Sample(uint32_t chan, WiiRemoteInput::KpadSample& sample) {
    if (chan >= PAD_MAX_CONTROLLERS) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    RefreshPadsLocked();
    const bool pad = g_pads[chan].err == PAD_ERR_NONE;
    if (chan != 0 && !pad) {
        return false;
    }
    AnnounceLocked(chan, pad);
    Intent intent;
    static const bool s_configMap = [] {
        SetGamepadMap(RuntimeConfigFile::GamepadMap().c_str());
        return true;
    }();
    (void)s_configMap;
    if (pad) {
        intent = PadIntent(chan, g_pads[chan]);
    }
    const auto merge = [&intent](const Intent& other) {
        intent.cl |= other.cl;
        for (int axis = 0; axis < 2; ++axis) {
            if (other.lStick[axis] != 0.0f) intent.lStick[axis] = other.lStick[axis];
            if (other.rStick[axis] != 0.0f) intent.rStick[axis] = other.rStick[axis];
            if (other.aimStick[axis] != 0.0f) intent.aimStick[axis] = other.aimStick[axis];
        }
        intent.triggerL = std::max(intent.triggerL, other.triggerL);
        intent.triggerR = std::max(intent.triggerR, other.triggerR);
    };
    // Who pressed + (the game's Options / skip), the first 30 times: a mouse click that opened Options
    // (1.1 report) shows whether it came in as a key, a gamepad button or the mouse itself.
    static uint32_t s_plusFrom = 0;
    uint32_t plusNow = 0;
    const auto plusFrom = [&plusNow](int source, const char* name, const Intent& other) {
        if ((other.cl & kClPlus) == 0) return;
        plusNow |= 1u << source;
        static int s_lines = 0;
        if ((s_plusFrom & (1u << source)) == 0 && s_lines++ < 30) {
            std::string keys;
            if (source == 0) {
                int count = 0;
                const bool* state = SDL_GetKeyboardState(&count);
                for (int i = 0; state != nullptr && i < count; ++i) {
                    if (state[i]) (keys += keys.empty() ? " " : ", ") += SDL_GetScancodeName(static_cast<SDL_Scancode>(i));
                }
            }
            RT_LOGF(RT_TAG_RUNTIME, "+ pressed by %s%s\n", name, keys.c_str());
        }
    };
    if (chan == 0 && pad) plusFrom(1, "a gamepad", intent);  // port 1's own pad
    if (chan == 0 && !g_keyboardOnAPort) {
        const Intent keyboard = KeyboardIntent();
        plusFrom(0, "keyboard:", keyboard);
        merge(keyboard);
    }
    if (chan == 0) {
        // RE4 has one player, so a gamepad on any port drives Leon. SDL hands out ports in the
        // order devices appear, and a Bluetooth pad's media-key half ("GamePadPlus V3 Consumer
        // Control", moto g57 tester, Beta 8) could take port 1 and push the real pad to port 2.
        for (uint32_t other = 1; other < PAD_MAX_CONTROLLERS; ++other) {
            if (g_pads[other].err == PAD_ERR_NONE) {
                const Intent pad = PadIntent(other, g_pads[other]);
                plusFrom(1, "a gamepad", pad);
                merge(pad);
            }
        }
        // Aim Sensitivity: the gamepad / keyboard left stick moves the sight (the touch controls
        // scale their own values, merged below).
        if (const float aim = GameTweaks::AimStickScale(); aim != 1.0f) {
            for (float& v : intent.lStick) v = std::clamp(v * aim, -1.0f, 1.0f);
        }
        const Intent touch = TouchIntent();
        plusFrom(2, "the touch controls", touch);
        merge(touch);
        merge(AutomationIntent());
        const Intent mouse = MouseButtonsLocked();
        plusFrom(3, "the mouse", mouse);
        merge(mouse);
        s_plusFrom = plusNow;
        ApplyAimLatch(intent);
        ApplyRunLatch(intent);
        // Alternate Controls: while L / R holds a weapon ready, the gamepad's right stick aims (the game
        // aims with the left one and leaves the right unused then).
        if ((intent.cl & (kClL | kClR)) != 0 && (intent.aimStick[0] != 0.0f || intent.aimStick[1] != 0.0f)) {
            const float aim = GameTweaks::AimStickScale();
            intent.lStick[0] = std::clamp(intent.aimStick[0] * aim, -1.0f, 1.0f);
            intent.lStick[1] = std::clamp(intent.aimStick[1] * aim, -1.0f, 1.0f);
            intent.rStick[0] = intent.rStick[1] = 0.0f;
        }
        ApplyMouseMotionLocked(intent);
        ApplyWheelZoomLocked(intent);
        ApplyTouchGesturesLocked(intent, SDL_GetTicksNS());
        ApplyReloadKeyLocked(intent, SDL_GetTicksNS());
        if (GameTweaks::SkipBootPressesA()) intent.cl |= kClA;
        if (const uint32_t qte = Cheats::AutoQteButtons(SDL_GetTicksNS()); qte != 0) {
            intent.cl |= ((qte & Cheats::kQteA) ? kClA : 0) | ((qte & Cheats::kQteB) ? kClB : 0) |
                         ((qte & Cheats::kQteL) ? kClL : 0) | ((qte & Cheats::kQteR) ? kClR : 0) |
                         ((qte & Cheats::kQteY) ? kClY : 0);
            if ((qte & (Cheats::kQteStickL | Cheats::kQteStickR)) != 0) {
                intent.lStick[0] = (qte & Cheats::kQteStickL) ? -1.0f : 1.0f;  // the gacha's flick
                intent.lStick[1] = 0.0f;
            }
        }
        // Binoculars: D-pad up/down zoom (the right stick's Y), as the touch controls do for the scope
        // while aiming; the touch camera hides the right stick and a D-pad has no other zoom.
        if ((intent.cl & (kClUp | kClDown)) != 0 && intent.rStick[1] == 0.0f && GameTweaks::BinocularView()) {
            intent.rStick[1] = (intent.cl & kClUp) != 0 ? 0.6f : -0.6f;
            intent.cl &= ~(kClUp | kClDown);
        }
        // Auto Run (Settings > Controls): B (run) flipped while the stick points forward, so Leon runs
        // and a held B walks. Back and sideways keep B as pressed: back + B is the quick turn.
        if (intent.lStick[1] > 0.3f && !g_runLatched.load(std::memory_order_relaxed) && GameTweaks::AutoRunActive()) {
            intent.cl ^= kClB;
        }
    }
    if (InputBindings::InputBlocked()) {
        intent = {};  // the settings overlay owns input; the remote stays connected at rest
    }
    sample = {};
    sample.acc[1] = -1.0f;  // a remote at rest (KPAD frame); RE4 reads no motion with the Classic
    // RE4 reads a Classic Controller like a GameCube pad (laser-sight aiming, right stick camera);
    // with a Nunchuk it would aim with the IR pointer, which a keyboard or gamepad cannot drive.
    sample.hasClassic = true;
    sample.clHold = intent.cl;
    if (chan == 0) {
        LogInputLocked(intent);
    }
    const auto raw = [](float v) {
        return static_cast<int16_t>(std::clamp(std::lround(std::clamp(v, -1.0f, 1.0f) * 512.0f), -512L, 511L));
    };
    for (int axis = 0; axis < 2; ++axis) {
        sample.clLStick[axis] = intent.lStick[axis];
        sample.clRStick[axis] = intent.rStick[axis];
        sample.clLStickRaw[axis] = raw(intent.lStick[axis]);
        sample.clRStickRaw[axis] = raw(intent.rStick[axis]);
    }
    sample.clTriggerL = (intent.cl & kClL) ? 255 : intent.triggerL;
    sample.clTriggerR = (intent.cl & kClR) ? 255 : intent.triggerR;
    // Screens that only read the Wii Remote (the wrist-strap warning, the HOME Menu) get the same
    // press on its buttons, as a player would reach for them with the Classic Controller attached.
    const auto wpad = [&](uint32_t cl, uint32_t button) {
        if (intent.cl & cl) sample.hold |= button;
    };
    wpad(kClA, kWpadA);
    wpad(kClB, kWpadB);
    wpad(kClPlus, kWpadPlus);
    wpad(kClMinus, kWpadMinus);
    wpad(kClHome, kWpadHome);
    wpad(kClUp, kWpadUp);
    wpad(kClDown, kWpadDown);
    wpad(kClLeft, kWpadLeft);
    wpad(kClRight, kWpadRight);
    return true;
}

void SetButtonsPad(int sdlButton, bool down, const char* name) {
    if (name != nullptr && !g_buttonsPadWanted.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_buttonsPadNameMutex);
        g_buttonsPadName = name;
    }
    g_buttonsPadWanted.store(true, std::memory_order_relaxed);
    // SDL_GamepadButton, or 30 / 31 for the L2 / R2 triggers (their axes in PumpButtonsPad).
    if (sdlButton < 0 || (sdlButton >= SDL_GAMEPAD_BUTTON_COUNT && sdlButton != 30 && sdlButton != 31) ||
        sdlButton >= 32) {
        return;
    }
    const uint32_t bit = 1u << sdlButton;
    if (down) {
        g_buttonsPadMask.fetch_or(bit, std::memory_order_relaxed);
    } else {
        g_buttonsPadMask.fetch_and(~bit, std::memory_order_relaxed);
    }
}

} // namespace VirtualRemote
