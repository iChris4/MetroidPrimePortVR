#include "gx.hpp"
#include "__gx.h"

#include "../../gfx/tex_copy_conv.hpp"
#include "../../gfx/texture.hpp"
#include "../../gfx/recording.hpp"
#include "../../gfx/probe.hpp"
#include "../../window.hpp"
#include "../../gfx/clear.hpp"
#include "../../webgpu/gpu.hpp"
#include "../../gx/texture.hpp"
#include "../vi/vi_internal.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <cmath>

namespace {
aurora::Vec2<uint32_t> scale_copy_dst(u32 logicalWidth, u32 logicalHeight) {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return {logicalWidth, logicalHeight};
  }

  const auto [logicalFbWidth, logicalFbHeight] = aurora::gx::logical_fb_size();
  const auto [targetWidth, targetHeight] = aurora::gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return {logicalWidth, logicalHeight};
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  const auto scaledWidth = std::max<u32>(static_cast<u32>(std::lround(static_cast<float>(logicalWidth) * scaleX)), 1);
  const auto scaledHeight = std::max<u32>(static_cast<u32>(std::lround(static_cast<float>(logicalHeight) * scaleY)), 1);
  return {scaledWidth, scaledHeight};
}
} // namespace

namespace aurora::gx {
// An EFB copy of the HDR scene keeps the tone curve as it is now; linear: GXPortCopyScene's.
static gfx::ResolveHdr resolve_hdr(bool linear) noexcept {
  gfx::ResolveHdr hdr{.linear = linear};
  static_assert(sizeof(hdr.tone) == sizeof(g_gxState.pbrTone));
  std::memcpy(hdr.tone.data(), g_gxState.pbrTone.data(), sizeof(hdr.tone));
  return hdr;
}

void copy_tex(const void* dest, GXBool clear) noexcept {
  const auto rect = map_logical_scissor(g_gxState.texCopySrc);
  const auto [dstWidth, dstHeight] = scale_copy_dst(g_gxState.texCopyDstWidth, g_gxState.texCopyDstHeight);
  const auto texCopyFmt = g_gxState.texCopyFmt;
  // GXPortCopyScene in the HDR scene's own pass, of a plain colour format: the linear RGBA16F
  // scene, in its own texture (the key's linear), never RGB565's 8-bit conversion. Every other
  // copy, of the HDR scene too, is the display copy it always was.
  const bool linear = g_gxState.copySceneLinear && gfx::scene_hdr_pass_active() && !is_depth_format(texCopyFmt) &&
                      (texCopyFmt == GX_TF_RGB565 || !gfx::tex_copy_conv::needs_conversion(texCopyFmt));
  // A mip chain only for plain colour copies (converted formats keep one level). A linear copy is
  // never converted (RGB565's is RGBA16F with alpha 1), so it keeps the chain it asked for.
  const bool copyMips =
      g_gxState.texCopyMips && (linear || !gfx::tex_copy_conv::needs_conversion(texCopyFmt));
  uint32_t mipCount = 1;
  if (copyMips) {
    for (u32 edge = std::max(dstWidth, dstHeight); edge > 1; edge >>= 1) {
      ++mipCount;
    }
  }

  const GXState::CopyTextureKey key{
      .dest = dest,
      .width = dstWidth,
      .height = dstHeight,
      .format = texCopyFmt,
      .mips = mipCount > 1,
      .linear = linear,
  };
  static u64 s_copySerial = 0;
  auto it = g_gxState.copyTextureCache.find(key);
  if (it == g_gxState.copyTextureCache.end()) {
    // Enough sizes for a few fog volumes' chunks (full, last column, last row, corner) in a frame.
    constexpr size_t kSizesPerDest = 16;
    trim_copy_sizes(dest, kSizesPerDest - 1);
    gfx::TextureHandle handle;
    if (linear) {
      // RGBA16F; gxFormat only picks the alpha-1 swizzle, as for "Resolved Texture".
      const auto fmt = texCopyFmt == GX_TF_RGB565 || g_gxState.pixelFmt == GX_PF_RGB8_Z24 ||
                               g_gxState.pixelFmt == GX_PF_RGB565_Z16
                           ? GX_TF_RGB565
                           : GX_TF_RGBA8;
      handle = gfx::new_render_texture(dstWidth, dstHeight, fmt, "Linear Scene Copy", mipCount,
                                       wgpu::TextureFormat::RGBA16Float);
    } else if (gfx::tex_copy_conv::needs_conversion(texCopyFmt)) {
      handle = gfx::new_conv_texture(dstWidth, dstHeight, texCopyFmt, "Copy Conv Texture");
    } else {
      // Configure the texture swizzle to use alpha 1.0 if targeting RGB565 or EFB doesn't have alpha
      const auto fmt =
          texCopyFmt == GX_TF_RGB565 || g_gxState.pixelFmt == GX_PF_RGB8_Z24 || g_gxState.pixelFmt == GX_PF_RGB565_Z16
              ? GX_TF_RGB565
              : GX_TF_RGBA8;
      handle = gfx::new_render_texture(dstWidth, dstHeight, fmt, "Resolved Texture", mipCount);
    }
    it = g_gxState.copyTextureCache.emplace(key, GXState::CopyTextureRef{.handle = handle, .revision = 0}).first;
    static const bool logNew = aurora::env_flag("MP_LOG_COPY_TEX");
    if (logNew) {
      std::fprintf(stderr, "[copytex] new %p %ux%u (logical %ux%u) fmt %u%s, %zu cached\n", dest, dstWidth,
                   dstHeight, g_gxState.texCopyDstWidth, g_gxState.texCopyDstHeight,
                   static_cast<unsigned>(texCopyFmt), linear ? " linear" : "", g_gxState.copyTextureCache.size());
    }
  }
  auto& handle = it->second;

  const auto clearColor = clear && g_gxState.colorUpdate;
  const auto clearAlpha = clear && g_gxState.alphaUpdate && efb_has_alpha(g_gxState.pixelFmt);
  const auto clearDepth = clear && g_gxState.depthUpdate;
  // Cost test 11 measures what the copies' pass breaks cost; a copy that clears still has to.
  if (clear || GXGetPBRCostTest() != 11) {
    const auto hdr = resolve_hdr(linear);
    gfx::resolve_pass_into(handle.handle, rect, clearColor, clearAlpha, clearDepth, g_gxState.clearColor,
                           clear_depth_value(), texCopyFmt, -1, mipCount > 1, &hdr,
                           !efb_has_alpha(g_gxState.pixelFmt));
  }
  ++handle.revision;
  handle.lastCopy = ++s_copySerial;
  handle.width = g_gxState.texCopyDstWidth;
  handle.height = g_gxState.texCopyDstHeight;
  g_gxState.copyTextures[dest] = handle;
  texture::invalidate_bindings();
}

void copy_probe_face(u32 face) noexcept {
  const auto rect = map_logical_scissor(g_gxState.texCopySrc);
  const auto hdr = resolve_hdr(false);
  gfx::resolve_pass_into(gfx::probe::face(face), rect, true, true, true, g_gxState.clearColor, clear_depth_value(),
                         GX_TF_RGBA8, static_cast<int>(face % 6), false, &hdr);
}
} // namespace aurora::gx

extern "C" {
GXRenderModeObj GXNtsc480IntDf = {
    VI_TVMODE_NTSC_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};
GXRenderModeObj GXNtsc480Int = {
    VI_TVMODE_NTSC_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {0, 0, 21, 22, 21, 0, 0},
};
GXRenderModeObj GXPal528IntDf = {
    VI_TVMODE_PAL_INT,
    704,
    528,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};
GXRenderModeObj GXMpal480IntDf = {
    VI_TVMODE_PAL_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};

void GXAdjustForOverscan(GXRenderModeObj* rmin, GXRenderModeObj* rmout, u16 hor, u16 ver) {
  // Overscan only insets the visible VI area; the EFB and XFB keep the game's
  // logical size, because they drive GXSetViewport and GXSetScissor. The GX
  // scissor is a 12-bit field biased by 342 (width <= 3754), so copying the OS window's pixel
  // size into fbWidth (2351, or 1878 once the port widens it to 16:9) wraps the
  // scissor to zero width and clips every draw away. Aurora scales the logical
  // framebuffer up to the window when it presents instead.
  *rmout = *rmin;
  rmout->viXOrigin = static_cast< u16 >(rmin->viXOrigin + hor);
  rmout->viYOrigin = static_cast< u16 >(rmin->viYOrigin + ver);
  rmout->viWidth = static_cast< u16 >(rmin->viWidth - 2 * hor);
  rmout->viHeight = static_cast< u16 >(rmin->viHeight - 2 * ver);
}

void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) {}

void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_SRC);
  GX_WRITE_U32(left);
  GX_WRITE_U32(top);
  GX_WRITE_U32(wd);
  GX_WRITE_U32(ht);
}

void GXSetDispCopyDst(u16 wd, u16 ht) {}

void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_DST);
  GX_WRITE_U32(wd);
  GX_WRITE_U32(ht);
  GX_WRITE_U32(fmt);
  GX_WRITE_U8(mipmap != GX_FALSE);
}

// TODO GXSetDispCopyFrame2Field
// TODO GXSetCopyClamp

u32 GXSetDispCopyYScale(f32 vscale) { return 0; }

void GXSetCopyClear(GXColor color, u32 depth) {
  // BP 0x4F: clear color R + A
  u32 reg0 = 0;
  SET_REG_FIELD(0, reg0, 8, 0, color.r);
  SET_REG_FIELD(0, reg0, 8, 8, color.a);
  SET_REG_FIELD(0, reg0, 8, 24, 0x4F);
  GX_WRITE_RAS_REG(reg0);

  // BP 0x50: clear color B + G
  u32 reg1 = 0;
  SET_REG_FIELD(0, reg1, 8, 0, color.b);
  SET_REG_FIELD(0, reg1, 8, 8, color.g);
  SET_REG_FIELD(0, reg1, 8, 24, 0x50);
  GX_WRITE_RAS_REG(reg1);

  // BP 0x51: clear Z (24-bit)
  u32 reg2 = 0;
  SET_REG_FIELD(0, reg2, 24, 0, depth);
  SET_REG_FIELD(0, reg2, 8, 24, 0x51);
  GX_WRITE_RAS_REG(reg2);
  __gx->bpSent = 1;
}

void GXSetCopyFilter(GXBool aa, u8 sample_pattern[12][2], GXBool vf, u8 vfilter[7]) {}

void GXSetDispCopyGamma(GXGamma gamma) {}

void GXCopyDisp(void* dest, GXBool clear) {}

// Port extension: counts EFB copies made from the game thread (GXPortCopySerial).
static u32 sCopySerial = 0;

u32 GXPortCopySerial(void) { return sCopySerial; }

void __GXPortInvalidateCopySerial() { ++sCopySerial; }

void GXCopyTex(void* dest, GXBool clear) {
  ++sCopySerial;
  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_DEST);
  GX_WRITE_U64(reinterpret_cast<u64>(dest));

  SET_REG_FIELD(0, __gx->cpTex, 1, 11, clear != GX_FALSE);
  SET_REG_FIELD(0, __gx->cpTex, 1, 14, 0);
  SET_REG_FIELD(0, __gx->cpTex, 8, 24, 0x52);
  GX_WRITE_RAS_REG(__gx->cpTex);
  aurora::gx::fifo::publish();
}

void GXPortCopyScene(void* dest, GXBool clear) {
  GX_WRITE_AURORA(GX_AURORA_PORT_COPY_SCENE);
  GX_WRITE_U8(1);
  GXCopyTex(dest, clear);
  GX_WRITE_AURORA(GX_AURORA_PORT_COPY_SCENE);
  GX_WRITE_U8(0);
}

// TODO GXGetYScaleFactor
// TODO GXGetNumXfbLines
// TODO GXClearBoundingBox
// TODO GXReadBoundingBox
}
