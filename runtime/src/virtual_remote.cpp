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
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
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

std::mutex g_mutex;
std::array<PADStatus, PAD_MAX_CONTROLLERS> g_pads{};
uint64_t g_padsReadNs = 0;
bool g_keyboardOnAPort = false;
std::array<bool, PAD_MAX_CONTROLLERS> g_announced{};

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
    PADRead(g_pads.data());
    LogGamepadsLocked(now);
    InputBindings::Apply(g_pads.data());
    g_keyboardOnAPort = false;
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        uint32_t count = 0;
        if (PADGetKeyButtonBindings(port, &count) != nullptr) {
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
                } else {
                    if (strstr(buf, "X") || strstr(buf, "2") || strstr(buf, "A")) file.cl |= kClA;
                    if (strstr(buf, "START") || strstr(buf, "PLUS") || strstr(buf, "ENTER")) file.cl |= kClPlus;
                    if (strstr(buf, "UP")) file.cl |= kClUp;
                    if (strstr(buf, "DOWN")) file.cl |= kClDown;
                    if (strstr(buf, "LEFT")) file.cl |= kClLeft;
                    if (strstr(buf, "RIGHT")) file.cl |= kClRight;
                    if (strstr(buf, "1") || strstr(buf, "B") || strstr(buf, "RUN")) file.cl |= kClB;
                }
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
    bit(PAD_BUTTON_B, kClA);
    bit(PAD_BUTTON_A, kClB);
    bit(PAD_BUTTON_Y, kClX);
    bit(PAD_BUTTON_X, kClY);
    bit(PAD_BUTTON_START, kClPlus);
    bit(PAD_TRIGGER_Z, kClZR);
    if (pad.extButton & PAD_BUTTON_BACK) intent.cl |= kClMinus;
    const s32 index = PADGetIndexForPort(chan);
    if (SDL_Gamepad* gamepad = index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr) {
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) intent.cl |= kClZL;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_GUIDE)) intent.cl |= kClHome;
    }
    intent.triggerL = (b & PAD_TRIGGER_L) != 0 ? 255 : pad.triggerLeft;
    intent.triggerR = (b & PAD_TRIGGER_R) != 0 ? 255 : pad.triggerRight;
    if (intent.triggerL >= kTriggerThreshold) intent.cl |= kClL;
    if (intent.triggerR >= kTriggerThreshold) intent.cl |= kClR;
    intent.lStick[0] = std::clamp(pad.stickX / 127.0f, -1.0f, 1.0f);
    intent.lStick[1] = std::clamp(pad.stickY / 127.0f, -1.0f, 1.0f);
    intent.rStick[0] = std::clamp(pad.substickX / 127.0f, -1.0f, 1.0f);
    intent.rStick[1] = std::clamp(pad.substickY / 127.0f, -1.0f, 1.0f);
    if (g_mapOn.load(std::memory_order_relaxed)) {
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

} // namespace

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
        }
        intent.triggerL = std::max(intent.triggerL, other.triggerL);
        intent.triggerR = std::max(intent.triggerR, other.triggerR);
    };
    if (chan == 0 && !g_keyboardOnAPort) {
        merge(KeyboardIntent());
    }
    if (chan == 0) {
        // RE4 has one player, so a gamepad on any port drives Leon. SDL hands out ports in the
        // order devices appear, and a Bluetooth pad's media-key half ("GamePadPlus V3 Consumer
        // Control", moto g57 tester, Beta 8) could take port 1 and push the real pad to port 2.
        for (uint32_t other = 1; other < PAD_MAX_CONTROLLERS; ++other) {
            if (g_pads[other].err == PAD_ERR_NONE) {
                merge(PadIntent(other, g_pads[other]));
            }
        }
        // Aim Sensitivity: the gamepad / keyboard left stick moves the sight (the touch controls
        // scale their own values, merged below).
        if (const float aim = GameTweaks::AimStickScale(); aim != 1.0f) {
            for (float& v : intent.lStick) v = std::clamp(v * aim, -1.0f, 1.0f);
        }
        merge(TouchIntent());
        merge(AutomationIntent());
        if (GameTweaks::SkipBootPressesA()) intent.cl |= kClA;
        if (const uint32_t qte = Cheats::AutoQteButtons(SDL_GetTicksNS()); qte != 0) {
            intent.cl |= ((qte & Cheats::kQteA) ? kClA : 0) | ((qte & Cheats::kQteB) ? kClB : 0) |
                         ((qte & Cheats::kQteL) ? kClL : 0) | ((qte & Cheats::kQteR) ? kClR : 0);
        }
        // Binoculars: D-pad up/down zoom (the right stick's Y), as the touch controls do for the scope
        // while aiming; the touch camera hides the right stick and a D-pad has no other zoom.
        if ((intent.cl & (kClUp | kClDown)) != 0 && intent.rStick[1] == 0.0f && GameTweaks::BinocularView()) {
            intent.rStick[1] = (intent.cl & kClUp) != 0 ? 0.6f : -0.6f;
            intent.cl &= ~(kClUp | kClDown);
        }
        // Auto Run (Settings > Controls): B (run) flipped while the stick points forward, so Leon runs
        // and a held B walks. Back and sideways keep B as pressed: back + B is the quick turn.
        if (intent.lStick[1] > 0.3f && GameTweaks::AutoRunActive()) {
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

} // namespace VirtualRemote
