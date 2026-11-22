#pragma once

#include "wii_remote_input.h"

#include <cstdint>

// RE4 is played on a Classic Controller here (laser-sight aiming, like the GameCube), so a keyboard, an ordinary gamepad or the touch overlay has
// to become one. Every port with no real Wii Remote but with an aurora PAD source (an SDL gamepad,
// or the keyboard when it is switched on for that port in the settings overlay) is presented to the
// game as a Wii Remote with a Classic Controller attached. aurora's PADRead supplies the state, which
// picks up the user's button remapping. Port 1 also always has a built-in keyboard layout, so the
// game is playable without any setup:
//
//   arrows        D-pad                       W A S D          left stick
//   X / Space     A                           I J K L          right stick
//   Z / L Shift   B                           Q / E            L / R
//   V             X                           1 / 3            ZL / ZR
//   C / L Ctrl    Y                           Enter / Esc      +
//   H             Home                        - / Tab          -
//
// Gamepads follow the Classic Controller by position, as in Dolphin's Classic layout: A east, B south,
// X north, Y west, triggers L and R, shoulders ZL and ZR, Start +, Back -, Guide Home, both sticks.

namespace VirtualRemote {

// True when `chan` should report a connected Wii Remote driven by a keyboard or gamepad.
bool Present(uint32_t chan);
// Fills one frame of the virtual remote on `chan`; false when Present(chan) is false.
bool Sample(uint32_t chan, WiiRemoteInput::KpadSample& sample);

// On-screen Classic Controller for player 1 (the Android app's overlay, through JNI). The bits match
// TouchControlsView.
enum TouchButton : uint32_t {
    kTouchLeft = 0x0001, kTouchRight = 0x0002, kTouchUp = 0x0004, kTouchDown = 0x0008,
    kTouchB = 0x0010, kTouchA = 0x0020, kTouchX = 0x0040, kTouchPlus = 0x0080, kTouchMinus = 0x0100,
    kTouchY = 0x0200, kTouchL = 0x0400, kTouchR = 0x0800, kTouchZL = 0x1000, kTouchZR = 0x2000,
    kTouchHome = 0x4000,
};
void SetTouchState(uint32_t touchButtons);
// The on-screen sticks, -1..1 with +y up.
void SetTouchSticks(float leftX, float leftY, float rightX, float rightY);

// The player's gamepad buttons (Android Settings > Controls > Gamepad Buttons, Config.toml [input]
// gamepad_map): "a=b,b=a,zl=leftshoulder,l=lefttrigger,..." gives each Classic button (a b x y l r zl
// zr plus minus home up down left right) one SDL gamepad button or trigger by its SDL name. A button
// left out is not on the gamepad. Empty: the Dolphin layout above, through aurora's PAD remapping.
// Applies to every gamepad; the sticks stay as they are. May be called from any thread.
void SetGamepadMap(const char* map);

// Toggle Aim and Toggle Run: the buttons they hold for the player now, 1 L (knife), 2 R (aim), 4 B
// (run); the touch controls draw them pressed and treat L / R as held (aiming moves the sight). Any thread.
uint32_t AimLatched();

// Settings > Controls (all any thread):
// - Alternate Controls (gamepad, its default layout only): LT / L2 aims (R), RT / R2 is A (fire, past
//   half way), LB / L1 readies the knife (L), RB / R1 the map (ZR); while L or R holds a weapon ready
//   the right stick aims (it takes over from the left one while pushed).
// - Swap A/B and X/Y: the face buttons by their labels (Xbox A = Classic A) instead of by position.
// - Stick dead zone and range, percent of a gamepad stick's way (0 and 100: as the pad reports).
void SetPadOptions(bool alternate, bool swapFace, int deadZonePercent, int rangePercent);
// Settings > Controls: Swap Sticks, Invert Camera Y, Right Stick Axes (SDL joystick axis numbers, -1 = the
// gamepad mapping's right stick).
void SetStickOptions(bool swapSticks, bool invertRightY, int rightAxisX, int rightAxisY);
// Toggle Run (CubeEvil4's "Latch B"): a press of B holds it (Leon keeps running) until the next press.
void SetToggleRun(bool on);
// Mouse: a click in the game window captures the mouse; while L / R aims it moves the sight, otherwise
// sideways turns Leon and up / down tilts the camera (GameTweaks::TouchLook). Left button A, right
// button R, middle button L. Released by the settings overlay, the app's menu or losing focus.
void SetMouseAim(bool on, float sensitivity);
// True while the captured mouse moves the sight (L / R held): GameTweaks then skips the game's aim
// acceleration, which the mouse's pauses between reports kept restarting.
bool MouseAiming();
// A tap on the picture: presses what the quick-time prompt on screen asks for (each tap a new press).
void TapQte();
// Knife by swipe: a quick swipe readies the knife and slashes; dir +1 high, -1 low, 0 straight.
void KnifeSwipe(int dir);

// A pad Android lists only as buttons (keyboard source, no gamepad or joystick source; SDL never
// opens it): its gamepad keys, as an SDL_GamepadButton, drive an SDL virtual gamepad. `name` once.
void SetButtonsPad(int sdlButton, bool down, const char* name);

} // namespace VirtualRemote
