// Auto-generated GX fatal stubs for the earlier port's game (tools/gen_gx_fatal_stubs.py)
#include "hle_stubs.h"
#include "runtime_log.h"

namespace {
[[noreturn]] void HaltGX(uint32_t addr, const char* name) {
    const char* symbol = name ? name : "<unknown GX symbol>";
    RT_LOGF(RT_TAG_GX,
            "unimplemented GX entry point: %s at guest address 0x%08X.\n"
            "[gx] This graphics call has no Aurora implementation bound to it yet, so the\n"
            "[gx] runtime cannot continue without silently dropping GPU state. Bind it in\n"
            "[gx] runtime/src/hle/gx/ and remove the stub from gx_fatal_stubs.cpp.\n",
            symbol, addr);
    std::fflush(stderr);
    char message[512]{};
    std::snprintf(message, sizeof(message),
                  "%s at guest address 0x%08X has no Aurora implementation bound to it, so the "
                  "runtime stopped rather than keep rendering with missing GPU state.",
                  symbol, addr);
    ShowRuntimeFatalPopup("the game called an unimplemented graphics function", message);
    std::abort();
}
} // namespace

// Every fatal stub is the same two statements with the address and the symbol
// name substituted, so the body comes from this macro. The registration is
// deliberately still spelled out per entry so the translator's runtime-native
// index sees the literal PPC_NATIVE_OVERRIDE_VOID invocation. Hiding it inside
// this macro would leave the index unable to associate an address with the stub.
#define GX_FATAL_STUB(addr, sym) \
    extern "C" void gx_stub_##addr(CpuContext* ctx) { (void)ctx; HaltGX(0x##addr, sym); }

GX_FATAL_STUB(8019fe30, "__GXShutdown_801c1b10") PPC_NATIVE_OVERRIDE_VOID(8019fe30, gx_stub_8019fe30, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801a0ee0, "GXCPInterruptHandler_801c2b80") PPC_NATIVE_OVERRIDE_VOID(801a0ee0, gx_stub_801a0ee0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801a16fc, "__GXCleanGPFifo_801c35c0") PPC_NATIVE_OVERRIDE_VOID(801a16fc, gx_stub_801a16fc, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801a1b4c, "__GXSetVCD_801c3c40") PPC_NATIVE_OVERRIDE_VOID(801a1b4c, gx_stub_801a1b4c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801a1bfc, "__GXCalculateVLim_801c3cf0") PPC_NATIVE_OVERRIDE_VOID(801a1bfc, gx_stub_801a1bfc, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801A20C0, "__GXSetVAT_801c4420") PPC_NATIVE_OVERRIDE_VOID(801A20C0, gx_stub_801A20C0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801A2468, "__GXAbort_801c4b70") PPC_NATIVE_OVERRIDE_VOID(801A2468, gx_stub_801A2468, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801a2fe0, "__GXSendFlushPrim_801c56c0") PPC_NATIVE_OVERRIDE_VOID(801a2fe0, gx_stub_801a2fe0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801a61a8, "__GXSetProjection_801c97f0") PPC_NATIVE_OVERRIDE_VOID(801a61a8, gx_stub_801a61a8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801A6458, "__GXSetViewport_801c9b40") PPC_NATIVE_OVERRIDE_VOID(801A6458, gx_stub_801A6458, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801A663C, "__GXSetMatrixIndex_801c9d90") PPC_NATIVE_OVERRIDE_VOID(801A663C, gx_stub_801A663C, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801a66c4, "GXSetGPMetric_801c9e20") PPC_NATIVE_OVERRIDE_VOID(801a66c4, gx_stub_801a66c4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801a6ef0, "GXClearGPMetric_801ca640") PPC_NATIVE_OVERRIDE_VOID(801a6ef0, gx_stub_801a6ef0, (CpuContext* ctx), (ctx));
