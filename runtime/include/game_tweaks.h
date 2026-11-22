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
// 60 FPS Full held at 30 for the bolt-action Rifle or the infrared scope (UpdateMenuScreen).
bool RifleHold();
// Boot logos (no room yet), the title and its Load Game, or the typewriter's save screen: the
// letterbox crop stays off there. Any thread.
bool FramedScreen();
// The framed screens Fill shows at 16:9 with bars and no crop: all but the title's main menu, which
// is cropped and filled like play. Any thread.
bool FramedBars();
// The room being played (0x104 for r104), 0 outside free play (boot, title, typewriter). Any thread.
uint16_t CurrentRoom();
// titleMain ran (the title's main menu is up). Game thread.
void NoteTitleMainMenu();
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
// Settings > Controls > Auto Run: the input holds B (run) for the player while the stick points forward,
// and a held B walks. Any thread.
void SetAutoRun(bool on);
// Auto Run is on and the player walks freely in play (no menu, pause, event, aiming or ladder):
// the only time B may be flipped. Any thread.
bool AutoRunActive();
// Settings > Controls > Toggle Aim (CubeEvil4's "Latch L and R"): a press of R (aim) or L (knife)
// holds it until the next press (virtual_remote.cpp). Any thread.
void SetToggleAim(bool on);
// Toggle Aim is on and may hold L / R now: free play, no menu, event or L / R prompt. Any thread.
bool AimLatchAllowed();
// Free play (pG->Rno0 3, no menu, sub screen or event: Status_flg 13, 14, 19); `prompt` gets the button
// prompt on screen (Cckpt.m_ActBttn.m_disp_flag, 0 none). Toggle Run, the mouse and the knife swipe
// act only then. Any thread.
bool FreePlay(uint8_t* prompt = nullptr);
// Settings > Controls > Aim Sensitivity (0.25 .. 3): the gamepad / keyboard stick is scaled by it while
// the player aims (the touch controls scale their own). The game's full tilt stays the top speed.
// Any thread.
void SetAimSensitivity(float scale);
// The scale for the stick now: the setting while aiming, 1 otherwise.
float AimStickScale();
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
// The binoculars are up (Status_flg STA_BINOCULAR, bit 21). Their zoom is the right stick's Y
// (CameraBinocular reads substickY), like the rifle scope's. Any thread.
bool BinocularView();
// A rifle in hand (weapon_no at pG + 0x4FC0: 9 the rifle, 0xA the semi-auto, 0x28 and 0x2F the
// semi-auto's copies in other characters' weapon modules): a weapon that aims through its scope.
// Any thread.
bool RifleInHand();
// A rifle aimed (pPL routine 0, r_no_1 6): the scope view, also in the frames before the game sets
// STA_LOOK_THROUGH (Status_flg 16) and after it clears it (moto g57, Final Beta: Camera Distance
// widened the scope's fov 45 with bit 16 off). Any thread.
bool RifleAimed();
// Touch camera, Swipe: yawDelta (radians, added up until the next game frame) turns the player,
// lookUp (-1..1, +1 the stick pushed up) holds the camera's pitch while held. Any thread.
void TouchLook(float yawDelta, float lookUp, bool held);
// Applies TouchLook after the game's calcDepressionRatio (qfps = CamCtrl.m_QuasiFPS). Guest thread.
void ApplyTouchLook(uint32_t qfps);

} // namespace GameTweaks
