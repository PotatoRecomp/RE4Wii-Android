#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"

#include <bit>
#include <cmath>
#include <cstdint>

// Profile-Guided HLE: a guest routine is done natively only when a device profile shows it hot,
// after its disassembly confirms what it is, and only when the native result matches the guest's
// bit for bit (or within the float rounding that hides the difference).
//
// The game's C math library (MSL, fdlibm based) done natively where the profile shows it. The
// Redmi profile of 2026-11-21 had the sqrtf wrapper at 0x801B26C4 at 2.25% of the game thread and
// sinf at 0x801B268C at 0.9%: fdlibm's sqrt computes the root one bit per loop iteration, while
// the host has a single instruction for it.
namespace {

// MSL's errno (EDOM = 33) sits in the small data area at r13 - 24728.
constexpr int32_t kErrnoSdaOffset = -24728;
constexpr uint32_t kEdom = 33u;
// The float NaN MSL returns for the square root of a negative number (__float_nan).
constexpr uint32_t kFloatNanAddr = 0x80440000u - 9688u;

void SetErrnoEdom(CpuContext* ctx) {
    Memory::Write32(ctx->gpr[13] + static_cast<uint32_t>(kErrnoSdaOffset), kEdom);
}

// sqrt (0x801B612C) with the special cases of the guest's version: infinity and NaN set errno and
// return x*x+x, signed zero returns itself, any other negative sets errno and returns __float_nan.
// For everything else both are the correctly rounded IEEE root, so the result is bit-identical.
double GuestSqrt(CpuContext* ctx, double x) {
    const uint64_t bits = std::bit_cast<uint64_t>(x);
    const uint32_t hi = static_cast<uint32_t>(bits >> 32);
    if ((hi & 0x7FF00000u) == 0x7FF00000u) {
        SetErrnoEdom(ctx);
        return x * x + x;
    }
    if ((hi & 0x80000000u) != 0) {
        if ((bits & 0x7FFFFFFFFFFFFFFFull) == 0) {
            return x;
        }
        SetErrnoEdom(ctx);
        return static_cast<double>(std::bit_cast<float>(Memory::Read32(kFloatNanAddr)));
    }
    return std::sqrt(x);
}

} // namespace

extern "C" void MslSqrt_HLE_801b612c(CpuContext* ctx) {
    ctx->fpr[1].d = GuestSqrt(ctx, ctx->fpr[1].d);
}
PPC_NATIVE_OVERRIDE_VOID(801B612C, MslSqrt_HLE_801b612c, (CpuContext* ctx), (ctx));

// sqrtf: (float)sqrt((double)x).
extern "C" void MslSqrtf_HLE_801b26c4(CpuContext* ctx) {
    const double x = static_cast<double>(PpcForceSingleValueInline(ctx->fpr[1].d));
    ctx->fpr[1].d = static_cast<double>(PpcForceSingleValueInline(GuestSqrt(ctx, x)));
}
PPC_NATIVE_OVERRIDE_VOID(801B26C4, MslSqrtf_HLE_801b26c4, (CpuContext* ctx), (ctx));

// sinf: (float)sin((double)x). 0x801B268C wraps 0x801B5FC8, which calls __kernel_sin with iy = 0
// (li r3, 0) for |x| <= pi/4; cos is 0x801B5BBC, which calls __kernel_cos. The host sin and
// fdlibm's agree to within an ulp of a double, which the rounding to float hides; a non-finite
// argument gives NaN in both, without errno.
extern "C" void MslSinf_HLE_801b268c(CpuContext* ctx) {
    const double x = static_cast<double>(PpcForceSingleValueInline(ctx->fpr[1].d));
    ctx->fpr[1].d = static_cast<double>(PpcForceSingleValueInline(std::isfinite(x) ? std::sin(x) : x - x));
}
PPC_NATIVE_OVERRIDE_VOID(801B268C, MslSinf_HLE_801b268c, (CpuContext* ctx), (ctx));
