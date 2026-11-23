// Mods: a player archive larger than the Wii's slot for it.
//
// ReadPlayerData (0x80141160) reads the player's archive (em/pl00.das ...) with DvdReadN (0x8009525C)
// to the fixed address 0x804F0000 and sets pG->pPlayer (pG + 0x50) to it. The weapon data follows at
// 0x80634000, so the slot holds 0x144000 bytes. A larger archive (a model mod, e.g. a 7.3 MB Leon)
// ran over the weapon data, the REL buffers and a thread stack ("Stack overflow in Thread 6").
// No MEM2 below the game's pointer limit 0x933E0000 is free for it: the memory map (0x80115BCC) gives
// heap 5 0x92C1FB00..0x932E0000, and the room loader reads the packed room to that heap's end (r120.das
// at 0x9301EBA0 ran over Leon's models there). So the archive goes above the limit, to MEM2 the Wii
// leaves to IOS and this runtime to nothing (kBigPlDataAddr), and with a mod folder the game's
// "pointer into game memory" checks (85 compares with 0x933E0000, tools/b-translate.ps1 makes them
// read g_re4GameTop) reach kModsGameTop; at 0x94000000 without them Leon's model drew nothing. The
// archive's entries are offsets from its base, so it works from any address; pG->pPlayer points there
// once ReadPlayerData returns. Disc-sized archives keep the game's own address.
#include "mod_player_data.h"

#include "abi_bridge.h"
#include "hle/storage/riivolution.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

#include <cstdint>
#include <string>
#include <vector>

extern "C" void func_8009525C(CpuContext* ctx);
bool DvdFileSize(const std::string& dvdPath, uint32_t& size);                                // hle/storage/dvd.cpp
bool DvdReadFileHead(const std::string& dvdPath, uint32_t length, std::vector<uint8_t>& out);  // hle/storage/dvd.cpp

// The game's upper bound of valid pointers (see above): the Wii's 0x933E0000 unless a mod folder is active.
extern "C" uint32_t g_re4GameTop = 0x933E0000u;

namespace {
constexpr uint32_t kPlDataAddr = 0x804F0000u;
constexpr uint32_t kPlDataEnd = 0x80634000u;          // the weapon data
constexpr uint32_t kReadPlayerDataCall = 0x80141328u;  // ReadPlayerData's DvdReadN return address
constexpr uint32_t kBigPlDataAddr = 0x95000000u;  // past the primitive overflow, below the RELs (0x96000000)
constexpr uint32_t kBigPlDataMax = 0x00E00000u;  // remake_move.cpp keeps move.dat at 0x95E00000
constexpr uint32_t kModsGameTop = 0x98000000u;    // the end of this runtime's MEM2
constexpr uint32_t kPlayerArcOffset = 0x50u;

uint32_t g_relocated = 0;  // where the last ReadPlayerData read went, when not to kPlDataAddr
std::string g_relocatedName;  // its file (with the extension that exists)

bool ModsActive() {
    static const bool s_active = !RuntimeRiivolution::Overlays().empty();
    return s_active;
}

std::string GuestString(uint32_t addr) {
    std::string s;
    for (uint32_t i = 0; i < 128; ++i) {
        const uint32_t a = addr + i;
        uint32_t word = 0;
        if (!Memory::TryRead32(a & ~3u, word)) break;
        const char c = static_cast<char>(word >> (24 - 8 * (a & 3u)));
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

// What the archive takes at the destination: part 0 of the file (the .das the game asks for, or the
// .drs it falls back to). The header's part table at 0x20 holds 0x10 per part (type, size, 0, offset);
// part 0 (type 0) is the archive the read puts at the destination, the type 4 part (sounds) goes
// elsewhere: Leon's disc file is 2.2 MB with a 1.05 MB part 0 in the 1.27 MB slot.
bool ArchiveSize(std::string name, uint32_t& size) {
    if (name.empty()) return false;
    if (name[0] != '/') name.insert(name.begin(), '/');
    std::vector<uint8_t> head;
    uint32_t fileSize = 0;
    if (!DvdFileSize(name, fileSize)) {
        const size_t dot = name.rfind('.');
        if (dot == std::string::npos) return false;
        name = name.substr(0, dot) + ".drs";
        if (!DvdFileSize(name, fileSize)) return false;
    }
    g_relocatedName = name;
    if (!DvdReadFileHead(name, 0x400, head) || head.size() < 0x400) {
        size = fileSize;  // no header to read: the whole file, on the safe side
        return true;
    }
    const auto be32 = [&head](size_t o) {
        return static_cast<uint32_t>(head[o]) << 24 | static_cast<uint32_t>(head[o + 1]) << 16 |
               static_cast<uint32_t>(head[o + 2]) << 8 | static_cast<uint32_t>(head[o + 3]);
    };
    size = be32(0x24);  // part 0's size
    return size != 0 && size <= fileSize;
}
} // namespace

extern "C" void ModPl_DvdReadN_8009525C(CpuContext* ctx) {
    static const bool s_top = [] {
        if (ModsActive()) {
            g_re4GameTop = kModsGameTop;
            RT_LOGF(RT_TAG_OS, "mods: the game's pointer checks reach 0x%08X\n", kModsGameTop);
        }
        return true;
    }();
    (void)s_top;
    if (ctx->lr == kReadPlayerDataCall && ctx->gpr[4] == kPlDataAddr) {
        const std::string name = GuestString(ctx->gpr[3]);
        uint32_t size = 0;
        g_relocated = 0;
        if (ArchiveSize(name, size) && size > kPlDataEnd - kPlDataAddr) {
            if (g_re4GameTop == kModsGameTop && size <= kBigPlDataMax) {
                ctx->gpr[4] = kBigPlDataAddr;
                g_relocated = kBigPlDataAddr;
                RT_LOGF(RT_TAG_OS, "mods: player archive %s (%u bytes) is larger than its slot (%u): read to 0x%08X\n",
                        g_relocatedName.c_str(), size, kPlDataEnd - kPlDataAddr, kBigPlDataAddr);
            } else {
                RT_LOGF(RT_TAG_OS, "mods: player archive %s (%u bytes) is larger than its slot and the place for it (%u): not moved\n",
                        g_relocatedName.c_str(), size, kBigPlDataMax);
            }
        }
    }
    func_8009525C(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009525C, ModPl_DvdReadN_8009525C, "ModPl_DvdReadN_8009525C");

// Mods: the room's primitive buffer. GetPrimBuff (0x80167BB8, r3 size) hands out this frame's half of the
// buffer at pG + 0x4F18 (nPrim bytes per half at pG + 0x4F1C, half index pG + 0x24, next free pG + 0x4F10)
// for skinned vertices; when it is full it returns 0 and the model is not drawn. A model mod skins many
// times the disc's vertices (CubeEvil4's mod fork, patch 0901: tens of thousands), so what does not fit
// comes from an overflow above the Wii's MEM2 (0x94000000, 8 MB per half), emptied when the game flips
// to that half. With a mod folder each request also gets a third more (the fork's patch 0908: a model
// with shapes is laid out at 8 bytes per vertex in a buffer sized for 6, and wrote past it).
extern "C" void func_80167BB8(CpuContext* ctx);
namespace {
constexpr uint32_t kPrimExtraBase = 0x94000000u;
constexpr uint32_t kPrimExtraHalf = 0x00800000u;
uint32_t g_primUsed[2] = {};
uint32_t g_primLastHalf = 0xFFFFFFFFu;
uint32_t g_primOverflows = 0;
} // namespace

extern "C" void ModPl_GetPrimBuff_80167BB8(CpuContext* ctx) {
    const uint32_t asked = ctx->gpr[3];
    const bool mods = ModsActive();
    uint32_t size = asked;
    if (mods && static_cast<int32_t>(asked) > 0) size = asked + asked / 3u;
    ctx->gpr[3] = size;
    func_80167BB8(ctx);
    if (ctx->gpr[3] != 0 || static_cast<int32_t>(size) <= 0) return;
    uint32_t pG = 0, base = 0, half = 0;
    if (!Memory::TryRead32(ctx->gpr[13] - 29856u, pG) || !Memory::TryRead32(pG + 0x4F18u, base) || base == 0 ||
        !Memory::TryRead32(pG + 0x24u, half) || half > 1) {
        return;  // the game's own "no buffer" case
    }
    if (half != g_primLastHalf) {
        g_primUsed[half] = 0;  // the game flipped to this half: its last use is drawn
        g_primLastHalf = half;
    }
    const uint32_t aligned = (size + 31u) & ~31u;
    if (g_primUsed[half] + aligned > kPrimExtraHalf) {
        if (g_primOverflows++ < 8) {
            RT_LOGF(RT_TAG_GX, "mods: primitive overflow full (%u bytes asked, half %u)\n", size, half);
        }
        return;
    }
    ctx->gpr[3] = kPrimExtraBase + half * kPrimExtraHalf + g_primUsed[half];
    g_primUsed[half] += aligned;
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        RT_LOGF(RT_TAG_GX, "mods: the room's primitive buffer is full: the overflow at 0x%08X takes the rest\n",
                kPrimExtraBase);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80167BB8, ModPl_GetPrimBuff_80167BB8, "ModPl_GetPrimBuff_80167BB8");

namespace ModPlayerData {
void AfterReadPlayerData(uint32_t pG) {
    if (g_relocated == 0 || pG == 0) return;
    uint32_t arc = 0;
    if (Memory::TryRead32(pG + kPlayerArcOffset, arc) && arc == kPlDataAddr) {
        Memory::Write32(pG + kPlayerArcOffset, g_relocated);
    }
    // Once a session: the read put the whole of part 0 there.
    static bool s_checked = false;
    if (s_checked) return;
    s_checked = true;
    uint32_t fileSize = 0;
    std::vector<uint8_t> file;
    if (!DvdFileSize(g_relocatedName, fileSize) || !DvdReadFileHead(g_relocatedName, fileSize, file) ||
        file.size() < 0x40) {
        return;
    }
    const auto be32 = [&file](size_t o) {
        return static_cast<uint32_t>(file[o]) << 24 | static_cast<uint32_t>(file[o + 1]) << 16 |
               static_cast<uint32_t>(file[o + 2]) << 8 | static_cast<uint32_t>(file[o + 3]);
    };
    const uint32_t size = be32(0x24), offset = be32(0x2C);
    if (offset + static_cast<uint64_t>(size) > file.size()) return;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < size; i += 4) {
        uint32_t word = 0;
        Memory::TryRead32(g_relocated + i, word);
        if (word != be32(offset + i)) ++bad;
    }
    RT_LOGF(RT_TAG_OS, "mods: player archive check: %u of %u words differ from the file\n", bad, size / 4);
}
} // namespace ModPlayerData

// hle/storage/dvd.cpp, each DVD read into guest RAM: one that lands on a relocated player archive is a bug of
// the place chosen for it (r120.das ran over it at 0x9301EBA0); logged.
void ModPlayerData_NoteGuestDma(uint32_t dest, size_t size, const char* what) {
    static int s_logged = 0;
    if (g_relocated == 0 || s_logged >= 8 || dest == g_relocated) return;  // not the archive's own read
    const uint64_t end = static_cast<uint64_t>(dest) + size;
    if (end <= g_relocated || dest >= g_relocated + kBigPlDataMax) return;
    ++s_logged;
    RT_LOGF(RT_TAG_OS, "mods: a DVD read lands on the player archive: 0x%08X+0x%zX %s\n", dest, size, what ? what : "?");
}
