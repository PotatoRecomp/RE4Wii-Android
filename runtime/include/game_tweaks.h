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
// A menu is open (sub screen, card access, merchant, typewriter): screen filters stay on. Any thread.
bool MenuScreen();
// Boot logos (no room yet), the title and its Load Game, or the typewriter's save screen: Fill
// shows these at 16:9 (display_settings.cpp). Any thread.
bool FramedScreen();
// Once a retrace on the guest thread.
void OnRetrace();

} // namespace GameTweaks
