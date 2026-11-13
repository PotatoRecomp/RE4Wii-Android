// RE4 REL modules at fixed homes.
//
// The game reads a REL into a heap buffer and links it in place (OSLink), so a module's code would
// sit at a different address on every load. Translated code needs one address per instruction:
// tools/re4-rels.py gives every module a fixed home in a MEM2 window the boot code keeps out of the
// arena (kRelRegionBase..kRelRegionLimit, generated/RelHomes.h) and the translator translates the
// modules linked there. These hooks replace the game's two module wrappers:
//
//   0x80117888 Link(rel, bss)  copies the loaded file to its home, links the home copy with the SDK's
//                              own OSLink (0x8017BE38) and the home .bss, then copies the linked header
//                              back over the buffer: the caller then reads rel->prolog (+0x34) and
//                              calls it, which now names the translated prolog at the home.
//   0x80117844 Unlink(rel)     calls the epilog, OSUnlinks the home copy, copies the home's sections
//                              back over the buffer and puts the buffer's own header back: the game may
//                              link the same buffer again without reading the file (Sscrn.rel, the title
//                              menu; r100's module 65 after 81), expecting the .data it left there, and
//                              OSLink on a header that still holds the linked (absolute) offsets
//                              relocated garbage.
//   0x80142654 cRoomData::stopRelData / 0x801426CC restartRelData save the room module's .bss to a
//                              backup (this+0x18) before the unlink and copy it back after the relink
//                              (opening the sub screen, Sscrn.rel). They work on the .bss the game
//                              allocated; the translated module uses its home .bss, so Link and Unlink
//                              move the backup to and from the home .bss when called from there.
//
// The buffer and the .bss the game allocated stay allocated and unused until the game frees them.
#include <cstdint>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <aurora/gfx.h>

#include "abi_bridge.h"
#include "generated/RelHomes.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

namespace {
constexpr uint32_t kModuleHeaderSize = 0x4C;  // OSModuleHeader, version 3
constexpr uint32_t kOffsetBssSize = 0x20;
constexpr uint32_t kOffsetPrologSection = 0x30;
constexpr uint32_t kOffsetEpilog = 0x38;
constexpr uint32_t kStopRelDataUnlinkReturn = 0x801426B8;    // cRoomData::stopRelData after Unlink
constexpr uint32_t kRestartRelDataLinkReturn = 0x80142718;   // cRoomData::restartRelData after Link
constexpr uint32_t kRoomDataBssBackup = 0x18;                 // cRoomData::m_pModule_bss_bak (r31 = this)

std::mutex g_relMutex;
struct LinkedBuffer {
    uint32_t home;
    std::array<uint8_t, 0x4C> header;  // the buffer's header as the file had it, before the linked copy
};
std::unordered_map<uint32_t, LinkedBuffer> g_linked;  // game buffer -> where it was linked

const RelHome* FindHome(uint32_t id) {
    for (const RelHome& home : kRelHomes) {
        if (home.id == id) return &home;
    }
    return nullptr;
}

void CopyGuest(uint32_t dst, uint32_t src, uint32_t size) {
    std::memmove(Memory::GetPointer(dst, size), Memory::GetPointer(src, size), size);
}

uint32_t CallGuestOSLink(CpuContext* ctx, uint32_t module, uint32_t bss) {
    const uint32_t savedLr = ctx->lr;
    ctx->gpr[3] = module;
    ctx->gpr[4] = bss;
    InvokeDirectCpu<0x8017BE38u>(ctx);  // OSLink(module, bss)
    ctx->lr = savedLr;
    return ctx->gpr[3];
}
} // namespace

extern "C" void RelLink_HLE_80117888(CpuContext* ctx) {
    const uint32_t buffer = ctx->gpr[3];
    const uint32_t gameBss = ctx->gpr[4];
    const uint32_t returnAddress = ctx->lr;
    const uint32_t caller = ctx->gpr[31];
    const uint32_t id = Memory::Read32(buffer);
    const RelHome* home = FindHome(id);
    if (home == nullptr || Memory::Read32(buffer + kOffsetBssSize) != home->bssSize) {
        RT_LOGF(RT_TAG_OS, "[rel] module %u at 0x%08X has no home: linking in place (its code is not translated)\n",
                id, buffer);
        ctx->gpr[3] = CallGuestOSLink(ctx, buffer, gameBss);
        return;
    }

    if (buffer != home->home) {
        CopyGuest(home->home, buffer, home->fileSize);
    }
    const uint32_t result = CallGuestOSLink(ctx, home->home, home->bss);
    if (result != 0 && buffer != home->home) {
        LinkedBuffer linked{home->home, {}};
        std::memcpy(linked.header.data(), Memory::GetPointer(buffer, kModuleHeaderSize), kModuleHeaderSize);
        CopyGuest(buffer, home->home, kModuleHeaderSize);
        std::lock_guard<std::mutex> lock(g_relMutex);
        g_linked[buffer] = linked;
    }
    if (result != 0 && returnAddress == kRestartRelDataLinkReturn && home->bssSize != 0) {
        // restartRelData copies the backup into the game's .bss next; the module reads the home one.
        if (const uint32_t backup = Memory::Read32(caller + kRoomDataBssBackup); backup != 0) {
            CopyGuest(home->bss, backup, home->bssSize);
        }
    }
    RT_LOGF(RT_TAG_OS, "[rel] linked module %u (buffer 0x%08X) at home 0x%08X: %s, prolog 0x%08X\n", id, buffer,
            home->home, result ? "ok" : "FAILED", Memory::Read32(home->home + kOffsetPrologSection + 4));
    // Module 81 runs the attache case, merchant and codec screens. The first two times it links, one
    // frame 45 frames later (the screen settled) is logged draw by draw ([drawtrace] in console.log),
    // so a tester's log shows how a phone drew those screens (Samsung A04: the case background).
    static int s_tracedScreens = 0;
    if (result != 0 && id == 81 && s_tracedScreens < 2) {
        ++s_tracedScreens;
        aurora_request_draw_trace(45);
    }
    ctx->gpr[3] = result;
}
PPC_NATIVE_OVERRIDE_VOID(80117888, RelLink_HLE_80117888, (CpuContext* ctx), (ctx));

extern "C" void RelUnlink_HLE_80117844(CpuContext* ctx) {
    const uint32_t buffer = ctx->gpr[3];
    uint32_t module = buffer;
    std::array<uint8_t, kModuleHeaderSize> header{};
    bool restoreHeader = false;
    {
        std::lock_guard<std::mutex> lock(g_relMutex);
        if (const auto it = g_linked.find(buffer); it != g_linked.end()) {
            module = it->second.home;
            header = it->second.header;
            restoreHeader = true;
            g_linked.erase(it);
        }
    }
    const uint32_t savedLr = ctx->lr;
    if (savedLr == kStopRelDataUnlinkReturn) {
        // stopRelData already copied the game's .bss (unused here) to its backup: take the home one.
        const RelHome* home = FindHome(Memory::Read32(module));
        const uint32_t backup = Memory::Read32(ctx->gpr[31] + kRoomDataBssBackup);
        if (home != nullptr && home->home == module && home->bssSize != 0 && backup != 0) {
            CopyGuest(backup, home->bss, home->bssSize);
        }
    }
    if (const uint32_t epilog = Memory::Read32(module + kOffsetEpilog); epilog != 0) {
        ctx->gpr[3] = module;
        InvokeIndirectCpu(epilog, ctx);
    }
    ctx->gpr[3] = module;
    InvokeDirectCpu<0x8017C060u>(ctx);  // OSUnlink(module)
    ctx->lr = savedLr;
    if (restoreHeader) {
        // On the console the buffer is the module, so a relink of the same buffer without reading
        // the file again keeps the .data the module wrote while it ran (room state, object pointers).
        // Carry the home copy back so the next Link copies it home again: without it r100's module 65
        // came back from module 81 with its pointers reset and read through NULL (0x80000028).
        if (const RelHome* home = FindHome(Memory::Read32(module)); home != nullptr && home->home == module) {
            CopyGuest(buffer + kModuleHeaderSize, module + kModuleHeaderSize, home->fileSize - kModuleHeaderSize);
        }
        std::memcpy(Memory::GetPointer(buffer, kModuleHeaderSize), header.data(), kModuleHeaderSize);
    }
    RT_LOGF(RT_TAG_OS, "[rel] unlinked module %u (buffer 0x%08X, home 0x%08X)\n", Memory::Read32(module), buffer,
            module);
}
PPC_NATIVE_OVERRIDE_VOID(80117844, RelUnlink_HLE_80117844, (CpuContext* ctx), (ctx));
