#pragma once

#include "gx.hpp"

#include "aurora/gfx.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

// Frame interpolation: generates intermediate presentation frames between two consecutive 60 Hz
// guest frames by re-staging each perspective draw's uniform block with interpolated transforms.
namespace aurora::gx {
constexpr uint32_t MaxInterpolatedFrames = 3;

// Identifies a draw across frames. `combined` includes the geometry signature and is exact, while
// `pipeline`/`texture` is the material-only fallback for meshes whose vertex data changes.
struct FrameInterpolationDrawIdentity {
  HashType combined = 0;
  HashType pipeline = 0;
  HashType texture = 0;
  // Hash of the per-vertex PNMTXIDX stream. Matrix values animate, but this topology decides which
  // absolute palette slot each vertex reads, so it must match across a pair.
  HashType matrixTopology = 0;
};

// Uniform values other than the vertex transforms that the in-between pictures blend: texture
// matrices (scrolling water, lava), TEV, konstant and channel colours (fades, a torch's light) and
// lights. Without them an inserted picture showed the newer frame's values on the half-way
// geometry, so these still stepped at 30. Ranges are captured from GX state as build_uniform
// writes them; offsets are bytes from the start of the draw's uniform block.
enum class InterpolationBlendKind : uint8_t {
  Color,         // plain floats
  TexMatrix,     // Mat3x4 run; a matrix whose translation jumps (a scroll wrapping) keeps the newer one
  Light,         // 20-float Light run; a light that moved far (slot reused) keeps the newer one
};
struct InterpolationBlendRange {
  uint16_t offset = 0;
  uint16_t floats = 0;
  InterpolationBlendKind kind = InterpolationBlendKind::Color;
  bool operator==(const InterpolationBlendRange&) const = default;
};
constexpr size_t MaxInterpolationBlendRanges = 16;
constexpr size_t MaxInterpolationBlendFloats = 640;
struct InterpolationBlendCapture {
  std::array<InterpolationBlendRange, MaxInterpolationBlendRanges> ranges{};
  std::array<float, MaxInterpolationBlendFloats> values{};
  uint32_t count = 0;
  uint32_t floats = 0;
  bool overflow = false;  // more than fits: the draw blends nothing extra, as before
  // Appends `n` floats at uniform offset `offset`, joining the previous range when contiguous.
  void add(size_t offset, const float* data, uint32_t n, InterpolationBlendKind kind) noexcept {
    if (overflow || floats + n > values.size() || offset + n * sizeof(float) > UINT16_MAX) {
      overflow = true;
      return;
    }
    std::memcpy(values.data() + floats, data, n * sizeof(float));
    floats += n;
    if (count > 0) {
      auto& last = ranges[count - 1];
      if (last.kind == kind && last.offset + last.floats * sizeof(float) == offset) {
        last.floats = static_cast<uint16_t>(last.floats + n);
        return;
      }
    }
    if (count == ranges.size()) {
      overflow = true;
      return;
    }
    ranges[count++] = {static_cast<uint16_t>(offset), static_cast<uint16_t>(n), kind};
  }
};

// Where the transforms live inside a draw's staged uniform block. Offsets are
// relative to the start of the mapped range.
struct InterpolatedUniformLayout {
  const uint8_t* sourceUniformData = nullptr;
  size_t uniformSize = 0;
  size_t projectionOffset = 0;
  size_t positionOffset = 0;
  size_t normalOffset = 0;
  // Slot the live matrix occupies; a compacted position region holds it at 0.
  size_t currentMatrix = 0;
  bool indexedMatrices = false;
  const InterpolationBlendCapture* blend = nullptr;
};

namespace detail {
// Defined in frame_interpolation.cpp, exposed so the early-outs below stay inline: the shipped
// build compiles shards without LTO, so a cross-TU call would land on every draw in the frame.
extern std::atomic_uint32_t g_frameInterpolationFps;
} // namespace detail

// 0 when interpolation is disabled; otherwise the configured target (60/120/180/240).
inline uint32_t frame_interpolation_fps() noexcept {
  return detail::g_frameInterpolationFps.load(std::memory_order_acquire);
}
inline bool frame_interpolation_active() noexcept { return frame_interpolation_fps() != 0; }

void set_frame_interpolation_fps(uint32_t targetFps) noexcept;
// Whether in-between pictures also blend texture matrices, colours and lights (AURORA_INTERP_BLEND=0
// turns it off, to compare).
bool frame_interpolation_blend_enabled() noexcept;
// The guest frame length the inserted slots subdivide (one or more retraces).
void set_frame_interpolation_source_period(uint64_t periodNanos) noexcept;

// Feedback for the adaptive slot controller: whether the producer met its
// retrace boundary for the frame that just presented.
void report_producer_paced(bool paced) noexcept;

void begin_frame_interpolation() noexcept;
void finalize_frame_interpolation() noexcept;
// Fills the observability snapshot behind aurora_get_frame_interpolation_diagnostics.
void get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics& diagnostics) noexcept;
bool has_interpolated_frame() noexcept;
uint32_t interpolated_frame_count() noexcept;
void mark_frame_interpolation_replay_unsafe() noexcept;
// Drops the interpolation tasks staged into the currently mapped uniform range. Required of
// anything that unmaps or rotates that range before the frame is finalized.
void drop_pending_frame_interpolation_uniforms() noexcept;
bool frame_interpolation_replay_safe() noexcept;
// 60 FPS Lite: the sorted uniform offsets (slot 0 copies) of the finalized frame's draws that moved
// by themselves rather than only with the camera, and the offset of the first draw of the frame's
// trailing screen-fixed run (the HUD; UINT32_MAX when there is none).
void take_lite_moving_ranges(std::vector<uint32_t>& out, uint32_t& tailOffset) noexcept;

// Records one perspective draw and maps its intermediate uniform copies, returning the mapped
// range per slot (empty when the draw has no counterpart). Called by build_uniform.
std::array<gfx::Range, MaxInterpolatedFrames> record_interpolation_draw(
    const FrameInterpolationDrawIdentity& identity, const Mat4x4<float>& projection,
    uint16_t usedPnMtxMask, const InterpolatedUniformLayout& uniformLayout) noexcept;

// Folds a merged draw back into the snapshot of the draw it joined. aurora renders merged
// primitives through the first one's uniform block, so without this the merged-in bones tear.
void extend_interpolation_draw(uint16_t usedPnMtxMask) noexcept;

bool interpolate_transform(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                           float weight, Mat3x4<float>& output) noexcept;
bool interpolate_transform_midpoint(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                                    Mat3x4<float>& output) noexcept;
// Matrix palettes are already-composed skinning transforms, so interpolating their coefficients
// keeps shared boundaries intact. Ordinary one-matrix draws keep the rigid TRS path above.
bool interpolate_indexed_transform(const Mat3x4<float>& previous,
                                   const Mat3x4<float>& current, float weight,
                                   Mat3x4<float>& output) noexcept;
float transform_match_distance_squared(const Mat3x4<float>& previous,
                                       const Mat3x4<float>& current) noexcept;
} // namespace aurora::gx
