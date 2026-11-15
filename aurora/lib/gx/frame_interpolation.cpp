#include <cstdlib>
#include "frame_interpolation.hpp"

#include "../internal.hpp"
#include "aurora/gfx.h"
#include "../gfx/frame_gen_lite.hpp"

// Guest matrices really do carry NaN/Inf, and the isfinite guards here keep them
// out of the MatchEdge sort. Needs -fno-finite-math-only (see runtime/CMakeLists.txt).
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
#error "frame_interpolation.cpp must be compiled with -fno-finite-math-only; its NaN filtering is load-bearing for memory safety (see comment above)."
#endif

#include <algorithm>
#include <bit>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <absl/container/flat_hash_map.h>
#include <vector>

namespace aurora::gx {
namespace detail {
std::atomic_uint32_t g_frameInterpolationFps{0};
} // namespace detail

namespace {
static Module Log("aurora::gx::interp");

// Diagnostics for aurora_get_frame_interpolation_diagnostics. Individually atomic:
// one writer, and a torn read is harmless for an overlay.
std::atomic_uint32_t s_diagCandidates{0};
std::atomic_uint32_t s_diagMatchable{0};
std::atomic_uint32_t s_diagMatches{0};
std::atomic_bool s_diagEligible{false};
std::atomic_bool s_diagReplaySafe{true};
std::atomic_uint64_t s_diagFramesSealed{0};
std::atomic_uint64_t s_diagFramesLowMatch{0};
// Blended uniform values (InterpolationBlendCapture): in-between draws whose blended values moved
// since the previous frame, and texture matrices or lights kept at the newer value because they jumped.
std::atomic_uint64_t s_diagBlendDraws{0};
std::atomic_uint64_t s_diagBlendJumps{0};
std::atomic_uint64_t s_diagFramesReplayUnsafe{0};
std::atomic_uint64_t s_diagSlotReductions{0};
std::atomic_uint64_t s_diagLateSealDrops{0};
// Persistent worker pool for the per-sample interpolation tasks. libc++ has no
// parallel execution policies, so without it the seal loop runs serially. Leaked.
class InterpolationWorkerPool {
public:
  static InterpolationWorkerPool& instance() {
    static InterpolationWorkerPool* pool = new InterpolationWorkerPool();
    return *pool;
  }

  // Runs fn(index) for every index in [0, count). Returns once every index has
  // been processed. Not reentrant; only the producer/seal thread dispatches.
  template <typename Fn>
  void run(size_t count, const Fn& fn) {
    if (count == 0) {
      return;
    }
    if (m_workers.empty()) {
      for (size_t i = 0; i < count; ++i) {
        fn(i);
      }
      return;
    }
    {
      // Job state and the generation bump publish under one lock, so a late worker
      // can never observe a half-written job.
      std::lock_guard lock(m_mutex);
      m_invoke = [&fn](size_t index) { fn(index); };
      m_count.store(count, std::memory_order_relaxed);
      m_next.store(0, std::memory_order_relaxed);
      m_remaining.store(count, std::memory_order_relaxed);
      ++m_generation;
    }
    m_wake.notify_all();
    consume();
    // Wait for stragglers to leave consume() entirely, not just finish their chunks,
    // so the next dispatch can safely reset the shared counters.
    while (m_remaining.load(std::memory_order_acquire) != 0 ||
           m_active.load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
    }
    m_invoke = nullptr;
  }

private:
  static constexpr size_t kChunk = 16;

  InterpolationWorkerPool() {
    const unsigned hardware = std::thread::hardware_concurrency();
    // The caller helps too. Cap the helpers: tasks are short memcpy+math, so dispatch
    // overhead and memory bandwidth dominate past a few threads.
    const unsigned helpers = hardware > 2 ? std::min(hardware - 1, 6u) : 0;
    m_workers.reserve(helpers);
    for (unsigned i = 0; i < helpers; ++i) {
      m_workers.emplace_back([this] { worker_loop(); });
    }
  }

  void consume() {
    const size_t count = m_count.load(std::memory_order_relaxed);
    while (true) {
      const size_t begin = m_next.fetch_add(kChunk, std::memory_order_relaxed);
      if (begin >= count) {
        return;
      }
      const size_t end = std::min(begin + kChunk, count);
      for (size_t index = begin; index < end; ++index) {
        m_invoke(index);
      }
      m_remaining.fetch_sub(end - begin, std::memory_order_release);
    }
  }

  void worker_loop() {
    uint64_t seenGeneration = 0;
    while (true) {
      {
        std::unique_lock lock(m_mutex);
        m_wake.wait(lock, [&] { return m_generation != seenGeneration; });
        seenGeneration = m_generation;
        // Counted under the mutex: when the dispatcher sees m_active == 0 every worker is
        // parked or has not read the current generation, so a counter reset is safe.
        m_active.fetch_add(1, std::memory_order_relaxed);
      }
      consume();
      m_active.fetch_sub(1, std::memory_order_release);
    }
  }

  std::vector<std::thread> m_workers;
  std::mutex m_mutex;
  std::condition_variable m_wake;
  uint64_t m_generation = 0;
  std::function<void(size_t)> m_invoke;
  std::atomic_size_t m_count{0};
  std::atomic_size_t m_next{0};
  std::atomic_size_t m_remaining{0};
  std::atomic_size_t m_active{0};
};
struct FrameTransformSnapshot {
  Mat4x4<float> projection{};
  HashType viewportIdentity = 0;
  Mat3x4<float> position{};
  Mat3x4<float> normal{};
  uint16_t usedMatrixMask = 1;
  struct IndexedMatrices {
    std::array<Mat3x4<float>, MaxPnMtx> position{};
    std::array<Mat3x4<float>, MaxPnMtx> normal{};
    // Content hash of each used slot's position/normal pair. A mesh split across draws
    // repeats bone matrices byte for byte, so the seal can pair slots across draws.
    std::array<HashType, MaxPnMtx> slotHash{};
  };
  std::unique_ptr<IndexedMatrices> indexedMatrices;
};

struct FrameTransformEntry {
  FrameInterpolationDrawIdentity identity;
  FrameTransformSnapshot transform;
  // The draw's InterpolationBlendCapture: ranges here, values in the frame's blend arena
  // (s_currentBlendValues / s_previousBlendValues) from blendValueOffset.
  std::array<InterpolationBlendRange, MaxInterpolationBlendRanges> blendRanges{};
  uint32_t blendCount = 0;
  uint32_t blendValueOffset = 0;
  // Identity plus placement (the draw's matrix): set when no other draw of the frame has the same
  // one. Stacked copies of one quad (the title's fade layers, drawn black and white in one place)
  // cannot be told apart by matching, so only unique draws blend their values.
  HashType placementKey = 0;
  bool placementUnique = false;
  // Constant-velocity prediction of a non-indexed transform, so repeated meshes match
  // on where they will be. Not for indexed draws: PNMTXIDX slot identity pairs those.
  Mat3x4<float> predictedPosition{};
  bool hasPrediction = false;
};

struct Quaternion {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

struct PendingUniformInterpolation {
  size_t currentTransformIndex = 0;
  const uint8_t* sourceUniformData = nullptr;
  uint8_t* uniformData = nullptr;
  size_t uniformSize = 0;
  size_t projectionOffset = 0;
  size_t positionOffset = 0;
  size_t normalOffset = 0;
  size_t currentMatrix = 0;
  uint32_t numerator = 0;
  uint32_t denominator = 1;
  bool indexedMatrices = false;
  // Blended over the scene without a depth write (fog sheets, light shafts, lasers, particles):
  // 60 FPS Lite leaves it in the warped picture. As a mover, a fog sheet outdoors made the whole
  // screen a hole, filled from samples up to 216 px away: copies of Leon and Ashley in rings.
  bool overlay = false;
  uint32_t rangeOffset = 0;  // of the slot's uniform copy, the key 60 FPS Lite looks draws up by
};

std::vector<FrameTransformEntry> s_previousFrameTransforms;
// Per task: how 60 FPS Lite treats the draw.
enum : uint8_t {
  kLiteStill = 0,     // moves only with the camera: left to the warp
  kLiteMoving = 1,    // moved by itself: masked out of the warp and redrawn half-way
  kLiteUnpaired = 2,  // no partner in the frame before: redrawn in its own place
  kLiteFixed = 3,     // kept its matrix while the camera moved: fixed to the screen
};
std::vector<uint8_t> s_liteTaskMoving;
uint32_t s_liteTailOffset = UINT32_MAX;
std::vector<uint32_t> s_liteMovingRanges;
std::vector<FrameTransformEntry> s_currentFrameTransforms;
// Blend values of every entry in the frame, kept apart so an entry stays small to move.
std::vector<float> s_previousBlendValues;
std::vector<float> s_currentBlendValues;
// Open addressing: libc++ unordered_map paid a prime modulo and a node chase per draw (4.5% of the GX
// thread on a Snapdragon 730 with 60 FPS on).
using TransformIndexMap = absl::flat_hash_map<HashType, std::vector<size_t>>;
TransformIndexMap s_previousTransformIndices;
TransformIndexMap s_currentTransformIndices;
TransformIndexMap s_previousStableTransformIndices;
TransformIndexMap s_currentStableTransformIndices;

// Free list for the indexed-matrix snapshots; per-draw heap allocation was the
// hottest cost in this path. Unused slots keep stale data, consumers mask first.
std::vector<std::unique_ptr<FrameTransformSnapshot::IndexedMatrices>> s_indexedMatricesPool;
constexpr size_t kMaximumPooledIndexedMatrices = 4096;

std::unique_ptr<FrameTransformSnapshot::IndexedMatrices> acquire_indexed_matrices() {
  if (s_indexedMatricesPool.empty()) {
    return std::make_unique<FrameTransformSnapshot::IndexedMatrices>();
  }
  auto block = std::move(s_indexedMatricesPool.back());
  s_indexedMatricesPool.pop_back();
  return block;
}

void recycle_transform_entries(std::vector<FrameTransformEntry>& entries) noexcept {
  for (auto& entry : entries) {
    if (entry.transform.indexedMatrices &&
        s_indexedMatricesPool.size() < kMaximumPooledIndexedMatrices) {
      s_indexedMatricesPool.push_back(std::move(entry.transform.indexedMatrices));
    }
  }
  entries.clear();
}

// Whether the frame in s_previousFrameTransforms recorded any palette draw;
// record_interpolation_draw uses it to decide whether staging can pay off.
bool s_previousFrameHasIndexedMatrices = false;

// Hands the frame that just sealed to the next frame's matching and returns
// the retiring one's matrix blocks to the pool.
void retire_frame_transforms() noexcept {
  s_previousFrameHasIndexedMatrices =
      std::any_of(s_currentFrameTransforms.begin(), s_currentFrameTransforms.end(),
                  [](const FrameTransformEntry& entry) noexcept {
                    return static_cast<bool>(entry.transform.indexedMatrices);
                  });
  s_previousFrameTransforms.swap(s_currentFrameTransforms);
  s_previousBlendValues.swap(s_currentBlendValues);
  recycle_transform_entries(s_currentFrameTransforms);
  s_currentBlendValues.clear();
}

// clear() destroys every node, so empty the vectors in place and let a stable scene
// reuse them. Fall back to a real clear once the map outgrows the live set.
void clear_index_map_keep_nodes(TransformIndexMap& map,
                                size_t liveEntries) {
  if (map.size() > liveEntries + 256) {
    map.clear();
    return;
  }
  for (auto& entry : map) {
    entry.second.clear();
  }
}
uint32_t s_perspectiveCandidates = 0;
// Candidates that also existed last frame. Freshly spawned effects have no partner
// by definition, so counting them measured spawn churn instead of matcher health.
uint32_t s_perspectiveMatchable = 0;
uint32_t s_perspectiveMatches = 0;
std::vector<PendingUniformInterpolation> s_pendingUniformInterpolations;
std::atomic_bool s_hasInterpolatedFrame{false};
std::atomic_bool s_frameInterpolationReplaySafe{true};
uint32_t s_previousInterpolationFps = 0;

// Adaptive slot count: the target moves at most one step per pacing window, while
// s_activeInterpolationSamples is latched per frame and must not move mid-frame.
std::atomic_uint32_t s_interpolationSampleTarget{0};
std::atomic_uint32_t s_activeInterpolationSamples{0};

// Set when the producer already overran its retrace budget; the next seal then skips
// inserted slots for that one frame, which helps the late frame catch back up.
std::atomic_bool s_dropInterpolationAtSeal{false};
// Frameskip with 60 FPS Lite (skip_interpolation_at_seal): the next frame seals without its slots too.
std::atomic_bool s_skipInterpolationAtSeal{false};
// Game menus (set_frame_interpolation_held): the inventory's item models blinked in 60 FPS Full
// (Poco F7, Adreno 825, Mercenaries). Menus already move at 30, so they lose nothing.
std::atomic_bool s_interpolationHeld{false};

// Windowed backstop for sustained overload. The wide reduce/restore gap is the
// hysteresis, so a scene that can sustain N slots settles there instead of flapping.
std::atomic_uint32_t s_pacingWindowFrames{0};
std::atomic_uint32_t s_pacingWindowMisses{0};
constexpr uint32_t kPacingWindowFrames = 60; // ~1 second of guest frames
constexpr uint32_t kPacingReducePercent = 75;
constexpr uint32_t kPacingRestorePercent = 25;

// Length of one guest frame, from the present schedule: one retrace for a 60 fps game, two for a
// 30 fps one (RE4 outside its menus). The slot count fills that period up to the target rate.
std::atomic_uint64_t s_sourcePeriodNanos{16'683'333};

uint32_t maximum_interpolation_samples() noexcept {
  const uint32_t targetFps = frame_interpolation_fps();
  if (targetFps == 0) {
    return 0;
  }
  const uint64_t period = s_sourcePeriodNanos.load(std::memory_order_acquire);
  // Rounded, so a 59.94 Hz retrace still gives a whole number of slots.
  const uint64_t frames = (static_cast<uint64_t>(targetFps) * period + 500'000'000u) / 1'000'000'000u;
  return frames <= 1 ? 0u : std::min(static_cast<uint32_t>(frames - 1), MaxInterpolatedFrames);
}

constexpr float kMinimumScale = 1.0e-5f;
constexpr float kMaximumTranslationPerFrame = 1500.0f;

// The per-game-frame translation gate (view space). Far from the camera a turn alone moves a draw
// further than kMaximumTranslationPerFrame: far draws then went unmatched or failed their pair and
// drew at the end position next to half-way neighbours, a light jagged band along the horizon in
// the in-between picture (60 FPS Full, Infinix Mali-G610 recording). So the gate also allows a
// quarter of the draw's distance, about a 14 degree turn per game frame. Full only: 60 FPS Lite
// classifies draws by their pairs and keeps its tested behaviour. NaN fails.
bool within_translation_gate(const Mat3x4<float>& previous, const Mat3x4<float>& current) noexcept {
  constexpr float kFarFraction = 0.25f;
  const float x = current.m0.w(), y = current.m1.w(), z = current.m2.w();
  const float far = gfx::frame_gen_lite::enabled() ? 0.0f : (x * x + y * y + z * z) * (kFarFraction * kFarFraction);
  const float gate = std::max(kMaximumTranslationPerFrame * kMaximumTranslationPerFrame, far);
  const float dx = x - previous.m0.w(), dy = y - previous.m1.w(), dz = z - previous.m2.w();
  return dx * dx + dy * dy + dz * dz <= gate;
}
constexpr float kMinimumQuaternionDot = 0.70710678f;
constexpr size_t kNoPreparedPair = std::numeric_limits<size_t>::max();

HashType combine_identity(HashType first, HashType second) noexcept {
  return xxh3_hash(second, first);
}

HashType stable_identity(const FrameInterpolationDrawIdentity& identity) noexcept {
  return combine_identity(combine_identity(identity.pipeline, identity.texture),
                          identity.matrixTopology);
}

float dot3(const std::array<float, 3>& a, const std::array<float, 3>& b) noexcept {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<float, 3> cross3(const std::array<float, 3>& a, const std::array<float, 3>& b) noexcept {
  return {
      a[1] * b[2] - a[2] * b[1],
      a[2] * b[0] - a[0] * b[2],
      a[0] * b[1] - a[1] * b[0],
  };
}

Quaternion quaternion_from_rotation(const std::array<std::array<float, 3>, 3>& m) noexcept {
  Quaternion q;
  const float trace = m[0][0] + m[1][1] + m[2][2];
  if (trace > 0.0f) {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    q.w = 0.25f * s;
    q.x = (m[2][1] - m[1][2]) / s;
    q.y = (m[0][2] - m[2][0]) / s;
    q.z = (m[1][0] - m[0][1]) / s;
  } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
    const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
    q.w = (m[2][1] - m[1][2]) / s;
    q.x = 0.25f * s;
    q.y = (m[0][1] + m[1][0]) / s;
    q.z = (m[0][2] + m[2][0]) / s;
  } else if (m[1][1] > m[2][2]) {
    const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
    q.w = (m[0][2] - m[2][0]) / s;
    q.x = (m[0][1] + m[1][0]) / s;
    q.y = 0.25f * s;
    q.z = (m[1][2] + m[2][1]) / s;
  } else {
    const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
    q.w = (m[1][0] - m[0][1]) / s;
    q.x = (m[0][2] + m[2][0]) / s;
    q.y = (m[1][2] + m[2][1]) / s;
    q.z = 0.25f * s;
  }
  return q;
}

bool decompose_affine(const Mat3x4<float>& matrix, std::array<std::array<float, 3>, 3>& rotation,
                      std::array<float, 3>& scale, std::array<float, 3>& translation,
                      Quaternion& quaternion) noexcept {
  const std::array<Vec4<float>, 3> rows{matrix.m0, matrix.m1, matrix.m2};
  // V*T*R*S puts non-uniform model scale on the columns, so scale must be read off the
  // columns; row extraction misreads R*S as shear whenever the rotation tilts an axis.
  for (size_t row = 0; row < 3; ++row) {
    translation[row] = rows[row].w();
    if (!std::isfinite(translation[row])) {
      return false;
    }
  }
  for (size_t column = 0; column < 3; ++column) {
    const float x = rows[0][column];
    const float y = rows[1][column];
    const float z = rows[2][column];
    scale[column] = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(scale[column]) || scale[column] < kMinimumScale) {
      return false;
    }
    rotation[0][column] = x / scale[column];
    rotation[1][column] = y / scale[column];
    rotation[2][column] = z / scale[column];
  }

  const auto rotationColumn = [&rotation](size_t column) noexcept {
    return std::array<float, 3>{rotation[0][column], rotation[1][column], rotation[2][column]};
  };
  if (std::abs(dot3(rotationColumn(0), rotationColumn(1))) > 0.05f ||
      std::abs(dot3(rotationColumn(0), rotationColumn(2))) > 0.05f ||
      std::abs(dot3(rotationColumn(1), rotationColumn(2))) > 0.05f) {
    return false;
  }

  const float determinant = dot3(rotation[0], cross3(rotation[1], rotation[2]));
  if (!std::isfinite(determinant) || std::abs(std::abs(determinant) - 1.0f) > 0.1f) {
    return false;
  }
  if (determinant < 0.0f) {
    scale[0] = -scale[0];
    for (size_t row = 0; row < 3; ++row) {
      rotation[row][0] = -rotation[row][0];
    }
  }

  quaternion = quaternion_from_rotation(rotation);
  return std::isfinite(quaternion.x) && std::isfinite(quaternion.y) && std::isfinite(quaternion.z) &&
         std::isfinite(quaternion.w);
}

std::array<std::array<float, 3>, 3> rotation_from_quaternion(const Quaternion& q) noexcept {
  const float xx = q.x * q.x;
  const float yy = q.y * q.y;
  const float zz = q.z * q.z;
  const float xy = q.x * q.y;
  const float xz = q.x * q.z;
  const float yz = q.y * q.z;
  const float wx = q.w * q.x;
  const float wy = q.w * q.y;
  const float wz = q.w * q.z;
  return {{
      {{1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz), 2.0f * (xz + wy)}},
      {{2.0f * (xy + wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx)}},
      {{2.0f * (xz - wy), 2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy)}},
  }};
}

struct PreparedAffinePair {
  Mat3x4<float> previous{};
  Mat3x4<float> current{};
  std::array<float, 3> previousScale{};
  std::array<float, 3> currentScale{};
  std::array<float, 3> previousTranslation{};
  std::array<float, 3> currentTranslation{};
  Quaternion previousQuaternion{};
  Quaternion currentQuaternion{};
  bool linear = false;
  bool identical = false;
  bool valid = false;
};

struct PreparedTransformInterpolation {
  size_t nonIndexedPairOffset = kNoPreparedPair;
  std::array<size_t, MaxPnMtx> indexedPairOffsets{};
  // Previous-frame entry the projection interpolates from. For a draw that borrowed
  // its matrices from a sibling, that is the sibling's partner.
  size_t previousProjectionEntry = kNoPreparedPair;
  bool indexedValid = false;

  PreparedTransformInterpolation() {
    indexedPairOffsets.fill(kNoPreparedPair);
  }
};

PreparedAffinePair prepare_affine_pair(const Mat3x4<float>& previous,
                                       const Mat3x4<float>& current) noexcept {
  PreparedAffinePair pair{.previous = previous, .current = current};
  if (std::memcmp(&previous, &current, sizeof(current)) == 0) {
    pair.identical = true;
    pair.valid = true;
    return pair;
  }

  std::array<std::array<float, 3>, 3> previousRotation{};
  std::array<std::array<float, 3>, 3> currentRotation{};
  if (!decompose_affine(previous, previousRotation, pair.previousScale,
                        pair.previousTranslation, pair.previousQuaternion) ||
      !decompose_affine(current, currentRotation, pair.currentScale,
                        pair.currentTranslation, pair.currentQuaternion)) {
    return pair;
  }

  if (!within_translation_gate(previous, current)) {
    return pair;
  }

  float quaternionDot = pair.previousQuaternion.x * pair.currentQuaternion.x +
                        pair.previousQuaternion.y * pair.currentQuaternion.y +
                        pair.previousQuaternion.z * pair.currentQuaternion.z +
                        pair.previousQuaternion.w * pair.currentQuaternion.w;
  if (quaternionDot < 0.0f) {
    pair.currentQuaternion.x = -pair.currentQuaternion.x;
    pair.currentQuaternion.y = -pair.currentQuaternion.y;
    pair.currentQuaternion.z = -pair.currentQuaternion.z;
    pair.currentQuaternion.w = -pair.currentQuaternion.w;
    quaternionDot = -quaternionDot;
  }
  if (!std::isfinite(quaternionDot) || quaternionDot < kMinimumQuaternionDot) {
    return pair;
  }
  pair.valid = true;
  return pair;
}

// Indexed matrices are final palette transforms, so interpolate coefficients instead
// of decomposing: ((1-t)M0 + tM1)v keeps a shared mesh boundary shared.
PreparedAffinePair prepare_indexed_pair(const Mat3x4<float>& previous,
                                        const Mat3x4<float>& current) noexcept {
  PreparedAffinePair pair{
      .previous = previous,
      .current = current,
      .linear = true,
  };
  if (std::memcmp(&previous, &current, sizeof(current)) == 0) {
    pair.identical = true;
    pair.valid = true;
    return pair;
  }

  const std::array<Vec4<float>, 3> previousRows{previous.m0, previous.m1, previous.m2};
  const std::array<Vec4<float>, 3> currentRows{current.m0, current.m1, current.m2};
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      if (!std::isfinite(previousRows[row][column]) ||
          !std::isfinite(currentRows[row][column])) {
        return pair;
      }
    }
  }

  if (!within_translation_gate(previous, current)) {
    return pair;
  }
  pair.valid = true;
  return pair;
}

bool evaluate_affine_pair(const PreparedAffinePair& pair, float weight,
                          Mat3x4<float>& output) noexcept {
  if (!pair.valid || pair.identical) {
    output = pair.current;
    return pair.valid;
  }

  if (pair.linear) {
    const std::array<const Vec4<float>*, 3> previousRows{
        &pair.previous.m0, &pair.previous.m1, &pair.previous.m2};
    const std::array<const Vec4<float>*, 3> currentRows{
        &pair.current.m0, &pair.current.m1, &pair.current.m2};
    const std::array<Vec4<float>*, 3> outputRows{&output.m0, &output.m1, &output.m2};
    for (size_t row = 0; row < 3; ++row) {
      for (size_t column = 0; column < 4; ++column) {
        const float before = (*previousRows[row])[column];
        (*outputRows[row])[column] =
            before + ((*currentRows[row])[column] - before) * weight;
      }
    }
    return true;
  }

  // Consecutive 60 Hz transforms stay in one hemisphere and within 90 degrees, so
  // normalized lerp is stable and skips three transcendentals per matrix.
  const float previousWeight = 1.0f - weight;
  const float currentWeight = weight;
  Quaternion interpolated{
      pair.previousQuaternion.x * previousWeight + pair.currentQuaternion.x * currentWeight,
      pair.previousQuaternion.y * previousWeight + pair.currentQuaternion.y * currentWeight,
      pair.previousQuaternion.z * previousWeight + pair.currentQuaternion.z * currentWeight,
      pair.previousQuaternion.w * previousWeight + pair.currentQuaternion.w * currentWeight,
  };
  const float quaternionLength = std::sqrt(interpolated.x * interpolated.x +
                                           interpolated.y * interpolated.y +
                                           interpolated.z * interpolated.z +
                                           interpolated.w * interpolated.w);
  if (!std::isfinite(quaternionLength) || quaternionLength < kMinimumScale) {
    output = pair.current;
    return false;
  }
  interpolated.x /= quaternionLength;
  interpolated.y /= quaternionLength;
  interpolated.z /= quaternionLength;
  interpolated.w /= quaternionLength;

  const auto interpolatedRotation = rotation_from_quaternion(interpolated);
  std::array<float, 3> interpolatedScale{};
  std::array<float, 3> interpolatedTranslation{};
  for (size_t i = 0; i < 3; ++i) {
    interpolatedScale[i] = pair.previousScale[i] +
                           (pair.currentScale[i] - pair.previousScale[i]) * weight;
    interpolatedTranslation[i] =
        pair.previousTranslation[i] +
        (pair.currentTranslation[i] - pair.previousTranslation[i]) * weight;
  }
  // Column scale mirrors decompose_affine: the reconstruction is R*S, with
  // each scale component applied down its column.
  output = {
      {interpolatedRotation[0][0] * interpolatedScale[0],
       interpolatedRotation[0][1] * interpolatedScale[1],
       interpolatedRotation[0][2] * interpolatedScale[2], interpolatedTranslation[0]},
      {interpolatedRotation[1][0] * interpolatedScale[0],
       interpolatedRotation[1][1] * interpolatedScale[1],
       interpolatedRotation[1][2] * interpolatedScale[2], interpolatedTranslation[1]},
      {interpolatedRotation[2][0] * interpolatedScale[0],
       interpolatedRotation[2][1] * interpolatedScale[1],
       interpolatedRotation[2][2] * interpolatedScale[2], interpolatedTranslation[2]},
  };
  return true;
}

bool interpolate_affine_impl(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                             float weight, Mat3x4<float>& output) noexcept {
  return evaluate_affine_pair(prepare_affine_pair(previous, current), weight, output);
}

Mat4x4<float> interpolate_projection(const Mat4x4<float>& previous, const Mat4x4<float>& current,
                                     float weight) noexcept {
  Mat4x4<float> result = current;
  for (size_t row = 0; row < 4; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      const float before = previous[row][column];
      const float after = current[row][column];
      if (std::isfinite(before) && std::isfinite(after)) {
        result[row][column] = before + (after - before) * weight;
      }
    }
  }
  return result;
}
} // namespace

bool interpolate_transform(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                           float weight, Mat3x4<float>& output) noexcept {
  return interpolate_affine_impl(previous, current, std::clamp(weight, 0.0f, 1.0f), output);
}

bool interpolate_transform_midpoint(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                                    Mat3x4<float>& output) noexcept {
  return interpolate_transform(previous, current, 0.5f, output);
}

bool interpolate_indexed_transform(const Mat3x4<float>& previous,
                                   const Mat3x4<float>& current, float weight,
                                   Mat3x4<float>& output) noexcept {
  return evaluate_affine_pair(prepare_indexed_pair(previous, current),
                              std::clamp(weight, 0.0f, 1.0f), output);
}

float transform_match_distance_squared(const Mat3x4<float>& previous,
                                       const Mat3x4<float>& current) noexcept {
  // Translation dominates in game units; the 3x3 part only breaks ties between
  // repeated meshes at the same origin.
  double distance = 0.0;
  const std::array<Vec4<float>, 3> previousRows{previous.m0, previous.m1, previous.m2};
  const std::array<Vec4<float>, 3> currentRows{current.m0, current.m1, current.m2};
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      const double before = previousRows[row][column];
      const double after = currentRows[row][column];
      if (!std::isfinite(before) || !std::isfinite(after)) {
        return std::numeric_limits<float>::infinity();
      }
      const double delta = after - before;
      distance += delta * delta;
    }
  }
  if (!std::isfinite(distance) || distance > std::numeric_limits<float>::max()) {
    return std::numeric_limits<float>::infinity();
  }
  return static_cast<float>(distance);
}

namespace {
// Translation-only delta, matching the kMaximumTranslationPerFrame gate. Non-finite
// inputs propagate NaN; callers compare with `<=` so NaN fails the gate.
float translation_delta_squared(const Mat3x4<float>& previous,
                                const Mat3x4<float>& current) noexcept {
  const float dx = current.m0.w() - previous.m0.w();
  const float dy = current.m1.w() - previous.m1.w();
  const float dz = current.m2.w() - previous.m2.w();
  return dx * dx + dy * dy + dz * dz;
}


// 2*current - previous. Not a valid rigid transform, but it is only a matching
// reference and its one-frame error stays far below instance spacing.
Mat3x4<float> extrapolate_transform(const Mat3x4<float>& previous,
                                    const Mat3x4<float>& current) noexcept {
  Mat3x4<float> predicted{};
  const std::array<const Vec4<float>*, 3> previousRows{&previous.m0, &previous.m1, &previous.m2};
  const std::array<const Vec4<float>*, 3> currentRows{&current.m0, &current.m1, &current.m2};
  const std::array<Vec4<float>*, 3> predictedRows{&predicted.m0, &predicted.m1, &predicted.m2};
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      (*predictedRows[row])[column] =
          2.0f * (*currentRows[row])[column] - (*previousRows[row])[column];
    }
  }
  return predicted;
}
} // namespace

float snapshot_match_distance_squared(const FrameTransformEntry& previousEntry,
                                      const FrameTransformSnapshot& current) noexcept {
  const FrameTransformSnapshot& previous = previousEntry.transform;
  if (static_cast<bool>(previous.indexedMatrices) != static_cast<bool>(current.indexedMatrices)) {
    return std::numeric_limits<float>::infinity();
  }
  if (!current.indexedMatrices) {
    // Match against the predicted position, falling back to the last known one for
    // entries with no motion history.
    const Mat3x4<float>& reference =
        previousEntry.hasPrediction ? previousEntry.predictedPosition : previous.position;
    return transform_match_distance_squared(reference, current.position);
  }

  if (previous.usedMatrixMask == 0 || current.usedMatrixMask == 0) {
    return std::numeric_limits<float>::infinity();
  }

  // PNMTXIDX is an absolute palette slot baked into the vertices, so nearest matching
  // swaps joint histories. A changed mask has no safe correspondence: leave the draw.
  if (previous.usedMatrixMask != current.usedMatrixMask) {
    return std::numeric_limits<float>::infinity();
  }

  double distance = 0.0;
  size_t matchedMatrices = 0;
  for (size_t currentIndex = 0; currentIndex < MaxPnMtx; ++currentIndex) {
    if ((current.usedMatrixMask & (1u << currentIndex)) == 0) {
      continue;
    }
    const float matrixDistance = transform_match_distance_squared(
        previous.indexedMatrices->position[currentIndex],
        current.indexedMatrices->position[currentIndex]);
    if (!std::isfinite(matrixDistance)) {
      return std::numeric_limits<float>::infinity();
    }
    distance += matrixDistance;
    ++matchedMatrices;
  }
  if (matchedMatrices == 0 || !std::isfinite(distance) ||
      distance > std::numeric_limits<float>::max()) {
    return std::numeric_limits<float>::infinity();
  }
  return static_cast<float>(distance / static_cast<double>(matchedMatrices));
}

void set_frame_interpolation_fps(uint32_t targetFps) noexcept {
  if (targetFps != 60 && targetFps != 120 && targetFps != 180 && targetFps != 240) {
    targetFps = 0;
  }
  detail::g_frameInterpolationFps.store(targetFps, std::memory_order_release);
  // Start at the configured quality; the controller only ever backs off from here. The cap is the
  // most slots any source period can use, so a 60 Hz menu between 30 Hz scenes does not lower it.
  s_interpolationSampleTarget.store(targetFps == 0 ? 0u : MaxInterpolatedFrames, std::memory_order_release);
  s_dropInterpolationAtSeal.store(false, std::memory_order_release);
  s_pacingWindowFrames.store(0, std::memory_order_release);
  s_pacingWindowMisses.store(0, std::memory_order_release);
  // A reconfiguration starts a fresh diagnostic window.
  s_diagFramesSealed.store(0, std::memory_order_relaxed);
  s_diagFramesLowMatch.store(0, std::memory_order_relaxed);
  s_diagFramesReplayUnsafe.store(0, std::memory_order_relaxed);
  s_diagSlotReductions.store(0, std::memory_order_relaxed);
  s_diagLateSealDrops.store(0, std::memory_order_relaxed);
}

void skip_interpolation_at_seal() noexcept { s_skipInterpolationAtSeal.store(true, std::memory_order_release); }
void set_frame_interpolation_held(bool held) noexcept { s_interpolationHeld.store(held, std::memory_order_release); }

void set_frame_interpolation_source_period(uint64_t periodNanos) noexcept {
  if (periodNanos != 0) {
    s_sourcePeriodNanos.store(periodNanos, std::memory_order_release);
  }
}

void report_producer_paced(bool paced) noexcept {
  const uint32_t maximumSamples = maximum_interpolation_samples();
  if (maximumSamples == 0) {
    // Disabled, or a guest frame already as short as the target (a 60 Hz menu at target 60): the
    // target keeps its value for the next 30 Hz scene.
    s_dropInterpolationAtSeal.store(false, std::memory_order_release);
    s_pacingWindowFrames.store(0, std::memory_order_release);
    s_pacingWindowMisses.store(0, std::memory_order_release);
    if (frame_interpolation_fps() == 0) {
      s_interpolationSampleTarget.store(0, std::memory_order_release);
    }
    return;
  }

  // Per-frame decision, no streaks: a late frame seals without its inserted slots and
  // the next one is back at full count. See s_dropInterpolationAtSeal.
  s_dropInterpolationAtSeal.store(!paced, std::memory_order_release);

  const uint32_t frames = s_pacingWindowFrames.load(std::memory_order_acquire) + 1;
  const uint32_t misses =
      s_pacingWindowMisses.load(std::memory_order_acquire) + (paced ? 0u : 1u);
  if (frames < kPacingWindowFrames) {
    s_pacingWindowFrames.store(frames, std::memory_order_release);
    s_pacingWindowMisses.store(misses, std::memory_order_release);
    return;
  }
  // Window complete: adjust the target one step at most, then start over. At
  // one decision per window this cannot spam the log or flap within a second.
  uint32_t target =
      std::min(s_interpolationSampleTarget.load(std::memory_order_acquire), maximumSamples);
  if (misses * 100u >= frames * kPacingReducePercent) {
    if (target > 0) {
      --target;
      s_diagSlotReductions.fetch_add(1, std::memory_order_relaxed);
      Log.info("interpolation slots reduced to {}: {}/{} frames overran their retrace budget",
               target, misses, frames);
    }
  } else if (misses * 100u <= frames * kPacingRestorePercent) {
    if (target < maximumSamples) {
      ++target;
      Log.info("interpolation slots restored to {}: {}/{} frames overran their retrace budget",
               target, misses, frames);
    }
  }
  s_interpolationSampleTarget.store(target, std::memory_order_release);
  s_pacingWindowFrames.store(0, std::memory_order_release);
  s_pacingWindowMisses.store(0, std::memory_order_release);
}

// Blends a matched draw's InterpolationBlendCapture values into its in-between uniform copy, which
// already holds the newer values. Both draws need the same exact identity (geometry and material, not
// the material-only fallback pairing), the same textures, unique placements and the same ranges: the transform matching pairs look-alike quads by position, and the title screen stacks its
// logos (warning, Capcom, CRI, Dolby, the RE4 logo) as same-material quads in one place, fading each
// by its colour. Paired across logos, the blend showed every hidden logo at half strength.
static void blend_uniform_values(const FrameTransformEntry& current, const FrameTransformEntry& previous,
                                 float weight, uint8_t* uniformData) noexcept {
  if (!current.placementUnique || !previous.placementUnique || current.blendCount == 0 ||
      current.blendCount != previous.blendCount ||
      current.identity.combined != previous.identity.combined ||
      current.identity.texture != previous.identity.texture) {
    return;
  }
  uint32_t total = 0;
  for (uint32_t i = 0; i < current.blendCount; ++i) {
    if (current.blendRanges[i] != previous.blendRanges[i]) {
      return;
    }
    total += current.blendRanges[i].floats;
  }
  if (current.blendValueOffset + total > s_currentBlendValues.size() ||
      previous.blendValueOffset + total > s_previousBlendValues.size()) {
    return;
  }
  const float* cur = s_currentBlendValues.data() + current.blendValueOffset;
  const float* prev = s_previousBlendValues.data() + previous.blendValueOffset;
  uint64_t jumps = 0;
  bool changed = false;
  for (uint32_t i = 0; i < current.blendCount; ++i) {
    const auto& range = current.blendRanges[i];
    const uint32_t stride = range.kind == InterpolationBlendKind::TexMatrix ? 12u
                            : range.kind == InterpolationBlendKind::Light   ? 20u
                                                                             : 4u;
    for (uint32_t start = 0; start < range.floats; start += stride) {
      const uint32_t n = std::min(stride, static_cast<uint32_t>(range.floats - start));
      const float* c = cur + start;
      const float* p = prev + start;
      bool jump = false;
      if (range.kind == InterpolationBlendKind::TexMatrix && n == 12) {
        // A scrolling texture wraps its offset (0.95 -> 0.05); half-way would slide it back.
        jump = std::abs(c[3] - p[3]) > 0.25f || std::abs(c[7] - p[7]) > 0.25f || std::abs(c[11] - p[11]) > 0.25f;
      } else if (range.kind == InterpolationBlendKind::Color) {
        // A fade moves a little a frame; a colour that jumps (a flash, a cut) stays at the newer one.
        for (uint32_t k = 0; k < n && !jump; ++k) {
          jump = std::abs(c[k] - p[k]) > 0.5f;
        }
      } else if (range.kind == InterpolationBlendKind::Light && n == 20) {
        // A camera turn swings view-space lights around it (the castle's at 200,000 units moved 10% of
        // that a frame), so a jump is both far and large for the light's distance; a light slot given
        // to another light moves more than its whole distance.
        const float dx = c[0] - p[0], dy = c[1] - p[1], dz = c[2] - p[2];
        const float moved = dx * dx + dy * dy + dz * dz;
        const float distance = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
        jump = moved > kMaximumTranslationPerFrame * kMaximumTranslationPerFrame && moved > distance * 0.0625f;
      }
      if (jump) {
        ++jumps;
        static const bool s_debug = std::getenv("AURORA_BLEND_DEBUG") != nullptr;
        static std::atomic_int s_debugLeft{40};
        if (s_debug && s_debugLeft.fetch_sub(1) > 0) {
          Log.info("blend jump kind {} off {} +{}: prev {} {} {} {} cur {} {} {} {} | combined {} {} tex {:x}",
                   static_cast<int>(range.kind), range.offset, start, p[0], p[1], p[2], n > 3 ? p[3] : 0.f, c[0],
                   c[1], c[2], n > 3 ? c[3] : 0.f, current.identity.combined == previous.identity.combined ? "same" : "diff",
                   current.identity.matrixTopology == previous.identity.matrixTopology ? "topo" : "-",
                   static_cast<uint64_t>(current.identity.texture));
        }
        continue;
      }
      std::array<float, 20> blended{};  // n <= 20: one colour, texture matrix or light
      float* out = blended.data();
      for (uint32_t k = 0; k < n; ++k) {
        out[k] = p[k] + (c[k] - p[k]) * weight;
        changed |= c[k] != p[k];
      }
      if (range.kind == InterpolationBlendKind::Light && n == 20) {
        // Light directions are unit vectors (prepare_shader_light); keep them so.
        const float length = std::sqrt(out[4] * out[4] + out[5] * out[5] + out[6] * out[6]);
        if (length > 1.0e-6f) {
          out[4] /= length;
          out[5] /= length;
          out[6] /= length;
        }
      }
      std::memcpy(uniformData + range.offset + start * sizeof(float), out, n * sizeof(float));
    }
    cur += range.floats;
    prev += range.floats;
  }
  if (changed) {
    s_diagBlendDraws.fetch_add(1, std::memory_order_relaxed);
  }
  if (jumps != 0) {
    s_diagBlendJumps.fetch_add(jumps, std::memory_order_relaxed);
  }
}

void begin_frame_interpolation() noexcept {
  const uint32_t targetFps = frame_interpolation_fps();
  if (targetFps != s_previousInterpolationFps) {
    recycle_transform_entries(s_previousFrameTransforms);
    s_previousBlendValues.clear();
    s_previousInterpolationFps = targetFps;
  }
  // Latch the slot count for the frame starting here; see s_activeInterpolationSamples
  // for why it cannot move again until the seal.
  s_activeInterpolationSamples.store(
      std::min(s_interpolationSampleTarget.load(std::memory_order_acquire),
               maximum_interpolation_samples()),
      std::memory_order_release);
  // Normally empty (finalize swapped and recycled); an aborted frame can leave
  // entries behind, whose matrix blocks go back to the pool here.
  recycle_transform_entries(s_currentFrameTransforms);
  s_currentBlendValues.clear();
  // Keep buckets, nodes and capacities across frames so a stable race scene does not
  // pay thousands of small allocations every frame.
  clear_index_map_keep_nodes(s_previousTransformIndices, s_previousFrameTransforms.size());
  clear_index_map_keep_nodes(s_previousStableTransformIndices, s_previousFrameTransforms.size());
  for (size_t i = 0; i < s_previousFrameTransforms.size(); ++i) {
    const auto& identity = s_previousFrameTransforms[i].identity;
    s_previousTransformIndices[identity.combined].push_back(i);
    s_previousStableTransformIndices[stable_identity(identity)].push_back(i);
  }
  clear_index_map_keep_nodes(s_currentTransformIndices, s_previousFrameTransforms.size());
  clear_index_map_keep_nodes(s_currentStableTransformIndices, s_previousFrameTransforms.size());
  s_pendingUniformInterpolations.clear();
  s_perspectiveCandidates = 0;
  s_perspectiveMatchable = 0;
  s_perspectiveMatches = 0;
  s_hasInterpolatedFrame.store(false, std::memory_order_release);
  s_frameInterpolationReplaySafe.store(true, std::memory_order_release);
}

// Sets placementUnique on the frame's draws that captured blend values (see FrameTransformEntry).
static void mark_unique_placements() noexcept {
  static std::vector<std::pair<HashType, size_t>> keys;
  keys.clear();
  for (size_t i = 0; i < s_currentFrameTransforms.size(); ++i) {
    if (s_currentFrameTransforms[i].blendCount != 0) {
      keys.emplace_back(s_currentFrameTransforms[i].placementKey, i);
    }
  }
  std::sort(keys.begin(), keys.end());
  for (size_t i = 0; i < keys.size(); ++i) {
    const bool sameAsPrevious = i > 0 && keys[i - 1].first == keys[i].first;
    const bool sameAsNext = i + 1 < keys.size() && keys[i + 1].first == keys[i].first;
    s_currentFrameTransforms[keys[i].second].placementUnique = !sameAsPrevious && !sameAsNext;
  }
}

// 60 FPS Lite: the matrix delta (prev * cur^-1, GX Mtx rows) that the most matrix pairs agree on,
// i.e. the camera's motion as the world-still draws show it. False when no delta gathers a
// clear group (cut, menu, nothing paired).
template <typename PairAt>
static bool vote_camera_delta(size_t count, const PairAt& pairAt, std::array<float, 12>& out) noexcept {
  if (count < 4) return false;
  const auto deltaOf = [](const Mat3x4<float>& prev, const Mat3x4<float>& cur, std::array<double, 12>& d) {
    const double a[3][4] = {{cur.m0[0], cur.m0[1], cur.m0[2], cur.m0[3]},
                            {cur.m1[0], cur.m1[1], cur.m1[2], cur.m1[3]},
                            {cur.m2[0], cur.m2[1], cur.m2[2], cur.m2[3]}};
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                       a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (std::fabs(det) < 1e-12) return false;
    double inv[3][4];
    inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
    inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
    inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
    inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
    inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
    inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
    inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
    inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
    inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
    for (int r = 0; r < 3; ++r)
      inv[r][3] = -(inv[r][0] * a[0][3] + inv[r][1] * a[1][3] + inv[r][2] * a[2][3]);
    const double p[3][4] = {{prev.m0[0], prev.m0[1], prev.m0[2], prev.m0[3]},
                            {prev.m1[0], prev.m1[1], prev.m1[2], prev.m1[3]},
                            {prev.m2[0], prev.m2[1], prev.m2[2], prev.m2[3]}};
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c) {
        double v = c == 3 ? p[r][3] : 0.0;
        for (int k = 0; k < 3; ++k) v += p[r][k] * inv[k][c];
        d[r * 4 + c] = v;
      }
    return true;
  };
  const auto agrees = [](const std::array<double, 12>& d, const Mat3x4<float>& prev, const Mat3x4<float>& cur) {
    const Vec4<float>* curRows[3] = {&cur.m0, &cur.m1, &cur.m2};
    const Vec4<float>* prevRows[3] = {&prev.m0, &prev.m1, &prev.m2};
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c) {
        double v = c == 3 ? d[r * 4 + 3] : 0.0;
        for (int k = 0; k < 3; ++k) v += d[r * 4 + k] * (*curRows[k])[c];
        if (std::fabs(v - (*prevRows[r])[c]) > (c == 3 ? 3.0 : 2e-3)) return false;
      }
    return true;
  };
  constexpr size_t kCandidates = 48;
  constexpr size_t kChecks = 400;
  const size_t candidateStep = std::max<size_t>(1, count / kCandidates);
  const size_t checkStep = std::max<size_t>(1, count / kChecks);
  size_t checked = 0;
  for (size_t j = 0; j < count; j += checkStep) ++checked;
  // A camera moves rigidly (R^T R = I): grass swaying in the wind shares one sheared delta, which
  // must never become the warp.
  const auto rigid = [](const std::array<double, 12>& d) {
    for (int a = 0; a < 3; ++a)
      for (int b = 0; b < 3; ++b) {
        double dot = 0.0;
        for (int r = 0; r < 3; ++r) dot += d[r * 4 + a] * d[r * 4 + b];
        if (std::fabs(dot - (a == b ? 1.0 : 0.0)) > 2e-3) return false;
      }
    return true;
  };
  const auto votesFor = [&](const std::array<double, 12>& d) {
    size_t votes = 0;
    for (size_t j = 0; j < count; j += checkStep) {
      const auto [p2, c2] = pairAt(j);
      votes += agrees(d, *p2, *c2);
    }
    return votes;
  };
  size_t bestVotes = 0;
  std::array<double, 12> best{};
  for (size_t i = 0; i < count; i += candidateStep) {
    const auto [prev, cur] = pairAt(i);
    std::array<double, 12> d{};
    if (!deltaOf(*prev, *cur, d) || !rigid(d)) continue;
    const size_t votes = votesFor(d);
    if (votes > bestVotes) {
      bestVotes = votes;
      best = d;
    }
  }
  if (bestVotes < std::max<size_t>(4, checked / 5)) return false;
  for (size_t i = 0; i < 12; ++i) out[i] = static_cast<float>(best[i]);
  return true;
}

void finalize_frame_interpolation() noexcept {
  // A frame reported late seals without inserted slots, so the encode phase renders
  // the native frame only. Its transforms still seed the next frame's matching.
  const bool skipped = s_skipInterpolationAtSeal.exchange(false, std::memory_order_acq_rel) ||
                       s_interpolationHeld.load(std::memory_order_acquire);
  if (s_dropInterpolationAtSeal.exchange(false, std::memory_order_acq_rel) || skipped) {
    if (!skipped) s_diagLateSealDrops.fetch_add(1, std::memory_order_relaxed);
    s_hasInterpolatedFrame.store(false, std::memory_order_release);
    s_pendingUniformInterpolations.clear();
    mark_unique_placements();  // the next frame blends against this one
    retire_frame_transforms();
    return;
  }
  mark_unique_placements();
  // Below this bound a direct all-pairs build is cheaper than setting up the
  // spatial grid; the produced edge set is identical either way.
  constexpr size_t kAllPairsEdgeLimit = 1024;
  // Bounds the all-pairs cost for indexed groups, which have no single translation to
  // bucket on. Non-indexed groups of any size use the grid instead.
  constexpr size_t kMaximumAssignmentEdges = 16384;
  // Hard bound for the grid path; only a swarm co-located within one cell reaches it,
  // which is the CPU-deformed shape the ordered fallback exists for.
  constexpr size_t kMaximumGridEdges = 65536;

  struct MatchEdge {
    float distance = std::numeric_limits<float>::infinity();
    size_t previous = SIZE_MAX;
    size_t current = SIZE_MAX;
  };

  // Frame-persistent matching scratch: sized to the frame's draw count, so a
  // stable scene performs no matching allocations at all after warm-up.
  static std::vector<size_t> currentToPrevious;
  static std::vector<uint8_t> previousMatched;
  static std::vector<uint8_t> currentMatched;
  static std::vector<size_t> groupCurrentIndices;
  static std::vector<size_t> groupPreviousIndices;
  static std::vector<MatchEdge> edges;
  static absl::flat_hash_map<uint64_t, std::vector<uint32_t>> gridCells;
  currentToPrevious.assign(s_currentFrameTransforms.size(), SIZE_MAX);
  previousMatched.assign(s_previousFrameTransforms.size(), 0);
  currentMatched.assign(s_currentFrameTransforms.size(), 0);

  // Quantizes a translation onto the matching grid. Cell size equals the gate distance,
  // so every acceptable pair lies within one cell along every axis.
  constexpr uint64_t kInvalidCell = std::numeric_limits<uint64_t>::max();
  const auto translation_cell = [](const Mat3x4<float>& m) noexcept -> uint64_t {
    const float x = m.m0.w();
    const float y = m.m1.w();
    const float z = m.m2.w();
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
      return kInvalidCell;
    }
    constexpr float kInverseCell = 1.0f / kMaximumTranslationPerFrame;
    constexpr int64_t kBias = int64_t{1} << 20;
    const int64_t cx = static_cast<int64_t>(std::floor(x * kInverseCell)) + kBias;
    const int64_t cy = static_cast<int64_t>(std::floor(y * kInverseCell)) + kBias;
    const int64_t cz = static_cast<int64_t>(std::floor(z * kInverseCell)) + kBias;
    // One cell of margin at each field edge so the +-1 neighbor arithmetic cannot borrow
    // into the next packed coordinate.
    if (cx < 1 || cy < 1 || cz < 1 || cx >= (kBias << 1) - 1 || cy >= (kBias << 1) - 1 ||
        cz >= (kBias << 1) - 1) {
      return kInvalidCell;
    }
    return (static_cast<uint64_t>(cx) << 42) | (static_cast<uint64_t>(cy) << 21) |
           static_cast<uint64_t>(cz);
  };

  const auto matchGroups =
      [&](const auto& currentGroups, const auto& previousGroups,
          bool allowOrderedFallback) {
    for (const auto& [signature, allCurrentIndices] : currentGroups) {
      if (allCurrentIndices.empty()) {
        continue;
      }
      const auto previousIt = previousGroups.find(signature);
      if (previousIt == previousGroups.end() || previousIt->second.empty()) {
        continue;
      }
      groupCurrentIndices.clear();
      groupPreviousIndices.clear();
      for (const size_t index : allCurrentIndices) {
        if (!currentMatched[index]) {
          groupCurrentIndices.push_back(index);
        }
      }
      for (const size_t index : previousIt->second) {
        if (!previousMatched[index]) {
          groupPreviousIndices.push_back(index);
        }
      }
      if (groupCurrentIndices.empty() || groupPreviousIndices.empty()) {
        continue;
      }

      // A unique draw has no identity ambiguity. Keep the conservative
      // interpolation fallback for malformed/non-finite matrices.
      if (groupPreviousIndices.size() == 1 && groupCurrentIndices.size() == 1) {
        currentToPrevious[groupCurrentIndices.front()] = groupPreviousIndices.front();
        previousMatched[groupPreviousIndices.front()] = 1;
        currentMatched[groupCurrentIndices.front()] = 1;
        continue;
      }

      // Drop pairs past the translation gate (`<=` also drops a NaN delta): they can only
      // render as a snap, and matching them steals a neighbor's true partner.
      const auto appendEdge = [&](size_t previousIndex, size_t currentIndex) {
        const auto& previousEntry = s_previousFrameTransforms[previousIndex];
        const auto& currentSnapshot = s_currentFrameTransforms[currentIndex].transform;
        if (!previousEntry.transform.indexedMatrices && !currentSnapshot.indexedMatrices &&
            !within_translation_gate(previousEntry.transform.position, currentSnapshot.position)) {
          return;
        }
        const float distance = snapshot_match_distance_squared(previousEntry, currentSnapshot);
        if (std::isfinite(distance)) {
          edges.push_back({
              .distance = distance,
              .previous = previousIndex,
              .current = currentIndex,
          });
        }
      };

      const auto orderedFallback = [&] {
        // GX preserves submission order for these draws, so pair the leftovers in order
        // once exact identities have claimed their instances.
        if (!allowOrderedFallback) {
          return;
        }
        const size_t pairCount = std::min(groupPreviousIndices.size(), groupCurrentIndices.size());
        for (size_t i = 0; i < pairCount; ++i) {
          // Submission order shifts when the transparent sort reorders or culling drops an
          // instance; leave over-gate pairs unmatched so they snap instead of sweeping.
          const auto& previousEntry = s_previousFrameTransforms[groupPreviousIndices[i]];
          const auto& currentEntry = s_currentFrameTransforms[groupCurrentIndices[i]];
          if (!previousEntry.transform.indexedMatrices &&
              !currentEntry.transform.indexedMatrices &&
              !within_translation_gate(previousEntry.transform.position,
                                       currentEntry.transform.position)) {
            continue;
          }
          currentToPrevious[groupCurrentIndices[i]] = groupPreviousIndices[i];
          previousMatched[groupPreviousIndices[i]] = 1;
          currentMatched[groupCurrentIndices[i]] = 1;
        }
      };

      edges.clear();
      const size_t allPairsEdges = groupPreviousIndices.size() * groupCurrentIndices.size();
      if (allPairsEdges <= kAllPairsEdgeLimit) {
        for (const size_t currentIndex : groupCurrentIndices) {
          for (const size_t previousIndex : groupPreviousIndices) {
            appendEdge(previousIndex, currentIndex);
          }
        }
      } else {
        // Grid buckets need one translation per entry, which mixed or skinned groups lack,
        // so those stay on the bounded all-pairs path.
        bool gridable = true;
        for (const size_t index : groupCurrentIndices) {
          if (s_currentFrameTransforms[index].transform.indexedMatrices) {
            gridable = false;
            break;
          }
        }
        if (gridable) {
          for (const size_t index : groupPreviousIndices) {
            if (s_previousFrameTransforms[index].transform.indexedMatrices) {
              gridable = false;
              break;
            }
          }
        }
        if (!gridable) {
          if (groupPreviousIndices.size() > kMaximumAssignmentEdges / groupCurrentIndices.size()) {
            orderedFallback();
            continue;
          }
          for (const size_t currentIndex : groupCurrentIndices) {
            for (const size_t previousIndex : groupPreviousIndices) {
              appendEdge(previousIndex, currentIndex);
            }
          }
        } else {
          // Previous entries are matched against their predicted position, so bucket them by
          // that same reference (see snapshot_match_distance_squared).
          gridCells.clear();
          for (const size_t previousIndex : groupPreviousIndices) {
            const auto& previousEntry = s_previousFrameTransforms[previousIndex];
            const Mat3x4<float>& reference = previousEntry.hasPrediction
                                                 ? previousEntry.predictedPosition
                                                 : previousEntry.transform.position;
            const uint64_t cell = translation_cell(reference);
            if (cell == kInvalidCell) {
              continue;
            }
            gridCells[cell].push_back(static_cast<uint32_t>(previousIndex));
          }
          bool overflowed = false;
          for (const size_t currentIndex : groupCurrentIndices) {
            const auto& position = s_currentFrameTransforms[currentIndex].transform.position;
            const uint64_t cell = translation_cell(position);
            if (cell == kInvalidCell) {
              continue;
            }
            for (int64_t dx = -1; dx <= 1 && !overflowed; ++dx) {
              for (int64_t dy = -1; dy <= 1 && !overflowed; ++dy) {
                for (int64_t dz = -1; dz <= 1 && !overflowed; ++dz) {
                  const uint64_t neighbor = cell + (static_cast<uint64_t>(dx) << 42) +
                                            (static_cast<uint64_t>(dy) << 21) +
                                            static_cast<uint64_t>(dz);
                  const auto cellIt = gridCells.find(neighbor);
                  if (cellIt == gridCells.end()) {
                    continue;
                  }
                  for (const uint32_t previousIndex : cellIt->second) {
                    appendEdge(previousIndex, currentIndex);
                    if (edges.size() > kMaximumGridEdges) {
                      overflowed = true;
                      break;
                    }
                  }
                }
              }
            }
            if (overflowed) {
              break;
            }
          }
          if (overflowed) {
            orderedFallback();
            continue;
          }
        }
      }

      std::sort(edges.begin(), edges.end(), [](const MatchEdge& lhs, const MatchEdge& rhs) {
        return lhs.distance < rhs.distance;
      });
      for (const auto& edge : edges) {
        if (previousMatched[edge.previous] || currentMatched[edge.current]) {
          continue;
        }
        currentToPrevious[edge.current] = edge.previous;
        previousMatched[edge.previous] = 1;
        currentMatched[edge.current] = 1;
      }
    }
  };
  matchGroups(s_currentTransformIndices, s_previousTransformIndices, false);
  matchGroups(s_currentStableTransformIndices, s_previousStableTransformIndices, true);

  s_perspectiveMatches = static_cast<uint32_t>(std::count_if(
      currentToPrevious.begin(), currentToPrevious.end(),
      [](size_t previousIndex) { return previousIndex != SIZE_MAX; }));
  // Interpolation never pauses on match quality: an unmatched draw just renders its
  // end-frame state, while a ratio gate flapped the whole output cadence instead.
  const bool eligible = frame_interpolation_fps() != 0;

  // Overlay observability: the live match ratio, and how often the scene sits in
  // low-match territory where inserted slots mostly duplicate draws.
  {
    constexpr uint32_t kLowMatchPercent = 55;
    const bool replaySafe = frame_interpolation_replay_safe();
    s_diagCandidates.store(s_perspectiveCandidates, std::memory_order_relaxed);
    s_diagMatchable.store(s_perspectiveMatchable, std::memory_order_relaxed);
    s_diagMatches.store(s_perspectiveMatches, std::memory_order_relaxed);
    s_diagEligible.store(eligible, std::memory_order_relaxed);
    s_diagReplaySafe.store(replaySafe, std::memory_order_relaxed);
    if (eligible) {
      s_diagFramesSealed.fetch_add(1, std::memory_order_relaxed);
      if (s_perspectiveMatchable != 0 &&
          s_perspectiveMatches * 100 < s_perspectiveMatchable * kLowMatchPercent) {
        s_diagFramesLowMatch.fetch_add(1, std::memory_order_relaxed);
      }
      if (!replaySafe) {
        s_diagFramesReplayUnsafe.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  // Only ordinary one-matrix draws use motion prediction. Indexed draws keep the slot
  // identity encoded by PNMTXIDX and are prepared as one coherent draw below.
  constexpr float kMaximumPredictionSeedDeltaSquared =
      kMaximumTranslationPerFrame * kMaximumTranslationPerFrame;
  for (size_t currentIndex = 0; currentIndex < currentToPrevious.size(); ++currentIndex) {
    const size_t previousIndex = currentToPrevious[currentIndex];
    if (previousIndex == SIZE_MAX) {
      continue;
    }
    auto& currentEntry = s_currentFrameTransforms[currentIndex];
    const auto& previousEntry = s_previousFrameTransforms[previousIndex];
    if (currentEntry.transform.indexedMatrices || previousEntry.transform.indexedMatrices) {
      continue;
    }
    // Seed the next frame's matching with a constant-velocity reference, but never from
    // a pair the interpolator would reject as a teleport (`<=` so NaN fails too).
    if (translation_delta_squared(previousEntry.transform.position,
                                  currentEntry.transform.position) <=
        kMaximumPredictionSeedDeltaSquared) {
      currentEntry.predictedPosition = extrapolate_transform(previousEntry.transform.position,
                                                             currentEntry.transform.position);
      currentEntry.hasPrediction = true;
    }
  }

  if (eligible) {
    // Prepare each matched pair once: every sample of a draw shares the same
    // previous/current matrices. A flat vector keeps the sample tasks parallel.
    std::vector<PreparedTransformInterpolation> preparedTransforms(s_currentFrameTransforms.size());
    std::vector<uint8_t> preparedTransformState(s_currentFrameTransforms.size(), 0);
    std::vector<PreparedAffinePair> preparedPairs;
    preparedPairs.reserve(s_pendingUniformInterpolations.size() * 2);

    const auto appendPreparedPair = [&](const Mat3x4<float>& previousPosition,
                                        const Mat3x4<float>& currentPosition,
                                        const Mat3x4<float>& previousNormal,
                                        const Mat3x4<float>& currentNormal,
                                        bool indexed) {
      const size_t pairOffset = preparedPairs.size();
      preparedPairs.push_back(indexed ? prepare_indexed_pair(previousPosition, currentPosition)
                                      : prepare_affine_pair(previousPosition, currentPosition));
      preparedPairs.push_back(indexed ? prepare_indexed_pair(previousNormal, currentNormal)
                                      : prepare_affine_pair(previousNormal, currentNormal));
      return pairOffset;
    };

    // Content-keyed palette pairing: a mesh split across draws repeats bone matrices byte
    // for byte, so a chunk with no partner of its own can borrow a sibling's.
    static std::vector<int32_t> paletteDrawIndex; // draw -> compact palette index
    static std::vector<uint32_t> paletteDraws;    // compact palette index -> draw
    paletteDrawIndex.assign(s_currentFrameTransforms.size(), -1);
    paletteDraws.clear();
    for (size_t drawIndex = 0; drawIndex < s_currentFrameTransforms.size(); ++drawIndex) {
      const auto& transform = s_currentFrameTransforms[drawIndex].transform;
      if (!transform.indexedMatrices || transform.usedMatrixMask == 0) {
        continue;
      }
      paletteDrawIndex[drawIndex] = static_cast<int32_t>(paletteDraws.size());
      paletteDraws.push_back(static_cast<uint32_t>(drawIndex));
    }

    struct PaletteSlotKey {
      HashType hash = 0;
      uint32_t palette = 0;
      uint32_t slot = 0;
    };
    struct ResolvedSlot {
      const Mat3x4<float>* position = nullptr;
      const Mat3x4<float>* normal = nullptr;
    };
    static std::vector<PaletteSlotKey> paletteSlotKeys;
    static std::vector<ResolvedSlot> resolvedSlots;
    static std::vector<size_t> resolvedProjectionEntry;
    paletteSlotKeys.clear();
    resolvedSlots.assign(paletteDraws.size() * MaxPnMtx, ResolvedSlot{});
    resolvedProjectionEntry.assign(paletteDraws.size(), kNoPreparedPair);

    for (uint32_t palette = 0; palette < paletteDraws.size(); ++palette) {
      const auto& transform = s_currentFrameTransforms[paletteDraws[palette]].transform;
      for (uint32_t slot = 0; slot < MaxPnMtx; ++slot) {
        if ((transform.usedMatrixMask & (1u << slot)) == 0) {
          continue;
        }
        paletteSlotKeys.push_back({combine_identity(transform.indexedMatrices->slotHash[slot],
                                                   transform.viewportIdentity), palette, slot});
      }
    }
    std::sort(paletteSlotKeys.begin(), paletteSlotKeys.end(),
              [](const PaletteSlotKey& lhs, const PaletteSlotKey& rhs) {
                if (lhs.hash != rhs.hash) {
                  return lhs.hash < rhs.hash;
                }
                if (lhs.palette != rhs.palette) {
                  return lhs.palette < rhs.palette;
                }
                return lhs.slot < rhs.slot;
              });

    for (size_t runStart = 0; runStart < paletteSlotKeys.size();) {
      size_t runEnd = runStart + 1;
      while (runEnd < paletteSlotKeys.size() &&
             paletteSlotKeys[runEnd].hash == paletteSlotKeys[runStart].hash) {
        ++runEnd;
      }
      // Where this matrix was last frame, per the draws that did match. Partners that
      // disagree mean no single previous pose, so the coupled unit duplicates.
      const Mat3x4<float>* sourcePosition = nullptr;
      const Mat3x4<float>* sourceNormal = nullptr;
      size_t sourceEntry = kNoPreparedPair;
      HashType sourceHash = 0;
      bool ambiguous = false;
      for (size_t keyIndex = runStart; keyIndex < runEnd && !ambiguous; ++keyIndex) {
        const uint32_t drawIndex = paletteDraws[paletteSlotKeys[keyIndex].palette];
        const size_t previousIndex = currentToPrevious[drawIndex];
        if (previousIndex >= s_previousFrameTransforms.size()) {
          continue;
        }
        const auto& current = s_currentFrameTransforms[drawIndex].transform;
        const auto& previous = s_previousFrameTransforms[previousIndex].transform;
        // A slot index is an absolute palette address, which is why it pairs matched draws
        // and why a changed layout invalidates every slot as a source.
        if (!previous.indexedMatrices || previous.usedMatrixMask != current.usedMatrixMask) {
          continue;
        }
        const uint32_t slot = paletteSlotKeys[keyIndex].slot;
        const HashType candidateHash = previous.indexedMatrices->slotHash[slot];
        if (sourcePosition == nullptr) {
          sourcePosition = &previous.indexedMatrices->position[slot];
          sourceNormal = &previous.indexedMatrices->normal[slot];
          sourceHash = candidateHash;
          sourceEntry = previousIndex;
        } else if (candidateHash != sourceHash) {
          ambiguous = true;
        }
      }
      if (ambiguous || sourcePosition == nullptr) {
        runStart = runEnd;
        continue;
      }
      for (size_t keyIndex = runStart; keyIndex < runEnd; ++keyIndex) {
        const auto& key = paletteSlotKeys[keyIndex];
        resolvedSlots[static_cast<size_t>(key.palette) * MaxPnMtx + key.slot] = {sourcePosition,
                                                                                sourceNormal};
        if (resolvedProjectionEntry[key.palette] == kNoPreparedPair) {
          resolvedProjectionEntry[key.palette] = sourceEntry;
        }
      }
      runStart = runEnd;
    }

    // 60 FPS Lite: which draws moved by themselves (not only with the camera). Those are redrawn
    // with the half-way uniforms; the rest is the warped picture. Room models use matrix palettes
    // too, so a skinned draw is judged slot by slot like a plain one.
    // The camera's motion is measured from the draws themselves: every world-still matrix pair
    // shares delta = prev * cur^-1 (= V_prev * V_cur^-1), so the delta most pairs agree on wins.
    // The game's look-at view did not match the drawn one closely enough to judge by.
    std::array<float, 12> cameraDelta{};
    bool liteClassify = gfx::frame_gen_lite::enabled();
    s_liteTaskMoving.assign(liteClassify ? s_pendingUniformInterpolations.size() : 0, 0);
    std::vector<uint8_t> liteIndexedMoving(liteClassify ? s_currentFrameTransforms.size() : 0, 0);
    // A skinned draw with a palette matrix that changed on screen (the rest are fixed to it).
    std::vector<uint8_t> liteIndexedChanged(liteIndexedMoving.size(), 0);
    struct LitePair {
      const Mat3x4<float>* prev;
      const Mat3x4<float>* cur;
      size_t owner;  // transform index of a skinned draw, SIZE_MAX for a plain one
    };
    std::vector<LitePair> litePairs;
    // A world-still matrix satisfies prev = delta * cur, delta = V_prev * V_cur^-1.
    const auto movedByItself = [&](const Mat3x4<float>& prev, const Mat3x4<float>& cur) {
      const Vec4<float>* curRows[3] = {&cur.m0, &cur.m1, &cur.m2};
      const Vec4<float>* prevRows[3] = {&prev.m0, &prev.m1, &prev.m2};
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
          float v = c == 3 ? cameraDelta[r * 4 + 3] : 0.0f;
          for (int k = 0; k < 3; ++k) v += cameraDelta[r * 4 + k] * (*curRows[k])[c];
          const float tolerance = c == 3 ? 3.0f : 2e-3f;
          if (std::fabs(v - (*prevRows[r])[c]) > tolerance) return true;
        }
      }
      return false;
    };
    // Fixed to the screen: the same matrix as the frame before, give or take float noise (far
    // inside movedByItself's tolerances, so a world-still draw under a moving camera is not it).
    const auto sameOnScreen = [](const Mat3x4<float>& prev, const Mat3x4<float>& cur) {
      const Vec4<float>* curRows[3] = {&cur.m0, &cur.m1, &cur.m2};
      const Vec4<float>* prevRows[3] = {&prev.m0, &prev.m1, &prev.m2};
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
          if (std::fabs((*curRows[r])[c] - (*prevRows[r])[c]) > (c == 3 ? 0.5f : 5e-4f)) return false;
        }
      }
      return true;
    };
    const auto classifyLite = [&](const PendingUniformInterpolation& task,
                                  const PreparedTransformInterpolation& prepared) {
      if (!liteClassify || s_liteTaskMoving.empty()) {
        return;
      }
      const size_t index = static_cast<size_t>(&task - s_pendingUniformInterpolations.data());
      if (task.indexedMatrices) {
        // Unpaired: the copy keeps the frame's own pose, redrawn there rather than moved.
        // Fixed: the HUD's 3D parts (RE4's life ring: palette draws in the scene's projection).
        s_liteTaskMoving[index] = !prepared.indexedValid                              ? kLiteUnpaired
                                  : !liteIndexedMoving[task.currentTransformIndex]    ? kLiteStill
                                  : liteIndexedChanged[task.currentTransformIndex]    ? kLiteMoving
                                                                                      : kLiteFixed;
        return;
      }
      if (prepared.nonIndexedPairOffset == kNoPreparedPair ||
          prepared.previousProjectionEntry >= s_previousFrameTransforms.size()) {
        // Unpaired: like a skinned one, it keeps its own place in the copy and is redrawn there
        // (warped with the camera it left pieces of Ashley behind).
        s_liteTaskMoving[index] = kLiteUnpaired;
        return;
      }
      const auto& previous = s_previousFrameTransforms[prepared.previousProjectionEntry].transform.position;
      const auto& current = s_currentFrameTransforms[task.currentTransformIndex].transform.position;
      s_liteTaskMoving[index] = !movedByItself(previous, current) ? kLiteStill
                                : sameOnScreen(previous, current) ? kLiteFixed
                                                                  : kLiteMoving;
    };

    for (const auto& task : s_pendingUniformInterpolations) {
      if (task.currentTransformIndex >= preparedTransformState.size() ||
          preparedTransformState[task.currentTransformIndex] != 0) {
        continue;
      }
      preparedTransformState[task.currentTransformIndex] = 1;

      const auto& current = s_currentFrameTransforms[task.currentTransformIndex].transform;
      auto& prepared = preparedTransforms[task.currentTransformIndex];
      if (task.indexedMatrices) {
        const int32_t palette = paletteDrawIndex[task.currentTransformIndex];
        if (palette < 0) {
          continue;
        }
        prepared.previousProjectionEntry = resolvedProjectionEntry[palette];
        // A palette is one deformation unit: interpolating only the resolved slots cracks
        // the mesh, so any unresolved slot duplicates the whole current draw.
        bool allSlotsValid = true;
        for (size_t slot = 0; slot < MaxPnMtx; ++slot) {
          if ((current.usedMatrixMask & (1u << slot)) == 0) {
            continue;
          }
          const auto& resolved = resolvedSlots[static_cast<size_t>(palette) * MaxPnMtx + slot];
          if (resolved.position == nullptr) {
            allSlotsValid = false;
            break;
          }
          if (liteClassify) {
            litePairs.push_back({resolved.position, &current.indexedMatrices->position[slot], task.currentTransformIndex});
          }
          const size_t pairOffset =
              appendPreparedPair(*resolved.position, current.indexedMatrices->position[slot],
                                 *resolved.normal, current.indexedMatrices->normal[slot], true);
          prepared.indexedPairOffsets[slot] = pairOffset;
          if (!preparedPairs[pairOffset].valid || !preparedPairs[pairOffset + 1].valid) {
            allSlotsValid = false;
            break;
          }
        }
        prepared.indexedValid = allSlotsValid;
      } else {
        const size_t previousTransformIndex = currentToPrevious[task.currentTransformIndex];
        if (previousTransformIndex >= s_previousFrameTransforms.size()) {
          continue;
        }
        const auto& previous = s_previousFrameTransforms[previousTransformIndex].transform;
        prepared.previousProjectionEntry = previousTransformIndex;
        prepared.nonIndexedPairOffset = appendPreparedPair(
            previous.position, current.position, previous.normal, current.normal, false);
        if (liteClassify) {
          litePairs.push_back({&previous.position, &current.position, SIZE_MAX});
        }
      }
    }

    if (liteClassify) {
      liteClassify = vote_camera_delta(litePairs.size(), [&](size_t i) { return std::pair{litePairs[i].prev, litePairs[i].cur}; },
                                       cameraDelta);
      gfx::frame_gen_lite::set_frame_delta(liteClassify ? cameraDelta.data() : nullptr);
      if (liteClassify) {
        for (const auto& pair : litePairs) {
          if (pair.owner == SIZE_MAX) {
            continue;
          }
          if (!liteIndexedMoving[pair.owner] && movedByItself(*pair.prev, *pair.cur)) {
            liteIndexedMoving[pair.owner] = 1;
          }
          if (!liteIndexedChanged[pair.owner] && !sameOnScreen(*pair.prev, *pair.cur)) {
            liteIndexedChanged[pair.owner] = 1;
          }
        }
      } else {
        s_liteTaskMoving.clear();
      }
    }

    const auto interpolatePendingUniform = [&](const auto& task) {
      if (task.currentTransformIndex >= s_currentFrameTransforms.size()) {
        return;
      }

      const auto& current = s_currentFrameTransforms[task.currentTransformIndex].transform;
      std::memcpy(task.uniformData, task.sourceUniformData, task.uniformSize);
      const auto& prepared = preparedTransforms[task.currentTransformIndex];
      // Before the early-outs: an unpaired skinned draw keeps its own pose in the copy (memcpy
      // above), and Lite still redraws it there rather than moving it with the camera.
      classifyLite(task, prepared);
      if (task.indexedMatrices && !prepared.indexedValid) {
        return;
      }
      // The projection comes from whichever previous entry supplied the transforms, which
      // for a borrowed palette is a sibling's partner.
      const size_t previousTransformIndex = prepared.previousProjectionEntry;
      if (previousTransformIndex >= s_previousFrameTransforms.size()) {
        return;
      }
      const auto& previous = s_previousFrameTransforms[previousTransformIndex].transform;
      const float weight =
          static_cast<float>(task.numerator) / static_cast<float>(task.denominator);
      const auto interpolatedProjection =
          interpolate_projection(previous.projection, current.projection, weight);
      std::memcpy(task.uniformData + task.projectionOffset, &interpolatedProjection,
                  sizeof(interpolatedProjection));
      blend_uniform_values(s_currentFrameTransforms[task.currentTransformIndex],
                           s_previousFrameTransforms[previousTransformIndex], weight, task.uniformData);

      const auto interpolateMatrixSlot = [&](size_t currentIndex, size_t pairOffset) {
        if (pairOffset == kNoPreparedPair) {
          return;
        }
        Mat3x4<float> interpolatedPosition{};
        Mat3x4<float> interpolatedNormal{};
        evaluate_affine_pair(preparedPairs[pairOffset], weight, interpolatedPosition);
        evaluate_affine_pair(preparedPairs[pairOffset + 1], weight, interpolatedNormal);
        std::memcpy(task.uniformData + task.positionOffset + currentIndex * sizeof(Mat3x4<float>),
                    &interpolatedPosition, sizeof(interpolatedPosition));
        std::memcpy(task.uniformData + task.normalOffset + currentIndex * sizeof(Mat3x4<float>),
                    &interpolatedNormal, sizeof(interpolatedNormal));
      };
      if (task.indexedMatrices) {
        if (!current.indexedMatrices) {
          return;
        }
        for (size_t currentIndex = 0; currentIndex < MaxPnMtx; ++currentIndex) {
          if ((current.usedMatrixMask & (1u << currentIndex)) == 0) {
            continue;
          }
          interpolateMatrixSlot(currentIndex, prepared.indexedPairOffsets[currentIndex]);
        }
      } else {
        interpolateMatrixSlot(task.currentMatrix, prepared.nonIndexedPairOffset);
      }
    };
    // A handful of tasks costs more to schedule than to run. The pool stands in for
    // std::execution::par, which libc++ does not provide at all.
    constexpr size_t kMinimumParallelInterpolationTasks = 64;
    if (s_pendingUniformInterpolations.size() < kMinimumParallelInterpolationTasks) {
      std::for_each(s_pendingUniformInterpolations.begin(), s_pendingUniformInterpolations.end(),
                    interpolatePendingUniform);
    } else {
      InterpolationWorkerPool::instance().run(
          s_pendingUniformInterpolations.size(),
          [&](size_t index) { interpolatePendingUniform(s_pendingUniformInterpolations[index]); });
    }
  }
  s_hasInterpolatedFrame.store(eligible, std::memory_order_release);
  s_liteMovingRanges.clear();
  // The frame's trailing run of screen-fixed (or unpaired) draws is the HUD: the scene ends before
  // it, and Lite replays it as is over the in-between picture. Warped with the scene's depth its
  // glass and icons smeared, and the picture under them cannot be filled in.
  const size_t liteTasks = std::min(s_liteTaskMoving.size(), s_pendingUniformInterpolations.size());
  size_t liteTail = liteTasks;
  while (liteTail > 0 && s_liteTaskMoving[liteTail - 1] >= kLiteUnpaired) {
    --liteTail;
  }
  s_liteTailOffset = liteTail < liteTasks ? s_pendingUniformInterpolations[liteTail].rangeOffset : UINT32_MAX;
  // A draw's current matrices, hashed: the passes of one object (RE4 draws characters, guns and
  // doors as a solid pass plus blended layers without a depth write) share them.
  const auto matrixKey = [](const PendingUniformInterpolation& task) -> uint64_t {
    if (task.currentTransformIndex >= s_currentFrameTransforms.size()) return 0;
    const auto& t = s_currentFrameTransforms[task.currentTransformIndex].transform;
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](const void* data, size_t size) {
      const auto* b = static_cast<const uint8_t*>(data);
      for (size_t k = 0; k < size; ++k) h = (h ^ b[k]) * 1099511628211ull;
    };
    if (task.indexedMatrices) {
      if (!t.indexedMatrices) return 0;
      for (size_t slot = 0; slot < MaxPnMtx; ++slot) {
        if (t.usedMatrixMask & (1u << slot)) mix(&t.indexedMatrices->position[slot], sizeof(Mat3x4<float>));
      }
    } else {
      mix(&t.position, sizeof(Mat3x4<float>));
    }
    return h;
  };
  // Overlays are effects (fog sheets, lasers, particles, muzzle flashes): left in the warped picture.
  // Except a blended layer of a self-moving object: left behind while its solid pass was redrawn
  // half-way, it showed every character, gun and opening door double.
  std::vector<uint64_t> liteSolidKeys;
  for (size_t i = 0; i < liteTail; ++i) {
    if (s_liteTaskMoving[i] != kLiteStill && !s_pendingUniformInterpolations[i].overlay) {
      liteSolidKeys.push_back(matrixKey(s_pendingUniformInterpolations[i]));
    }
  }
  std::sort(liteSolidKeys.begin(), liteSolidKeys.end());
  // AURORA_LITE_ALL=1 (research): every scene draw redrawn half-way, the warp only underneath.
  static const bool s_liteAll = [] {
    const char* v = std::getenv("AURORA_LITE_ALL");
    return v && *v == '1';
  }();
  for (size_t i = 0; i < liteTail; ++i) {
    const auto& task = s_pendingUniformInterpolations[i];
    if (s_liteAll) {
      s_liteMovingRanges.push_back(task.rangeOffset);
      continue;
    }
    if (s_liteTaskMoving[i] == kLiteStill) continue;
    if (task.overlay && !std::binary_search(liteSolidKeys.begin(), liteSolidKeys.end(), matrixKey(task))) continue;
    s_liteMovingRanges.push_back(task.rangeOffset);
  }
  static const bool s_liteTrace = [] {
    const char* v = std::getenv("AURORA_LITE_TRACE");
    return v && *v == '1';
  }();
  static uint32_t s_liteTraceTick = 0;
  if (s_liteTrace && !s_liteTaskMoving.empty() && ++s_liteTraceTick % 300 == 0) {
    std::string classes;  // of the last tasks: 0 still, 1 moving, 2 unpaired, 3 fixed
    for (size_t i = liteTasks > 64 ? liteTasks - 64 : 0; i < liteTasks; ++i) {
      classes.push_back(static_cast<char>('0' + s_liteTaskMoving[i]));
    }
    // Per class (still, moving, unpaired, fixed): skinned draws, and blended draws without a depth write.
    std::array<size_t, 4> skinned{}, overlays{};
    for (size_t i = 0; i < liteTail; ++i) {
      const auto& task = s_pendingUniformInterpolations[i];
      if (task.indexedMatrices) ++skinned[s_liteTaskMoving[i] & 3];
      if (task.overlay) ++overlays[s_liteTaskMoving[i] & 3];
    }
    const auto st = gfx::frame_gen_lite::stats();
    Log.info("lite classify: {} tasks, {} self-moving, {} in the screen-fixed tail, skinned s/m/u/f {}/{}/{}/{}, "
             "overlay s/m/u/f {}/{}/{}/{}, warped {} repeated {}, last {}",
             s_liteTaskMoving.size(), s_liteMovingRanges.size(), liteTasks - liteTail, skinned[0], skinned[1],
             skinned[2], skinned[3], overlays[0], overlays[1], overlays[2], overlays[3], st.warped, st.repeated,
             classes);
  }
  s_liteTaskMoving.clear();
  std::sort(s_liteMovingRanges.begin(), s_liteMovingRanges.end());
  s_pendingUniformInterpolations.clear();
  // The index maps are emptied node-preservingly by the next
  // begin_frame_interpolation.
  retire_frame_transforms();
}

void get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics& diagnostics) noexcept {
  diagnostics.targetFps = frame_interpolation_fps();
  diagnostics.targetSamples = std::min(s_interpolationSampleTarget.load(std::memory_order_acquire),
                                       maximum_interpolation_samples());
  diagnostics.activeSamples = s_activeInterpolationSamples.load(std::memory_order_acquire);
  diagnostics.candidates = s_diagCandidates.load(std::memory_order_relaxed);
  diagnostics.matchable = s_diagMatchable.load(std::memory_order_relaxed);
  diagnostics.matches = s_diagMatches.load(std::memory_order_relaxed);
  diagnostics.eligible = s_diagEligible.load(std::memory_order_relaxed) ? 1 : 0;
  diagnostics.replaySafe = s_diagReplaySafe.load(std::memory_order_relaxed) ? 1 : 0;
  diagnostics.framesSealed = s_diagFramesSealed.load(std::memory_order_relaxed);
  diagnostics.framesLowMatch = s_diagFramesLowMatch.load(std::memory_order_relaxed);
  diagnostics.framesReplayUnsafe = s_diagFramesReplayUnsafe.load(std::memory_order_relaxed);
  diagnostics.slotReductions = s_diagSlotReductions.load(std::memory_order_relaxed);
  diagnostics.lateSealDrops = s_diagLateSealDrops.load(std::memory_order_relaxed);
  diagnostics.blendDraws = s_diagBlendDraws.load(std::memory_order_relaxed);
  diagnostics.blendJumps = s_diagBlendJumps.load(std::memory_order_relaxed);
}

bool frame_interpolation_blend_enabled() noexcept {
  static const bool enabled = [] {
    const char* v = std::getenv("AURORA_INTERP_BLEND");
    return v == nullptr || v[0] != '0';
  }();
  return enabled;
}

bool has_interpolated_frame() noexcept {
  return s_hasInterpolatedFrame.load(std::memory_order_acquire);
}

uint32_t interpolated_frame_count() noexcept {
  if (!has_interpolated_frame()) {
    return 0;
  }
  // The count latched when this frame began recording, not the configured maximum:
  // build_uniform staged exactly this many ranges for every draw.
  return s_activeInterpolationSamples.load(std::memory_order_acquire);
}

void drop_pending_frame_interpolation_uniforms() noexcept {
  // Pending tasks hold raw pointers into the mapped uniform staging range, so anything
  // that unmaps or rotates that buffer first has to drop the tasks.
  s_pendingUniformInterpolations.clear();
}

void mark_frame_interpolation_replay_unsafe() noexcept {
  s_frameInterpolationReplaySafe.store(false, std::memory_order_release);
  // Marking the frame unsafe rotates the staging buffer mid-frame, and the seal then
  // duplicates slots instead of replaying, so drop the copies staged so far.
  drop_pending_frame_interpolation_uniforms();
}

bool frame_interpolation_replay_safe() noexcept {
  return s_frameInterpolationReplaySafe.load(std::memory_order_acquire);
}

void extend_interpolation_draw(uint16_t usedPnMtxMask) noexcept {
  if (s_currentFrameTransforms.empty()) {
    return;
  }
  auto& snapshot = s_currentFrameTransforms.back().transform;
  if (!snapshot.indexedMatrices) {
    // One-matrix draws all read the current matrix index, which a merge cannot
    // have changed either.
    return;
  }
  const uint16_t addedSlots = static_cast<uint16_t>(usedPnMtxMask & ~snapshot.usedMatrixMask);
  if (addedSlots == 0) {
    return;
  }
  for (size_t slot = 0; slot < MaxPnMtx; ++slot) {
    if ((addedSlots & (1u << slot)) == 0) {
      continue;
    }
    snapshot.indexedMatrices->position[slot] = g_gxState.pnMtx[slot].pos;
    snapshot.indexedMatrices->normal[slot] = g_gxState.pnMtx[slot].nrm;
    snapshot.indexedMatrices->slotHash[slot] =
        xxh3_hash_s(&g_gxState.pnMtx[slot].pos, sizeof(Mat3x4<float>),
                    xxh3_hash_s(&g_gxState.pnMtx[slot].nrm, sizeof(Mat3x4<float>)));
  }
  snapshot.usedMatrixMask |= addedSlots;
}

std::array<gfx::Range, MaxInterpolatedFrames> record_interpolation_draw(
    const FrameInterpolationDrawIdentity& drawIdentity, const Mat4x4<float>& projection,
    uint16_t usedPnMtxMask, const InterpolatedUniformLayout& uniformLayout) noexcept {
  // Split-screen cameras can draw identical meshes in unrelated view spaces.
  // Scope exact, material-only and sibling-palette history to the guest viewport.
  // Logical coordinates keep render-scale changes out of the camera identity.
  const auto& viewport = g_gxState.logicalViewport;
  const std::array viewportValues{viewport.left, viewport.top, viewport.width,
                                  viewport.height, viewport.znear, viewport.zfar};
  const HashType viewportIdentity = xxh3_hash_s(viewportValues.data(), sizeof(viewportValues));
  auto identity = drawIdentity;
  identity.combined = combine_identity(identity.combined, viewportIdentity);
  identity.pipeline = combine_identity(identity.pipeline, viewportIdentity);
  FrameTransformSnapshot snapshot{
      .projection = projection,
      .viewportIdentity = viewportIdentity,
      .usedMatrixMask = usedPnMtxMask,
  };
  if (uniformLayout.indexedMatrices) {
    snapshot.indexedMatrices = acquire_indexed_matrices();
    // Only the used slots are copied; consumers mask with usedMatrixMask, so the stale
    // pool contents of unused slots are never read.
    for (size_t i = 0; i < MaxPnMtx; ++i) {
      if ((usedPnMtxMask & (1u << i)) == 0) {
        continue;
      }
      snapshot.indexedMatrices->position[i] = g_gxState.pnMtx[i].pos;
      snapshot.indexedMatrices->normal[i] = g_gxState.pnMtx[i].nrm;
      snapshot.indexedMatrices->slotHash[i] =
          xxh3_hash_s(&g_gxState.pnMtx[i].pos, sizeof(Mat3x4<float>),
                      xxh3_hash_s(&g_gxState.pnMtx[i].nrm, sizeof(Mat3x4<float>)));
    }
  } else {
    const size_t currentMatrix = std::min<size_t>(g_gxState.currentPnMtx, MaxPnMtx - 1);
    snapshot.position = g_gxState.pnMtx[currentMatrix].pos;
    snapshot.normal = g_gxState.pnMtx[currentMatrix].nrm;
  }

  const size_t currentTransformIndex = s_currentFrameTransforms.size();
  s_currentFrameTransforms.emplace_back(FrameTransformEntry{
      .identity = identity,
      .transform = std::move(snapshot),
  });
  if (const InterpolationBlendCapture* blend = uniformLayout.blend) {
    auto& entry = s_currentFrameTransforms.back();
    entry.blendRanges = blend->ranges;
    entry.blendCount = blend->count;
    entry.blendValueOffset = static_cast<uint32_t>(s_currentBlendValues.size());
    const auto& placed = entry.transform.indexedMatrices
                             ? entry.transform.indexedMatrices->position[std::countr_zero(
                                   static_cast<uint32_t>(std::max<uint16_t>(entry.transform.usedMatrixMask, 1)))]
                             : entry.transform.position;
    entry.placementKey = xxh3_hash_s(&placed, sizeof(placed), identity.combined);
    s_currentBlendValues.insert(s_currentBlendValues.end(), blend->values.begin(),
                                blend->values.begin() + blend->floats);
  }
  s_currentTransformIndices[identity.combined].push_back(currentTransformIndex);
  const HashType stableIdentity = stable_identity(identity);
  s_currentStableTransformIndices[stableIdentity].push_back(currentTransformIndex);

  std::array<gfx::Range, MaxInterpolatedFrames> interpolatedRanges{};
  ++s_perspectiveCandidates;
  const auto exactPrevious = s_previousTransformIndices.find(identity.combined);
  const auto stablePrevious = s_previousStableTransformIndices.find(stableIdentity);
  const bool hasPreviousPartner =
      (exactPrevious != s_previousTransformIndices.end() && !exactPrevious->second.empty()) ||
      (stablePrevious != s_previousStableTransformIndices.end() && !stablePrevious->second.empty());
  if (hasPreviousPartner) {
    ++s_perspectiveMatchable;
  }
  // A skinned draw with no identity partner can still borrow sibling transforms at
  // seal time, so stage copies whenever the previous frame held any palette.
  const bool stageInterpolation =
      hasPreviousPartner ||
      (uniformLayout.indexedMatrices && s_previousFrameHasIndexedMatrices);
  // A frame already split by a submitted prefix duplicates its slots instead of
  // replaying, so staging copies for the resumed suffix would only waste space.
  if (frame_interpolation_replay_safe() && stageInterpolation) {
    const uint32_t sampleCount = s_activeInterpolationSamples.load(std::memory_order_acquire);
    for (uint32_t sample = 0; sample < sampleCount; ++sample) {
      auto [interpolatedBuffer, interpolatedRange] = gfx::map_uniform(uniformLayout.uniformSize);
      s_pendingUniformInterpolations.push_back({
          .currentTransformIndex = currentTransformIndex,
          .sourceUniformData = uniformLayout.sourceUniformData,
          .uniformData = interpolatedBuffer.data(),
          .uniformSize = uniformLayout.uniformSize,
          .projectionOffset = uniformLayout.projectionOffset,
          .positionOffset = uniformLayout.positionOffset,
          .normalOffset = uniformLayout.normalOffset,
          .currentMatrix = uniformLayout.currentMatrix,
          .numerator = sample + 1,
          .denominator = sampleCount + 1,
          .indexedMatrices = uniformLayout.indexedMatrices,
          .overlay = g_gxState.blendMode != GX_BM_NONE && !g_gxState.depthUpdate,
          .rangeOffset = interpolatedRange.offset,
      });
      interpolatedRanges[sample] = interpolatedRange;
    }
  }
  return interpolatedRanges;
}
void take_lite_moving_ranges(std::vector<uint32_t>& out, uint32_t& tailOffset) noexcept {
  out.clear();
  out.swap(s_liteMovingRanges);
  tailOffset = s_liteTailOffset;
  s_liteTailOffset = UINT32_MAX;
}
} // namespace aurora::gx
