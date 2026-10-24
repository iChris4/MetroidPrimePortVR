#include "gx.hpp"
#include "__gx.h"

namespace {
bool sCullMirrored = false;
GXCullMode sRequestedCullMode = GX_CULL_BACK;
}

extern "C" {

constexpr s32 kViewOffset = -342;

void GXSetScissor(u32 left, u32 top, u32 width, u32 height) {
  const u32 tp = top - static_cast<u32>(kViewOffset);
  const u32 lf = left - static_cast<u32>(kViewOffset);
  const u32 bm = tp + height - 1;
  const u32 rt = lf + width - 1;

  SET_REG_FIELD(0, __gx->suScis0, 12, 0, tp);
  SET_REG_FIELD(0, __gx->suScis0, 12, 12, lf);
  SET_REG_FIELD(0, __gx->suScis1, 12, 0, bm);
  SET_REG_FIELD(0, __gx->suScis1, 12, 12, rt);

  GX_WRITE_RAS_REG(__gx->suScis0);
  GX_WRITE_RAS_REG(__gx->suScis1);
  __gx->bpSent = 1;
}

void GXSetScissorBoxOffset(s32 x, s32 y) {
  u32 reg = 0;

  u32 hx = (x + static_cast<u32>(kViewOffset)) >> 1;
  u32 hy = (y + static_cast<u32>(kViewOffset)) >> 1;
  SET_REG_FIELD(0, reg, 10, 0, hx);
  SET_REG_FIELD(0, reg, 10, 10, hy);
  SET_REG_FIELD(0, reg, 8, 24, 0x59);

  GX_WRITE_RAS_REG(reg);
  __gx->bpSent = 1;
}

void GXSetCullMode(GXCullMode mode) {
  sRequestedCullMode = mode;
  if (sCullMirrored) {
    if (mode == GX_CULL_FRONT) mode = GX_CULL_BACK;
    else if (mode == GX_CULL_BACK) mode = GX_CULL_FRONT;
  }
  // Swap front/back to match hardware convention
  GXCullMode hwMode;
  switch (mode) {
  case GX_CULL_FRONT:
    hwMode = GX_CULL_BACK;
    break;
  case GX_CULL_BACK:
    hwMode = GX_CULL_FRONT;
    break;
  default:
    hwMode = mode;
    break;
  }
  SET_REG_FIELD(0, __gx->genMode, 2, 14, hwMode);
  __gx->dirtyState |= 4; // gen mode dirty
}

u8 AuroraSetCullMirrored(u8 mirrored) {
  const bool previous = sCullMirrored;
  if (previous != (mirrored != GX_FALSE)) {
    sCullMirrored = mirrored != GX_FALSE;
    GXSetCullMode(sRequestedCullMode);
  }
  return previous ? GX_TRUE : GX_FALSE;
}

void GXSetClipMode(GXClipMode mode) {
  GX_WRITE_XF_REG(5, mode);
  __gx->bpSent = 1;
}

void GXSetCoPlanar(GXBool enable) {
  u32 reg;
  SET_REG_FIELD(0, __gx->genMode, 1, 19, enable);

  // This sets a mask that causes only one bit to be updated in genMode on the next write.
  reg = 0x0008000;
  SET_REG_FIELD(0, reg, 8, 24, 0xFE);
  SET_REG_FIELD(0, reg, 24, 0, 1 << 19);
  GX_WRITE_RAS_REG(reg);

  // immediately flush
  GX_WRITE_RAS_REG(__gx->genMode);
}
}
