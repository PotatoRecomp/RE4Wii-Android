#include "keybinds.h"

#include "runtime_config.h"
#include "runtime_log.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>

namespace Keybinds {
namespace {

struct Default {
    const char* key;   // Config.toml [keyboard] key
    const char* name;  // shown in the Keybinds tab
    std::vector<SDL_Scancode> codes;
};

const std::array<Default, kActionCount>& Defaults() {
    static const std::array<Default, kActionCount> defaults = {{
        // RE4 reads the Classic Controller as a GameCube pad (0x80129D34): a=A, b=B, x=X, y and -=Y,
        // L=L, R=R, ZL and ZR=Z, +=START. Left hand: WASD, Shift, Space; right hand: J K L; arrows: camera.
        {"up", "D-pad Up", {SDL_SCANCODE_KP_8}},
        {"down", "D-pad Down", {SDL_SCANCODE_KP_2}},
        {"left", "D-pad Left", {SDL_SCANCODE_KP_4}},
        {"right", "D-pad Right", {SDL_SCANCODE_KP_6}},
        {"classic_a", "A: Action / Fire", {SDL_SCANCODE_SPACE, SDL_SCANCODE_J}},
        {"classic_b", "B: Run / Cancel", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_BACKSPACE}},
        {"classic_x", "X: Ashley", {SDL_SCANCODE_Q}},
        {"classic_y", "Y: Inventory", {SDL_SCANCODE_TAB, SDL_SCANCODE_I}},
        {"classic_l", "L: Ready Knife", {SDL_SCANCODE_L}},
        {"classic_r", "R: Ready Weapon", {SDL_SCANCODE_K, SDL_SCANCODE_RSHIFT}},
        {"classic_zl", "ZL: Map", {SDL_SCANCODE_N}},
        {"classic_zr", "ZR: Map", {SDL_SCANCODE_M}},
        {"plus", "+: Pause / Skip", {SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE}},
        {"minus", "-: Inventory", {SDL_SCANCODE_MINUS}},
        {"classic_home", "Home", {SDL_SCANCODE_H}},
        {"lstick_up", "Move Up", {SDL_SCANCODE_W}},
        {"lstick_down", "Move Down", {SDL_SCANCODE_S}},
        {"lstick_left", "Move Left", {SDL_SCANCODE_A}},
        {"lstick_right", "Move Right", {SDL_SCANCODE_D}},
        {"rstick_up", "Camera Up", {SDL_SCANCODE_UP}},
        {"rstick_down", "Camera Down", {SDL_SCANCODE_DOWN}},
        {"rstick_left", "Camera Left", {SDL_SCANCODE_LEFT}},
        {"rstick_right", "Camera Right", {SDL_SCANCODE_RIGHT}},
    }};
    return defaults;
}

std::mutex g_mutex;
bool g_loaded = false;
std::array<std::vector<SDL_Scancode>, kActionCount> g_codes;
int g_capturing = -1;
std::vector<bool> g_heldAtCapture;  // keys already down when the capture began
bool g_suppress = false;            // a capture just ended: ignore the keyboard until all keys are up

// Mouse buttons bound like keys (Settings > Controls > Keyboard Keys: "Mouse Left" ...): codes past
// SDL's scancodes, held from the buttons VirtualRemote hands over while the game has the mouse captured.
constexpr int kMouseCodeBase = 600;
constexpr const char* kMouseNames[kMouseButtonCount] = {"Mouse Left", "Mouse Right", "Mouse Middle", "Mouse X1",
                                                        "Mouse X2"};
std::atomic<uint32_t> g_mouseButtons{0};

int MouseIndex(SDL_Scancode code) {
    const int i = static_cast<int>(code) - kMouseCodeBase;
    return i >= 0 && i < kMouseButtonCount ? i : -1;
}

const char* CodeName(SDL_Scancode code) {
    const int mouse = MouseIndex(code);
    return mouse >= 0 ? kMouseNames[mouse] : SDL_GetScancodeName(code);
}

SDL_Scancode CodeFromName(const std::string& name) {
    for (int i = 0; i < kMouseButtonCount; ++i) {
        if (SDL_strcasecmp(name.c_str(), kMouseNames[i]) == 0) return static_cast<SDL_Scancode>(kMouseCodeBase + i);
    }
    return SDL_GetScancodeFromName(name.c_str());
}

std::vector<SDL_Scancode> Parse(const std::string& text) {
    std::vector<SDL_Scancode> codes;
    const auto lookup = [&text](size_t start, size_t length) {
        const std::string name = RuntimeConfigFile::Trim(std::string_view(text).substr(start, length));
        return name.empty() ? SDL_SCANCODE_UNKNOWN : CodeFromName(name);
    };
    size_t start = 0;
    while (start < text.size()) {
        const size_t comma = text.find(',', start);
        size_t next = comma == std::string::npos ? text.size() : comma + 1;
        SDL_Scancode code = SDL_SCANCODE_UNKNOWN;
        if (comma != std::string::npos) {
            // A key's own name can end in the separator ("," or "Keypad ,").
            code = lookup(start, comma + 1 - start);
            if (code != SDL_SCANCODE_UNKNOWN && next < text.size() && text[next] == ',') {
                ++next;
            }
        }
        if (code == SDL_SCANCODE_UNKNOWN) {
            code = lookup(start, comma == std::string::npos ? std::string::npos : comma - start);
        }
        if (code != SDL_SCANCODE_UNKNOWN) {
            codes.push_back(code);
        }
        start = next;
    }
    return codes;
}

std::string Format(const std::vector<SDL_Scancode>& codes, const char* separator) {
    std::string text;
    for (const SDL_Scancode code : codes) {
        if (!text.empty()) {
            text += separator;
        }
        text += CodeName(code);
    }
    return text;
}

void LoadLocked() {
    if (g_loaded) {
        return;
    }
    g_loaded = true;
    for (int i = 0; i < kActionCount; ++i) {
        const std::string saved = RuntimeConfigFile::KeyboardBinding(Defaults()[i].key);
        g_codes[i] = saved.empty() ? Defaults()[i].codes : Parse(saved);
        if (g_codes[i].size() > kKeysPerAction) {
            g_codes[i].resize(kKeysPerAction);
        }
    }
}

void SaveLocked(int action) {
    // An empty value means "use the default" when loading, so an unbound action is written out.
    const std::string text = Format(g_codes[action], ",");
    RuntimeConfigFile::SetKeyboardBinding(Defaults()[action].key, text.empty() ? "None" : text);
}

bool AnyKeyDown() {
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    for (int i = 0; keys != nullptr && i < count; ++i) {
        if (keys[i]) {
            return true;
        }
    }
    return false;
}

} // namespace

const char* ActionName(int action) {
    return action >= 0 && action < kActionCount ? Defaults()[action].name : "";
}

std::string KeyLabel(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (action < 0 || action >= kActionCount) {
        return {};
    }
    const std::string text = Format(g_codes[action], " / ");
    return text.empty() ? "None" : text;
}

bool Held(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (g_capturing >= 0 || g_suppress || action < 0 || action >= kActionCount) {
        return false;
    }
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    for (const SDL_Scancode code : g_codes[action]) {
        if (const int mouse = MouseIndex(code); mouse >= 0) {
            if ((g_mouseButtons.load(std::memory_order_relaxed) >> mouse) & 1u) return true;
        } else if (keys != nullptr && code < count && keys[code]) {
            return true;
        }
    }
    return false;
}

void SetMouseButtons(uint32_t mask) { g_mouseButtons.store(mask, std::memory_order_relaxed); }

bool MouseButtonBound(int button) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    for (const auto& codes : g_codes) {
        for (const SDL_Scancode code : codes) {
            if (MouseIndex(code) == button) return true;
        }
    }
    return false;
}

std::string KeyName(int action, int slot) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (action < 0 || action >= kActionCount || slot < 0 || slot >= static_cast<int>(g_codes[action].size())) {
        return "None";
    }
    return CodeName(g_codes[action][slot]);
}

int Bind(int action, int slot, int scancode) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (action < 0 || action >= kActionCount || slot < 0 || slot >= kKeysPerAction) {
        return -1;
    }
    std::vector<SDL_Scancode>& codes = g_codes[action];
    if (scancode <= 0) {
        if (slot < static_cast<int>(codes.size())) {
            codes.erase(codes.begin() + slot);
        }
        SaveLocked(action);
        return -1;
    }
    const auto code = static_cast<SDL_Scancode>(scancode);
    int moved = -1;
    for (int other = 0; other < kActionCount; ++other) {
        std::vector<SDL_Scancode>& held = g_codes[other];
        const auto it = std::find(held.begin(), held.end(), code);
        if (other != action && it != held.end()) {
            held.erase(it);
            SaveLocked(other);
            moved = other;
        }
    }
    if (std::find(codes.begin(), codes.end(), code) == codes.end()) {
        if (slot < static_cast<int>(codes.size())) {
            codes[slot] = code;
        } else {
            codes.push_back(code);
        }
    }
    SaveLocked(action);
    return moved;
}

void BeginCapture(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    g_heldAtCapture.assign(static_cast<size_t>(count), false);
    for (int i = 0; keys != nullptr && i < count; ++i) {
        g_heldAtCapture[i] = keys[i];
    }
    g_capturing = action;
}

int Capturing() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_capturing;
}

bool PollCapture() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_capturing < 0) {
        return true;
    }
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    for (int i = 0; keys != nullptr && i < count; ++i) {
        const bool before = i < static_cast<int>(g_heldAtCapture.size()) && g_heldAtCapture[i];
        if (!keys[i]) {
            if (before) {
                g_heldAtCapture[i] = false;  // released: a later press of it counts
            }
            continue;
        }
        if (before) {
            continue;
        }
        const auto code = static_cast<SDL_Scancode>(i);
        if (code != SDL_SCANCODE_ESCAPE) {
            g_codes[g_capturing] = {code};
            SaveLocked(g_capturing);
        }
        g_capturing = -1;
        g_suppress = true;
        return true;
    }
    return false;
}

bool Suppressed() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_suppress && !AnyKeyDown()) {
        g_suppress = false;
    }
    return g_capturing >= 0 || g_suppress;
}

void SuppressUntilReleased() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_suppress = true;
}

void ResetDefaults() {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    for (int i = 0; i < kActionCount; ++i) {
        g_codes[i] = Defaults()[i].codes;
        SaveLocked(i);
    }
}

void SetFromApp(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_loaded = true;
    for (int i = 0; i < kActionCount; ++i) {
        g_codes[i] = Defaults()[i].codes;
    }
    size_t start = 0;
    int set = 0;
    while (start < text.size()) {
        size_t end = text.find('|', start);
        if (end == std::string::npos) end = text.size();
        const std::string entry = text.substr(start, end - start);
        start = end + 1;
        const size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = RuntimeConfigFile::Trim(std::string_view(entry).substr(0, eq));
        for (int i = 0; i < kActionCount; ++i) {
            if (key != Defaults()[i].key) continue;
            g_codes[i] = Parse(entry.substr(eq + 1));
            if (g_codes[i].size() > kKeysPerAction) g_codes[i].resize(kKeysPerAction);
            ++set;
            break;
        }
    }
    static std::string s_last = "\x01";
    if (s_last != text) {
        s_last = text;
        RT_LOGF(RT_TAG_RUNTIME, "keyboard keys: %s\n", text.empty() ? "default layout" : "set by the player");
        (void)set;
    }
}

} // namespace Keybinds
