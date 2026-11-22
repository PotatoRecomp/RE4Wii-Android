#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// The keyboard's Classic Controller, one or two keys per control, changeable from the F10 bar
// (Controller settings) and saved to Config.toml's [keyboard] table as SDL scancode names
// ("classic_a = \"X,Space\"", "None" when unbound).
namespace Keybinds {

enum Action : int {
    kUp,
    kDown,
    kLeft,
    kRight,
    kA,
    kB,
    kX,
    kY,
    kL,
    kR,
    kZL,
    kZR,
    kPlus,
    kMinus,
    kHome,
    kLStickUp,
    kLStickDown,
    kLStickLeft,
    kLStickRight,
    kRStickUp,
    kRStickDown,
    kRStickLeft,
    kRStickRight,
    kReload,  // not a Classic button: holds R and presses B (VirtualRemote), the game's aim + B reload
    kActionCount,
};

const char* ActionName(int action);
// "X / Space", or "None".
std::string KeyLabel(int action);
// Any of the action's keys held (false while a key is being captured, or until it is let go).
bool Held(int action);

constexpr int kKeysPerAction = 2;
// The key in `slot` (0 or 1), "None" when empty.
std::string KeyName(int action, int slot);
// Puts `scancode` in the action's `slot`, or empties the slot for 0. A key belongs to one action,
// so it leaves any other action holding it; returns that action, or -1.
int Bind(int action, int slot, int scancode);
// Waits for the next key pressed and binds it to `action` alone; Escape cancels.
void BeginCapture(int action);
// The action waiting for a key, or -1.
int Capturing();
// Polls the keyboard during a capture; true once it has ended (bound or cancelled).
bool PollCapture();
// Keys held from a capture are ignored until they are released.
bool Suppressed();
// The same for keys held now: the key that closes the F10 bar must not reach the game as well.
void SuppressUntilReleased();
void ResetDefaults();
// The Android app's Settings > Controls > Keyboard Keys: "classic_a=Space,J|classic_b=Left Shift|..." (the
// Config.toml keys, SDL scancode names, "None" for no key); actions left out keep their default, and
// "" is the default layout. Kept in memory only: the app saves it. Any thread.
void SetFromApp(const std::string& text);

// Mouse buttons as keys: 0 left, 1 right, 2 middle, 3 X1, 4 X2 (VirtualRemote's captured mouse).
constexpr int kMouseButtonCount = 5;
void SetMouseButtons(uint32_t mask);
bool MouseButtonBound(int button);

} // namespace Keybinds
