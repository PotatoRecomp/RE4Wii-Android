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
//   0x80117844 Unlink(rel)     calls the epilog and OSUnlinks the home copy.
//
// The buffer and the .bss the game allocated stay allocated and unused until the game frees them.
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

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

std::mutex g_relMutex;
std::unordered_map<uint32_t, uint32_t> g_homeOfBuffer;  // game buffer -> home it was linked at

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
        CopyGuest(buffer, home->home, kModuleHeaderSize);
        std::lock_guard<std::mutex> lock(g_relMutex);
        g_homeOfBuffer[buffer] = home->home;
    }
    RT_LOGF(RT_TAG_OS, "[rel] linked module %u (buffer 0x%08X) at home 0x%08X: %s, prolog 0x%08X\n", id, buffer,
            home->home, result ? "ok" : "FAILED", Memory::Read32(home->home + kOffsetPrologSection + 4));
    ctx->gpr[3] = result;
}
PPC_NATIVE_OVERRIDE_VOID(80117888, RelLink_HLE_80117888, (CpuContext* ctx), (ctx));

extern "C" void RelUnlink_HLE_80117844(CpuContext* ctx) {
    const uint32_t buffer = ctx->gpr[3];
    uint32_t module = buffer;
    {
        std::lock_guard<std::mutex> lock(g_relMutex);
        if (const auto it = g_homeOfBuffer.find(buffer); it != g_homeOfBuffer.end()) {
            module = it->second;
            g_homeOfBuffer.erase(it);
        }
    }
    const uint32_t savedLr = ctx->lr;
    if (const uint32_t epilog = Memory::Read32(module + kOffsetEpilog); epilog != 0) {
        ctx->gpr[3] = module;
        InvokeIndirectCpu(epilog, ctx);
    }
    ctx->gpr[3] = module;
    InvokeDirectCpu<0x8017C060u>(ctx);  // OSUnlink(module)
    ctx->lr = savedLr;
    RT_LOGF(RT_TAG_OS, "[rel] unlinked module %u (buffer 0x%08X, home 0x%08X)\n", Memory::Read32(module), buffer,
            module);
}
PPC_NATIVE_OVERRIDE_VOID(80117844, RelUnlink_HLE_80117844, (CpuContext* ctx), (ctx));
