#include "minimap.h"

#include "memory.h"
#include "runtime_log.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

// Addresses of the Wii build (RB4E08), found in a MEM1 dump of room r100 (docs/riset-riwayat.md):
// - cManager on the Wii is {pArray, nArray, size, flag, pAlive, ?, vptr}; cUnit is {be_flag, pNext, vptr}.
// - SatMgr (the room's scenario collision pieces, 0xC0 a piece, laid out as the GameCube's cSat):
//   vertices +0x0C, polygons +0x18 (SAT_POLY, 0x14 each: three vertex indices first), counts +0x1C
//   (vertices), +0x1E (polygons), +0x20 / +0x22 / +0x24 (floor, slope, wall groups, in that order), the
//   piece-to-world matrix at +0x60.
// - EmMgr (characters, 0xE04 a work): cCoord pos +0x94, ang.y +0xA4, model id +0x100, hp +0x322, hp_max
//   +0x324, status +0x3C8 (bit 0: in battle, i.e. it has spotted Leon; the GameCube's 0x3C4 + 4, as
//   Guard_r 0x3CC -> 0x3D0). pPL / pSUB (Leon, his partner) in sbss.
// - The scenario areas (pS at 0x8043E2C8): an ordering table at pS + 0x50 (16 words, ot[15] the start),
//   each record's first word links to the next (high bit set: a record, clear: a table slot, ~0 the end).
//   Records 0x9C bytes: be_flg +0x34 (bit 0 on), type +0x35 (3 = item), item: pos +0x5C, model +0x68
//   (pos +0x94; none while the item is not in the world), item id +0x78.
// - Cam (CAMERA): Look (pos - target, normalised) +0xD0, Right +0xDC.
// - pG: room_id (stage << 8 | room) +0x4FAC, Status_flg[4] +0x501C (the GameCube offsets + 0x10).
namespace Minimap {
namespace {

constexpr uint32_t kSatMgr = 0x80328E6Cu;
constexpr uint32_t kEmMgr = 0x8032E274u;
constexpr uint32_t kPPL = 0x8043DEA0u;
constexpr uint32_t kPSUB = 0x8043DEA4u;
constexpr uint32_t kCam = 0x80332FE4u;
constexpr uint32_t kPG = 0x8043D580u;
constexpr uint32_t kRoomIdOffset = 0x4FACu;
constexpr uint32_t kStatusOffset = 0x501Cu;
constexpr uint32_t kPS = 0x8043E2C8u;
constexpr uint32_t kOtStart = 0x50u + 15u * 4u;
constexpr uint32_t kEmStatus = 0x3C8u;

// Status_flg bits the map hides for: movies, events, the sub screen (inventory, map, files), the death
// scene, the binoculars and the rifle scope.
constexpr int kHideBits[] = {3, 5, 11, 13, 19, 21, 25};

constexpr size_t kMaxEdges = 12000;
constexpr int kMaxMarkers = 64;
constexpr int kMaxItems = 96;

// MEM1 only, with room for the largest record read from it. A pointer past MEM1 (0x818xxxxx) made
// TryRead32 throw; after the catch the item array's register held garbage and ReadItems faulted on
// an Infinix (Mali-G615) at 2026-11-12 00:08. Nothing here may rely on the throw.
bool Guest(uint32_t p) {
    return p >= 0x80000000u && p < 0x81800000u - 0x1000u;
}

uint32_t U32(uint32_t addr) {
    uint32_t v = 0;
    if (Memory::Contains(addr, 4)) Memory::TryRead32(addr, v);  // never the throwing path
    return v;
}

uint16_t U16(uint32_t addr) {
    const uint32_t w = U32(addr & ~3u);
    return static_cast<uint16_t>((addr & 2u) ? w : w >> 16);
}

uint8_t U8(uint32_t addr) {
    return static_cast<uint8_t>(U32(addr & ~3u) >> (8 * (3 - (addr & 3u))));
}

float F32(uint32_t addr) {
    const uint32_t v = U32(addr);
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return std::isfinite(f) ? f : 0.0f;
}

struct Edge {
    float x0, z0, x1, z1, y;
};

enum Kind : uint8_t { kEnemy, kBoss, kAnimal, kAshley, kLuis };

struct Marker {
    float x, y, z, ang;
    float hp;  // 0..1
    Kind kind;
    bool alert;  // in battle: it has spotted Leon
};

enum ItemClass : uint8_t { kHealing, kAmmo, kOtherItem };

struct Item {
    float x, y, z;
    ItemClass cls;
};

struct Snapshot {
    bool visible = false;
    float px = 0, py = 0, pz = 0, pang = 0;
    float rightX = 1, rightZ = 0, upX = 0, upZ = -1;  // camera right and screen-up in world XZ
    int markers = 0;
    Marker marker[kMaxMarkers];
    int items = 0;
    Item item[kMaxItems];
};

std::atomic<uint32_t> g_flags{0};
std::atomic<int> g_size{30}, g_corner{kTopLeft}, g_zoom{1}, g_opacity{80};

std::mutex g_mutex;
Snapshot g_shot;
std::vector<Edge> g_edges;          // guarded by g_mutex: the drawn copy
std::vector<Edge> g_build;          // guest thread only
uint32_t g_room = 0xFFFFFFFFu;
uint64_t g_signature = 0;

Kind KindOf(uint8_t id) {
    switch (id) {
    case 0x03: return kAshley;
    case 0x04: return kLuis;
    case 0x23: case 0x24: case 0x26: case 0x27: case 0x28: return kAnimal;  // crow, snake, cow, bass, chicken
    case 0x2B: case 0x2C: case 0x30: case 0x32: case 0x33: case 0x34: case 0x35:
    case 0x37: case 0x38: case 0x39: case 0x3A: return kBoss;
    default: return kEnemy;
    }
}

// Characters the map shows: Ashley and Luis, the enemy ids 0x10..0x3F (not the vehicles, the bear trap
// or the helicopter), alive with hit points left.
bool Shown(uint8_t id) {
    if (id == 0x03 || id == 0x04) return true;
    return id >= 0x10 && id <= 0x3F && id != 0x2A && id != 0x3D;
}

uint64_t Mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

// What the room's collision is made of now: its pieces, their sizes, flags and places.
uint64_t SatSignature() {
    uint64_t h = 0;
    int n = 0;
    for (uint32_t p = U32(kSatMgr + 0x10); Guest(p) && n < 64; p = U32(p + 4), ++n) {
        h = Mix(h, p);
        h = Mix(h, U32(p + 0x1C));
        h = Mix(h, U8(p + 0x2A));
        h = Mix(h, U32(p + 0x6C));
        h = Mix(h, U32(p + 0x8C));
    }
    return h;
}

struct Vec3 {
    float x, y, z;
};

Vec3 Transform(const float m[12], const Vec3& v) {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3], m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7],
            m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11]};
}

// The outline: every edge of the floor and slope triangles that only one of them has, then the foot of
// each standing wall (walls, pillars and closed doors on a floor that goes on under them).
void BuildOutline() {
    g_build.clear();
    std::vector<Vec3> verts;
    std::unordered_map<uint32_t, int> edgeCount;
    std::vector<uint32_t> keys;
    int pieces = 0;
    for (uint32_t p = U32(kSatMgr + 0x10); Guest(p) && pieces < 64; p = U32(p + 4), ++pieces) {
        if ((U32(p) & 1u) == 0) continue;
        const uint32_t vtx = U32(p + 0x0C), poly = U32(p + 0x18);
        const uint32_t nv = U16(p + 0x1C), np = U16(p + 0x1E);
        const uint32_t nFloor = U16(p + 0x20), nSlope = U16(p + 0x22), nWall = U16(p + 0x24);
        if (!Guest(vtx) || !Guest(poly) || nv == 0 || nv > 0x4000 || np == 0 || np > 0x2000 ||
            nFloor + nSlope + nWall > np) {
            continue;
        }
        float m[12];
        for (int i = 0; i < 12; ++i) m[i] = F32(p + 0x60 + 4 * i);
        verts.resize(nv);
        for (uint32_t i = 0; i < nv; ++i) {
            verts[i] = Transform(m, {F32(vtx + 12 * i), F32(vtx + 12 * i + 4), F32(vtx + 12 * i + 8)});
        }
        const auto index = [&](uint32_t poly_i, int k) -> uint32_t {
            return U16(poly + 0x14 * poly_i + 2 * k);
        };
        edgeCount.clear();
        keys.clear();
        const uint32_t ground = nFloor + nSlope;
        for (uint32_t t = 0; t < ground; ++t) {
            for (int k = 0; k < 3; ++k) {
                uint32_t a = index(t, k), b = index(t, (k + 1) % 3);
                if (a >= nv || b >= nv) continue;
                if (a > b) std::swap(a, b);
                const uint32_t key = a << 16 | b;
                if (edgeCount[key]++ == 0) keys.push_back(key);
            }
        }
        for (const uint32_t key : keys) {
            if (edgeCount[key] != 1 || g_build.size() >= kMaxEdges) continue;
            const Vec3& a = verts[key >> 16];
            const Vec3& b = verts[key & 0xFFFFu];
            g_build.push_back({a.x, a.z, b.x, b.z, std::min(a.y, b.y)});
        }
        // Wall feet: steep triangles only (sloped roofs and ceilings in the wall group would cross the map),
        // the longest edge with both ends near the lowest point, once per line.
        const size_t wallsFrom = g_build.size();
        for (uint32_t t = ground; t < ground + nWall && g_build.size() < kMaxEdges; ++t) {
            const uint32_t i0 = index(t, 0), i1 = index(t, 1), i2 = index(t, 2);
            if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
            const Vec3 v[3] = {verts[i0], verts[i1], verts[i2]};
            const float ux = v[1].x - v[0].x, uy = v[1].y - v[0].y, uz = v[1].z - v[0].z;
            const float wx = v[2].x - v[0].x, wy = v[2].y - v[0].y, wz = v[2].z - v[0].z;
            const float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len <= 0.0f || std::fabs(ny / len) > 0.5f) continue;
            const float low = std::min({v[0].y, v[1].y, v[2].y});
            int best = -1;
            float bestLen = 0.0f;
            for (int k = 0; k < 3; ++k) {
                const Vec3& a = v[k];
                const Vec3& b = v[(k + 1) % 3];
                if (a.y - low > 150.0f || b.y - low > 150.0f) continue;
                const float l = std::hypot(a.x - b.x, a.z - b.z);
                if (l > bestLen) {
                    bestLen = l;
                    best = k;
                }
            }
            if (best < 0 || bestLen < 50.0f) continue;
            const Vec3& a = v[best];
            const Vec3& b = v[(best + 1) % 3];
            bool seen = false;
            for (size_t e = wallsFrom; e < g_build.size() && !seen; ++e) {
                const Edge& o = g_build[e];
                const auto close = [](float x0, float z0, float x1, float z1) {
                    return std::fabs(x0 - x1) < 16.0f && std::fabs(z0 - z1) < 16.0f;
                };
                seen = (close(o.x0, o.z0, a.x, a.z) && close(o.x1, o.z1, b.x, b.z)) ||
                       (close(o.x0, o.z0, b.x, b.z) && close(o.x1, o.z1, a.x, a.z));
            }
            if (!seen) g_build.push_back({a.x, a.z, b.x, b.z, low});
        }
    }
}

ItemClass ClassOf(uint16_t id) {
    switch (id) {
    case 0x05: case 0x06: case 0x08: case 0x09: case 0x0A: case 0x12: case 0x13: case 0x14:
    case 0x15: case 0x16: case 0x19: case 0x1C:
        return kHealing;  // first aid spray, herbs and their mixes, eggs
    case 0x00: case 0x01: case 0x02: case 0x04: case 0x07: case 0x0E: case 0x11: case 0x18:
    case 0x1A: case 0x20:
        return kAmmo;  // ammunition and grenades
    default:
        return kOtherItem;
    }
}

// The items lying in the room: the enabled item areas, at their model when it has one. A stack of three
// or more on one spot is the room's hidden drop pool (under the floor until an enemy drops one).
int ReadItems(Item* out) {
    const uint32_t ps = U32(kPS);
    if (!Guest(ps)) return 0;
    int n = 0;
    uint32_t p = ps + kOtStart;
    for (int steps = 0; steps < 1024 && n < kMaxItems; ++steps) {
        const uint32_t v = U32(p);
        if (v == 0xFFFFFFFFu || v == 0) break;
        p = v | 0x80000000u;
        if (!(v & 0x80000000u) || !Guest(p)) continue;
        if (U8(p + 0x35) != 3 || !(U8(p + 0x34) & 1u)) continue;
        const uint16_t id = U16(p + 0x78);
        const uint32_t model = U32(p + 0x68);
        // no model: not in the world (still in a crate, or the room is starting)
        if (id >= 0x1000 || !Guest(model)) continue;
        out[n++] = {F32(model + 0x94), F32(model + 0x98), F32(model + 0x9C), ClassOf(id)};
    }
    int kept = 0;
    for (int i = 0; i < n; ++i) {
        int same = 0;
        for (int j = 0; j < n; ++j) {
            if (std::fabs(out[i].x - out[j].x) < 1.0f && std::fabs(out[i].z - out[j].z) < 1.0f) ++same;
        }
        if (same < 3) out[kept++] = out[i];
    }
    return kept;
}

bool Hidden(uint32_t pg) {
    for (const int bit : kHideBits) {
        if (U32(pg + kStatusOffset + 4u * (bit >> 5)) & (0x80000000u >> (bit & 31))) return true;
    }
    return false;
}

} // namespace

void Configure(uint32_t flags, int sizePercent, int corner, int zoom, int opacityPercent) {
    g_flags.store(flags, std::memory_order_relaxed);
    g_size.store(std::clamp(sizePercent, 15, 60), std::memory_order_relaxed);
    g_corner.store(std::clamp(corner, 0, 3), std::memory_order_relaxed);
    g_zoom.store(std::clamp(zoom, 0, 2), std::memory_order_relaxed);
    g_opacity.store(std::clamp(opacityPercent, 20, 100), std::memory_order_relaxed);
}

void OnRetrace() {
    static bool envChecked = false;
    if (!envChecked) {
        envChecked = true;
        // RE4_MINIMAP=1 (Windows tests, env.txt): on with the defaults.
        if (const char* e = std::getenv("RE4_MINIMAP"); e && *e == '1' && g_flags.load() == 0) {
            g_flags.store(kEnabled | kEnemies | kPartner | kItems, std::memory_order_relaxed);
        }
    }
    const uint32_t flags = g_flags.load(std::memory_order_relaxed);
    static bool wasEnabled = false;
    if (!(flags & kEnabled)) {
        // off: nothing read, and the next turn-on builds the outline anew
        if (wasEnabled) {
            wasEnabled = false;
            g_room = 0xFFFFFFFFu;
            std::lock_guard<std::mutex> lock(g_mutex);
            g_shot.visible = false;
        }
        return;
    }
    wasEnabled = true;
    Snapshot shot;
    const uint32_t pl = U32(kPPL), pg = U32(kPG);
    if ((flags & kEnabled) && Guest(pl) && Guest(pg) && !Hidden(pg)) {
        static uint32_t retraces = 0;
        const uint32_t room = U16(pg + kRoomIdOffset);
        if (room != g_room || ++retraces % 30 == 0) {
            const uint64_t sig = SatSignature();
            if (room != g_room || sig != g_signature) {
                g_room = room;
                g_signature = sig;
                BuildOutline();
                RT_LOGF(RT_TAG_RUNTIME, "minimap: r%03x outline, %zu edges\n", room, g_build.size());
                // RE4_MINIMAP_DUMP=1: the outline and Leon's place to minimap_dump.txt (checked offline).
                if (const char* d = std::getenv("RE4_MINIMAP_DUMP"); d && *d == '1') {
                    if (FILE* f = std::fopen("minimap_dump.txt", "w")) {
                        std::fprintf(f, "P %.0f %.0f %.0f %.3f\n", F32(pl + 0x94), F32(pl + 0x98), F32(pl + 0x9C),
                                     F32(pl + 0xA4));
                        for (const Edge& e : g_build) {
                            std::fprintf(f, "E %.0f %.0f %.0f %.0f %.0f\n", e.x0, e.z0, e.x1, e.z1, e.y);
                        }
                        std::fclose(f);
                    }
                }
                std::lock_guard<std::mutex> lock(g_mutex);
                g_edges = g_build;
            }
        }
        shot.visible = true;
        shot.px = F32(pl + 0x94);
        shot.py = F32(pl + 0x98);
        shot.pz = F32(pl + 0x9C);
        shot.pang = F32(pl + 0xA4);
        if (flags & kNorthUp) {
            shot.rightX = 1.0f, shot.rightZ = 0.0f, shot.upX = 0.0f, shot.upZ = -1.0f;
        } else {
            const float rx = F32(kCam + 0xDC), rz = F32(kCam + 0xE4);
            const float lx = F32(kCam + 0xD0), lz = F32(kCam + 0xD8);
            const float rl = std::hypot(rx, rz), ll = std::hypot(lx, lz);
            if (rl > 0.1f && ll > 0.1f) {
                shot.rightX = rx / rl, shot.rightZ = rz / rl;
                shot.upX = -lx / ll, shot.upZ = -lz / ll;  // the camera looks along -Look
            }
        }
        const uint32_t sub = U32(kPSUB);
        int n = 0;
        for (uint32_t e = U32(kEmMgr + 0x10); Guest(e) && n < kMaxMarkers; e = U32(e + 4)) {
            if (e == pl || (U32(e) & 1u) == 0) continue;
            const uint8_t id = U8(e + 0x100);
            if (!Shown(id)) continue;
            const Kind kind = KindOf(id);
            if ((kind == kAshley || kind == kLuis) ? !(flags & kPartner)
                : kind == kAnimal                  ? !(flags & kAnimals)
                                                   : !(flags & kEnemies)) {
                continue;
            }
            const int16_t hp = static_cast<int16_t>(U16(e + 0x322));
            const int16_t hpMax = static_cast<int16_t>(U16(e + 0x324));
            if (kind != kAshley && kind != kLuis && hp <= 0) continue;
            Marker& m = shot.marker[n++];
            m.x = F32(e + 0x94);
            m.y = F32(e + 0x98);
            m.z = F32(e + 0x9C);
            m.ang = F32(e + 0xA4);
            m.hp = hpMax > 0 ? std::clamp(static_cast<float>(hp) / hpMax, 0.0f, 1.0f) : 1.0f;
            m.kind = (e == sub && kind != kLuis) ? kAshley : kind;
            m.alert = (U32(e + kEmStatus) & 1u) != 0;
        }
        shot.markers = n;
        if (flags & kItems) shot.items = ReadItems(shot.item);
        static int lastItems = -1, lastAlert = -1;
        int alert = 0;
        for (int i = 0; i < n; ++i) alert += shot.marker[i].alert ? 1 : 0;
        if (shot.items != lastItems || alert != lastAlert) {
            lastItems = shot.items;
            lastAlert = alert;
            RT_LOGF(RT_TAG_RUNTIME, "minimap: %d markers (%d in battle), %d items\n", n, alert, shot.items);
            if (const char* d = std::getenv("RE4_MINIMAP_DUMP"); d && *d == '1') {
                const uint32_t ps = U32(kPS);
                for (uint32_t p = ps + kOtStart, v = 0, k = 0; Guest(ps) && k < 1024; ++k) {
                    v = U32(p);
                    if (v == 0xFFFFFFFFu || v == 0) break;
                    p = v | 0x80000000u;
                    if ((v & 0x80000000u) && U8(p + 0x35) == 3) {
                        const uint32_t m = U32(p + 0x68);
                        RT_LOGF(RT_TAG_RUNTIME, "  item %02x be=%02x id=%03x ctrl=%02x pos %.0f %.0f model %08x %.0f %.0f\n",
                                U8(p + 0x36), U8(p + 0x34), U16(p + 0x78), U8(p + 0x84), F32(p + 0x5C), F32(p + 0x64), m,
                                Guest(m) ? F32(m + 0x94) : 0.0f, Guest(m) ? F32(m + 0x9C) : 0.0f);
                    }
                }
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_shot = shot;
}

void Draw() {
    if (!(g_flags.load(std::memory_order_relaxed) & kEnabled)) return;
    Snapshot shot;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_shot.visible) return;
        shot = g_shot;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const float side = std::round(display.y * g_size.load(std::memory_order_relaxed) / 100.0f);
    const float margin = std::round(display.y * 0.03f);
    const int corner = g_corner.load(std::memory_order_relaxed);
    const float x0 = (corner == kTopLeft || corner == kBottomLeft) ? margin : display.x - margin - side;
    const float y0 = (corner == kTopLeft || corner == kTopRight) ? margin : display.y - margin - side;
    const ImVec2 lo(x0, y0), hi(x0 + side, y0 + side), mid(x0 + side * 0.5f, y0 + side * 0.5f);
    const float alpha = g_opacity.load(std::memory_order_relaxed) / 100.0f;
    static constexpr float kRange[] = {5000.0f, 9000.0f, 15000.0f};  // world units from the centre to an edge
    const float scale = side * 0.5f / kRange[g_zoom.load(std::memory_order_relaxed)];
    const auto toScreen = [&](float x, float z) {
        const float dx = x - shot.px, dz = z - shot.pz;
        return ImVec2(mid.x + (dx * shot.rightX + dz * shot.rightZ) * scale,
                      mid.y - (dx * shot.upX + dz * shot.upZ) * scale);
    };
    const auto color = [&](float r, float g, float b, float a) {
        return ImGui::GetColorU32(ImVec4(r, g, b, a * alpha));
    };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float rounding = side * 0.06f;
    dl->AddRectFilled(lo, hi, color(0.03f, 0.04f, 0.05f, 0.62f), rounding);
    dl->PushClipRect(lo, hi, true);
    const float thick = std::max(1.0f, side / 220.0f);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // only the edges near Leon: the map shows a square of `range` around him (its corners at
        // range * sqrt(2)), so anything with both ends farther off on one axis is never seen
        const float reach = kRange[g_zoom.load(std::memory_order_relaxed)] * 1.42f;
        for (const Edge& e : g_edges) {
            if ((e.x0 < shot.px - reach && e.x1 < shot.px - reach) || (e.x0 > shot.px + reach && e.x1 > shot.px + reach) ||
                (e.z0 < shot.pz - reach && e.z1 < shot.pz - reach) || (e.z0 > shot.pz + reach && e.z1 > shot.pz + reach)) {
                continue;
            }
            // other floors fade out: full within 1.5 m of Leon's height, gone past 4 m
            const float dy = std::fabs(e.y - shot.py);
            if (dy > 4000.0f) continue;
            const float fade = dy < 1500.0f ? 1.0f : 1.0f - (dy - 1500.0f) / 2500.0f;
            dl->AddLine(toScreen(e.x0, e.z0), toScreen(e.x1, e.z1), color(0.86f, 0.89f, 0.93f, 0.85f * fade), thick);
        }
    }
    const float dot = std::max(3.5f, side / 40.0f);
    // items: small diamonds under the characters
    for (int i = 0; i < shot.items; ++i) {
        const Item& it = shot.item[i];
        const ImVec2 at = toScreen(it.x, it.z);
        const float a = std::fabs(it.y - shot.py) > 3000.0f ? 0.45f : 1.0f;
        const float s = dot * 0.95f;
        const ImU32 fill = it.cls == kHealing ? color(0.35f, 0.95f, 0.4f, a)
                           : it.cls == kAmmo  ? color(1.0f, 0.78f, 0.2f, a)
                                              : color(0.45f, 0.75f, 1.0f, a);
        const ImVec2 n(at.x, at.y - s), e(at.x + s, at.y), so(at.x, at.y + s), w(at.x - s, at.y);
        dl->AddQuadFilled(n, e, so, w, fill);
        dl->AddQuad(n, e, so, w, color(0.0f, 0.0f, 0.0f, 0.7f * a), 1.0f);
    }
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
    for (int i = 0; i < shot.markers; ++i) {
        const Marker& m = shot.marker[i];
        const ImVec2 at = toScreen(m.x, m.z);
        const float dy = std::fabs(m.y - shot.py);
        const float a = dy > 3000.0f ? 0.45f : 1.0f;
        switch (m.kind) {
        case kAshley: dl->AddCircleFilled(at, dot * 1.2f, color(1.0f, 0.85f, 0.25f, a)); break;
        case kLuis: dl->AddCircleFilled(at, dot * 1.1f, color(0.35f, 0.85f, 1.0f, a)); break;
        case kAnimal: dl->AddCircleFilled(at, dot * 0.7f, color(0.65f, 0.65f, 0.65f, a)); break;
        case kBoss:
            dl->AddCircleFilled(at, dot * 1.7f, color(0.85f, 0.25f, 0.95f, a));
            dl->AddCircle(at, dot * 1.7f, color(1.0f, 1.0f, 1.0f, a), 0, thick);
            break;
        case kEnemy:
            // red, turning orange as it is hurt; dim until it has spotted Leon, then a pulsing ring
            if (m.alert) {
                dl->AddCircleFilled(at, dot, color(1.0f, 0.2f + 0.45f * (1.0f - m.hp), 0.15f, a));
                dl->AddCircle(at, dot * (1.5f + 0.4f * pulse), color(1.0f, 0.3f, 0.2f, a * (0.9f - 0.5f * pulse)), 0,
                              thick);
            } else {
                dl->AddCircleFilled(at, dot * 0.85f, color(0.6f, 0.12f + 0.3f * (1.0f - m.hp), 0.1f, a));
            }
            break;
        }
    }
    // Leon: an arrow in his facing, at the centre
    {
        const float fx = std::sin(shot.pang), fz = std::cos(shot.pang);
        const ImVec2 dir((fx * shot.rightX + fz * shot.rightZ), -(fx * shot.upX + fz * shot.upZ));
        const ImVec2 side2(-dir.y, dir.x);
        const float s = dot * 2.2f;
        const ImVec2 tip(mid.x + dir.x * s, mid.y + dir.y * s);
        const ImVec2 l(mid.x - dir.x * s * 0.6f + side2.x * s * 0.7f, mid.y - dir.y * s * 0.6f + side2.y * s * 0.7f);
        const ImVec2 r(mid.x - dir.x * s * 0.6f - side2.x * s * 0.7f, mid.y - dir.y * s * 0.6f - side2.y * s * 0.7f);
        dl->AddTriangleFilled(tip, l, r, color(0.3f, 1.0f, 0.45f, 1.0f));
        dl->AddTriangle(tip, l, r, color(0.0f, 0.0f, 0.0f, 0.8f), 1.0f);
    }
    dl->PopClipRect();
    dl->AddRect(lo, hi, color(0.75f, 0.78f, 0.82f, 0.55f), rounding, 0, thick);
}

} // namespace Minimap
