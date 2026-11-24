// Draws a mod's distance-field font in place of the disc's glyph images (port_hd_font.h).

#include "port_debug.h"
#include "port_env.h"
#include "port_hd_font.h"
#include "port_strings.h"

#include "port_gci.h"
#include "port_log.h"
#include "port_mods.h"
#include "port_utf8_path.h"

#include "Kyoto/Graphics/CGX.hpp"
#include "Kyoto/Graphics/CGraphicsPalette.hpp"
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Text/CRasterFont.hpp"

#include <dolphin/gx.h>
#include <dolphin/gx/GXExtra.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace PortHdFont {
namespace {

int sEnabled = -1;
bool sTried = false;
bool sLoaded = false;
Font sFont;
GXTexObj sTexture;
// What Begin worked out for the font being drawn.
Fit sFit;

// The whole file, in one read when its size is known. The stream is left as a read
// through istreambuf_iterator leaves it: failed only when the file did not open.
using port::ReadAll;

bool Load() {
  if (sTried) {
    return sLoaded;
  }
  // Not latched until there is a file: a font made before the mods are read asks
  // again (PortAddStandIns runs while the game's fonts load).
  const std::string path = PortMods::FontPath();
  if (path.empty()) {
    return false;
  }
  sTried = true;
  std::ifstream file(PortUtf8::FromUtf8(path), std::ios::binary);
  const std::vector<uint8_t> data = ReadAll(file);
  std::string error;
  if (!file || !ReadFont(data.data(), data.size(), sFont, error)) {
    PortLog::Write("[font] %s: %s\n", path.c_str(), file ? error.c_str() : "cannot read");
    sFont = {};
    return false;
  }
  GXInitTexObj(&sTexture, sFont.distance.data(), u16(sFont.width), u16(sFont.height),
               static_cast<GXTexFmt>(GX_TF_R8_PC), GX_CLAMP, GX_CLAMP, GX_FALSE);
  GXInitTexObjLOD(&sTexture, GX_LINEAR, GX_LINEAR, 0.f, 0.f, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);
  PortLog::Write("[font] %s: %zu glyphs, %ux%u\n", path.c_str(), sFont.glyphs.size(), sFont.width, sFont.height);
  sLoaded = true;
  return true;
}

// One of the palette's RGB5A3 entries, which are stored big endian.
GXColor PaletteColor(const CGraphicsPalette* palette, int index, const GXColor& tint) {
  GXColor color = {255, 255, 255, 255};
  if (palette != nullptr && palette->GetTlutFmt() == GX_TL_RGB5A3) {
    const uint8_t* const bytes = reinterpret_cast<const uint8_t*>(palette->GetPaletteData() + index);
    const unsigned value = unsigned(bytes[0]) << 8 | bytes[1];
    if (value & 0x8000) {
      color.r = u8(((value >> 10) & 31) * 255 / 31);
      color.g = u8(((value >> 5) & 31) * 255 / 31);
      color.b = u8((value & 31) * 255 / 31);
    } else {
      color.a = u8(((value >> 12) & 7) * 255 / 7);
      color.r = u8(((value >> 8) & 15) * 17);
      color.g = u8(((value >> 4) & 15) * 17);
      color.b = u8((value & 15) * 17);
    }
  }
  color.r = u8(color.r * tint.r / 255);
  color.g = u8(color.g * tint.g / 255);
  color.b = u8(color.b * tint.b / 255);
  color.a = u8(color.a * tint.a / 255);
  return color;
}

}  // namespace

bool Enabled() {
  if (sEnabled < 0) {
    sEnabled = port::EnvFlag("MP_HD_FONT", true) ? 1 : 0;
  }
  return sEnabled != 0 && !PortDebug::OriginalExperience();
}

void SetEnabled(bool enabled) { sEnabled = enabled ? 1 : 0; }

// The disc's fonts cut from the typeface the distance field holds.
bool SameTypeface(const CRasterFont& font) { return std::strstr(font.PortGetName(), "Deface") != nullptr; }

float ModAdvanceRatio(uint32_t character, uint32_t base) {
  return Enabled() && Load() ? AdvanceRatio(sFont, character, base) : 1.f;
}

void Reset() {
  if (sLoaded) {
    GXDestroyTexObj(&sTexture);
  }
  sTried = false;
  sLoaded = false;
  sFont = {};
}

bool Begin(const CRasterFont& font) {
  if (!Enabled() || !SameTypeface(font) || !Load()) {
    return false;
  }
  // Both fonts are measured by a capital, or a digit in the font that has digits only.
  const CGlyph* reference = font.GetGlyph(L'H');
  if (reference == nullptr) {
    reference = font.GetGlyph(L'0');
  }
  if (reference == nullptr || !FitFont(sFont, reference->GetCellHeight(), reference->GetBaseLine(),
                                       font.GetMode() == kFM_OneLayerOutline, sFit)) {
    return false;
  }

  static const GXVtxDescList skDescList[] = {
      {GX_VA_POS, GX_DIRECT},
      {GX_VA_TEX0, GX_DIRECT},
      {GX_VA_NULL, GX_NONE},
  };
  CTexture::InvalidateTexmap(GX_TEXMAP0);
  GXLoadTexObj(&sTexture, GX_TEXMAP0);
  GXSetSDF(sFit.edge);
  // The sample's colour is the ink's coverage and its alpha the outline's. Stage 0 picks
  // between the outline (register 1) and the fill (register 0) by the ink, with the ink
  // swapped into the alpha too; stage 1 fades the result out past the outline.
  CGX::SetTevColorIn(GX_TEVSTAGE0, GX_CC_C1, GX_CC_C0, GX_CC_TEXC, GX_CC_ZERO);
  CGX::SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_A1, GX_CA_A0, GX_CA_TEXA, GX_CA_ZERO);
  CGX::SetStandardTevColorAlphaOp(GX_TEVSTAGE0);
  CGX::SetTevDirect(GX_TEVSTAGE0);
  CGX::SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
  GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP1);
  CGX::SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
  CGX::SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_APREV, GX_CA_TEXA, GX_CA_ZERO);
  CGX::SetStandardTevColorAlphaOp(GX_TEVSTAGE1);
  CGX::SetTevDirect(GX_TEVSTAGE1);
  CGX::SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
  GXSetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);
  CGX::SetVtxDescv(skDescList);
  CGX::SetNumChans(0);
  CGX::SetNumTexGens(1);
  CGX::SetNumTevStages(2);
  CGX::SetNumIndStages(0);
  CGX::SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
  return true;
}

bool DrawGlyph(const CRasterFont& font, const CGraphicsPalette* palette, int chr, int x, int y,
               const GXColor& tint) {
  const Glyph* const glyph = sFont.Find(uint32_t(chr) & 0xFFFF);
  const CGlyph* const cell = font.GetGlyph(wchar_t(chr));
  if (glyph == nullptr || cell == nullptr) {
    return false;
  }
  const Box box = GlyphBox(*glyph, sFit, float(x), float(y), cell->GetCellWidth(), cell->GetBaseLine());
  GXSetTevColor(GX_TEVREG0, PaletteColor(palette, 1, tint));
  GXSetTevColor(GX_TEVREG1, PaletteColor(palette, 2, tint));
  CGX::Begin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
  GXPosition3f32(box.left, 0.f, box.top);
  GXTexCoord2f32(glyph->u0, glyph->v0);
  GXPosition3f32(box.right, 0.f, box.top);
  GXTexCoord2f32(glyph->u1, glyph->v0);
  GXPosition3f32(box.left, 0.f, box.bottom);
  GXTexCoord2f32(glyph->u0, glyph->v1);
  GXPosition3f32(box.right, 0.f, box.bottom);
  GXTexCoord2f32(glyph->u1, glyph->v1);
  CGX::End();
  return true;
}

void End() {
  GXSetSDF(0);
  GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
  CTexture::InvalidateTexmap(GX_TEXMAP0);
}

}  // namespace PortHdFont
