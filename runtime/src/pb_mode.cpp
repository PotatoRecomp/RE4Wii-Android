#include "pb_mode.h"

#include "pb_gun.h"

#include "abi_bridge.h"
#include "game_tweaks.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

#include "runtime_config.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Title main menu (Wii). 0x8015D038 builds it: the entry units (IdSys group 0x29, units 1/3/5/7/0xB:
// the orange highlight of each label) go to w+0x30.., menu_num to w+0x1C (4, or 5 when
// 0x800272D0() is true), the cursor lives at w+0x20. Each entry is a pair of units: the even one
// (2/4/6/8/0xC) draws the label, the odd one the highlight, both shown by be_flag bit 8. With 4
// entries the init clears bit 8 on 0xB and 0xC, which hides the fifth slot.
// 0x8015D2F8 runs once a frame while the menu takes input: A returns the chosen mode (cursor 0 =
// NEW GAME), otherwise it moves the cursor and sets bit 8 on the highlight of the cursor entry only.
//
// PB MODE takes that fifth slot: the game draws and highlights it like the others, and its label
// texture (193x14) is swapped for "PB MODE" by a texture replacement. A on it starts the game as
// NEW GAME does (cursor 0, original handler) with the PB flag set.
extern "C" void func_8015D038(CpuContext* ctx);  // title main menu: init
extern "C" void func_8015D2F8(CpuContext* ctx);  // title main menu: select / cursor
extern "C" void func_8010B7C8(CpuContext* ctx);  // IDSystem::unitPtr(no, class)

namespace PbMode {
namespace {
constexpr uint32_t kIdSys = 0x80332188u;
constexpr uint32_t kTitleClass = 0x29u;
constexpr uint32_t kKeyTrg = 0x80332E9Cu;  // Key.trg (Key 0x80332E80 + 0x1C)
constexpr uint32_t kKeyA = 0x80000000u, kKeyA2 = 0x00001000u;
constexpr uint32_t kMenuNum = 0x1Cu, kCursor = 0x20u;
constexpr uint32_t kPbEntry = 4u;  // the fifth slot

std::atomic<bool> g_active{false};

// IDSystem::unitPtr(kIdSys, no, kTitleClass) on a frame of our own below the caller's stack.
uint32_t UnitPtr(CpuContext* ctx, uint32_t no) {
    const uint32_t sp = ctx->gpr[1], lr = ctx->lr, r3 = ctx->gpr[3];
    ctx->gpr[1] = (sp - 0x40u) & ~0xFu;
    ctx->gpr[3] = kIdSys;
    ctx->gpr[4] = no;
    ctx->gpr[5] = kTitleClass;
    func_8010B7C8(ctx);
    const uint32_t unit = ctx->gpr[3];
    ctx->gpr[1] = sp;
    ctx->lr = lr;
    ctx->gpr[3] = r3;
    return unit;
}
} // namespace

bool Enabled() {
    static const bool s_on = std::getenv("RE4_PB_MODE") != nullptr;
    return s_on;
}

bool Active() {
    static const bool s_always = [] {
        const char* v = std::getenv("RE4_PB_MODE");
        return v != nullptr && std::strcmp(v, "2") == 0;
    }();
    return s_always || g_active.load(std::memory_order_relaxed);
}

// ---- RE4 weapon -> PB weapon --------------------------------------------------------------------
// pG (0x8043D580) + 0x4FC0: weapon_no in the top byte (GC global.h 0x4FB0). Numbers from the item
// table (GC item.cpp wep_info: item id -> weapon_no). Only PB weapons whose WEAPON file holds the gun
// alone (not CS_*); BARRETT's model is empty, so the semi-auto rifle shows the KAR98K with the
// Barrett sound. Weapons without a PB model (rocket launcher, mine thrower) keep their own.
namespace {
struct PbWeapon {
    const char* model;
    const char* sound;
};

PbWeapon PbWeaponFor(uint32_t no) {
    switch (no) {
    case 0x01:  // Punisher
    case 0x02:  // Handgun
    case 0x03:  // Red9
    case 0x04:  // Blacktail
    case 0x05:  // Broken Butterfly
    case 0x06:  // Killer7
    case 0x0F:  // Handcannon
    case 0x11:  // Matilda
        return {"RB454", "RB454"};
    case 0x07: return {"M1887", "M1887"};     // Shotgun
    case 0x08:                                // Striker
    case 0x21: return {"SPAS15", "SPAS15"};   // Riot Gun
    case 0x09: return {"KAR98K", "KAR98K"};   // Rifle
    case 0x0A: return {"KAR98K", "BARRETT"};  // Rifle (semi-auto)
    case 0x0B: return {"T77", "T77"};         // TMP
    case 0x0C: return {"AUGA3", "AUGA3"};     // Chicago Typewriter
    case 0x10: return {"KARAMBIT", nullptr};  // Knife
    case 0x13:                                // Hand, incendiary and flash grenades
    case 0x16:
    case 0x17: return {"GRENADE", nullptr};
    case 0x1C: return {"BOW", "BOW"};  // Krauser's bow
    default: return {nullptr, nullptr};
    }
}

// RE4_PB_GUN / RE4_PB_WEAPON force one model / one sound bank for every weapon (tests).
const char* EnvName(const char* var) {
    const char* v = std::getenv(var);
    return v != nullptr && *v != '\0' ? v : nullptr;
}
} // namespace

uint32_t WeaponNo() {
    uint32_t pG = 0, word = 0;
    if (!Memory::TryRead32(0x8043D580u, pG) || !Memory::Contains(pG + 0x4FC0u, 4) || !Memory::TryRead32(pG + 0x4FC0u, word)) {
        return 0xFFu;
    }
    return word >> 24;
}

const char* WeaponModel() {
    static const char* s_env = EnvName("RE4_PB_GUN");
    return s_env != nullptr ? s_env : PbWeaponFor(WeaponNo()).model;
}

const char* WeaponSound() {
    static const char* s_env = EnvName("RE4_PB_WEAPON");
    return s_env != nullptr ? s_env : PbWeaponFor(WeaponNo()).sound;
}

// ---- Laser in the weapon's parts space ----------------------------------------------------------
namespace {
std::mutex g_laserMutex;
float g_laser[6] = {};
bool g_hasLaser = false;

float ReadF(uint32_t addr) {
    const uint32_t v = Memory::Read32(addr);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// Every 2 s while the laser shows: its start and direction in the parts space (inverse of the parts
// world matrix), in the units PB models use there (scale 1000: mm). A PB model lines up with the
// laser when its +Y turns to that direction.
void LogLaserInParts(uint32_t partsMtxAddr) {
    static unsigned s_frames = 0;
    float l[6];
    {
        std::lock_guard<std::mutex> lock(g_laserMutex);
        if (!g_hasLaser) return;
        g_hasLaser = false;
        std::memcpy(l, g_laser, sizeof(l));
    }
    if (s_frames++ % 120u != 0u || !Memory::Contains(partsMtxAddr, 48)) return;
    float m[3][4];
    for (int i = 0; i < 12; ++i) m[i / 4][i % 4] = ReadF(partsMtxAddr + static_cast<uint32_t>(i) * 4u);
    float d[3] = {l[3] - l[0], l[4] - l[1], l[5] - l[2]}, o[3] = {l[0] - m[0][3], l[1] - m[1][3], l[2] - m[2][3]};
    float ld[3], lo[3];
    for (int j = 0; j < 3; ++j) {  // R^T v / |column|^2 (rotation with a uniform scale)
        const float n2 = m[0][j] * m[0][j] + m[1][j] * m[1][j] + m[2][j] * m[2][j];
        ld[j] = (m[0][j] * d[0] + m[1][j] * d[1] + m[2][j] * d[2]) / n2;
        lo[j] = (m[0][j] * o[0] + m[1][j] * o[1] + m[2][j] * o[2]) / n2;
    }
    const float len = std::sqrt(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2]);
    if (len <= 0.0f) return;
    RT_LOGF(RT_TAG_RUNTIME, "pb gun: weapon %02X laser in parts space: dir %.3f %.3f %.3f start %.1f %.1f %.1f (scale %.3f)\n",
            WeaponNo(), ld[0] / len, ld[1] / len, ld[2] / len, lo[0], lo[1], lo[2],
            std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0] + m[2][0] * m[2][0]));
}
} // namespace

void NoteLaser(uint32_t posAddr, uint32_t crossAddr) {
    if (!Enabled() || !Active() || !Memory::Contains(posAddr, 12) || !Memory::Contains(crossAddr, 12)) return;
    std::lock_guard<std::mutex> lock(g_laserMutex);
    for (int k = 0; k < 3; ++k) {
        g_laser[k] = ReadF(posAddr + k * 4u);
        g_laser[3 + k] = ReadF(crossAddr + k * 4u);
    }
    g_hasLaser = true;
}

// ---- PB weapon sounds ----------------------------------------------------------------------------
// Banks are the friend's PBA1 files (UserData/pb/PB_<NAME>_AUDIO.bin, local only): u32 magic, u32
// count, then per entry u32 name length, u32 size, the name and a RIFF WAV (PCM mono, 8 or 16 bit).
namespace {
struct Sound {
    std::vector<float> pcm;  // mono, -1..1
    uint32_t rate = 0;
};
struct Voice {
    const Sound* sound = nullptr;
    double pos = 0.0;
};
constexpr size_t kMaxVoices = 6;
constexpr float kGain = 0.8f;

std::mutex g_audioMutex;
std::vector<Voice> g_voices;
std::atomic<bool> g_hasVoices{false};

uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

bool DecodeWav(const uint8_t* w, size_t size, Sound& out) {
    if (size < 12 || std::memcmp(w, "RIFF", 4) != 0 || std::memcmp(w + 8, "WAVE", 4) != 0) return false;
    uint16_t channels = 0, bits = 0;
    for (size_t o = 12; o + 8 <= size;) {
        const uint32_t len = Le32(w + o + 4);
        const uint8_t* body = w + o + 8;
        const size_t avail = std::min<size_t>(len, size - o - 8);
        if (std::memcmp(w + o, "fmt ", 4) == 0 && avail >= 16) {
            if (Le16(body) != 1) return false;  // PCM only
            channels = Le16(body + 2);
            out.rate = Le32(body + 4);
            bits = Le16(body + 14);
        } else if (std::memcmp(w + o, "data", 4) == 0 && channels != 0 && (bits == 8 || bits == 16)) {
            const size_t frameBytes = channels * (bits / 8u);
            const size_t frames = avail / frameBytes;
            out.pcm.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                const uint8_t* f = body + i * frameBytes;
                out.pcm[i] = bits == 16 ? static_cast<int16_t>(Le16(f)) / 32768.0f : (f[0] - 128) / 128.0f;
            }
            return frames != 0 && out.rate != 0;
        }
        o += 8 + len + (len & 1u);
    }
    return false;
}

// The bank's fire sound: the first "_Fire" entry that is not a dual-wield one, else any "_Fire", else
// "_Attack" (melee weapons).
bool LoadBankSound(const std::filesystem::path& file, Sound& out) {
    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (d.size() < 8 || std::memcmp(d.data(), "PBA1", 4) != 0) {
        RT_LOGF(RT_TAG_RUNTIME, "pb audio: no bank %s\n", file.string().c_str());
        return false;
    }
    const uint32_t count = Le32(&d[4]);
    size_t best = 0, bestNameLen = 0, bestSize = 0;
    int bestRank = 0;
    std::string bestName;
    size_t o = 8;
    for (uint32_t i = 0; i < count && o + 8 <= d.size(); ++i) {
        const uint32_t nameLen = Le32(&d[o]), size = Le32(&d[o + 4]);
        if (o + 8 + nameLen + size > d.size()) break;
        const std::string name(reinterpret_cast<const char*>(&d[o + 8]), nameLen);
        const bool fire = name.find("_Fire") != std::string::npos;
        const int rank = fire && name.find("Dual") == std::string::npos ? 3
                         : fire                                          ? 2
                         : name.find("_Attack") != std::string::npos     ? 1
                                                                         : 0;
        if (rank > bestRank) {
            bestRank = rank;
            best = o;
            bestNameLen = nameLen;
            bestSize = size;
            bestName = name;
        }
        o += 8 + nameLen + size;
    }
    if (bestRank == 0) {
        RT_LOGF(RT_TAG_RUNTIME, "pb audio: no fire sound in %s\n", file.string().c_str());
        return false;
    }
    const bool ok = DecodeWav(&d[best + 8 + bestNameLen], bestSize, out);
    RT_LOGF(RT_TAG_RUNTIME, "pb audio: %s %s from %s (%zu samples, %u Hz)\n", ok ? "loaded" : "bad wav",
            bestName.c_str(), file.filename().string().c_str(), out.pcm.size(), out.rate);
    return ok;
}

// Fire sounds by bank name, loaded on the first shot of that weapon (game thread). Entries are never
// removed, so the voices may keep pointers to them.
const Sound* FireSound(const char* bank) {
    static std::map<std::string, std::unique_ptr<Sound>> s_sounds;
    if (bank == nullptr) return nullptr;
    auto it = s_sounds.find(bank);
    if (it == s_sounds.end()) {
        auto sound = std::make_unique<Sound>();
        const auto path = RuntimeConfigFile::ApplicationDataDirectory() / "pb" / (std::string("PB_") + bank + "_AUDIO.bin");
        if (!LoadBankSound(path, *sound)) sound.reset();
        it = s_sounds.emplace(bank, std::move(sound)).first;
    }
    return it->second.get();
}

void PlaySound(const Sound* sound) {
    if (sound == nullptr) return;
    std::lock_guard<std::mutex> lock(g_audioMutex);
    if (g_voices.size() >= kMaxVoices) g_voices.erase(g_voices.begin());  // drop the oldest
    g_voices.push_back({sound, 0.0});
    g_hasVoices.store(true, std::memory_order_relaxed);
}
} // namespace

bool HasVoices() { return g_hasVoices.load(std::memory_order_relaxed); }

void MixInto(int16_t* stereo, size_t frames, uint32_t sampleRate) {
    if (sampleRate == 0) return;
    std::lock_guard<std::mutex> lock(g_audioMutex);
    for (Voice& v : g_voices) {
        const std::vector<float>& pcm = v.sound->pcm;
        const double step = static_cast<double>(v.sound->rate) / sampleRate;
        for (size_t i = 0; i < frames && v.pos + 1.0 < pcm.size(); ++i, v.pos += step) {
            const size_t k = static_cast<size_t>(v.pos);
            const float t = static_cast<float>(v.pos - k);
            const float s = (pcm[k] + (pcm[k + 1] - pcm[k]) * t) * kGain * 32767.0f;
            for (int c = 0; c < 2; ++c) {
                const int mixed = stereo[i * 2 + c] + static_cast<int>(s);
                stereo[i * 2 + c] = static_cast<int16_t>(std::clamp(mixed, -32768, 32767));
            }
        }
    }
    g_voices.erase(std::remove_if(g_voices.begin(), g_voices.end(),
                                  [sampleRate](const Voice& v) {
                                      const bool done = v.pos + 1.0 >= v.sound->pcm.size();
                                      static int s_logged = 0;
                                      if (done && s_logged < 3) {
                                          ++s_logged;
                                          RT_LOGF(RT_TAG_RUNTIME, "pb audio: shot mixed (%zu samples into %u Hz)\n",
                                                  v.sound->pcm.size(), sampleRate);
                                      }
                                      return done;
                                  }),
                   g_voices.end());
    g_hasVoices.store(!g_voices.empty(), std::memory_order_relaxed);
}

void OnMenuInit(CpuContext* ctx) {
    const uint32_t w = ctx->gpr[3];
    func_8015D038(ctx);
    GameTweaks::EndBootTurbo();  // RE4_FAST_BOOT ends on the first main menu
    uint32_t num = 0;
    if (!Enabled() || !Memory::TryRead32(w + kMenuNum, num) || num != 4u) return;
    // Show the fifth slot: its label unit (0xC) visible, its highlight (0xB) left to the select loop.
    const uint32_t label = UnitPtr(ctx, 0xCu);
    if (!Memory::Contains(label, 4)) return;
    Memory::Write8(label, Memory::Read8(label) | 0x08u);
    Memory::Write32(w + kMenuNum, 5u);
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        RT_LOGF(RT_TAG_RUNTIME, "pb mode: title menu slot 5 shown (work %08X)\n", w);
    }
}

void OnMenuSelect(CpuContext* ctx) {
    const uint32_t w = ctx->gpr[3];
    uint32_t num = 0, cursor = 0, trg = 0;
    if (!Enabled() || !Memory::TryRead32(w + kMenuNum, num) || !Memory::TryRead32(w + kCursor, cursor) ||
        !Memory::TryRead32(kKeyTrg, trg)) {
        func_8015D2F8(ctx);
        return;
    }
    const bool pb = num == 5u && cursor == kPbEntry && (trg & (kKeyA | kKeyA2)) != 0;
    if (pb) Memory::Write32(w + kCursor, 0);  // start as NEW GAME
    func_8015D2F8(ctx);
    if (ctx->gpr[3] == 0) return;
    g_active.store(pb, std::memory_order_relaxed);
    if (pb) RT_LOGF(RT_TAG_RUNTIME, "pb mode: started (title mode %u)\n", ctx->gpr[3]);
}

} // namespace PbMode

// SndCall(blk, no, pos, ., ., unit): block 2 is the weapon SE bank. A handgun shot (Windows log,
// 1-1) calls numbers 0, 1 and 2 from the weapon's REL code, then 15 (shell drop, kept). In PB MODE
// 0..2 are skipped and the fire sound of the held weapon's PB bank plays on number 0.
extern "C" void func_800779AC(CpuContext* ctx);
extern "C" void PbMode_SndCall_800779AC(CpuContext* ctx) {
    const uint32_t blk = ctx->gpr[3] & 0xFFFFu, no = ctx->gpr[4] & 0xFFFFu;
    if (blk == 2u && PbMode::Enabled() && PbMode::Active()) {
        // Which weapon SE numbers each weapon calls (the fire numbers are proven for the handgun only).
        static uint32_t s_lastWep = 0xFFFFFFFFu;
        static int s_logged = 0;
        const uint32_t wep = PbMode::WeaponNo();
        if (wep != s_lastWep) {
            s_lastWep = wep;
            s_logged = 0;
        }
        if (s_logged < 8) {
            ++s_logged;
            RT_LOGF(RT_TAG_RUNTIME, "pb audio: weapon %02X SndCall(2, %u) lr %08X\n", wep, no, ctx->lr);
        }
        const PbMode::Sound* fire = no <= 2u ? PbMode::FireSound(PbMode::WeaponSound()) : nullptr;
        if (fire != nullptr) {  // no bank for this weapon: the original sound stays
            if (no == 0u) PbMode::PlaySound(fire);
            ctx->gpr[3] = 0;
            return;
        }
    }
    func_800779AC(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x800779AC, PbMode_SndCall_800779AC, "PbMode_SndCall_800779AC");

// commonModelTrans(cModel* m, cModelInfo* info, Mtx viewMat, int flag): draws one model (OT pass).
// Leon's weapon object: pPL (0x8043DEA0) -> cPlWep (+0x7AC) -> m_pWep (+0x34) / m_pWepHand (+0x38);
// The model's own mat stays identity: the weapon sits at its first parts' world matrix (cModel::pList
// +0xF4, cCoord::mat +0x0C), which partsWorldCalc builds from Leon's hand. In PB MODE the PB model is
// drawn there (flag 0 only: the normal pass) and Leon's weapon is skipped in every pass. A weapon
// without a PB model, or whose model file is missing, draws as usual.
extern "C" void func_80163A7C(CpuContext* ctx);
extern "C" void PbMode_ModelTrans_80163A7C(CpuContext* ctx) {
    if (PbMode::Enabled() && PbMode::Active()) {
        uint32_t pl = 0, plWep = 0, wep = 0;
        if (Memory::TryRead32(0x8043DEA0u, pl) && Memory::Contains(pl, 0x800) && Memory::TryRead32(pl + 0x7ACu, plWep) &&
            Memory::Contains(plWep, 0x40)) {
            Memory::TryRead32(plWep + 0x34u, wep);
        }
        uint32_t parts = 0;
        const char* model = ctx->gpr[3] != 0 && ctx->gpr[3] == wep ? PbMode::WeaponModel() : nullptr;
        if (model != nullptr && Memory::TryRead32(wep + 0xF4u, parts) && Memory::Contains(parts, 0x40)) {
            // flag 0 = the normal pass (lit); the shadow / depth passes skip the weapon.
            if (ctx->gpr[6] == 0) PbMode::LogLaserInParts(parts + 0x0Cu);
            if (ctx->gpr[6] != 0 || PbGun::Draw(model, ctx->gpr[5], parts + 0x0Cu, ctx->gpr[4])) return;
        }
    }
    func_80163A7C(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80163A7C, PbMode_ModelTrans_80163A7C, "PbMode_ModelTrans_80163A7C");

extern "C" void PbMode_TitleMenuInit_8015D038(CpuContext* ctx) { PbMode::OnMenuInit(ctx); }
REGISTER_NATIVE_FUNCTION_AS(0x8015D038, PbMode_TitleMenuInit_8015D038, "PbMode_TitleMenuInit_8015D038");
extern "C" void PbMode_TitleMenuSelect_8015D2F8(CpuContext* ctx) { PbMode::OnMenuSelect(ctx); }
REGISTER_NATIVE_FUNCTION_AS(0x8015D2F8, PbMode_TitleMenuSelect_8015D2F8, "PbMode_TitleMenuSelect_8015D2F8");
