// The importer's image filtering (port_remastered_image.h): colour maps are
// mipped in linear light, normal maps stay unit length, data maps are untouched,
// and the emissive scale is applied in linear light.

#include "port_remastered_image.h"
#include "port_test.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace PortRemastered;

namespace {

// A one-texel checker of black and white, opaque.
Image Checker(int size) {
  Image img;
  img.width = img.height = size;
  img.rgba.resize(size_t(size) * size_t(size) * 4);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const uint8_t v = (x + y) & 1 ? 255 : 0;
      uint8_t* p = &img.rgba[(size_t(y) * size_t(size) + size_t(x)) * 4];
      p[0] = p[1] = p[2] = v;
      p[3] = 255;
    }
  }
  return img;
}

// Reads a mip level's RGBA8 texel 0 out of a TXTR written by EncodeTxtrRgba8
// (format 9): the header is 12 bytes, then the top level's AR/GB block pairs.
// A solid-valued level lets any block do, so this reads the last level.
uint8_t LastLevelRed(const std::vector<uint8_t>& txtr, int topSize, int minSize) {
  size_t offset = 12;
  int side = topSize;
  size_t last = offset;
  while (true) {
    last = offset;
    offset += size_t(side) * size_t(side) * 4;
    if (side <= minSize) {
      break;
    }
    side /= 2;
  }
  return txtr[last + 1];  // block 0: sixteen (A, R) pairs, so byte 1 is R
}

}  // namespace

int main() {
  // Colour: half black and half white averages to 188 (the linear mean of the two
  // is 0.5, which is 188 as sRGB), not the 128 the bytes' mean gives.
  {
    const Image half = Resize(Checker(64), 32, 32, MapKind::Colour);
    int minR = 255, maxR = 0;
    // Away from the border, where the filter is not cut short.
    for (int y = 8; y < 24; ++y) {
      for (int x = 8; x < 24; ++x) {
        const int r = half.rgba[(size_t(y) * 32 + size_t(x)) * 4];
        minR = std::min(minR, r);
        maxR = std::max(maxR, r);
      }
    }
    CHECK_MSG(minR >= 186 && maxR <= 190, "checker as colour halves to about 188");
    const Image data = Resize(Checker(64), 32, 32, MapKind::Data);
    const int d = data.rgba[(16 * 32 + 16) * 4];
    CHECK_MSG(d >= 125 && d <= 131, "checker as data halves to about 128");
    CHECK_MSG(half.rgba[3] == 255, "alpha stays opaque");
  }

  // Colour alpha stays linear: a 0/255 alpha checker means 128, not 188.
  {
    Image img = Checker(64);
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
      img.rgba[i + 3] = img.rgba[i];
    }
    const Image half = Resize(img, 32, 32, MapKind::Colour);
    const int a = half.rgba[(16 * 32 + 16) * 4 + 3];
    CHECK_MSG(a >= 125 && a <= 131, "colour alpha is filtered as it is");
  }

  // The mip chain of a colour map: the smallest level of the checker is 188.
  {
    const std::vector<uint8_t> txtr = EncodeTxtrRgba8(Checker(32), 8, MapKind::Colour);
    const int r = LastLevelRed(txtr, 32, 8);
    CHECK_MSG(r >= 184 && r <= 192, "colour mip chain ends near 188");
    const int d = LastLevelRed(EncodeTxtrRgba8(Checker(32), 8, MapKind::Data), 32, 8);
    CHECK_MSG(d >= 120 && d <= 136, "data mip chain ends near 128");
  }

  // Normals: a field of tilts that cancel (+x and -x on alternate texels) averages
  // to a straight-out normal, and any filtered texel is a unit vector.
  {
    Image n;
    n.width = n.height = 64;
    n.rgba.resize(64 * 64 * 4);
    for (int y = 0; y < 64; ++y) {
      for (int x = 0; x < 64; ++x) {
        uint8_t* p = &n.rgba[(size_t(y) * 64 + size_t(x)) * 4];
        const float tilt = (x & 1 ? 0.6f : -0.6f) + 0.2f * float(y & 1);
        const float ty = 0.3f;
        const float z = std::sqrt(1.0f - tilt * tilt - ty * ty);
        p[0] = uint8_t(std::lround((tilt * 0.5f + 0.5f) * 255.0f));
        p[1] = uint8_t(std::lround((ty * 0.5f + 0.5f) * 255.0f));
        p[2] = uint8_t(std::lround((z * 0.5f + 0.5f) * 255.0f));
        p[3] = 255;
      }
    }
    for (const bool withZ : {true, false}) {
      Image src = n;
      if (!withZ) {
        for (size_t i = 2; i < src.rgba.size(); i += 4) {
          src.rgba[i] = 0;
        }
      }
      const Image half = Resize(src, 16, 16, MapKind::Normal);
      float worst = 0.0f;
      for (size_t i = 0; i < half.rgba.size(); i += 4) {
        const float x = half.rgba[i] / 255.0f * 2.0f - 1.0f, y = half.rgba[i + 1] / 255.0f * 2.0f - 1.0f;
        // With z in the map, all three channels are the vector; without, z is what
        // x and y leave, so the vector is unit length by construction and the
        // test is that x and y fit inside the unit circle.
        const float z = withZ ? half.rgba[i + 2] / 255.0f * 2.0f - 1.0f : std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
        worst = std::max(worst, std::abs(std::sqrt(x * x + y * y + z * z) - 1.0f));
        if (!withZ) {
          CHECK_MSG(half.rgba[i + 2] == 0, "a two-channel normal keeps B at 0");
        }
      }
      CHECK_MSG(worst < 0.02f, withZ ? "normal mip is unit length" : "two-channel normal mip fits the unit circle");
      // The tilt in x cancels; y survives at full length because it was constant.
      const float cx = half.rgba[(8 * 16 + 8) * 4] / 255.0f * 2.0f - 1.0f;
      CHECK_MSG(std::abs(cx) < 0.15f, "opposed tilts average out");
    }
    // Without the renormalising, plain byte filtering of this map would give a
    // vector clearly shorter than 1.
    const Image plain = Resize(n, 16, 16, MapKind::Data);
    const size_t at = (8 * 16 + 8) * 4;
    const float px = plain.rgba[at] / 255.0f * 2.0f - 1.0f, py = plain.rgba[at + 1] / 255.0f * 2.0f - 1.0f,
                pz = plain.rgba[at + 2] / 255.0f * 2.0f - 1.0f;
    CHECK_MSG(std::sqrt(px * px + py * py + pz * pz) < 0.97f, "byte filtering shortens normals (the case this fixes)");
  }

  // A flat normal map stays flat through every level of a BC5 chain.
  {
    Image n;
    n.width = n.height = 16;
    n.rgba.assign(16 * 16 * 4, 0);
    for (size_t i = 0; i < n.rgba.size(); i += 4) {
      n.rgba[i] = n.rgba[i + 1] = 128;
      n.rgba[i + 3] = 255;
    }
    const Image half = Resize(n, 8, 8, MapKind::Normal);
    CHECK_MSG(half.rgba[0] == 128 && half.rgba[1] == 128 && half.rgba[2] == 0, "a flat normal stays flat");
  }

  // Emissive scale: in linear light. 0.1 of mid grey (188 as sRGB is 0.5 linear)
  // is 0.05 linear, which is sRGB 63; scaling the bytes would give 19.
  {
    const int v = ScaleSrgbByte(188, 0.1);
    CHECK_MSG(v >= 61 && v <= 65, "emissive scale in linear light");
    CHECK_MSG(ScaleSrgbByte(0, 0.1) == 0, "black stays black");
    CHECK_MSG(ScaleSrgbByte(255, 1.0) == 255 && ScaleSrgbByte(77, 1.0) == 77, "scale 1 is the identity");
    CHECK_MSG(ScaleSrgbByte(255, 4.0) == 255, "scaling up clamps");
  }

  // The sRGB round trip is exact for every byte.
  {
    bool exact = true;
    for (int i = 0; i < 256; ++i) {
      exact = exact && ScaleSrgbByte(uint8_t(i), 1.0) == i;
    }
    CHECK_MSG(exact, "sRGB byte round trip");
  }

  // HDR: BC6H blocks pass through to a DX10 .dds with the BC6H DXGI code, every mip as given.
  {
    const auto le32 = [](const std::vector<uint8_t>& d, size_t at) {
      return uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24;
    };
    std::vector<std::vector<uint8_t>> mips;
    for (uint32_t w = 16, h = 8; w > 0 || h > 0; w >>= 1, h >>= 1) {
      const size_t blocks = size_t((std::max(w, 1u) + 3) / 4) * ((std::max(h, 1u) + 3) / 4);
      mips.emplace_back(blocks * 16, uint8_t(mips.size() + 1));
      if (w <= 1 && h <= 1) {
        break;
      }
    }
    CHECK_MSG(mips.size() == 5, "16x8 has five levels");
    const std::vector<uint8_t> dds = EncodeDdsBc6h(16, 8, false, mips);
    CHECK_MSG(dds.size() == 148 + (8 + 2 + 1 + 1 + 1) * 16, "header plus every level's blocks");
    if (dds.size() >= 148) {
      CHECK_MSG(dds[0] == 'D' && dds[84] == 'D' && dds[85] == 'X' && dds[86] == '1' && dds[87] == '0', "DX10 FourCC");
      CHECK_MSG(le32(dds, 12) == 8 && le32(dds, 16) == 16 && le32(dds, 20) == 8 * 16 && le32(dds, 28) == 5,
                "height, width, linear size, mip count");
      CHECK_MSG(le32(dds, 128) == 95 && le32(dds, 132) == 3 && le32(dds, 140) == 1, "BC6H_UF16, 2D, one");
      CHECK_MSG(dds[148] == 1 && dds[148 + 128] == 2 && dds.back() == 5, "the levels in order, as given");
    }
    const std::vector<uint8_t> sf = EncodeDdsBc6h(16, 8, true, mips);
    CHECK_MSG(sf.size() > 148 && le32(sf, 128) == 96, "signed: BC6H_SF16");
    mips[1].pop_back();
    CHECK_MSG(EncodeDdsBc6h(16, 8, false, mips).empty(), "a level of the wrong size writes nothing");

    // Without BC: RGBA16F, DXGI 10, with a pitch rather than a linear size.
    std::vector<std::vector<uint16_t>> half{std::vector<uint16_t>(4 * 2 * 4, 0x3C00)};
    half[0][0] = 0x4800;  // 8.0: kept over 1
    half.push_back(HalveRgba16f(half[0], 4, 2));
    half.push_back(HalveRgba16f(half[1], 2, 1));
    CHECK_MSG(half[1].size() == 2 * 1 * 4 && half[2].size() == 4, "halved levels' sizes");
    CHECK_MSG(half[1][0] == 0x4180 && half[1][4] == 0x3C00, "a 2x2 box in float: (8 + 1 + 1 + 1) / 4 = 2.75");
    const std::vector<uint8_t> f16 = EncodeDdsRgba16f(4, 2, half);
    CHECK_MSG(f16.size() == 148 + (8 + 2 + 1) * 8, "RGBA16F header plus levels");
    if (f16.size() >= 152) {
      CHECK_MSG(le32(f16, 128) == 10 && le32(f16, 20) == 4 * 8 && (le32(f16, 8) & 0x8) != 0, "DXGI 10, pitch");
      CHECK_MSG(f16[148] == 0x00 && f16[149] == 0x48, "the halves little-endian, unclamped");
    }
  }

  if (port_test::Failures() == 0) {
    std::printf("port_remastered_image_tests: all passed\n");
  }
  return PORT_TEST_RESULT();
}
