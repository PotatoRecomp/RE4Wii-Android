#pragma once

// Crosshair (crosshair.cpp): a "+" or a dot drawn by the settings overlay where the laser sight hits,
// while the player aims. It follows the laser's end point, so it shows even when the laser line does
// not (Disable Effects, a hard-to-see colour).
namespace Crosshair {

enum Style : int { kOff = 0, kPlus = 1, kDot = 2 };

// style Style, sizePercent 50..200 (100 = the default size). Any thread.
void Configure(int style, int sizePercent);

// From the settings overlay's frame, with ImGui ready.
void Draw();

} // namespace Crosshair
