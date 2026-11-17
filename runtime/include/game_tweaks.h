#pragma once

#include <cstddef>
#include <cstdint>

// RE4 options the game itself has, set from the launcher (game_tweaks.cpp):
// - Skip Boot: the Wii Remote strap screen skipped and the title intro skipped with A presses ([video]
//   skip_boot in Config.toml, RE4_SKIP_BOOT=0/1 overrides it).
// - Brightness: the game's own brightness filter (Options > Brightness), held at a level picked in
//   the launcher.
// - Reduce Screen Filters: the heavy post-process filters skipped.
// - Reduce Shadows: the shadow-light passes skipped.
namespace GameTweaks {

// Offset from the game's default brightness, -50..50 (the Options menu allows -30..50). Any thread.
void SetBrightness(int offset);

// Skip Boot holds the Classic Controller's A (virtual_remote.cpp) while it skips the title intro. Any thread.
bool SkipBootPressesA();

// Screen filters (FilterTrans, game_tweaks.cpp): bit n skips filter n (0 blur / contrast, 1 depth of
// field, 3 alpha glow, 4 radial glow, 6 dust / snow / rain, 7 thermal scope, 8 aim zoom blur, 0xA scope
// mask, 0xB screen cross-fade). Any thread.
constexpr unsigned kReducedScreenFilters = 0x15Fu;  // 0, 1, 2, 3, 4, 6, 8: all but 7, 0xA, 0xB
void SetSkippedScreenFilters(unsigned mask);
// Draw Distance: the game's far plane (models and effects past it are culled) times scale, 0.1..1.
// Any thread.
void SetDrawDistance(float scale);
// Reduce Shadows: the game's own shadow switch (DPF_SHADOW) held on, so rooms with a shadow light skip
// the cast / self shadow passes. Foot shadows stay. Any thread.
void SetReduceShadows(bool on);
// Fill Screen (no bars): the game's own crop mode, the letterboxed picture scaled to the full height.
// Any thread.
void SetRemoveBars(bool on);
// A menu is open (sub screen, card access, merchant, typewriter): screen filters stay on. Any thread.
bool MenuScreen();
// Boot logos (no room yet), the title and its Load Game, or the typewriter's save screen: Fill
// shows these at 16:9 (display_settings.cpp). Any thread.
bool FramedScreen();
// Settings > Display > Laser Color: the laser sight's line and dot, 0 red (the game's), 1 blue,
// 2 yellow, 3 green. Any thread.
void SetLaserColor(int preset);
int LaserColor();
// Settings > Display > Camera Distance: 1 = the game's view, up to 2 = as if the camera stood twice as
// far back (a wider view in free play). Any thread.
void SetCameraDistance(float distance);
float CameraDistance();
// Settings > Display > Speedrun Mode: the game's play clock (IGT), real time since New Game (RTA) and
// chapter splits on screen and in speedrun.csv; cheats and Stage Select stay off. Any thread.
void SetSpeedrunMode(bool on);
bool SpeedrunMode();
// The overlay's two lines ("IGT 1:02:03  RTA 1:05:00", "Ch 2-1   1-3 12:34"); false when the mode is off.
// Any thread.
bool SpeedrunLines(char* top, size_t topSize, char* bottom, size_t bottomSize);
// Once a retrace on the guest thread.
void OnRetrace();
// Developer Stage Select: enters room (stage << 8 | room) at entry point `point` of debug/roominfo.dat,
// at pos / angle, as the GameCube's debug AREA JUMP (room_jmp.cpp roomJumpExec) does. Applied at the
// next retrace in free play (pG->Rno0 == 3); any thread. `part`: the room's part (Part_next); `refillLife`:
// the life filled up (Stage Select), kept otherwise (Play As restarting the room).
void RequestRoomJump(uint16_t room, uint8_t point, float x, float y, float z, float angle, uint8_t part = 0,
                     bool refillLife = true);
// Retraces the game's main loop waits a step (system_vcnt): 2 in play (30 fps), 1 in some menus. Any thread.
uint32_t RetracesPerFrame();
// The game's Options > Controller > Aim is Reverse (CFG_AIM_REVERSE): the stick pushed up aims up.
// Normal, the default, aims down with the stick up. Any thread.
bool AimReversed();
// Touch camera, Swipe: yawDelta (radians, added up until the next game frame) turns the player,
// lookUp (-1..1, +1 the stick pushed up) holds the camera's pitch while held. Any thread.
void TouchLook(float yawDelta, float lookUp, bool held);
// Applies TouchLook after the game's calcDepressionRatio (qfps = CamCtrl.m_QuasiFPS). Guest thread.
void ApplyTouchLook(uint32_t qfps);

} // namespace GameTweaks
