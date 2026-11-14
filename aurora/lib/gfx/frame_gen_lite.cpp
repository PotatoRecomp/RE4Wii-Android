#include "frame_gen_lite.hpp"

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"

#include <absl/container/flat_hash_map.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>

namespace aurora::gfx::frame_gen_lite {
namespace {
Module Log("aurora::gfx::frame_gen_lite");
using webgpu::g_device;
using webgpu::g_queue;

std::atomic_bool g_enabled{false};
std::atomic_bool g_held{false};  // menus: the full interpolation instead
std::atomic_bool g_olderStale{false};  // held since the last snapshot: the one before is another screen
// Render thread only: snapshots left before the warp is used.
int g_settle = 30;

std::mutex g_viewMutex;
std::array<float, 12> g_view{};
bool g_viewSet = false;
std::array<float, 12> g_takenView{};  // the previous frame's, for camera_delta
bool g_takenSet = false;
std::array<float, 12> g_frameDelta{};  // measured from the draws at finalize, taken at seal
bool g_frameDeltaSet = false;

std::atomic<uint64_t> g_warped{0}, g_repeated{0}, g_cuts{0};

constexpr std::string_view FullscreenVertex = R"(
struct VOut { @builtin(position) pos: vec4f };
@vertex fn vs_main(@builtin(vertex_index) i: u32) -> VOut {
    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    var o: VOut;
    o.pos = vec4f(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}
)";

// The EFB as it is after the frame's last 3D draw: colour and depth into the Lite textures.
constexpr std::string_view SnapshotShader = R"(
@group(0) @binding(0) var src_color: texture_2d<f32>;
@group(0) @binding(1) var src_depth: texture_depth_2d;
struct FOut { @location(0) color: vec4f, @location(1) depth: f32 };
@fragment fn fs_main(in: VOut) -> FOut {
    let c = vec2i(in.pos.xy);
    var o: FOut;
    o.color = textureLoad(src_color, c, 0);
    o.depth = textureLoad(src_depth, c, 0);
    return o;
}
)";

// Backward warp: for each pixel of the in-between picture, find the pixel of the newer frame that
// moves there (a few fixed-point steps on the newer frame's depth), take its colour and depth.
constexpr std::string_view WarpShader = R"(
struct P { m: mat4x4f, vp: vec4f, size: vec4f, mask: vec4f, bounds: vec4f, edge: vec4f };
@group(0) @binding(0) var col: texture_2d<f32>;
@group(0) @binding(1) var dep: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;
@group(0) @binding(3) var<uniform> p: P;
@group(0) @binding(4) var mdep: texture_depth_2d;
@group(0) @binding(5) var mcol: texture_2d<f32>;
@group(0) @binding(6) var pcol: texture_2d<f32>;
@group(0) @binding(7) var pmcol: texture_2d<f32>;  // older mask: bound, no longer read
fn to_ndc(px: vec2f) -> vec2f {
    let u = (px - p.vp.xy) / p.vp.zw;
    return vec2f(u.x * 2.0 - 1.0, 1.0 - u.y * 2.0);
}
fn to_px(n: vec2f) -> vec2f { return p.vp.xy + vec2f(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5) * p.vp.zw; }
fn load_z(px: vec2f) -> f32 {
    let c = clamp(vec2i(px), vec2i(0, 0), vec2i(p.size.xy) - vec2i(1, 1));
    let d = textureLoad(dep, c, 0).r;
    return (d - p.size.z) / max(p.size.w - p.size.z, 1e-6);
}
fn move_px(px: vec2f, z: f32) -> vec3f {
    let c = p.m * vec4f(to_ndc(px), z, 1.0);
    let w = select(1e-6, c.w, abs(c.w) > 1e-6);
    return vec3f(to_px(c.xy / w), c.z / w);
}// A pixel of a self-moving draw: it is redrawn at its half-way place, so the warp takes background
// from around it instead. The mask pass starts from the scene's depth (pushed back a hair) and a
// magenta colour, so its draws only touch the colour where the scene shows them: with or without
// a depth write (the HUD's glass icons, effects), and not where a still draw hides them.
fn touched(px: vec2f) -> bool {
    let c = clamp(vec2i(px), vec2i(0, 0), vec2i(p.size.xy) - vec2i(1, 1));
    let k = textureLoad(mcol, c, 0).rgb;
    return any(abs(k - vec3f(1.0, 0.0, 1.0)) >= vec3f(0.003));
}
fn is_moving(px: vec2f) -> bool {
    if (p.mask.x < 0.5) { return false; }
    return touched(px) || touched(px + vec2f(2.0, 2.0)) || touched(px + vec2f(-2.0, 2.0)) ||
           touched(px + vec2f(2.0, -2.0)) || touched(px + vec2f(-2.0, -2.0));
}
// The mover there wrote the scene's depth (its own surface), not just colour over a still one.
fn wrote_depth(px: vec2f) -> bool {
    let c = clamp(vec2i(px), vec2i(0, 0), vec2i(p.size.xy) - vec2i(1, 1));
    let m = textureLoad(mdep, c, 0);
    let d = textureLoad(dep, c, 0).r;
    return abs(m - d) <= 0.5 * p.mask.z * max(abs(d), 1e-3);
}
struct FOut { @location(0) color: vec4f, @builtin(frag_depth) depth: f32 };
// restore: the background's depth for a hole the mover wrote, else -1.
struct W { color: vec4f, depth: f32, restore: f32 };
@fragment fn fs_main(in: VOut) -> FOut {
    let w = warp_at(in.pos.xy);
    var o: FOut;
    o.color = w.color;
    o.depth = w.depth;
    return o;
}
// After the half-way redraw: the hole pixels it left uncovered (still far) get the background's
// depth back, through the depth test (colour untouched). Far there, the game's screen filter after
// the scene fogged a flat grey patch ahead of running characters.
@fragment fn fs_restore(in: VOut) -> @builtin(frag_depth) f32 {
    let w = warp_at(in.pos.xy);
    if (w.restore < 0.0) { discard; }
    return w.restore;
}
fn warp_at(q: vec2f) -> W {
    var s = q;
    // Inside what the scene drew: past its edge is letterbox black, not scene.
    let lo = p.bounds.xy;
    let hi = max(p.bounds.zw - vec2f(1.0, 1.0), lo);
    if (any(q < lo) || any(q >= p.bounds.zw)) {
        // Letterbox: as the frame left it.
        var b: W;
        b.color = textureLoad(col, vec2i(q), 0);
        b.depth = textureLoad(dep, vec2i(q), 0).r;
        b.restore = -1.0;
        return b;
    }
    var sp = s;
    for (var i = 0; i < 3; i++) {
        let moved = move_px(s, load_z(s));
        sp = s;
        s = clamp(s + (q - moved.xy), lo, hi);
    }
    // Background the camera swung out from behind a near character (RE4's camera circles Leon,
    // who stays put on screen while the background slides): no newer pixel lands here, and the
    // steps swing between the background and the character. Ending on the character drew it
    // again here, 2-3 stacked copies of Leon and Ashley (more steps, more copies). Take the
    // farther of the last two: the background beside the character, stretched, never a copy.
    if (length(move_px(s, load_z(s)).xy - q) > 1.5 && p.mask.y * load_z(sp) > p.mask.y * load_z(s)) {
        s = sp;
    }
    // Where the newer frame would have it, unclamped: past the scene's edge (the camera turned
    // that side into view) the newer frame never saw it and the clamp smeared its edge row. The
    // older frame did see it, half the motion the other way: q - (t - q).
    let t = s + (q - move_px(s, load_z(s)).xy);
    let unseen = p.edge.x > 0.5 && (any(t < lo - vec2f(1.0)) || any(t > hi + vec2f(1.0)));
    let s0 = s;
    let hole = is_moving(s0);
    var po = clamp(2.0 * q - t, lo, hi);
    var fromNewer = false;
    var fromRing = false;
    var ringColor = vec4f(0.0);
    var holeAlpha = -1.0;
    if (hole) {
        // The farthest still pixels on the nearest ring that has one. The rings grow threefold
        // (8, 24, 72, 216 px): the gap between a close character's legs has none nearer.
        var best = s;
        var found = false;
        var bestFar = -1e30;
        var bestDepth = 0.0;
        var r = 8.0;
        var cs: array<vec2f, 16>;
        var fs: array<f32, 16>;
        for (var ring = 0; ring < 4; ring++) {
            if (found) { break; }
            for (var k = 0; k < 16; k++) {
                let a = f32(k) * 0.3926991;
                let c = clamp(s + vec2f(cos(a), sin(a)) * r, lo, hi);
                cs[k] = c;
                fs[k] = -1e30;
                if (!is_moving(c)) {
                    // Farthest wins (background behind the mover), but never the cleared depth:
                    // that is letterbox or empty EFB, black next to the scene.
                    let d = textureLoad(dep, vec2i(c), 0).r;
                    let f = select(p.mask.y * d, -1e29, abs(d - p.mask.w) < 1e-7);
                    fs[k] = f;
                    if (f > bestFar) { bestFar = f; best = c; bestDepth = d; found = true; }
                }
            }
            r = r * 3.0;
        }
        if (found) {
            // The mean of the ring's samples about as far as the farthest. One farthest sample
            // per pixel flipped between directions from pixel to pixel: a jagged grey outline
            // around running characters every in-between picture.
            var sum = vec4f(0.0);
            var n = 0.0;
            for (var k = 0; k < 16; k++) {
                if (fs[k] > -1e28 && bestFar - fs[k] <= 0.02 * max(abs(bestFar), 1e-3)) {
                    sum = sum + textureSampleLevel(col, samp, cs[k] / p.size.xy, 0.0);
                    n = n + 1.0;
                }
            }
            ringColor = sum / max(n, 1.0);
            fromRing = n > 0.0;
            // RE4's screen filter (the tail) reads the EFB alpha, and the older frame's or a mover
            // edge pixel's alpha is not the background's here: a flat light grey body shape and
            // outline beside running characters every in-between picture. Colour from the fill,
            // alpha from the background beside the mover in this frame.
            holeAlpha = textureLoad(col, vec2i(best), 0).a;
            // The background behind the mover, about as deep as the farthest still pixel around
            // it. Moving it with the mover's own depth found the mover again in the older frame:
            // a grey body shape beside the character every in-between picture (running).
            let zb = (bestDepth - p.size.z) / max(p.size.w - p.size.z, 1e-6);
            var sb = q;
            for (var i = 0; i < 3; i++) { sb = sb + (q - move_px(sb, zb).xy); }
            // Pixel centres: a bilinear sample between pixels blended in the mover beside the
            // background pixel, a thin outline of the character in every in-between picture.
            let sbc = floor(clamp(sb, lo, hi)) + vec2f(0.5);
            // Exact pixels here, not the widened mask: the widening put every pixel of the band
            // around a character on the ring fill.
            // Never the older frame here: its characters (where its mask missed them) came into
            // the hole as a second copy beside the redraw, every in-between picture.
            if (all(sb == clamp(sb, lo, hi)) && !touched(sbc)) {
                // The newer frame shows that background beside the mover.
                s = sbc;
                fromNewer = true;
            }
        }
        if (!fromNewer && !touched(s0)) {
            // Only in the widened band: the pixel itself is background.
            fromNewer = true;
            s = floor(s0) + vec2f(0.5);
        }
        if (!fromNewer) { s = best; }
    }
    let z = load_z(s);
    var o: W;
    o.restore = -1.0;
    o.color = textureSampleLevel(col, samp, s / p.size.xy, 0.0);
    if (hole && !fromNewer && fromRing) { o.color = ringColor; }
    if (unseen) {
        o.color = textureSampleLevel(pcol, samp, po / p.size.xy, 0.0);
        if (p.mask.x > 1.5 && p.mask.x < 2.5) { o.color = vec4f(0.0, 1.0, 0.0, 1.0); }
    }
    if (holeAlpha >= 0.0) { o.color.a = holeAlpha; }
    if (p.mask.x > 1.5 && p.mask.x < 2.5 && fromNewer) { o.color = vec4f(0.0, 1.0, 1.0, 1.0); }
    if (p.mask.x > 1.5 && p.mask.x < 2.5 && hole && !fromNewer) { o.color = vec4f(1.0, 0.0, 0.0, 1.0); }
    if (p.mask.x > 2.5) { o.color = vec4f(1.0, 0.0, 1.0, 1.0); }
    o.depth = clamp(p.size.z + move_px(s, z).z * (p.size.w - p.size.z), 0.0, 1.0);
    if (is_moving(s0)) {
        if (wrote_depth(s0)) {
            // The mover's own surface: far, so nothing of the warp hides its half-way redraw (with
            // the background's depth there, parts of Ashley lost the depth test and showed the newer
            // pose: she shook against the redrawn rest). fs_restore brings the background's depth
            // back where the redraw left the hole uncovered.
            if (holeAlpha >= 0.0) { o.restore = o.depth; }
            o.depth = p.mask.w;
        } else {
            // Colour over a still surface (text on paper, a shadow): that surface's depth stays.
            o.depth = clamp(p.size.z + move_px(s0, load_z(s0)).z * (p.size.w - p.size.z), 0.0, 1.0);
        }
    }
    if (p.mask.x > 3.5) { o.depth = p.mask.w; }
    return o;
}
)";

// The mask pass's starting depth: the scene's, a hair farther, so a draw at the scene's own depth
// passes whatever its compare function.
constexpr std::string_view MaskDepthShader = R"(
struct P { m: mat4x4f, vp: vec4f, size: vec4f, mask: vec4f };
@group(0) @binding(0) var dep: texture_2d<f32>;
@group(0) @binding(1) var<uniform> p: P;
@fragment fn fs_main(in: VOut) -> @builtin(frag_depth) f32 {
    let d = textureLoad(dep, vec2i(in.pos.xy), 0).r;
    return clamp(d + p.mask.y * p.mask.z * max(abs(d), 1e-3), 0.0, 1.0);
}
)";

struct Resources {
  bool ready = false;
  wgpu::BindGroupLayout maskDepthLayout;
  absl::flat_hash_map<wgpu::TextureFormat, wgpu::RenderPipeline> maskDepthPipelines;
  wgpu::BindGroupLayout snapshotLayout;
  wgpu::BindGroupLayout warpLayout;
  wgpu::RenderPipeline snapshotPipeline;
  absl::flat_hash_map<wgpu::TextureFormat, wgpu::RenderPipeline> warpPipelines;
  absl::flat_hash_map<wgpu::TextureFormat, wgpu::RenderPipeline> restorePipelines;
  wgpu::BindGroup warpGroup;  // of the last draw_warp, for draw_restore
  wgpu::Sampler sampler;
  wgpu::Buffer uniform;
  wgpu::Texture color, depth;
  wgpu::TextureView colorView, depthView;
  // The snapshot before (swapped with `color` at each snapshot): the older frame, for what the
  // camera turned into view at the screen's edge.
  wgpu::Texture prevColor;
  wgpu::TextureView prevColorView;
  bool haveSnapshot = false, prevValid = false;
  uint32_t width = 0, height = 0;
  // Mask pass target: the self-moving draws again, in the EFB's formats so their pipelines fit.
  wgpu::Texture maskColor, maskDepth;
  wgpu::TextureView maskColorView, maskDepthView;
  // The mask before (swapped at each mask pass): the older frame's movers, valid when it was the
  // frame right before this one's.
  wgpu::Texture prevMaskColor;
  wgpu::TextureView prevMaskColorView;
  bool prevMaskValid = false;
  uint64_t snapshots = 0, maskSnapshot = 0;
  wgpu::TextureFormat maskFormat = wgpu::TextureFormat::Undefined;
  uint32_t maskWidth = 0, maskHeight = 0;
};
Resources g_res;

wgpu::ShaderModule make_module(std::string_view body, const char* label) {
  std::string source;
  source += FullscreenVertex;
  source += body;
  const wgpu::ShaderSourceWGSL wgsl{wgpu::ShaderSourceWGSL::Init{.code = source.c_str()}};
  const wgpu::ShaderModuleDescriptor descriptor{.nextInChain = &wgsl, .label = label};
  return g_device.CreateShaderModule(&descriptor);
}

wgpu::BindGroupLayout make_layout(const wgpu::BindGroupLayoutEntry* entries, size_t count, const char* label) {
  const wgpu::BindGroupLayoutDescriptor descriptor{.label = label, .entryCount = count, .entries = entries};
  return g_device.CreateBindGroupLayout(&descriptor);
}

void ensure_resources() {
  if (g_res.ready) {
    return;
  }
  const std::array snapshotEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                      .viewDimension = wgpu::TextureViewDimension::e2D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Depth,
                      .viewDimension = wgpu::TextureViewDimension::e2D},
      },
  };
  g_res.snapshotLayout = make_layout(snapshotEntries.data(), snapshotEntries.size(), "Lite Snapshot Layout");
  const std::array warpEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Float,
                      .viewDimension = wgpu::TextureViewDimension::e2D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                      .viewDimension = wgpu::TextureViewDimension::e2D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 2,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler = {.type = wgpu::SamplerBindingType::Filtering},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 3,
          .visibility = wgpu::ShaderStage::Fragment,
          .buffer = {.type = wgpu::BufferBindingType::Uniform},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 4,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Depth,
                      .viewDimension = wgpu::TextureViewDimension::e2D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 5,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                      .viewDimension = wgpu::TextureViewDimension::e2D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 6,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Float,
                      .viewDimension = wgpu::TextureViewDimension::e2D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 7,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                      .viewDimension = wgpu::TextureViewDimension::e2D},
      },
  };
  g_res.warpLayout =make_layout(warpEntries.data(), warpEntries.size(), "Lite Warp Layout");

  const auto module = make_module(SnapshotShader, "Lite Snapshot");
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{.bindGroupLayoutCount = 1,
                                                        .bindGroupLayouts = &g_res.snapshotLayout};
  const auto layout = g_device.CreatePipelineLayout(&layoutDescriptor);
  const std::array targets{
      wgpu::ColorTargetState{.format = wgpu::TextureFormat::RGBA8Unorm},
      wgpu::ColorTargetState{.format = wgpu::TextureFormat::R32Float},
  };
  const wgpu::FragmentState fragment{
      .module = module, .entryPoint = "fs_main", .targetCount = targets.size(), .targets = targets.data()};
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = "Lite Snapshot",
      .layout = layout,
      .vertex = {.module = module, .entryPoint = "vs_main"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
      .fragment = &fragment,
  };
  g_res.snapshotPipeline = g_device.CreateRenderPipeline(&descriptor);

  const wgpu::SamplerDescriptor samplerDescriptor{
      .label = "Lite Sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
  };
  g_res.sampler = g_device.CreateSampler(&samplerDescriptor);
  const wgpu::BufferDescriptor bufferDescriptor{
      .label = "Lite Warp Params",
      .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
      .size = sizeof(WarpParams),
  };
  g_res.uniform = g_device.CreateBuffer(&bufferDescriptor);
  g_res.ready = true;
}

wgpu::RenderPipeline warp_pipeline(wgpu::TextureFormat colorFormat) {
  if (const auto it = g_res.warpPipelines.find(colorFormat); it != g_res.warpPipelines.end()) {
    return it->second;
  }
  const auto module = make_module(WarpShader, "Lite Warp");
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{.bindGroupLayoutCount = 1,
                                                        .bindGroupLayouts = &g_res.warpLayout};
  const auto layout = g_device.CreatePipelineLayout(&layoutDescriptor);
  const std::array targets{wgpu::ColorTargetState{.format = colorFormat}};
  const wgpu::FragmentState fragment{
      .module = module, .entryPoint = "fs_main", .targetCount = targets.size(), .targets = targets.data()};
  const wgpu::DepthStencilState depthStencil{
      .format = wgpu::TextureFormat::Depth32Float,
      .depthWriteEnabled = wgpu::OptionalBool::True,
      .depthCompare = wgpu::CompareFunction::Always,
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = "Lite Warp",
      .layout = layout,
      .vertex = {.module = module, .entryPoint = "vs_main"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
      .depthStencil = &depthStencil,
      .fragment = &fragment,
  };
  auto pipeline = g_device.CreateRenderPipeline(&descriptor);
  g_res.warpPipelines.emplace(colorFormat, pipeline);
  return pipeline;
}

wgpu::RenderPipeline restore_pipeline(wgpu::TextureFormat colorFormat, bool reversedZ) {
  if (const auto it = g_res.restorePipelines.find(colorFormat); it != g_res.restorePipelines.end()) {
    return it->second;
  }
  const auto module = make_module(WarpShader, "Lite Restore");
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{.bindGroupLayoutCount = 1,
                                                        .bindGroupLayouts = &g_res.warpLayout};
  const auto layout = g_device.CreatePipelineLayout(&layoutDescriptor);
  const std::array targets{wgpu::ColorTargetState{.format = colorFormat, .writeMask = wgpu::ColorWriteMask::None}};
  const wgpu::FragmentState fragment{
      .module = module, .entryPoint = "fs_restore", .targetCount = targets.size(), .targets = targets.data()};
  // Only where the depth is still farther than the background's: the hole the redraw left.
  const wgpu::DepthStencilState depthStencil{
      .format = wgpu::TextureFormat::Depth32Float,
      .depthWriteEnabled = wgpu::OptionalBool::True,
      .depthCompare = reversedZ ? wgpu::CompareFunction::Greater : wgpu::CompareFunction::Less,
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = "Lite Restore",
      .layout = layout,
      .vertex = {.module = module, .entryPoint = "vs_main"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
      .depthStencil = &depthStencil,
      .fragment = &fragment,
  };
  auto pipeline = g_device.CreateRenderPipeline(&descriptor);
  g_res.restorePipelines.emplace(colorFormat, pipeline);
  return pipeline;
}

wgpu::RenderPipeline mask_depth_pipeline(wgpu::TextureFormat colorFormat) {
  if (const auto it = g_res.maskDepthPipelines.find(colorFormat); it != g_res.maskDepthPipelines.end()) {
    return it->second;
  }
  if (!g_res.maskDepthLayout) {
    const std::array entries{
        wgpu::BindGroupLayoutEntry{
            .binding = 0,
            .visibility = wgpu::ShaderStage::Fragment,
            .texture = {.sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                        .viewDimension = wgpu::TextureViewDimension::e2D},
        },
        wgpu::BindGroupLayoutEntry{
            .binding = 1,
            .visibility = wgpu::ShaderStage::Fragment,
            .buffer = {.type = wgpu::BufferBindingType::Uniform},
        },
    };
    g_res.maskDepthLayout = make_layout(entries.data(), entries.size(), "Lite Mask Depth Layout");
  }
  const auto module = make_module(MaskDepthShader, "Lite Mask Depth");
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{.bindGroupLayoutCount = 1,
                                                        .bindGroupLayouts = &g_res.maskDepthLayout};
  const auto layout = g_device.CreatePipelineLayout(&layoutDescriptor);
  const std::array targets{wgpu::ColorTargetState{.format = colorFormat, .writeMask = wgpu::ColorWriteMask::None}};
  const wgpu::FragmentState fragment{
      .module = module, .entryPoint = "fs_main", .targetCount = targets.size(), .targets = targets.data()};
  const wgpu::DepthStencilState depthStencil{
      .format = wgpu::TextureFormat::Depth32Float,
      .depthWriteEnabled = wgpu::OptionalBool::True,
      .depthCompare = wgpu::CompareFunction::Always,
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = "Lite Mask Depth",
      .layout = layout,
      .vertex = {.module = module, .entryPoint = "vs_main"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
      .depthStencil = &depthStencil,
      .fragment = &fragment,
  };
  auto pipeline = g_device.CreateRenderPipeline(&descriptor);
  g_res.maskDepthPipelines.emplace(colorFormat, pipeline);
  return pipeline;
}

void ensure_textures(uint32_t width, uint32_t height) {
  if (g_res.color && g_res.width == width && g_res.height == height) {
    return;
  }
  const auto make = [&](wgpu::TextureFormat format, const char* label) {
    const wgpu::TextureDescriptor descriptor{
        .label = label,
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
        .size = {width, height, 1},
        .format = format,
    };
    return g_device.CreateTexture(&descriptor);
  };
  g_res.color = make(wgpu::TextureFormat::RGBA8Unorm, "Lite Color");
  g_res.depth = make(wgpu::TextureFormat::R32Float, "Lite Depth");
  g_res.prevColor = make(wgpu::TextureFormat::RGBA8Unorm, "Lite Color Older");
  g_res.colorView = g_res.color.CreateView();
  g_res.depthView = g_res.depth.CreateView();
  g_res.prevColorView = g_res.prevColor.CreateView();
  g_res.haveSnapshot = false;
  g_res.width = width;
  g_res.height = height;
}

void ensure_mask_textures(uint32_t width, uint32_t height, wgpu::TextureFormat colorFormat) {
  if (g_res.maskColor && g_res.maskWidth == width && g_res.maskHeight == height &&
      g_res.maskFormat == colorFormat) {
    return;
  }
  const auto make = [&](wgpu::TextureFormat format, const char* label) {
    const wgpu::TextureDescriptor descriptor{
        .label = label,
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
        .size = {width, height, 1},
        .format = format,
    };
    return g_device.CreateTexture(&descriptor);
  };
  g_res.maskColor = make(colorFormat, "Lite Mask Color");
  g_res.maskDepth = make(wgpu::TextureFormat::Depth32Float, "Lite Mask Depth");
  g_res.prevMaskColor = make(colorFormat, "Lite Mask Color Older");
  g_res.prevMaskColorView = g_res.prevMaskColor.CreateView();
  g_res.maskSnapshot = 0;
  g_res.maskColorView = g_res.maskColor.CreateView();
  g_res.maskDepthView = g_res.maskDepth.CreateView();
  g_res.maskFormat = colorFormat;
  g_res.maskWidth = width;
  g_res.maskHeight = height;
}

// Plain 4x4 helpers (row-major, column vectors).
using M4 = std::array<std::array<double, 4>, 4>;

M4 mul(const M4& a, const M4& b) {
  M4 r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r[i][j] += a[i][k] * b[k][j];
  return r;
}

bool invert(const M4& m, M4& out) {
  double a[4][8];
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      a[i][j] = m[i][j];
      a[i][j + 4] = i == j ? 1.0 : 0.0;
    }
  }
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
    if (std::fabs(a[pivot][c]) < 1e-12) return false;
    if (pivot != c)
      for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[pivot][j]);
    const double inv = 1.0 / a[c][c];
    for (int j = 0; j < 8; ++j) a[c][j] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      const double f = a[r][c];
      for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
    }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out[i][j] = a[i][j + 4];
  return true;
}

M4 from_view(const std::array<float, 12>& v) {
  M4 m{};
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) m[r][c] = v[r * 4 + c];
  m[3][3] = 1.0;
  return m;
}
} // namespace

bool enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }
bool held() noexcept { return g_held.load(std::memory_order_relaxed); }
void set_held(bool on) noexcept {
  g_held.store(on, std::memory_order_relaxed);
  if (on) {
    g_olderStale.store(true, std::memory_order_relaxed);  // no snapshots while held
  }
}
void set_enabled(bool on) noexcept {
  if (g_enabled.exchange(on, std::memory_order_relaxed) != on) {
    Log.info("60 FPS Lite: {}", on ? "on" : "off");
  }
}

void set_scene_view(const float view[12]) noexcept {
  std::lock_guard lock(g_viewMutex);
  std::memcpy(g_view.data(), view, sizeof(float) * 12);
  g_viewSet = true;
}

bool take_scene_view(std::array<float, 12>& out) noexcept {
  std::lock_guard lock(g_viewMutex);
  const bool set = g_viewSet;
  if (set) {
    out = g_view;
    g_takenView = g_view;
    g_takenSet = true;
  }
  g_viewSet = false;
  return set;
}

bool camera_delta(std::array<float, 12>& out) noexcept {
  std::array<float, 12> older{}, newer{};
  {
    std::lock_guard lock(g_viewMutex);
    if (!g_viewSet || !g_takenSet) return false;
    older = g_takenView;
    newer = g_view;
  }
  M4 newerInv{};
  if (!invert(from_view(newer), newerInv)) return false;
  const M4 d = mul(from_view(older), newerInv);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = static_cast<float>(d[r][c]);
  return true;
}

bool mask_targets(uint32_t width, uint32_t height, wgpu::TextureFormat colorFormat, wgpu::TextureView& color,
                  wgpu::TextureView& depth) noexcept {
  ensure_resources();
  ensure_mask_textures(width, height, colorFormat);
  std::swap(g_res.maskColor, g_res.prevMaskColor);
  std::swap(g_res.maskColorView, g_res.prevMaskColorView);
  g_res.prevMaskValid = g_res.prevValid && g_res.maskSnapshot != 0 && g_res.maskSnapshot + 1 == g_res.snapshots;
  g_res.maskSnapshot = g_res.snapshots;
  color = g_res.maskColorView;
  depth = g_res.maskDepthView;
  return true;
}

void set_frame_delta(const float* delta) noexcept {
  std::lock_guard lock(g_viewMutex);
  g_frameDeltaSet = delta != nullptr;
  if (delta) std::memcpy(g_frameDelta.data(), delta, sizeof(float) * 12);
}

bool take_frame_delta(std::array<float, 12>& out) noexcept {
  std::lock_guard lock(g_viewMutex);
  const bool set = g_frameDeltaSet;
  if (set) out = g_frameDelta;
  g_frameDeltaSet = false;
  return set;
}

static bool make_warp_r4(const SceneSplit& split, const M4& r4, WarpParams& out) noexcept;

bool make_warp(const SceneSplit& split, const std::array<float, 12>& olderView,
               const std::array<float, 12>& newerView, WarpParams& out) noexcept {
  // Newer view space -> older view space: R4 = V_old * V_new^-1 = [Rr | t].
  M4 newerInv{};
  if (!invert(from_view(newerView), newerInv)) return false;
  return make_warp_r4(split, mul(from_view(olderView), newerInv), out);
}

bool make_warp_from_delta(const SceneSplit& split, const std::array<float, 12>& delta, WarpParams& out) noexcept {
  return make_warp_r4(split, from_view(delta), out);
}

static bool make_warp_r4(const SceneSplit& split, const M4& r4, WarpParams& out) noexcept {
  // Rotation as a quaternion (Shepperd), and the older camera's centre in the newer view space.
  const double m00 = r4[0][0], m11 = r4[1][1], m22 = r4[2][2];
  double qw, qx, qy, qz;
  const double trace = m00 + m11 + m22;
  if (trace > 0.0) {
    const double s = std::sqrt(trace + 1.0) * 2.0;
    qw = 0.25 * s;
    qx = (r4[2][1] - r4[1][2]) / s;
    qy = (r4[0][2] - r4[2][0]) / s;
    qz = (r4[1][0] - r4[0][1]) / s;
  } else if (m00 > m11 && m00 > m22) {
    const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
    qw = (r4[2][1] - r4[1][2]) / s;
    qx = 0.25 * s;
    qy = (r4[0][1] + r4[1][0]) / s;
    qz = (r4[0][2] + r4[2][0]) / s;
  } else if (m11 > m22) {
    const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
    qw = (r4[0][2] - r4[2][0]) / s;
    qx = (r4[0][1] + r4[1][0]) / s;
    qy = 0.25 * s;
    qz = (r4[1][2] + r4[2][1]) / s;
  } else {
    const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
    qw = (r4[1][0] - r4[0][1]) / s;
    qx = (r4[0][2] + r4[2][0]) / s;
    qy = (r4[1][2] + r4[2][1]) / s;
    qz = 0.25 * s;
  }
  if (qw < 0.0) {
    qw = -qw, qx = -qx, qy = -qy, qz = -qz;
  }
  const double angle = 2.0 * std::acos(std::min(1.0, qw));
  const double cx = -(r4[0][0] * r4[0][3] + r4[1][0] * r4[1][3] + r4[2][0] * r4[2][3]);
  const double cy = -(r4[0][1] * r4[0][3] + r4[1][1] * r4[1][3] + r4[2][1] * r4[2][3]);
  const double cz = -(r4[0][2] * r4[0][3] + r4[1][2] * r4[1][3] + r4[2][2] * r4[2][3]);
  const double travel = std::sqrt(cx * cx + cy * cy + cz * cz);
  // A cut: more than 25 degrees or 2000 units (2 m in RE4) in one game frame.
  if (angle > 0.436 || travel > 2000.0) {
    g_cuts.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // Half the rotation, about the half-way camera centre.
  double hw = qw + 1.0, hx = qx, hy = qy, hz = qz;
  const double hl = std::sqrt(hw * hw + hx * hx + hy * hy + hz * hz);
  hw /= hl, hx /= hl, hy /= hl, hz /= hl;
  M4 d{};
  d[0][0] = 1 - 2 * (hy * hy + hz * hz);
  d[0][1] = 2 * (hx * hy - hz * hw);
  d[0][2] = 2 * (hx * hz + hy * hw);
  d[1][0] = 2 * (hx * hy + hz * hw);
  d[1][1] = 1 - 2 * (hx * hx + hz * hz);
  d[1][2] = 2 * (hy * hz - hx * hw);
  d[2][0] = 2 * (hx * hz - hy * hw);
  d[2][1] = 2 * (hy * hz + hx * hw);
  d[2][2] = 1 - 2 * (hx * hx + hy * hy);
  for (int r = 0; r < 3; ++r) {
    d[r][3] = -(d[r][0] * cx + d[r][1] * cy + d[r][2] * cz) * 0.5;
  }
  d[3][3] = 1.0;

  M4 proj{};
  const Vec4<float>* rows[4] = {&split.projection.m0, &split.projection.m1, &split.projection.m2,
                                &split.projection.m3};
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) proj[r][c] = (*rows[r])[c];
  M4 projInv{};
  if (!invert(proj, projInv)) return false;
  const M4 m = mul(proj, mul(d, projInv));
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) out.matrix[c * 4 + r] = static_cast<float>(m[r][c]);
  for (int i = 0; i < 4; ++i) out.viewport[i] = split.viewport[i];
  for (int i = 0; i < 4; ++i) out.bounds[i] = split.bounds[i];
  out.size = {static_cast<float>(split.targetWidth), static_cast<float>(split.targetHeight), split.minDepth,
              split.maxDepth};
  return true;
}

void encode_snapshot(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& color,
                     const wgpu::TextureView& depth, uint32_t width, uint32_t height) noexcept {
  ensure_resources();
  ensure_textures(width, height);
  std::swap(g_res.color, g_res.prevColor);
  std::swap(g_res.colorView, g_res.prevColorView);
  const bool stale = g_olderStale.exchange(false, std::memory_order_relaxed);
  g_res.prevValid = g_res.haveSnapshot && !stale;
  g_res.haveSnapshot = true;
  ++g_res.snapshots;
  // Right after a Load the game streams at a few fps with the camera and characters jumping
  // between frames: half-way redraws stacked copies of them. No warp for the first 30 snapshots
  // after a held screen (5 s at that speed, 1 s at full). Not timed: the render thread's gaps
  // jitter past any threshold, and each repeat costs a full redraw.
  if (stale) {
    g_settle = 30;
  } else if (g_settle > 0) {
    --g_settle;
  }
  const std::array entries{
      wgpu::BindGroupEntry{.binding = 0, .textureView = color},
      wgpu::BindGroupEntry{.binding = 1, .textureView = depth},
  };
  const wgpu::BindGroupDescriptor groupDescriptor{
      .label = "Lite Snapshot Group", .layout = g_res.snapshotLayout, .entryCount = entries.size(), .entries = entries.data()};
  const auto group = g_device.CreateBindGroup(&groupDescriptor);
  const std::array attachments{
      wgpu::RenderPassColorAttachment{
          .view = g_res.colorView, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store},
      wgpu::RenderPassColorAttachment{
          .view = g_res.depthView, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store},
  };
  const wgpu::RenderPassDescriptor passDescriptor{
      .label = "Lite Snapshot", .colorAttachmentCount = attachments.size(), .colorAttachments = attachments.data()};
  const auto pass = cmd.BeginRenderPass(&passDescriptor);
  pass.SetPipeline(g_res.snapshotPipeline);
  pass.SetBindGroup(0, group);
  pass.Draw(3);
  pass.End();
}

bool draw_warp(const wgpu::RenderPassEncoder& pass, wgpu::TextureFormat colorFormat,
               const WarpParams& params) noexcept {
  if (!g_res.ready || !g_res.color) {
    return false;
  }
  if (!g_res.maskDepth) {
    ensure_mask_textures(1, 1, colorFormat);  // the binding is needed even with the mask off
  }
  auto warp = params;
  warp.edge[0] = g_res.prevValid ? 1.f : 0.f;
  warp.edge[1] = 0.f;  // unused (was the older-frame hole fill)
  g_queue.WriteBuffer(g_res.uniform, 0, &warp, sizeof(warp));
  const std::array entries{
      wgpu::BindGroupEntry{.binding = 0, .textureView = g_res.colorView},
      wgpu::BindGroupEntry{.binding = 1, .textureView = g_res.depthView},
      wgpu::BindGroupEntry{.binding = 2, .sampler = g_res.sampler},
      wgpu::BindGroupEntry{.binding = 3, .buffer = g_res.uniform, .size = sizeof(WarpParams)},
      wgpu::BindGroupEntry{.binding = 4, .textureView = g_res.maskDepthView},
      wgpu::BindGroupEntry{.binding = 5, .textureView = g_res.maskColorView},
      wgpu::BindGroupEntry{.binding = 6, .textureView = g_res.prevColorView},
      wgpu::BindGroupEntry{.binding = 7, .textureView = g_res.prevMaskColorView},
  };
  const wgpu::BindGroupDescriptor groupDescriptor{
      .label = "Lite Warp Group", .layout = g_res.warpLayout, .entryCount = entries.size(), .entries = entries.data()};
  const auto group = g_device.CreateBindGroup(&groupDescriptor);
  pass.SetPipeline(warp_pipeline(colorFormat));
  pass.SetBindGroup(0, group);
  pass.Draw(3);
  g_res.warpGroup = group;
  g_warped.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void draw_restore(const wgpu::RenderPassEncoder& pass, wgpu::TextureFormat colorFormat, bool reversedZ) noexcept {
  if (!g_res.warpGroup) return;
  pass.SetPipeline(restore_pipeline(colorFormat, reversedZ));
  pass.SetBindGroup(0, g_res.warpGroup);
  pass.Draw(3);
  g_res.warpGroup = nullptr;
}

bool draw_mask_depth(const wgpu::RenderPassEncoder& pass, wgpu::TextureFormat colorFormat,
                     const WarpParams& params) noexcept {
  if (!g_res.ready || !g_res.depth) {
    return false;
  }
  // The same params draw_warp writes later in this frame: one write, both draws read it.
  g_queue.WriteBuffer(g_res.uniform, 0, &params, sizeof(params));
  const auto pipeline = mask_depth_pipeline(colorFormat);
  const std::array entries{
      wgpu::BindGroupEntry{.binding = 0, .textureView = g_res.depthView},
      wgpu::BindGroupEntry{.binding = 1, .buffer = g_res.uniform, .size = sizeof(WarpParams)},
  };
  const wgpu::BindGroupDescriptor groupDescriptor{.label = "Lite Mask Depth Group",
                                                  .layout = g_res.maskDepthLayout,
                                                  .entryCount = entries.size(),
                                                  .entries = entries.data()};
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, g_device.CreateBindGroup(&groupDescriptor));
  pass.Draw(3);
  return true;
}

Stats stats() noexcept {
  return {g_warped.load(std::memory_order_relaxed), g_repeated.load(std::memory_order_relaxed),
          g_cuts.load(std::memory_order_relaxed)};
}

bool settled() noexcept { return g_settle == 0; }

void count_repeat(bool cut) noexcept {
  g_repeated.fetch_add(1, std::memory_order_relaxed);
  (void)cut;
}

} // namespace aurora::gfx::frame_gen_lite
