#pragma once

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
constexpr unsigned kReducedScreenFilters = 0x15Bu;  // 0, 1, 3, 4, 6, 8
void SetSkippedScreenFilters(unsigned mask);
// Reduce Shadows: the game's own shadow switch (DPF_SHADOW) held on, so rooms with a shadow light skip
// the cast / self shadow passes. Foot shadows stay. Any thread.
void SetReduceShadows(bool on);
// Fill Screen (no bars): the game's own crop mode, the letterboxed picture scaled to the full height.
// Any thread.
void SetRemoveBars(bool on);
// The crop is on this frame: Fill Screen is set and the game is in free gameplay (not a movie,
// event, sub screen or title).
bool BarsRemoved();
// The sub screen or the merchant is open: their 3D panels must not be widened (Hor+). Any thread.
bool MenuScreen();
// The share of the frame's height the picture keeps with the bars cropped (336 of 448 lines).
constexpr float kCroppedPictureHeight = 336.0f / 448.0f;
// Once a retrace on the guest thread.
void OnRetrace();

} // namespace GameTweaks
