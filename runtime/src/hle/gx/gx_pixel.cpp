// gx_pixel.cpp - Pixel Processing, Blending, and Fog
#include "gx_internal.h"

// ============================================================================
// Blend Mode
// ============================================================================

extern "C" void GX__SetBlendMode_8017277c(uint32_t t, uint32_t s, uint32_t d, uint32_t op) {
    GXSetBlendMode(static_cast<GXBlendMode>(t), static_cast<GXBlendFactor>(s),
                   static_cast<GXBlendFactor>(d), static_cast<GXLogicOp>(op));
}
PPC_NATIVE_OVERRIDE_VOID(801a5cf8, GX__SetBlendMode_8017277c, (uint32_t t, uint32_t s, uint32_t d, uint32_t op), (t, s, d, op));

extern "C" void GX__SetColorUpdate_801727cc(uint32_t en) {
    GXSetColorUpdate(static_cast<GXBool>(en));
}
// Basara: address checked against the register fields this function writes.
PPC_NATIVE_OVERRIDE_VOID(801a5d48, GX__SetColorUpdate_801727cc, (uint32_t en), (en));

extern "C" void GX__SetAlphaUpdate_801727f8(uint32_t en) {
    GXSetAlphaUpdate(static_cast<GXBool>(en));
}
// Basara: address checked against the register fields this function writes.
PPC_NATIVE_OVERRIDE_VOID(801a5d74, GX__SetAlphaUpdate_801727f8, (uint32_t en), (en));

// ============================================================================
// Z Buffer
// ============================================================================

extern "C" void GX__SetZMode_80172824(uint32_t ce, uint32_t f, uint32_t ue) {
    GXSetZMode(static_cast<GXBool>(ce), static_cast<GXCompare>(f), static_cast<GXBool>(ue));
}
PPC_NATIVE_OVERRIDE_VOID(801A5DA0, GX__SetZMode_80172824, (uint32_t ce, uint32_t f, uint32_t ue), (ce, f, ue));

extern "C" void GX__SetZCompLoc_80172858(uint32_t bt) {
    GXSetZCompLoc(static_cast<GXBool>(bt));
}
PPC_NATIVE_OVERRIDE_VOID(801a5dd4, GX__SetZCompLoc_80172858, (uint32_t bt), (bt));

// ============================================================================
// Pixel Format and Dither
// ============================================================================

extern "C" void GX__SetPixelFmt_80172888(uint32_t pf, uint32_t zf) {
    GXSetPixelFmt(static_cast<GXPixelFmt>(pf), static_cast<GXZFmt16>(zf));
}
PPC_NATIVE_OVERRIDE_VOID(801A5E04, GX__SetPixelFmt_80172888, (uint32_t pf, uint32_t zf), (pf, zf));

extern "C" void GX__SetDither_80172930(uint32_t d) {
    GXSetDither(static_cast<GXBool>(d));
}
PPC_NATIVE_OVERRIDE_VOID(801a5eac, GX__SetDither_80172930, (uint32_t d), (d));

extern "C" void GX__SetDstAlpha_8017295c(uint32_t en, uint32_t a) {
    GXSetDstAlpha(static_cast<GXBool>(en), static_cast<u8>(a));
}
PPC_NATIVE_OVERRIDE_VOID(801a5ed8, GX__SetDstAlpha_8017295c, (uint32_t en, uint32_t a), (en, a));

// ============================================================================
// Fog and Alpha Compare
// ============================================================================

// GXSetFog (0x803848b8 in RB4E08) is not overridden: the game's own code writes the fog BP
// registers through the FIFO. Settings > Performance > Disable Fog is handled in aurora (gx.cpp).

extern "C" void GX__SetAlphaCompare_80172088(uint32_t c0, uint32_t r0, uint32_t op, uint32_t c1, uint32_t r1) {
    g_alphaCompareValid = true;
    GXSetAlphaCompare((GXCompare)c0, (u8)r0, (GXAlphaOp)op, (GXCompare)c1, (u8)r1);
}
PPC_NATIVE_OVERRIDE_VOID(801A5764, GX__SetAlphaCompare_80172088, (uint32_t c0, uint32_t r0, uint32_t op, uint32_t c1, uint32_t r1), (c0, r0, op, c1, r1));

extern "C" void GX__SetZTexture_801720c0(uint32_t op, uint32_t f, uint32_t b) { GXSetZTexture((GXZTexOp)op, (GXTexFmt)f, b); }
PPC_NATIVE_OVERRIDE_VOID(801a579c, GX__SetZTexture_801720c0, (uint32_t op, uint32_t f, uint32_t b), (op, f, b));

// ============================================================================
// Culling and Clipping
// ============================================================================

extern "C" void GX__SetCullMode_8016f3b8(uint32_t m) {
    GXSetCullMode(static_cast<GXCullMode>(m));
}
PPC_NATIVE_OVERRIDE_VOID(801a315c, GX__SetCullMode_8016f3b8, (uint32_t m), (m));

extern "C" void GX__SetCoPlanar_8016f3e0(uint32_t en) { GXSetCoPlanar((GXBool)en); }
PPC_NATIVE_OVERRIDE_VOID(801a3184, GX__SetCoPlanar_8016f3e0, (uint32_t en), (en));

extern "C" void GX__SetClipMode_8017351c(uint32_t m) { GXSetClipMode((GXClipMode)m); }
PPC_NATIVE_OVERRIDE_VOID(801A6614, GX__SetClipMode_8017351c, (uint32_t m), (m));
