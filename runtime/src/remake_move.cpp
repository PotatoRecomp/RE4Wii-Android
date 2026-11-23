// Mods: Leon's movements of the 2023 remake from a mod folder's melee/move.dat (the "RE9 Finishers" pack).
//
// Follows CubeEvil4's mod fork (runtime/locomotion.cpp, GPL-3.0, phase 1 of it): with bare hands, a handgun
// or a throwable, cPlayer's motion table (m_MotTbl) takes the remake's stand (slot 0), walk (2), run (6)
// and back step (8), each with a sequence made here in the next slot, and the same in the hurt pairs
// (0x5F, 0x61, 0x65, 0x63). The game's pace stays: the remake's cycle plays faster or slower by the
// square root of the ratio of the two motions' travel (kTimeMin..kTimeMax) and its root motion is
// scaled by the rest. The footsteps are the game's own two sounds of that movement on the frames the
// remake's feet strike. The head's turn toward an enemy blends with "looking" variants (0x3F, 0x39, 0x41
// and the slot after each); with the remake's motion those are taken out, and while one of them plays
// the blend motion's weight (pMotionB->Brate) is held at 0 (MotionMove hook). The shotgun's reload
// (move.dat 0x90 + reload level) takes the game's place when both are the same length (MotionSetCore
// hook). Not yet: the starts, stops and turns (0x80..), the sprint and the item pick-up hand (0xA0..,
// it needs the fork's upper-body overlay).
//
// Wii: cPlayer::weaponInit 0x80138F94 refills m_MotTbl[0..0x5E] (the table pointer at cPlayer + 0x448,
// the GC's 0x424); hooked here, the remake's slots go in after it. move.dat is copied as it is (the
// motion format is the Wii's own, big-endian) to kMoveAddr, inside the player archive's MEM2 place
// (mod_player_data.cpp keeps the archive below it); the game turns a motion's offsets into pointers
// the first time it plays it, so the data never moves. RE4_REMAKE_MOVE=0 turns it off.
#include "abi_bridge.h"
#include "hle/storage/riivolution.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

extern "C" void func_80138F94(CpuContext* ctx);  // cPlayer::weaponInit
extern "C" void func_8011C310(CpuContext* ctx);  // MotionSetCore(model, &Motion, mot, seq, ...)
extern "C" void func_8011CCD8(CpuContext* ctx);  // MotionMove(model, camera)

namespace {
constexpr uint32_t kMoveAddr = 0x95E00000u;
constexpr uint32_t kMoveMax = 0x00180000u;
constexpr uint32_t kSeqAddr = 0x95F80000u;  // the made sequences, kSeqStride each
constexpr uint32_t kSeqStride = 0x410u;
constexpr uint32_t kPG = 0x8043D580u;
constexpr uint32_t kMotTblOffset = 0x448u;
constexpr uint32_t kPPL = 0x8043DEA0u;
constexpr uint32_t kMotionOffset = 0x1D8u;    // cModel::Motion (MOTION_INFO, pMot first)
constexpr uint32_t kMotionBOffset = 0x2A8u;   // cModel::pMotionB
constexpr uint32_t kBrateOffset = 0xC8u;      // MOTION_INFO::Brate
constexpr uint32_t kPWepOffset = 0x44u;       // pG->pWep, the weapon archive (offsets from its start)
constexpr int kReloadMot[3] = {0x2A, 0x2C, 0x2E};  // the shotgun's reload per level (wep07.rel 0x96E26028)

constexpr int kMoves = 4;
constexpr int kSlots[kMoves] = {0, 2, 6, 8};
constexpr int kHurt[kMoves] = {0x5F, 0x61, 0x65, 0x63};
constexpr int kNeck[kMoves] = {0x3F, 0x39, 0x41, 0};
constexpr const char* kNames[kMoves] = {"stand", "walk", "run", "back step"};
constexpr float kTimeMin = 0.75f, kTimeMax = 1.35f;
constexpr float kJogPace = 0.85f;  // the remake's jog a little slower than the game's run
constexpr int kSeqMax = 255;
constexpr int kStepsMax = 24;

struct Move {
    uint32_t mot = 0;  // guest address of the remake's motion
    uint8_t flags = 0;  // 1: matched to the game's pace
    uint16_t last = 0;
    uint16_t foot[2] = {0xFFFF, 0xFFFF};
    int nSteps = 0;
    uint16_t steps[kStepsMax] = {};
    float scale = 1.0f;
    float time = 1.0f;
    uint32_t orig = 0, origSeq = 0, origH = 0, origHSeq = 0;
};
Move g_move[kMoves];
int g_count = 0;
uint32_t g_reload[3] = {};
uint16_t g_reloadLast[3] = {};
bool g_loaded = false;

uint8_t* Guest(uint32_t addr, size_t len) { return Memory::Contains(addr, len) ? Memory::GetPointer(addr, len) : nullptr; }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t Be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
float BeF32(const uint8_t* p) {
    const uint32_t u = Be32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
void PutBeF32(uint8_t* p, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    p[0] = uint8_t(u >> 24), p[1] = uint8_t(u >> 16), p[2] = uint8_t(u >> 8), p[3] = uint8_t(u);
}
int KeyBytes(int fmt) {
    static const int kBytes[16] = {12, 8, 6, 0, 10, 6, 4, 0, 9, 5, 3, 0, 0, 0, 0, 4};
    return kBytes[fmt & 15];
}

bool Enabled() {
    static const bool s_on = [] {
        const char* env = std::getenv("RE4_REMAKE_MOVE");
        return env == nullptr || *env == '\0' || *env != '0';
    }();
    return s_on;
}

// The root position's curve of motion `m` (its joint of kind 1), 0 without one. Offsets are relative to
// the motion until the game first plays it, guest pointers after that.
uint32_t RootCurve(uint32_t m, int* fmt) {
    const uint8_t* h = Guest(m, 4);
    if (h == nullptr) return 0;
    const unsigned n = h[2];
    const uint8_t* head = Guest(m, 3 + n * 3 + 3 + 4 + n * 4);
    if (head == nullptr) return 0;
    const uint32_t table = (3 + n * 3 + 3) & ~3u;
    for (unsigned j = 0; j < n; ++j) {
        const uint16_t kind = Be16(head + 3 + j * 2);
        if ((kind & 0xFF) != 1) continue;
        const uint32_t off = Be32(head + table + 4 + j * 4);
        *fmt = kind >> 12;
        return static_cast<int32_t>(off) >= 0 ? m + off : off;
    }
    return 0;
}

// How far the root goes over the motion on the floor (x, z: last key against the first).
bool Travel(uint32_t m, float* dx, float* dz) {
    int fmt = 0;
    uint32_t p = m ? RootCurve(m, &fmt) : 0;
    if (p == 0 || fmt > 2) return false;
    float d[3] = {};
    for (int axis = 0; axis < 3; ++axis) {
        const uint8_t* c = Guest(p, 2);
        if (c == nullptr) return false;
        const unsigned count = Be16(c);
        if (count == 0 || count > 0x400) return false;
        const uint32_t keys = p + 2 + count * 2;
        const uint8_t* k = Guest(keys, size_t(count) * KeyBytes(fmt));
        if (k == nullptr) return false;
        d[axis] = BeF32(k + (count - 1) * KeyBytes(fmt)) - BeF32(k);
        p = keys + count * KeyBytes(fmt);
    }
    *dx = d[0], *dz = d[2];
    return std::isfinite(d[0]) && std::isfinite(d[2]) && std::fabs(d[0]) < 100000.0f && std::fabs(d[2]) < 100000.0f;
}

// The remake's root motion (f32 keys with f32 slopes) scaled on the floor to `to`.
void ScaleRoot(Move& mv, float to) {
    int fmt = 0;
    uint32_t p = RootCurve(mv.mot, &fmt);
    if (p == 0 || fmt != 0 || mv.scale <= 0.0f || to <= 0.0f || to == mv.scale) return;
    const float k = to / mv.scale;
    for (int axis = 0; axis < 3; ++axis) {
        const uint8_t* c = Guest(p, 2);
        if (c == nullptr) return;
        const unsigned count = Be16(c);
        uint8_t* keys = Guest(p + 2 + count * 2, size_t(count) * 12);
        if (keys == nullptr) return;
        if (axis != 1) {
            for (unsigned i = 0; i < count * 3; ++i) PutBeF32(keys + i * 4, BeF32(keys + i * 4) * k);
        }
        p += 2 + count * 2 + count * 12;
    }
    mv.scale = to;
}

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    std::filesystem::path path;
    for (const auto& overlay : RuntimeRiivolution::Overlays()) {
        std::error_code ec;
        const auto p = overlay.root / "melee" / "move.dat";
        if (std::filesystem::is_regular_file(p, ec)) path = p;
    }
    if (path.empty()) return;
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::string name = path.string();
    uint8_t* base = Guest(kMoveAddr, kMoveMax);
    if (d.size() < 0x40 || d.size() > kMoveMax || base == nullptr) {
        RT_LOGF(RT_TAG_RUNTIME, "move: %s: %zu bytes, not used\n", name.c_str(), d.size());
        return;
    }
    const uint32_t size = static_cast<uint32_t>(d.size());
    const uint32_t count = Be32(d.data());
    bool ok = count >= 2 && count <= 64 && 0x10 + count * 8 <= size;
    uint32_t at[64] = {};
    for (uint32_t i = 0; ok && i < count; ++i) {
        at[i] = Be32(d.data() + 0x10 + i * 4);
        ok = at[i] >= 0x10 + count * 8 && at[i] < size && (at[i] & 3) == 0 && (i == 0 || at[i] >= at[i - 1]);
    }
    const uint8_t* tab = ok ? d.data() + at[0] : nullptr;
    const uint32_t n = ok && at[1] - at[0] >= 8 && std::memcmp(tab, "MOV1", 4) == 0 ? Be32(tab + 4) : 0;
    ok = ok && n >= 1 && n <= 32 && 8 + n * 12 <= at[1] - at[0];
    Move found[kMoves];
    uint32_t reload[3] = {};
    uint16_t reloadLast[3] = {};
    for (uint32_t i = 0; ok && i < n; ++i) {
        const uint8_t* r = tab + 8 + i * 12;
        const uint16_t entry = Be16(r + 2), last = Be16(r + 4);
        if (entry == 0 || entry >= count || last < 2 || Be16(d.data() + at[entry]) != last || d[at[entry] + 2] == 0) {
            ok = false;
            break;
        }
        if (r[0] >= 0x90 && r[0] < 0x93) {
            reload[r[0] - 0x90] = kMoveAddr + at[entry];
            reloadLast[r[0] - 0x90] = last;
        }
        for (int k = 0; k < kMoves; ++k) {
            if (kSlots[k] != r[0]) continue;
            found[k].mot = kMoveAddr + at[entry];
            found[k].flags = r[1];
            found[k].last = last;
            found[k].foot[0] = Be16(r + 6);
            found[k].foot[1] = Be16(r + 8);
        }
    }
    if (!ok) {
        RT_LOGF(RT_TAG_RUNTIME, "move: %s is no archive of Leon's movements (%u bytes): the game's\n", name.c_str(), size);
        return;
    }
    // every footstep after the rows: "STK1", per row u8 n and n u16 (frame, 0x4000 the left foot)
    const uint8_t* stk = tab + 8 + n * 12;
    const uint8_t* const stkEnd = d.data() + at[1];
    int nStk = 0;
    if (stk + 4 <= stkEnd && std::memcmp(stk, "STK1", 4) == 0) {
        const uint8_t* q = stk + 4;
        for (uint32_t i = 0; i < n && q < stkEnd; ++i) {
            const unsigned c = *q++;
            if (q + c * 2 > stkEnd) break;
            const uint8_t code = tab[8 + i * 12];
            for (int k = 0; k < kMoves; ++k) {
                if (kSlots[k] != code || found[k].mot == 0) continue;
                found[k].nSteps = int(c < unsigned(kStepsMax) ? c : kStepsMax);
                for (int j = 0; j < found[k].nSteps; ++j) found[k].steps[j] = Be16(q + j * 2);
                nStk += found[k].nSteps;
            }
            q += c * 2;
        }
    }
    std::memcpy(base, d.data(), size);
    for (int k = 0; k < kMoves; ++k) {
        g_move[k] = found[k];
        if (g_move[k].mot == 0) continue;
        ++g_count;
        RT_LOGF(RT_TAG_RUNTIME, "move: %s: the remake's %s, %u frames\n", name.c_str(), kNames[k], g_move[k].last);
    }
    if (nStk != 0) RT_LOGF(RT_TAG_RUNTIME, "move: %d footsteps\n", nStk);
    int nReload = 0;
    for (int i = 0; i < 3; ++i) {
        g_reload[i] = reload[i];
        g_reloadLast[i] = reloadLast[i];
        nReload += reload[i] != 0;
    }
    if (nReload != 0) RT_LOGF(RT_TAG_RUNTIME, "move: %s: the remake's shotgun reload, %d levels\n", name.c_str(), nReload);
}

bool Active() { return Enabled() && !RuntimeRiivolution::Overlays().empty(); }

bool IsLeon() {
    uint32_t pG = 0, word = 0;
    return Memory::TryRead32(kPG, pG) && Memory::TryRead32(pG + 0x4FC8u, word) && (word >> 24) == 0;
}

bool IsOurs(uint32_t mot) {
    if (mot == 0) return false;
    for (const Move& mv : g_move) {
        if (mv.mot == mot) return true;
    }
    return false;
}

// MotionSetCore for the player with the shotgun's reload motion of a level: the remake's, when move.dat
// has it and it is as long as the game's (another disc or pack: the game's).
uint32_t Reload(uint32_t model, uint32_t mot) {
    uint32_t pl = 0, pG = 0, wep = 0, arc = 0;
    if (!Memory::TryRead32(kPPL, pl) || pl != model || !Memory::TryRead32(kPG, pG) ||
        !Memory::TryRead32(pG + 0x4FC0u, wep) || (wep >> 24) != 7 || !IsLeon() ||
        !Memory::TryRead32(pG + kPWepOffset, arc) || arc < 0x80000000u) {
        return mot;
    }
    Load();
    for (int level = 0; level < 3; ++level) {
        uint32_t off = 0;
        if (g_reload[level] == 0 || !Memory::TryRead32(arc + 4u * uint32_t(kReloadMot[level]), off) || arc + off != mot) {
            continue;
        }
        const uint8_t* h = Guest(mot, 2);
        const unsigned own = h != nullptr ? (Be16(h) & 0x3FFFu) : 0u;
        static int said = 0;
        if (own != g_reloadLast[level]) {
            if (!(said & (8 << level))) {
                said |= 8 << level;
                RT_LOGF(RT_TAG_RUNTIME, "move: the shotgun's reload at level %d is %u frames on this disc, the remake's %u: the game's\n",
                        level, own, unsigned(g_reloadLast[level]));
            }
            return mot;
        }
        if (!(said & (1 << level))) {
            said |= 1 << level;
            RT_LOGF(RT_TAG_RUNTIME, "move: the remake's shotgun reload (level %d, %u frames)\n", level, own);
        }
        return g_reload[level];
    }
    return mot;
}

// The footstep sounds on a made sequence's keys (`perFrame` keys per motion frame).
void PutSteps(const Move& mv, uint8_t* key, int keys, float perFrame, const uint8_t se[2]) {
    auto put = [&](int at, uint8_t sound) { key[(at >= keys ? keys - 1 : at) * 4 + 2] = sound; };
    if (mv.nSteps != 0) {
        for (int i = 0; i < mv.nSteps; ++i) put(int(float(mv.steps[i] & 0x3FFF) * perFrame + 0.5f), se[(mv.steps[i] & 0x4000) ? 1 : 0]);
        return;
    }
    for (int foot = 0; foot < 2; ++foot) {
        if (mv.foot[foot] != 0xFFFF) put(int(float(mv.foot[foot]) * perFrame + 0.5f), se[foot]);
    }
}

// The remake's movement k in place of the game's (`mot`, `seq`): its root motion scaled, its sequence
// made at kSeqAddr + k * kSeqStride (u16 key count, u16 0, then keys {u16 frame in 64ths, u8 sound, u8 0}).
uint32_t Fit(int k, uint32_t mot, uint32_t seq, uint8_t weaponNo) {
    Move& mv = g_move[k];
    const float frames = float(mv.last);
    float time = 1.0f, scale = 1.0f, pace = 0.0f, own = 0.0f;
    const uint8_t* s = seq != 0 ? Guest(seq, 4) : nullptr;
    if ((mv.flags & 1) != 0) {
        const uint8_t* gm = Guest(mot, 2);
        const unsigned gFrames = s != nullptr ? Be16(s) : gm != nullptr ? (Be16(gm) & 0x3FFFu) : 0u;
        ScaleRoot(mv, 1.0f);
        float gx = 0, gz = 0, ox = 0, oz = 0;
        if (gFrames != 0 && Travel(mot, &gx, &gz) && Travel(mv.mot, &ox, &oz)) {
            pace = std::sqrt(gx * gx + gz * gz) / float(gFrames);
            own = std::sqrt(ox * ox + oz * oz) / frames;
        }
        if (k == 2) pace *= kJogPace;
        if (pace > 5.0f && own > 5.0f) time = std::clamp(std::sqrt(pace / own), kTimeMin, kTimeMax);
    }
    int keys = std::clamp(int(frames / time + 0.5f), 2, kSeqMax);
    if (pace > 5.0f && own > 5.0f) {
        scale = pace * float(keys) / (own * frames);
        ScaleRoot(mv, scale);
    }
    mv.time = time;
    uint8_t se[2] = {uint8_t(k == 1 ? 3 : 1), uint8_t(k == 1 ? 4 : 2)};
    if (s != nullptr) {
        const unsigned gKeys = Be16(s);
        const uint8_t* sk = Guest(seq + 4, size_t(gKeys) * 4);
        for (unsigned i = 0, got = 0; sk != nullptr && i < gKeys && got < 2; ++i) {
            if (sk[i * 4 + 2] != 0) se[got++] = sk[i * 4 + 2];
        }
    }
    const uint32_t made = kSeqAddr + uint32_t(k) * kSeqStride;
    uint8_t* out = Guest(made, kSeqStride);
    if (out == nullptr) return 0;
    std::memset(out, 0, kSeqStride);
    out[0] = uint8_t(keys >> 8), out[1] = uint8_t(keys);
    uint8_t* key = out + 4;
    for (int i = 0; i < keys; ++i) {
        const auto frame = static_cast<uint16_t>(float(i) * frames / float(keys) * 64.0f + 0.5f);
        key[i * 4] = uint8_t(frame >> 8), key[i * 4 + 1] = uint8_t(frame);
    }
    if (k != 0) PutSteps(mv, key, keys, float(keys) / frames, se);
    RT_LOGF(RT_TAG_RUNTIME,
            "move: the remake's %s for weapon %u: the game's pace %.0f a frame, the cycle's %.0f: played at %.2f (%d "
            "frames), its steps scaled %.2f\n",
            kNames[k], weaponNo, double(pace), double(own), double(time), keys, double(scale));
    return made;
}

// Bare hands, a handgun (HandgunCk: 1..4, 0x11, 0x26, 0x2B) or a throwable: the remake's motions are
// one-handed / unarmed ones.
bool Light(uint8_t no) {
    switch (no) {
    case 0: case 1: case 2: case 3: case 4: case 0x11: case 0x26: case 0x2B:   // bare hands, handguns
    case 0x13: case 0x16: case 0x17: case 0x19: case 0x1F: case 0x20:          // throwables
        return true;
    default:
        return false;
    }
}

uint32_t Slot(uint32_t tbl, int i) {
    uint32_t v = 0;
    Memory::TryRead32(tbl + 4u * uint32_t(i), v);
    return v;
}

// After weaponInit refilled slots 0..0x5E: the remake's movements in, or the hurt pairs (not refilled)
// back to Leon's own.
void Apply(uint32_t pl) {
    uint32_t pG = 0, tbl = 0, word = 0;
    if (!Memory::TryRead32(kPG, pG) || !Memory::TryRead32(pG + 0x4FC8u, word) || (word >> 24) != 0) return;  // Leon only
    if (!Memory::TryRead32(pl + kMotTblOffset, tbl) || tbl < 0x80000000u || !Memory::Contains(tbl, 0x6C * 4)) return;
    Load();
    uint32_t wepWord = 0;
    Memory::TryRead32(pG + 0x4FC0u, wepWord);
    const uint8_t weaponNo = uint8_t(wepWord >> 24);
    const bool want = g_count != 0 && Light(weaponNo);
    for (int k = 0; k < kMoves; ++k) {
        Move& mv = g_move[k];
        if (mv.mot == 0) continue;
        const uint32_t slot = tbl + 4u * uint32_t(kSlots[k]), hurt = tbl + 4u * uint32_t(kHurt[k]);
        mv.orig = Slot(tbl, kSlots[k]);
        mv.origSeq = Slot(tbl, kSlots[k] + 1);
        const bool oursH = Slot(tbl, kHurt[k]) == mv.mot;
        if (!oursH) {
            mv.origH = Slot(tbl, kHurt[k]);
            mv.origHSeq = Slot(tbl, kHurt[k] + 1);
        }
        if (want && mv.orig != 0 && mv.orig != mv.mot) {
            const uint32_t seq = Fit(k, mv.orig, mv.origSeq, weaponNo);
            if (seq == 0) continue;
            Memory::Write32(slot, mv.mot);
            Memory::Write32(slot + 4, seq);
            if (mv.origH != 0) {
                Memory::Write32(hurt, mv.mot);
                Memory::Write32(hurt + 4, seq);
            }
            if (kNeck[k] != 0) {
                Memory::Write32(tbl + 4u * uint32_t(kNeck[k]), 0);
                Memory::Write32(tbl + 4u * uint32_t(kNeck[k] + 1), 0);
            }
        } else if (!want && oursH) {
            Memory::Write32(hurt, mv.origH);
            Memory::Write32(hurt + 4, mv.origHSeq);
        }
    }
}
} // namespace

extern "C" void RemakeMove_WeaponInit_80138F94(CpuContext* ctx) {
    const uint32_t pl = static_cast<uint32_t>(ctx->gpr[3]);
    func_80138F94(ctx);
    if (Enabled() && !RuntimeRiivolution::Overlays().empty()) {
        const CpuContext saved = *ctx;
        Apply(pl);
        *ctx = saved;
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80138F94, RemakeMove_WeaponInit_80138F94, "RemakeMove_WeaponInit_80138F94");

extern "C" void RemakeMove_MotionSetCore_8011C310(CpuContext* ctx) {
    if (Active()) {
        const uint32_t mot = static_cast<uint32_t>(ctx->gpr[5]);
        const uint32_t want = Reload(static_cast<uint32_t>(ctx->gpr[3]), mot);
        if (want != mot) ctx->gpr[5] = want;
    }
    func_8011C310(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8011C310, RemakeMove_MotionSetCore_8011C310, "RemakeMove_MotionSetCore_8011C310");

// One of the remake's movements plays on Leon: no blend with a game's variant (the fork's watch()).
extern "C" void RemakeMove_MotionMove_8011CCD8(CpuContext* ctx) {
    const uint32_t model = static_cast<uint32_t>(ctx->gpr[3]);
    uint32_t pl = 0, mot = 0, blend = 0;
    if (g_count != 0 && Memory::TryRead32(kPPL, pl) && pl == model && Active() &&
        Memory::TryRead32(pl + kMotionOffset, mot) && IsOurs(mot) && IsLeon() &&
        Memory::TryRead32(pl + kMotionBOffset, blend) && blend >= 0x80000000u &&
        Memory::Contains(blend + kBrateOffset, 4)) {
        uint32_t was = 0;
        Memory::TryRead32(blend + kBrateOffset, was);
        if (was != 0) {
            Memory::Write32(blend + kBrateOffset, 0);  // 0.0f
            static bool said = false;
            if (!said) {
                said = true;
                RT_LOGF(RT_TAG_RUNTIME, "move: the blend motion's weight %08X held at 0 under the remake's motion\n", was);
            }
        }
    }
    func_8011CCD8(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8011CCD8, RemakeMove_MotionMove_8011CCD8, "RemakeMove_MotionMove_8011CCD8");
