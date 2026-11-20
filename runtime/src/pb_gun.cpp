// PB MODE weapon models: a PNR1 model (friend's Point Black format, local file in UserData/pb) drawn
// in place of Leon's weapon, at the weapon's first parts matrix (static pose, no PB animation).
//
// PNR1 (pb::ParseModel in the friend's libmain.so): 9 x u32 header (magic, version 1, file size,
// bones, vertices, indices, clips, texture w, h), one 52-byte record, bones 100 bytes each, vertices
// 84 bytes (pos f3, normal f3, uv f2, f4, bone u32x4, weight f4, u32), u32 triangle indices, clips,
// and the RGBA8 texture (w x h) at the end of the file. Non-CS weapons only: their WEAPON file holds
// the gun alone, in meters.
//
// Lighting: ModelRender runs LightSetModel for the RE4 weapon before commonModelTrans, so the lights,
// ambient and channel 0 (lit, material from the register) are already the weapon's when this draws.
// Only the material color (the model info's, as commonModelTrans uses) and the normals are ours.
#include "pb_gun.h"

#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <dolphin/gx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace PbGun {
namespace {
struct Mesh {
    std::vector<float> pos;  // xyz per vertex
    std::vector<float> nrm;  // xyz per vertex
    std::vector<float> uv;   // st per vertex
    std::vector<uint32_t> idx;
    std::vector<uint8_t> rgba;
    uint16_t tw = 0, th = 0;
    GXTexObj tex{};
};

uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
float LeF(const uint8_t* p) {
    const uint32_t v = Le32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

std::filesystem::path PbDir() { return RuntimeConfigFile::ApplicationDataDirectory() / "pb"; }

bool LoadPnr1(const std::filesystem::path& file, Mesh& m) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (d.size() < 0x58 || std::memcmp(d.data(), "PNR1", 4) != 0 || Le32(&d[4]) != 1 || Le32(&d[8]) != d.size()) {
        RT_LOGF(RT_TAG_RUNTIME, "pb gun: %s is not a PNR1 model\n", file.string().c_str());
        return false;
    }
    const uint32_t nb = Le32(&d[12]), nv = Le32(&d[16]), ni = Le32(&d[20]), tw = Le32(&d[28]), th = Le32(&d[32]);
    const size_t vs = 0x24 + 0x34 + size_t{nb} * 100, is = vs + size_t{nv} * 84, texBytes = size_t{tw} * th * 4;
    if (nb > 256 || nv > 1000000 || ni > 3000000 || ni % 3 || tw > 8192 || th > 8192 || is + size_t{ni} * 4 > d.size() ||
        texBytes > d.size() - (is + size_t{ni} * 4) || nv < 4) {
        RT_LOGF(RT_TAG_RUNTIME, "pb gun: %s has bad counts or no mesh (%u vertices)\n", file.string().c_str(), nv);
        return false;
    }
    m.pos.resize(size_t{nv} * 3);
    m.nrm.resize(size_t{nv} * 3);
    m.uv.resize(size_t{nv} * 2);
    for (uint32_t i = 0; i < nv; ++i) {
        const uint8_t* v = &d[vs + size_t{i} * 84];
        for (int k = 0; k < 3; ++k) {
            m.pos[i * 3 + k] = LeF(v + k * 4);
            m.nrm[i * 3 + k] = LeF(v + 12 + k * 4);
        }
        m.uv[i * 2] = LeF(v + 24);
        m.uv[i * 2 + 1] = LeF(v + 28);
    }
    m.idx.resize(ni);
    for (uint32_t i = 0; i < ni; ++i) {
        m.idx[i] = Le32(&d[is + size_t{i} * 4]);
        if (m.idx[i] >= nv) return false;
    }
    m.rgba.assign(d.end() - static_cast<std::ptrdiff_t>(texBytes), d.end());
    m.tw = static_cast<uint16_t>(tw);
    m.th = static_cast<uint16_t>(th);
    GXInitTexObj(&m.tex, m.rgba.data(), m.tw, m.th, GX_TF_RGBA8_PC, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GXInitTexObjLOD(&m.tex, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    RT_LOGF(RT_TAG_RUNTIME, "pb gun: loaded %s (%u vertices, %u triangles, texture %ux%u)\n",
            file.filename().string().c_str(), nv, ni / 3, tw, th);
    return true;
}

// Models by name, loaded on first use (game thread only). A missing or bad file stays null.
const Mesh* Model(const char* name) {
    static std::map<std::string, std::unique_ptr<Mesh>> s_models;
    auto it = s_models.find(name);
    if (it == s_models.end()) {
        auto mesh = std::make_unique<Mesh>();
        if (!LoadPnr1(PbDir() / (std::string("PB_") + name + "_WEAPON.bin"), *mesh)) mesh.reset();
        it = s_models.emplace(name, std::move(mesh)).first;
    }
    return it->second.get();
}

using Mtx34 = float[3][4];

void Mul(const Mtx34 a, const Mtx34 b, Mtx34 out) {
    Mtx34 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0.0f);
        }
    }
    std::memcpy(out, r, sizeof(r));
}

void ReadGuestMtx(uint32_t addr, Mtx34 out) {
    for (int i = 0; i < 12; ++i) {
        const uint32_t v = Memory::Read32(addr + static_cast<uint32_t>(i) * 4u);
        std::memcpy(&out[i / 4][i % 4], &v, 4);
    }
}

// Placement of a PB model in the weapon's parts space: scale, rx, ry, rz (degrees), tx, ty, tz (game
// units, mm). Default 1000 (PB meters to millimeters) and rz 90: PB barrels point along +Y, RE4 weapons
// along -X of their parts space (the laser there: dir -1 0 0, logged in 1-1 with the handgun).
struct Xform {
    float v[7] = {1000.0f, 0, 0, 90.0f, 0, 0, 0};
};

// UserData/pb/gun_xform.txt, one line per model: "<NAME> scale rx ry rz tx ty tz" ('*' = every model
// without its own line, '#' starts a comment). Read again when the file changes, so a grip can be
// tuned while the game runs. RE4_PB_GUN_XFORM="scale,rx,ry,rz,tx,ty,tz" overrides it for every model.
Xform XformFor(const char* name) {
    static Xform s_env;
    static const bool s_hasEnv = [] {
        const char* e = std::getenv("RE4_PB_GUN_XFORM");
        return e != nullptr && std::sscanf(e, "%f,%f,%f,%f,%f,%f,%f", &s_env.v[0], &s_env.v[1], &s_env.v[2], &s_env.v[3],
                                           &s_env.v[4], &s_env.v[5], &s_env.v[6]) >= 1;
    }();
    if (s_hasEnv) return s_env;

    static std::map<std::string, Xform> s_table;
    static std::filesystem::file_time_type s_stamp{};
    static unsigned s_calls = 0;
    if (s_calls++ % 30u == 0u) {
        const auto file = PbDir() / "gun_xform.txt";
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(file, ec);
        if (!ec && stamp != s_stamp) {
            s_stamp = stamp;
            s_table.clear();
            std::ifstream in(file);
            std::string line;
            while (std::getline(in, line)) {
                char key[64] = {};
                Xform x;
                if (line.empty() || line[0] == '#') continue;
                if (std::sscanf(line.c_str(), "%63s %f %f %f %f %f %f %f", key, &x.v[0], &x.v[1], &x.v[2], &x.v[3], &x.v[4],
                                &x.v[5], &x.v[6]) >= 2) {
                    s_table[key] = x;
                }
            }
            RT_LOGF(RT_TAG_RUNTIME, "pb gun: gun_xform.txt read (%zu entries)\n", s_table.size());
        }
    }
    if (auto it = s_table.find(name); it != s_table.end()) return it->second;
    if (auto it = s_table.find("*"); it != s_table.end()) return it->second;
    return {};
}

void LocalXform(const Xform& x, Mtx34 out) {
    const float d2r = 3.14159265f / 180.0f;
    const float cx = std::cos(x.v[1] * d2r), sx = std::sin(x.v[1] * d2r);
    const float cy = std::cos(x.v[2] * d2r), sy = std::sin(x.v[2] * d2r);
    const float cz = std::cos(x.v[3] * d2r), sz = std::sin(x.v[3] * d2r);
    // R = Rz * Ry * Rx, then scale.
    const float r[3][3] = {{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
                           {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
                           {-sy, cy * sx, cy * cx}};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[i][j] = r[i][j] * x.v[0];
        out[i][3] = x.v[4 + i];
    }
}

// Normal matrix for a model-view matrix with uniform scale: its 3x3 part over the scale (cube root
// of the determinant), so normals stay unit length for the lighting.
void NormalMtx(const Mtx34 mv, Mtx34 out) {
    const float det = mv[0][0] * (mv[1][1] * mv[2][2] - mv[1][2] * mv[2][1]) -
                      mv[0][1] * (mv[1][0] * mv[2][2] - mv[1][2] * mv[2][0]) +
                      mv[0][2] * (mv[1][0] * mv[2][1] - mv[1][1] * mv[2][0]);
    const float s = std::cbrt(det);
    const float inv = std::fabs(s) > 1e-12f ? 1.0f / s : 1.0f;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[i][j] = mv[i][j] * inv;
        out[i][3] = 0.0f;
    }
}
} // namespace

bool Draw(const char* name, uint32_t viewMtxAddr, uint32_t partsMtxAddr, uint32_t infoAddr) {
    const Mesh* m = name != nullptr ? Model(name) : nullptr;
    if (m == nullptr || !Memory::Contains(viewMtxAddr, 48) || !Memory::Contains(partsMtxAddr, 48)) return false;
    Mtx34 view, parts, local, mv, nrm;
    ReadGuestMtx(viewMtxAddr, view);
    ReadGuestMtx(partsMtxAddr, parts);
    LocalXform(XformFor(name), local);
    Mul(parts, local, mv);
    Mul(view, mv, mv);
    NormalMtx(mv, nrm);

    // cModelInfo::color (+0x8C, 0xFFFFFFFF unless the model sets one), as commonModelTrans uses it.
    uint32_t color = 0xFFFFFFFFu;
    if (infoAddr != 0 && Memory::Contains(infoAddr + 0x8Cu, 4)) color = Memory::Read32(infoAddr + 0x8Cu);
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        RT_LOGF(RT_TAG_RUNTIME, "pb gun: lit by the weapon's lights, material %08X\n", color);
    }

    GXLoadPosMtxImm(mv, GX_PNMTX0);
    GXLoadNrmMtxImm(nrm, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT7, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT7, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT7, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetChanMatColor(GX_COLOR0A0, GXColor{static_cast<u8>(color >> 24), static_cast<u8>(color >> 16),
                                           static_cast<u8>(color >> 8), 255});
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GXSetNumIndStages(0);
    GXSetNumTevStages(1);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GXLoadTexObj(const_cast<GXTexObj*>(&m->tex), GX_TEXMAP0);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetCullMode(GX_CULL_NONE);

    constexpr size_t kChunk = 65532;  // GXBegin takes a u16 count
    for (size_t first = 0; first < m->idx.size(); first += kChunk) {
        const size_t n = std::min(kChunk, m->idx.size() - first);
        GXBegin(GX_TRIANGLES, GX_VTXFMT7, static_cast<u16>(n));
        for (size_t k = first; k < first + n; ++k) {
            const uint32_t i = m->idx[k];
            GXPosition3f32(m->pos[i * 3], m->pos[i * 3 + 1], m->pos[i * 3 + 2]);
            GXNormal3f32(m->nrm[i * 3], m->nrm[i * 3 + 1], m->nrm[i * 3 + 2]);
            GXTexCoord2f32(m->uv[i * 2], m->uv[i * 2 + 1]);
        }
        GXEnd();
    }
    return true;
}

} // namespace PbGun
