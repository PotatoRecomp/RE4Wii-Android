// gx_poke.cpp - GXPoke* (direct EFB access) entry points, as no-ops.
#include "gx_internal.h"
#include "runtime_log.h"

#include <dolphin/gx/GXGet.h>

namespace {
void LogOnce(bool& logged, const char* what)
{
    if (!logged) {
        logged = true;
        RT_LOGF(RT_TAG_GX, "%s\n", what);
    }
}
} // namespace

// GXGetCullMode(GXCullMode* mode): Aurora holds the mode GXSetCullMode (gx_pixel.cpp) forwards to it.
// The earlier port reads it while building its warning and message layouts.
extern "C" void GX__GetCullMode_801c5890(uint32_t modePtr) {
    GXCullMode mode = GX_CULL_NONE;
    GXGetCullMode(&mode);
    if (modePtr != 0) {
        Memory::Write32(modePtr, static_cast<uint32_t>(mode));
    }
}

// GXAbortFrame asks the GPU to drop the frame in flight. Aurora submits whole frames and has nothing
// half-finished to abandon, so there is nothing to do.
extern "C" void GX__AbortFrame_801c4ce0() {
    static bool logged = false;
    LogOnce(logged, "GXAbortFrame: no partial GPU frame to abandon (no-op)");
}
PPC_NATIVE_OVERRIDE_VOID(801A25CC, GX__AbortFrame_801c4ce0, (), ());

// GXPoke*: modes for CPU writes straight into the embedded frame buffer (GXPokeARGB/GXPokeZ). On
// hardware they program pixel-engine registers; the runtime does not emulate CPU-to-EFB access, so
// the modes have no consumer. Accept them (Aurora declares but does not implement these).
#define RE4_GX_POKE_NOOP(addr, name, params, uses)                                        \
    extern "C" void GX__##name##_##addr params {                                            \
        static bool logged = false;                                                          \
        (void)(uses);                                                                        \
        LogOnce(logged, "GX" #name ": CPU-to-EFB poke modes are not emulated (no-op)");     \
    }

RE4_GX_POKE_NOOP(801c4fa0, PokeAlphaMode, (uint32_t func, uint32_t threshold), (func, threshold))
PPC_NATIVE_OVERRIDE_VOID(801a2918, GX__PokeAlphaMode_801c4fa0, (uint32_t func, uint32_t threshold), (func, threshold));
RE4_GX_POKE_NOOP(801c4fb0, PokeAlphaRead, (uint32_t mode), (mode))
PPC_NATIVE_OVERRIDE_VOID(801a2928, GX__PokeAlphaRead_801c4fb0, (uint32_t mode), (mode));
RE4_GX_POKE_NOOP(801c4fd0, PokeAlphaUpdate, (uint32_t enable), (enable))
// Basara: address checked against the register fields this function writes.
PPC_NATIVE_OVERRIDE_VOID(801a2940, GX__PokeAlphaUpdate_801c4fd0, (uint32_t enable), (enable));
RE4_GX_POKE_NOOP(801c4ff0, PokeBlendMode, (uint32_t type, uint32_t src, uint32_t dst, uint32_t op), (type, src, dst, op))
PPC_NATIVE_OVERRIDE_VOID(801a2954, GX__PokeBlendMode_801c4ff0, (uint32_t type, uint32_t src, uint32_t dst, uint32_t op),
                         (type, src, dst, op));
RE4_GX_POKE_NOOP(801c5050, PokeColorUpdate, (uint32_t enable), (enable))
// Basara: address checked against the register fields this function writes.
PPC_NATIVE_OVERRIDE_VOID(801A29B0, GX__PokeColorUpdate_801c5050, (uint32_t enable), (enable));
RE4_GX_POKE_NOOP(801c5070, PokeDstAlpha, (uint32_t enable, uint32_t alpha), (enable, alpha))
PPC_NATIVE_OVERRIDE_VOID(801a29c4, GX__PokeDstAlpha_801c5070, (uint32_t enable, uint32_t alpha), (enable, alpha));
RE4_GX_POKE_NOOP(801c5090, PokeDither, (uint32_t dither), (dither))
PPC_NATIVE_OVERRIDE_VOID(801A29DC, GX__PokeDither_801c5090, (uint32_t dither), (dither));
RE4_GX_POKE_NOOP(801c50b0, PokeZMode, (uint32_t compareEnable, uint32_t func, uint32_t updateEnable),
                   (compareEnable, func, updateEnable))
PPC_NATIVE_OVERRIDE_VOID(801a29f0, GX__PokeZMode_801c50b0, (uint32_t compareEnable, uint32_t func, uint32_t updateEnable),
                         (compareEnable, func, updateEnable));

#undef RE4_GX_POKE_NOOP
