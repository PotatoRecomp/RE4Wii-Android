#include "crosshair.h"

#include "pb_mode.h"

#include "abi_bridge.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include <aurora/gfx.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Settings > Display > Crosshair. cObjWep::drawLaserSight calls EspDrawLaserLine (Wii 0x800D9318,
// r3 = &lpos, r4 = &lcross, the point the shot hits) once a frame while the laser sight is shown. The
// wrapper runs it, then hands lcross to the game's own GetScreenPos (0x8015A704: GXProject with the
// camera's view matrix pG+0xA4, its projection and the viewport, so the game's Hor+ widening is in
// it), which gives the point in EFB units. The overlay maps that through the display copy's EFB
// rectangle and the picture's place on the surface (aurora_get_picture_map), so the bars, Fill
// Screen's crop and Display Resolution are all accounted for.
extern "C" void func_8015A704(CpuContext* ctx);  // GetScreenPos(Vec* pos, Vec* scr)

namespace Crosshair {
namespace {
std::atomic<int> g_style{kOff};
std::atomic<int> g_size{100};
std::atomic<float> g_efbX{0.0f}, g_efbY{0.0f};
std::atomic<int64_t> g_seenNanos{0};  // steady clock of the last laser frame, 0 = none

int64_t NowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

float FloatBits(uint32_t w) {
    float f;
    std::memcpy(&f, &w, sizeof f);
    return f;
}

void EnvDefaults() {
    static const bool s_done = [] {
        // RE4_CROSSHAIR=1 (+) or 2 (dot), RE4_CROSSHAIR_SIZE=50..200 (Windows tests).
        if (const char* e = std::getenv("RE4_CROSSHAIR")) g_style.store(std::clamp(std::atoi(e), 0, 2));
        if (const char* e = std::getenv("RE4_CROSSHAIR_SIZE")) g_size.store(std::clamp(std::atoi(e), 50, 200));
        return true;
    }();
    (void)s_done;
}
} // namespace

void Configure(int style, int sizePercent) {
    g_style.store(std::clamp(style, 0, 2), std::memory_order_relaxed);
    g_size.store(std::clamp(sizePercent, 50, 200), std::memory_order_relaxed);
}

void OnLaser(CpuContext* ctx, uint32_t cross) {
    EnvDefaults();
    if (g_style.load(std::memory_order_relaxed) == kOff || !Memory::Contains(cross, 12)) return;
    // GetScreenPos(&lcross, &scr) on a frame of our own below the caller's stack. EspDrawLaserLine has
    // returned, so the volatile registers are free; r1 and lr are put back.
    const uint32_t sp = ctx->gpr[1];
    const uint32_t lr = ctx->lr;
    const uint32_t frame = (sp - 0x40u) & ~0xFu;
    const uint32_t scr = frame + 0x20u;
    ctx->gpr[1] = frame;
    ctx->gpr[3] = cross;
    ctx->gpr[4] = scr;
    func_8015A704(ctx);
    const bool front = ctx->gpr[3] != 0;
    ctx->gpr[1] = sp;
    ctx->lr = lr;
    uint32_t x = 0, y = 0;
    if (!front || !Memory::TryRead32(scr, x) || !Memory::TryRead32(scr + 4u, y)) return;
    const float fx = FloatBits(x), fy = FloatBits(y);
    if (!std::isfinite(fx) || !std::isfinite(fy)) return;
    g_efbX.store(fx, std::memory_order_relaxed);
    g_efbY.store(fy, std::memory_order_relaxed);
    g_seenNanos.store(NowNanos(), std::memory_order_relaxed);
}

void Draw() {
    const int style = g_style.load(std::memory_order_relaxed);
    const int64_t seen = g_seenNanos.load(std::memory_order_relaxed);
    if (style == kOff || seen == 0 || NowNanos() - seen > 150000000) return;  // no laser for 150 ms
    float map[10];
    aurora_get_picture_map(map);
    if (map[2] <= 0.0f || map[3] <= 0.0f || map[6] <= 0.0f || map[7] <= 0.0f || map[8] <= 0.0f) return;
    const float u = (g_efbX.load(std::memory_order_relaxed) - map[0]) / map[2];
    const float v = (g_efbY.load(std::memory_order_relaxed) - map[1]) / map[3];
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float scale = io.DisplaySize.x / map[8];
    const ImVec2 c((map[4] + u * map[6]) * scale, (map[5] + v * map[7]) * scale);
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        RT_LOGF(RT_TAG_RUNTIME, "crosshair: efb %.1f,%.1f in copy %.0f,%.0f %.0fx%.0f -> screen %.0f,%.0f of %.0fx%.0f\n",
                g_efbX.load(), g_efbY.load(), map[0], map[1], map[2], map[3], c.x, c.y, io.DisplaySize.x,
                io.DisplaySize.y);
    }
    const float s = io.DisplaySize.y * 0.03f * g_size.load(std::memory_order_relaxed) / 100.0f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 white = IM_COL32(255, 255, 255, 230), shade = IM_COL32(0, 0, 0, 160);
    if (style == kDot) {
        const float r = std::max(2.0f, s * 0.18f);
        dl->AddCircleFilled(c, r + 1.5f, shade, 16);
        dl->AddCircleFilled(c, r, white, 16);
        return;
    }
    const float half = s * 0.5f, gap = s * 0.15f, t = std::max(2.0f, s * 0.08f);
    const auto arm = [&](float dx, float dy) {
        const ImVec2 a(c.x + dx * gap, c.y + dy * gap), b(c.x + dx * half, c.y + dy * half);
        dl->AddLine(a, b, shade, t + 2.0f);
        dl->AddLine(a, b, white, t);
    };
    arm(1, 0);
    arm(-1, 0);
    arm(0, 1);
    arm(0, -1);
}

} // namespace Crosshair

extern "C" void func_800D9318(CpuContext* ctx);
extern "C" void Crosshair_EspDrawLaserLine_800D9318(CpuContext* ctx) {
    const uint32_t cross = ctx->gpr[4];
    PbMode::NoteLaser(ctx->gpr[3], cross);
    func_800D9318(ctx);
    Crosshair::OnLaser(ctx, cross);
}
REGISTER_NATIVE_FUNCTION_AS(0x800D9318, Crosshair_EspDrawLaserLine_800D9318, "Crosshair_EspDrawLaserLine_800D9318");
