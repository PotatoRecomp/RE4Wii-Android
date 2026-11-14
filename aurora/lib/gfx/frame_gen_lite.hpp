#pragma once

#include <aurora/math.hpp>
#include <webgpu/webgpu_cpp.h>

#include <array>
#include <cstdint>

// 60 FPS Lite (aurora_set_frame_gen_lite): the picture between two 30 fps game frames is the newer
// frame's 3D picture moved back by half the camera's motion (depth-based reprojection, like VR's
// positional timewarp), with the frame's 2D (screen filters, HUD, menus) drawn on top again. The
// game's GX draws are not replayed, so it costs one full-screen pass instead of a second frame.
namespace aurora::gfx::frame_gen_lite {

// Where the frame's 3D ends: the last perspective draw into the EFB. Everything after it in the
// sealed passes (pass `pass` from command `command` on) is drawn again over the moved picture.
struct SceneSplit {
  bool valid = false;
  uint32_t pass = 0;
  uint32_t command = 0;
  Mat4x4<float> projection{};  // the draw's projection as the vertex shader applies it
  float viewport[4]{};         // left, top, width, height in target pixels
  // Left, top, right, bottom of the viewport cut by the scissor: where the scene really drew
  // (RE4 letterboxes it with the scissor inside a full-height viewport).
  float bounds[4]{};
  float minDepth = 0.f, maxDepth = 1.f;
  uint32_t targetWidth = 0, targetHeight = 0;
};

struct WarpParams {
  std::array<float, 16> matrix{};  // column-major: NDC of the newer frame -> clip of the in-between
  std::array<float, 4> viewport{};
  std::array<float, 4> size{};  // target width, height, min depth, max depth
  // x: mask on, y: +1 when a larger depth is farther (-1 reversed Z), z: relative depth bias of
  // the mask pass's starting depth, w: the far depth
  std::array<float, 4> mask{};
  std::array<float, 4> bounds{};  // left, top, right, bottom of the drawn scene in target pixels
  std::array<float, 4> edge{};    // set by draw_warp: x the older snapshot, y the older mask is there
};

bool enabled() noexcept;
void set_enabled(bool on) noexcept;
// Held (menus, title, Load screen): the in-between picture is the full interpolation's replay. The
// warp only fits a 3D scene under a moving camera; menu 2D drawn before the last 3D draw broke.
bool held() noexcept;
void set_held(bool on) noexcept;

// The camera of the frame being recorded (GX Mtx rows); taken by take_scene_view at seal.
void set_scene_view(const float view[12]) noexcept;
// The view set since the last call, if any.
bool take_scene_view(std::array<float, 12>& out) noexcept;

// V_prev * V_cur^-1 (GX Mtx rows) from the last taken view and the one set since; false without
// both. A draw that only moved with the camera has prev = delta * cur.
bool camera_delta(std::array<float, 12>& out) noexcept;
// The mask pass target (colour in the EFB's format, Depth32Float), sized to the EFB.
bool mask_targets(uint32_t width, uint32_t height, wgpu::TextureFormat colorFormat, wgpu::TextureView& color,
                  wgpu::TextureView& depth) noexcept;

// The camera delta (V_prev * V_cur^-1, GX Mtx rows) measured from the frame's draws at finalize,
// null when none; taken at seal for the warp.
void set_frame_delta(const float* delta) noexcept;
bool take_frame_delta(std::array<float, 12>& out) noexcept;
// The warp from a measured delta instead of the two views.
bool make_warp_from_delta(const SceneSplit& split, const std::array<float, 12>& delta, WarpParams& out) noexcept;

// The in-between picture's warp from the views of the older and newer frame; false when the camera
// cut or turned too far for a reprojection to look right (the picture is then repeated).
bool make_warp(const SceneSplit& split, const std::array<float, 12>& olderView,
               const std::array<float, 12>& newerView, WarpParams& out) noexcept;

// Copies the EFB's colour and depth (single-sample) into the Lite textures, outside any pass.
void encode_snapshot(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& color,
                     const wgpu::TextureView& depth, uint32_t width, uint32_t height) noexcept;
// First draw of the mask pass: its depth starts as the scene's snapshot, a hair farther.
bool draw_mask_depth(const wgpu::RenderPassEncoder& pass, wgpu::TextureFormat colorFormat,
                     const WarpParams& params) noexcept;
// Draws the moved picture into an open pass over the EFB (colour `colorFormat`, Depth32Float).
bool draw_warp(const wgpu::RenderPassEncoder& pass, wgpu::TextureFormat colorFormat,
               const WarpParams& params) noexcept;
// After the half-way redraw, in the same pass: the background's depth back in the holes it left.
void draw_restore(const wgpu::RenderPassEncoder& pass, wgpu::TextureFormat colorFormat, bool reversedZ) noexcept;

// Diagnostics for the runtime's log line.
struct Stats {
  uint64_t warped = 0;
  uint64_t repeated = 0;   // no split, no camera, MSAA, or a cut
  uint64_t cuts = 0;       // of `repeated`: camera moved too far
};
Stats stats() noexcept;
// False for the first 30 frames after a held screen (Load): the in-between picture is then the
// repeat.
bool settled() noexcept;
void count_repeat(bool cut) noexcept;

} // namespace aurora::gfx::frame_gen_lite
