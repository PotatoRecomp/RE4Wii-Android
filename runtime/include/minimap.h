#pragma once

#include <cstdint>

// The minimap (minimap.cpp): the current room's outline from its floor collision, with Leon, Ashley,
// the enemies and the items, drawn over the game by the settings overlay. It only reads guest memory.
namespace Minimap {

enum Flags : uint32_t {
    kEnabled = 1u << 0,
    kEnemies = 1u << 1,
    kPartner = 1u << 2,   // Ashley (and Luis)
    kAnimals = 1u << 3,   // crows, chickens, cows, snakes, fish
    kNorthUp = 1u << 4,   // otherwise it turns with the camera
    kItems = 1u << 5,     // pick-ups lying in the room
};

enum Corner : int { kTopLeft = 0, kTopRight = 1, kBottomLeft = 2, kBottomRight = 3 };

// sizePercent: the map's side as a percent of the screen's height (15..60); zoom 0 near, 1 medium,
// 2 far; opacityPercent 20..100. Any thread.
void Configure(uint32_t flags, int sizePercent, int corner, int zoom, int opacityPercent);

// Once a retrace on the guest thread: the snapshot the overlay draws.
void OnRetrace();

// From the settings overlay's frame, with ImGui ready.
void Draw();

} // namespace Minimap
