// bcdec_rs 0.1.2, ported for the Remastered TXTR reader (port_remastered_txtr.cpp):
// the BC1/BC2/BC3/BC4/BC5/BC6H/BC7 block decoders, written as they are there,
// "approximate" integer expansions included, so the pixels match retrotool's.
//
// tegra_swizzle and bcdec_rs are MIT licensed (Copyright (c) 2017-2018 IOrange,
// Copyright (c) 2016-2017 Smith Lin, Copyright (c) 2021 Nathan Kelso, Copyright
// (c) 2020-2021 hds).

#include "port_remastered_txtr_codecs.h"

#include <algorithm>
#include <cstring>

namespace PortRemastered {
namespace TxtrDetail {

// ---------------------------------------------------------------------------
// bcdec_rs 0.1.2: the BC block decoders
// ---------------------------------------------------------------------------

namespace {

// bcdec's Bitstream: a little endian bit reader over the block's 16 bytes.
struct BitStream {
  uint64_t low = 0;
  uint64_t high = 0;

  uint32_t ReadBits(uint32_t numBits) {
    if (numBits == 0) {
      return 0;
    }
    const uint64_t mask = numBits >= 64 ? ~uint64_t(0) : ((uint64_t(1) << numBits) - 1);
    const uint32_t bits = uint32_t(low & mask);
    low >>= numBits;
    low |= (high & mask) << (64 - numBits);
    high >>= numBits;
    return bits;
  }

  uint32_t ReadBit() { return ReadBits(1); }

  // BC6H reads a few fields with their bits reversed, as the format defines them.
  int32_t ReadBitsReversed(uint32_t numBits) {
    int32_t bits = int32_t(ReadBits(numBits));
    int32_t result = 0;
    for (uint32_t i = 0; i < numBits; ++i) {
      result <<= 1;
      result |= bits & 1;
      bits >>= 1;
    }
    return result;
  }
};

}  // namespace

// The colour half of BC1, BC2 and BC3, 8 bytes to sixteen RGBA texels.
// `onlyOpaqueMode` forces the four colour mode of BC1 even when the endpoints
// are in the wrong order, which BC2 and BC3 do because their alpha is stored
// separately, and which BC1 uses only when they are not.
void DecodeColorBlock(const uint8_t* src, uint8_t* dst, bool onlyOpaqueMode) {
  uint8_t refColors[4][4];
  const uint32_t c0 = uint32_t(src[0]) | (uint32_t(src[1]) << 8);
  const uint32_t c1 = uint32_t(src[2]) | (uint32_t(src[3]) << 8);
  const uint32_t r0 = (c0 >> 11) & 0x1F, g0 = (c0 >> 5) & 0x3F, b0 = c0 & 0x1F;
  const uint32_t r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
  refColors[0][0] = uint8_t((r0 * 527 + 23) >> 6);
  refColors[0][1] = uint8_t((g0 * 259 + 33) >> 6);
  refColors[0][2] = uint8_t((b0 * 527 + 23) >> 6);
  refColors[0][3] = 255;
  refColors[1][0] = uint8_t((r1 * 527 + 23) >> 6);
  refColors[1][1] = uint8_t((g1 * 259 + 33) >> 6);
  refColors[1][2] = uint8_t((b1 * 527 + 23) >> 6);
  refColors[1][3] = 255;
  if (c0 > c1 || onlyOpaqueMode) {
    refColors[2][0] = uint8_t(((2 * r0 + r1) * 351 + 61) >> 7);
    refColors[2][1] = uint8_t(((2 * g0 + g1) * 2763 + 1039) >> 11);
    refColors[2][2] = uint8_t(((2 * b0 + b1) * 351 + 61) >> 7);
    refColors[2][3] = 255;
    refColors[3][0] = uint8_t(((r0 + r1 * 2) * 351 + 61) >> 7);
    refColors[3][1] = uint8_t(((g0 + g1 * 2) * 2763 + 1039) >> 11);
    refColors[3][2] = uint8_t(((b0 + b1 * 2) * 351 + 61) >> 7);
    refColors[3][3] = 255;
  } else {
    refColors[2][0] = uint8_t(((r0 + r1) * 1053 + 125) >> 8);
    refColors[2][1] = uint8_t(((g0 + g1) * 4145 + 1019) >> 11);
    refColors[2][2] = uint8_t(((b0 + b1) * 1053 + 125) >> 8);
    refColors[2][3] = 255;
    refColors[3][0] = 0;
    refColors[3][1] = 0;
    refColors[3][2] = 0;
    refColors[3][3] = 0;
  }
  uint32_t indices = uint32_t(src[4]) | (uint32_t(src[5]) << 8) | (uint32_t(src[6]) << 16) |
                     (uint32_t(src[7]) << 24);
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 4; ++j) {
      std::memcpy(dst + (i * 4 + j) * 4, refColors[indices & 3], 4);
      indices >>= 2;
    }
  }
}

// BC2's alpha: four bits per texel, scaled to eight by 17.
void DecodeSharpAlpha(const uint8_t* src, uint8_t* dst) {
  for (size_t i = 0; i < 4; ++i) {
    const uint32_t alpha = uint32_t(src[i * 2]) | (uint32_t(src[i * 2 + 1]) << 8);
    for (size_t j = 0; j < 4; ++j) {
      dst[(i * 4 + j) * 4 + 3] = uint8_t(((alpha >> (4 * j)) & 0x0F) * 17);
    }
  }
}

// BC3 and BC4's alpha: two endpoints and six (or five) interpolated values,
// written into one channel every `pixelSize` bytes.
void DecodeSmoothAlpha(const uint8_t* src, uint8_t* dst, size_t pixelSize, size_t channel) {
  uint32_t alpha[8];
  alpha[0] = src[0];
  alpha[1] = src[1];
  if (alpha[0] > alpha[1]) {
    for (uint32_t i = 2; i < 8; ++i) {
      alpha[i] = ((8 - i) * alpha[0] + (i - 1) * alpha[1] + 1) / 7;
    }
  } else {
    for (uint32_t i = 2; i < 6; ++i) {
      alpha[i] = ((6 - i) * alpha[0] + (i - 1) * alpha[1] + 1) / 5;
    }
    alpha[6] = 0x00;
    alpha[7] = 0xFF;
  }
  uint64_t indices = 0;
  for (size_t i = 2; i < 8; ++i) {
    indices |= uint64_t(src[i]) << ((i - 2) * 8);
  }
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 4; ++j) {
      dst[(i * 4 + j) * pixelSize + channel] = uint8_t(alpha[indices & 7]);
      indices >>= 3;
    }
  }
}

namespace {

// BC6H's 32 partition sets, as one byte per texel. The high bit marks the two
// fix-up indices, whose index field is one bit shorter than the rest.
const uint8_t kBc6hPartitionSets[32][4][4] = {
    {{128, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 129}},
    {{128, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 129}},
    {{128, 1, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 129}},
    {{128, 0, 0, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 1, 1, 129}},
    {{128, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 1, 129}},
    {{128, 0, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 129}},
    {{128, 0, 0, 1}, {0, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 129}},
    {{128, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 1, 1}, {0, 1, 1, 129}},
    {{128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 1, 129}},
    {{128, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 129}},
    {{128, 0, 0, 0}, {0, 0, 0, 1}, {0, 1, 1, 1}, {1, 1, 1, 129}},
    {{128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 1}, {0, 1, 1, 129}},
    {{128, 0, 0, 1}, {0, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 129}},
    {{128, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 1}, {1, 1, 1, 129}},
    {{128, 0, 0, 0}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 129}},
    {{128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 129}},
    {{128, 0, 0, 0}, {1, 0, 0, 0}, {1, 1, 1, 0}, {1, 1, 1, 129}},
    {{128, 1, 129, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {{128, 0, 0, 0}, {0, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 1, 0}},
    {{128, 1, 129, 1}, {0, 0, 1, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}},
    {{128, 0, 129, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {{128, 0, 0, 0}, {1, 0, 0, 0}, {129, 1, 0, 0}, {1, 1, 1, 0}},
    {{128, 0, 0, 0}, {0, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 0, 0}},
    {{128, 1, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 0, 129}},
    {{128, 0, 129, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}},
    {{128, 0, 0, 0}, {1, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 0, 0}},
    {{128, 1, 129, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}},
    {{128, 0, 129, 1}, {0, 1, 1, 0}, {0, 1, 1, 0}, {1, 1, 0, 0}},
    {{128, 0, 0, 1}, {0, 1, 1, 1}, {129, 1, 1, 0}, {1, 0, 0, 0}},
    {{128, 0, 0, 0}, {1, 1, 1, 1}, {129, 1, 1, 1}, {0, 0, 0, 0}},
    {{128, 1, 129, 1}, {0, 0, 0, 1}, {1, 0, 0, 0}, {1, 1, 1, 0}},
    {{128, 0, 129, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}, {1, 1, 0, 0}},
};

const uint8_t kBc6hActualBits[4][14] = {
    {10, 7, 11, 11, 11, 9, 8, 8, 8, 6, 10, 11, 12, 16},  // W
    {5, 6, 5, 4, 4, 5, 6, 5, 5, 6, 10, 9, 8, 4},         // dR
    {5, 6, 4, 5, 4, 5, 5, 6, 5, 6, 10, 9, 8, 4},         // dG
    {5, 6, 4, 4, 5, 5, 5, 5, 6, 6, 10, 9, 8, 4},         // dB
};

int32_t ExtendSign(int32_t value, int32_t bits) { return (value << (32 - bits)) >> (32 - bits); }

int32_t TransformInverse(int32_t value, int32_t base, int32_t bits, bool isSigned) {
  int32_t result = (value + base) & ((1 << bits) - 1);
  if (isSigned) {
    result = ExtendSign(result, bits);
  }
  return result;
}

int32_t Unquantize(int32_t value, int32_t bits, bool isSigned) {
  if (!isSigned) {
    if (bits >= 15) {
      return value;
    }
    if (value == 0) {
      return 0;
    }
    if (value == (1 << bits) - 1) {
      return 0xFFFF;
    }
    return ((value << 16) + 0x8000) >> bits;
  }
  // The signed form is the magnitude unquantized to a 16 bit half float and
  // then negated, with the largest magnitude saturated.
  int32_t sign = 0;
  int32_t magnitude = value;
  if (bits < 16 && magnitude < 0) {
    sign = 1;
    magnitude = -magnitude;
  }
  int32_t result;
  if (magnitude == 0) {
    result = 0;
  } else if (magnitude >= ((1 << (bits - 1)) - 1)) {
    result = 0x7FFF;
  } else {
    result = ((magnitude << 15) + 0x4000) >> (bits - 1);
  }
  return sign != 0 ? -result : result;
}

int32_t InterpolateInt(int32_t a, int32_t b, int32_t weight) {
  return (a * (64 - weight) + b * weight + 32) >> 6;
}

uint16_t FinishUnquantize(int32_t value, bool isSigned) {
  if (!isSigned) {
    return uint16_t((value * 31) >> 6);
  }
  int32_t scaled = value < 0 ? -((-value * 31) >> 5) : ((value * 31) >> 5);
  uint32_t sign = 0;
  if (scaled < 0) {
    sign = 0x8000;
    scaled = -scaled;
  }
  return uint16_t(sign | uint32_t(scaled));
}

}  // namespace

// BC6H: 16 bytes to sixteen RGB texels of half precision. The 8 bit image
// reduces them; the HDR cube reader keeps them as they are. Every mode stores its endpoints in its own bit layout, so
// the mode decode is mostly a long list of bit field reads in a fixed order.
void DecodeBc6h(const uint8_t* src, uint16_t* dst, bool isSigned) {
  const uint32_t weight3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
  const uint32_t weight4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

  BitStream stream;
  for (size_t i = 0; i < 8; ++i) {
    stream.low |= uint64_t(src[i]) << (8 * i);
    stream.high |= uint64_t(src[i + 8]) << (8 * i);
  }

  // The endpoints of each of the four components, held as w, x, y, z.
  int32_t r[4] = {0, 0, 0, 0};
  int32_t g[4] = {0, 0, 0, 0};
  int32_t b[4] = {0, 0, 0, 0};

  uint32_t mode = stream.ReadBits(2);
  if (mode > 1) {
    mode |= stream.ReadBits(3) << 2;
  }
  uint32_t partition = 0;

  switch (mode) {
    case 0b00:
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[2] |= int32_t(stream.ReadBit()) << 4;
      b[3] |= int32_t(stream.ReadBit()) << 4;
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(5));
      g[3] |= int32_t(stream.ReadBit()) << 4;
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit());
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 2;
      r[3] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 3;
      partition = stream.ReadBits(5);
      mode = 0;
      break;
    case 0b01:
      g[2] |= int32_t(stream.ReadBit()) << 5;
      g[3] |= int32_t(stream.ReadBit()) << 4;
      g[3] |= int32_t(stream.ReadBit()) << 5;
      r[0] |= int32_t(stream.ReadBits(7));
      b[3] |= int32_t(stream.ReadBit());
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBit()) << 4;
      g[0] |= int32_t(stream.ReadBits(7));
      b[2] |= int32_t(stream.ReadBit()) << 5;
      b[3] |= int32_t(stream.ReadBit()) << 2;
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[0] |= int32_t(stream.ReadBits(7));
      b[3] |= int32_t(stream.ReadBit()) << 3;
      b[3] |= int32_t(stream.ReadBit()) << 5;
      b[3] |= int32_t(stream.ReadBit()) << 4;
      r[1] |= int32_t(stream.ReadBits(6));
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(6));
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(6));
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(6));
      r[3] |= int32_t(stream.ReadBits(6));
      partition = stream.ReadBits(5);
      mode = 1;
      break;
    case 0b00010:
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(5));
      r[0] |= int32_t(stream.ReadBit()) << 10;
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(4));
      g[0] |= int32_t(stream.ReadBit()) << 10;
      b[3] |= int32_t(stream.ReadBit());
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(4));
      b[0] |= int32_t(stream.ReadBit()) << 10;
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 2;
      r[3] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 3;
      partition = stream.ReadBits(5);
      mode = 2;
      break;
    case 0b00110:
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(4));
      r[0] |= int32_t(stream.ReadBit()) << 10;
      g[3] |= int32_t(stream.ReadBit()) << 4;
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(5));
      g[0] |= int32_t(stream.ReadBit()) << 10;
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(4));
      b[0] |= int32_t(stream.ReadBit()) << 10;
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(4));
      b[3] |= int32_t(stream.ReadBit());
      b[3] |= int32_t(stream.ReadBit()) << 2;
      r[3] |= int32_t(stream.ReadBits(4));
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[3] |= int32_t(stream.ReadBit()) << 3;
      partition = stream.ReadBits(5);
      mode = 3;
      break;
    case 0b01010:
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(4));
      r[0] |= int32_t(stream.ReadBit()) << 10;
      b[2] |= int32_t(stream.ReadBit()) << 4;
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(4));
      g[0] |= int32_t(stream.ReadBit()) << 10;
      b[3] |= int32_t(stream.ReadBit());
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(5));
      b[0] |= int32_t(stream.ReadBit()) << 10;
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(4));
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[3] |= int32_t(stream.ReadBit()) << 2;
      r[3] |= int32_t(stream.ReadBits(4));
      b[3] |= int32_t(stream.ReadBit()) << 4;
      b[3] |= int32_t(stream.ReadBit()) << 3;
      partition = stream.ReadBits(5);
      mode = 4;
      break;
    case 0b01110:
      r[0] |= int32_t(stream.ReadBits(9));
      b[2] |= int32_t(stream.ReadBit()) << 4;
      g[0] |= int32_t(stream.ReadBits(9));
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[0] |= int32_t(stream.ReadBits(9));
      b[3] |= int32_t(stream.ReadBit()) << 4;
      r[1] |= int32_t(stream.ReadBits(5));
      g[3] |= int32_t(stream.ReadBit()) << 4;
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit());
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 2;
      r[3] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 3;
      partition = stream.ReadBits(5);
      mode = 5;
      break;
    case 0b10010:
      r[0] |= int32_t(stream.ReadBits(8));
      g[3] |= int32_t(stream.ReadBit()) << 4;
      b[2] |= int32_t(stream.ReadBit()) << 4;
      g[0] |= int32_t(stream.ReadBits(8));
      b[3] |= int32_t(stream.ReadBit()) << 2;
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[0] |= int32_t(stream.ReadBits(8));
      b[3] |= int32_t(stream.ReadBit()) << 3;
      b[3] |= int32_t(stream.ReadBit()) << 4;
      r[1] |= int32_t(stream.ReadBits(6));
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit());
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(6));
      r[3] |= int32_t(stream.ReadBits(6));
      partition = stream.ReadBits(5);
      mode = 6;
      break;
    case 0b10110:
      r[0] |= int32_t(stream.ReadBits(8));
      b[3] |= int32_t(stream.ReadBit());
      b[2] |= int32_t(stream.ReadBit()) << 4;
      g[0] |= int32_t(stream.ReadBits(8));
      g[2] |= int32_t(stream.ReadBit()) << 5;
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[0] |= int32_t(stream.ReadBits(8));
      g[3] |= int32_t(stream.ReadBit()) << 5;
      b[3] |= int32_t(stream.ReadBit()) << 4;
      r[1] |= int32_t(stream.ReadBits(5));
      g[3] |= int32_t(stream.ReadBit()) << 4;
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(6));
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 2;
      r[3] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 3;
      partition = stream.ReadBits(5);
      mode = 7;
      break;
    case 0b11010:
      r[0] |= int32_t(stream.ReadBits(8));
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBit()) << 4;
      g[0] |= int32_t(stream.ReadBits(8));
      b[2] |= int32_t(stream.ReadBit()) << 5;
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[0] |= int32_t(stream.ReadBits(8));
      b[3] |= int32_t(stream.ReadBit()) << 5;
      b[3] |= int32_t(stream.ReadBit()) << 4;
      r[1] |= int32_t(stream.ReadBits(5));
      g[3] |= int32_t(stream.ReadBit()) << 4;
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit());
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(6));
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 2;
      r[3] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 3;
      partition = stream.ReadBits(5);
      mode = 8;
      break;
    case 0b11110:
      r[0] |= int32_t(stream.ReadBits(6));
      g[3] |= int32_t(stream.ReadBit()) << 4;
      b[3] |= int32_t(stream.ReadBit());
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBit()) << 4;
      g[0] |= int32_t(stream.ReadBits(6));
      g[2] |= int32_t(stream.ReadBit()) << 5;
      b[2] |= int32_t(stream.ReadBit()) << 5;
      b[3] |= int32_t(stream.ReadBit()) << 2;
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[0] |= int32_t(stream.ReadBits(6));
      g[3] |= int32_t(stream.ReadBit()) << 5;
      b[3] |= int32_t(stream.ReadBit()) << 3;
      b[3] |= int32_t(stream.ReadBit()) << 5;
      b[3] |= int32_t(stream.ReadBit()) << 4;
      r[1] |= int32_t(stream.ReadBits(6));
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(6));
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(6));
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(6));
      r[3] |= int32_t(stream.ReadBits(6));
      partition = stream.ReadBits(5);
      mode = 9;
      break;
    case 0b00011:
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(10));
      g[1] |= int32_t(stream.ReadBits(10));
      b[1] |= int32_t(stream.ReadBits(10));
      mode = 10;
      break;
    case 0b00111:
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(9));
      r[0] |= int32_t(stream.ReadBit()) << 10;
      g[1] |= int32_t(stream.ReadBits(9));
      g[0] |= int32_t(stream.ReadBit()) << 10;
      b[1] |= int32_t(stream.ReadBits(9));
      b[0] |= int32_t(stream.ReadBit()) << 10;
      mode = 11;
      break;
    case 0b01011:
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(8));
      r[0] |= stream.ReadBitsReversed(2) << 10;
      g[1] |= int32_t(stream.ReadBits(8));
      g[0] |= stream.ReadBitsReversed(2) << 10;
      b[1] |= int32_t(stream.ReadBits(8));
      b[0] |= stream.ReadBitsReversed(2) << 10;
      mode = 12;
      break;
    case 0b01111:
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(4));
      r[0] |= stream.ReadBitsReversed(6) << 10;
      g[1] |= int32_t(stream.ReadBits(4));
      g[0] |= stream.ReadBitsReversed(6) << 10;
      b[1] |= int32_t(stream.ReadBits(4));
      b[0] |= stream.ReadBitsReversed(6) << 10;
      mode = 13;
      break;
    default:
      // A reserved mode. The format says such a block must come out black.
      for (size_t i = 0; i < 16; ++i) {
        dst[i * 3 + 0] = 0;
        dst[i * 3 + 1] = 0;
        dst[i * 3 + 2] = 0;
      }
      return;
  }

  const uint32_t numPartitions = mode >= 10 ? 0 : 1;
  const int32_t actualBits0 = kBc6hActualBits[0][mode];
  if (isSigned) {
    r[0] = ExtendSign(r[0], actualBits0);
    g[0] = ExtendSign(g[0], actualBits0);
    b[0] = ExtendSign(b[0], actualBits0);
  }
  // Modes 11 and 12 (the last two of the fourteen) store both endpoints, and
  // mode 10 stores them too, so only the delta ones transform and sign extend.
  const bool hasDelta = mode != 9 && mode != 10;
  if (hasDelta || isSigned) {
    for (uint32_t i = 1; i < (numPartitions + 1) * 2; ++i) {
      r[i] = ExtendSign(r[i], kBc6hActualBits[1][mode]);
      g[i] = ExtendSign(g[i], kBc6hActualBits[2][mode]);
      b[i] = ExtendSign(b[i], kBc6hActualBits[3][mode]);
    }
  }
  if (hasDelta) {
    for (uint32_t i = 1; i < (numPartitions + 1) * 2; ++i) {
      r[i] = TransformInverse(r[i], r[0], actualBits0, isSigned);
      g[i] = TransformInverse(g[i], g[0], actualBits0, isSigned);
      b[i] = TransformInverse(b[i], b[0], actualBits0, isSigned);
    }
  }
  for (uint32_t i = 0; i < (numPartitions + 1) * 2; ++i) {
    r[i] = Unquantize(r[i], actualBits0, isSigned);
    g[i] = Unquantize(g[i], actualBits0, isSigned);
    b[i] = Unquantize(b[i], actualBits0, isSigned);
  }

  const uint32_t* weights = mode >= 10 ? weight4 : weight3;
  for (uint32_t i = 0; i < 4; ++i) {
    for (uint32_t j = 0; j < 4; ++j) {
      uint32_t set = mode >= 10 ? ((i | j) != 0 ? 0u : 128u)
                                : uint32_t(kBc6hPartitionSets[partition][i][j]);
      uint32_t indexBits = mode >= 10 ? 4 : 3;
      // A fix-up index is stored with one bit fewer; the first one is always 0.
      if ((set & 0x80) != 0) {
        indexBits -= 1;
      }
      set &= 1;
      const uint32_t weight = weights[stream.ReadBits(indexBits)];
      const uint32_t endpoint = set * 2;
      dst[(i * 4 + j) * 3 + 0] =
          (FinishUnquantize(InterpolateInt(r[endpoint], r[endpoint + 1], int32_t(weight)),
                                   isSigned));
      dst[(i * 4 + j) * 3 + 1] =
          (FinishUnquantize(InterpolateInt(g[endpoint], g[endpoint + 1], int32_t(weight)),
                                   isSigned));
      dst[(i * 4 + j) * 3 + 2] =
          (FinishUnquantize(InterpolateInt(b[endpoint], b[endpoint + 1], int32_t(weight)),
                                   isSigned));
    }
  }
}

namespace {

const uint8_t kBc7ActualBits[2][8] = {
    {4, 6, 5, 7, 5, 7, 7, 5},  // RGB
    {0, 0, 0, 0, 6, 8, 7, 5},  // alpha
};

// BC7's 64 partition sets for two subsets, then its 64 for three. As in BC6H
// the high bit marks a fix-up index.
const uint8_t kBc7PartitionSets[2][64][4][4] = {
    {
        {{128, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 129}},
        {{128, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 129}},
        {{128, 1, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 129}},
        {{128, 0, 0, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 1, 129}},
        {{128, 0, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 0, 1}, {0, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 1, 1}, {0, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 1, 129}},
        {{128, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 1}, {0, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 1}, {0, 1, 1, 129}},
        {{128, 0, 0, 1}, {0, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 0, 0}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 129}},
        {{128, 0, 0, 0}, {1, 0, 0, 0}, {1, 1, 1, 0}, {1, 1, 1, 129}},
        {{128, 1, 129, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 1, 0}},
        {{128, 1, 129, 1}, {0, 0, 1, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}},
        {{128, 0, 129, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{128, 0, 0, 0}, {1, 0, 0, 0}, {129, 1, 0, 0}, {1, 1, 1, 0}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 0, 0}},
        {{128, 1, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 0, 129}},
        {{128, 0, 129, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}},
        {{128, 0, 0, 0}, {1, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 0, 0}},
        {{128, 1, 129, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}},
        {{128, 0, 129, 1}, {0, 1, 1, 0}, {0, 1, 1, 0}, {1, 1, 0, 0}},
        {{128, 0, 0, 1}, {0, 1, 1, 1}, {129, 1, 1, 0}, {1, 0, 0, 0}},
        {{128, 0, 0, 0}, {1, 1, 1, 1}, {129, 1, 1, 1}, {0, 0, 0, 0}},
        {{128, 1, 129, 1}, {0, 0, 0, 1}, {1, 0, 0, 0}, {1, 1, 1, 0}},
        {{128, 0, 129, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}, {1, 1, 0, 0}},
        {{128, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 129}},
        {{128, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {1, 1, 1, 129}},
        {{128, 1, 0, 1}, {1, 0, 129, 0}, {0, 1, 0, 1}, {1, 0, 1, 0}},
        {{128, 0, 1, 1}, {0, 0, 1, 1}, {129, 1, 0, 0}, {1, 1, 0, 0}},
        {{128, 0, 129, 1}, {1, 1, 0, 0}, {0, 0, 1, 1}, {1, 1, 0, 0}},
        {{128, 1, 0, 1}, {0, 1, 0, 1}, {129, 0, 1, 0}, {1, 0, 1, 0}},
        {{128, 1, 1, 0}, {1, 0, 0, 1}, {0, 1, 1, 0}, {1, 0, 0, 129}},
        {{128, 1, 0, 1}, {1, 0, 1, 0}, {1, 0, 1, 0}, {0, 1, 0, 129}},
        {{128, 1, 129, 1}, {0, 0, 1, 1}, {1, 1, 0, 0}, {1, 1, 1, 0}},
        {{128, 0, 0, 1}, {0, 0, 1, 1}, {129, 1, 0, 0}, {1, 0, 0, 0}},
        {{128, 0, 129, 1}, {0, 0, 1, 0}, {0, 1, 0, 0}, {1, 1, 0, 0}},
        {{128, 0, 129, 1}, {1, 0, 1, 1}, {1, 1, 0, 1}, {1, 1, 0, 0}},
        {{128, 1, 129, 0}, {1, 0, 0, 1}, {1, 0, 0, 1}, {0, 1, 1, 0}},
        {{128, 0, 1, 1}, {1, 1, 0, 0}, {1, 1, 0, 0}, {0, 0, 1, 129}},
        {{128, 1, 1, 0}, {0, 1, 1, 0}, {1, 0, 0, 1}, {1, 0, 0, 129}},
        {{128, 0, 0, 0}, {0, 1, 129, 0}, {0, 1, 1, 0}, {0, 0, 0, 0}},
        {{128, 1, 0, 0}, {1, 1, 129, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}},
        {{128, 0, 129, 0}, {0, 1, 1, 1}, {0, 0, 1, 0}, {0, 0, 0, 0}},
        {{128, 0, 0, 0}, {0, 0, 129, 0}, {0, 1, 1, 1}, {0, 0, 1, 0}},
        {{128, 0, 0, 0}, {0, 1, 0, 0}, {129, 1, 1, 0}, {0, 1, 0, 0}},
        {{128, 1, 1, 0}, {1, 1, 0, 0}, {1, 0, 0, 1}, {0, 0, 1, 129}},
        {{128, 0, 1, 1}, {0, 1, 1, 0}, {1, 1, 0, 0}, {1, 0, 0, 129}},
        {{128, 1, 129, 0}, {0, 0, 1, 1}, {1, 0, 0, 1}, {1, 1, 0, 0}},
        {{128, 0, 129, 1}, {1, 0, 0, 1}, {1, 1, 0, 0}, {0, 1, 1, 0}},
        {{128, 1, 1, 0}, {1, 1, 0, 0}, {1, 1, 0, 0}, {1, 0, 0, 129}},
        {{128, 1, 1, 0}, {0, 0, 1, 1}, {0, 0, 1, 1}, {1, 0, 0, 129}},
        {{128, 1, 1, 1}, {1, 1, 1, 0}, {1, 0, 0, 0}, {0, 0, 0, 129}},
        {{128, 0, 0, 1}, {1, 0, 0, 0}, {1, 1, 1, 0}, {0, 1, 1, 129}},
        {{128, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 129}},
        {{128, 0, 129, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}},
        {{128, 0, 129, 0}, {0, 0, 1, 0}, {1, 1, 1, 0}, {1, 1, 1, 0}},
        {{128, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 1}, {0, 1, 1, 129}},
    },
    {
        {{128, 0, 1, 129}, {0, 0, 1, 1}, {0, 2, 2, 1}, {2, 2, 2, 130}},
        {{128, 0, 0, 129}, {0, 0, 1, 1}, {130, 2, 1, 1}, {2, 2, 2, 1}},
        {{128, 0, 0, 0}, {2, 0, 0, 1}, {130, 2, 1, 1}, {2, 2, 1, 129}},
        {{128, 2, 2, 130}, {0, 0, 2, 2}, {0, 0, 1, 1}, {0, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {129, 1, 2, 2}, {1, 1, 2, 130}},
        {{128, 0, 1, 129}, {0, 0, 1, 1}, {0, 0, 2, 2}, {0, 0, 2, 130}},
        {{128, 0, 2, 130}, {0, 0, 2, 2}, {1, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 1, 1}, {0, 0, 1, 1}, {130, 2, 1, 1}, {2, 2, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {129, 1, 1, 1}, {2, 2, 2, 130}},
        {{128, 0, 0, 0}, {1, 1, 1, 1}, {129, 1, 1, 1}, {2, 2, 2, 130}},
        {{128, 0, 0, 0}, {1, 1, 129, 1}, {2, 2, 2, 2}, {2, 2, 2, 130}},
        {{128, 0, 1, 2}, {0, 0, 129, 2}, {0, 0, 1, 2}, {0, 0, 1, 130}},
        {{128, 1, 1, 2}, {0, 1, 129, 2}, {0, 1, 1, 2}, {0, 1, 1, 130}},
        {{128, 1, 2, 2}, {0, 129, 2, 2}, {0, 1, 2, 2}, {0, 1, 2, 130}},
        {{128, 0, 1, 129}, {0, 1, 1, 2}, {1, 1, 2, 2}, {1, 2, 2, 130}},
        {{128, 0, 1, 129}, {2, 0, 0, 1}, {130, 2, 0, 0}, {2, 2, 2, 0}},
        {{128, 0, 0, 129}, {0, 0, 1, 1}, {0, 1, 1, 2}, {1, 1, 2, 130}},
        {{128, 1, 1, 129}, {0, 0, 1, 1}, {130, 0, 0, 1}, {2, 2, 0, 0}},
        {{128, 0, 0, 0}, {1, 1, 2, 2}, {129, 1, 2, 2}, {1, 1, 2, 130}},
        {{128, 0, 2, 130}, {0, 0, 2, 2}, {0, 0, 2, 2}, {1, 1, 1, 129}},
        {{128, 1, 1, 129}, {0, 1, 1, 1}, {0, 2, 2, 2}, {0, 2, 2, 130}},
        {{128, 0, 0, 129}, {0, 0, 0, 1}, {130, 2, 2, 1}, {2, 2, 2, 1}},
        {{128, 0, 0, 0}, {0, 0, 129, 1}, {0, 1, 2, 2}, {0, 1, 2, 130}},
        {{128, 0, 0, 0}, {1, 1, 0, 0}, {130, 2, 129, 0}, {2, 2, 1, 0}},
        {{128, 1, 2, 130}, {0, 129, 2, 2}, {0, 0, 1, 1}, {0, 0, 0, 0}},
        {{128, 0, 1, 2}, {0, 0, 1, 2}, {129, 1, 2, 2}, {2, 2, 2, 130}},
        {{128, 1, 1, 0}, {1, 2, 130, 1}, {129, 2, 2, 1}, {0, 1, 1, 0}},
        {{128, 0, 0, 0}, {0, 1, 129, 0}, {1, 2, 130, 1}, {1, 2, 2, 1}},
        {{128, 0, 2, 2}, {1, 1, 0, 2}, {129, 1, 0, 2}, {0, 0, 2, 130}},
        {{128, 1, 1, 0}, {0, 129, 1, 0}, {2, 0, 0, 2}, {2, 2, 2, 130}},
        {{128, 0, 1, 1}, {0, 1, 2, 2}, {0, 1, 130, 2}, {0, 0, 1, 129}},
        {{128, 0, 0, 0}, {2, 0, 0, 0}, {130, 2, 1, 1}, {2, 2, 2, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 2}, {129, 1, 2, 2}, {1, 2, 2, 130}},
        {{128, 2, 2, 130}, {0, 0, 2, 2}, {0, 0, 1, 2}, {0, 0, 1, 129}},
        {{128, 0, 1, 129}, {0, 0, 1, 2}, {0, 0, 2, 2}, {0, 2, 2, 130}},
        {{128, 1, 2, 0}, {0, 129, 2, 0}, {0, 1, 130, 0}, {0, 1, 2, 0}},
        {{128, 0, 0, 0}, {1, 1, 129, 1}, {2, 2, 130, 2}, {0, 0, 0, 0}},
        {{128, 1, 2, 0}, {1, 2, 0, 1}, {130, 0, 129, 2}, {0, 1, 2, 0}},
        {{128, 1, 2, 0}, {2, 0, 1, 2}, {129, 130, 0, 1}, {0, 1, 2, 0}},
        {{128, 0, 1, 1}, {2, 2, 0, 0}, {1, 1, 130, 2}, {0, 0, 1, 129}},
        {{128, 0, 1, 1}, {1, 1, 130, 2}, {2, 2, 0, 0}, {0, 0, 1, 129}},
        {{128, 1, 0, 129}, {0, 1, 0, 1}, {2, 2, 2, 2}, {2, 2, 2, 130}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {130, 1, 2, 1}, {2, 1, 2, 129}},
        {{128, 0, 2, 2}, {1, 129, 2, 2}, {0, 0, 2, 2}, {1, 1, 2, 130}},
        {{128, 0, 2, 130}, {0, 0, 1, 1}, {0, 0, 2, 2}, {0, 0, 1, 129}},
        {{128, 2, 2, 0}, {1, 2, 130, 1}, {0, 2, 2, 0}, {1, 2, 2, 129}},
        {{128, 1, 0, 1}, {2, 2, 130, 2}, {2, 2, 2, 2}, {0, 1, 0, 129}},
        {{128, 0, 0, 0}, {2, 1, 2, 1}, {130, 1, 2, 1}, {2, 1, 2, 129}},
        {{128, 1, 0, 129}, {0, 1, 0, 1}, {0, 1, 0, 1}, {2, 2, 2, 130}},
        {{128, 2, 2, 130}, {0, 1, 1, 1}, {0, 2, 2, 2}, {0, 1, 1, 129}},
        {{128, 0, 0, 2}, {1, 129, 1, 2}, {0, 0, 0, 2}, {1, 1, 1, 130}},
        {{128, 0, 0, 0}, {2, 129, 1, 2}, {2, 1, 1, 2}, {2, 1, 1, 130}},
        {{128, 2, 2, 2}, {0, 129, 1, 1}, {0, 1, 1, 1}, {0, 2, 2, 130}},
        {{128, 0, 0, 2}, {1, 1, 1, 2}, {129, 1, 1, 2}, {0, 0, 0, 130}},
        {{128, 1, 1, 0}, {0, 129, 1, 0}, {0, 1, 1, 0}, {2, 2, 2, 130}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {2, 1, 129, 2}, {2, 1, 1, 130}},
        {{128, 1, 1, 0}, {0, 129, 1, 0}, {2, 2, 2, 2}, {2, 2, 2, 130}},
        {{128, 0, 2, 2}, {0, 0, 1, 1}, {0, 0, 129, 1}, {0, 0, 2, 130}},
        {{128, 0, 2, 2}, {1, 1, 2, 2}, {129, 1, 2, 2}, {0, 0, 2, 130}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {2, 129, 1, 130}},
        {{128, 0, 0, 130}, {0, 0, 0, 1}, {0, 0, 0, 2}, {0, 0, 0, 129}},
        {{128, 2, 2, 2}, {1, 2, 2, 2}, {0, 2, 2, 2}, {129, 2, 2, 130}},
        {{128, 1, 0, 129}, {2, 2, 2, 2}, {2, 2, 2, 2}, {2, 2, 2, 130}},
        {{128, 1, 1, 129}, {2, 0, 1, 1}, {130, 2, 0, 1}, {2, 2, 2, 0}},
    },
};

uint32_t Interpolate(uint32_t a, uint32_t b, uint32_t weight) {
  return (a * (64 - weight) + b * weight + 32) >> 6;
}

}  // namespace

// BC7: 16 bytes to RGBA, one of eight modes each with its own endpoint layout.
void DecodeBc7(const uint8_t* src, uint8_t* dst) {
  const uint32_t weight2[4] = {0, 21, 43, 64};
  const uint32_t weight3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
  const uint32_t weight4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
  // Which modes have a p-bit per endpoint: 0, 3, 6 and 7. Mode 1 shares one
  // p-bit between the two endpoints of a subset.
  const uint32_t kModeHasPbits = 0xC9U;

  BitStream stream;
  for (size_t i = 0; i < 8; ++i) {
    stream.low |= uint64_t(src[i]) << (8 * i);
    stream.high |= uint64_t(src[i + 8]) << (8 * i);
  }

  uint32_t mode = 0;
  while (mode < 8 && stream.ReadBit() == 0) {
    mode += 1;
  }
  if (mode >= 8) {
    // A reserved mode comes out transparent black.
    std::memset(dst, 0, 16 * 4);
    return;
  }

  uint32_t partition = 0;
  uint32_t numPartitions = 1;
  uint32_t rotation = 0;
  uint32_t indexSelectionBit = 0;
  if (mode <= 3 || mode == 7) {
    numPartitions = (mode == 0 || mode == 2) ? 3 : 2;
    partition = stream.ReadBits(mode == 0 ? 4 : 6);
  }
  const uint32_t numEndpoints = numPartitions * 2;
  if (mode == 4 || mode == 5) {
    rotation = stream.ReadBits(2);
    if (mode == 4) {
      indexSelectionBit = stream.ReadBit();
    }
  }

  uint32_t endpoints[6][4] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    for (uint32_t e = 0; e < numEndpoints; ++e) {
      endpoints[e][i] = stream.ReadBits(kBc7ActualBits[0][mode]);
    }
  }
  if (kBc7ActualBits[1][mode] > 0) {
    for (uint32_t e = 0; e < numEndpoints; ++e) {
      endpoints[e][3] = stream.ReadBits(kBc7ActualBits[1][mode]);
    }
  }

  // Modes with a p-bit: shift the endpoints up to make room, then fold it in.
  const bool hasPbits = mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7;
  if (hasPbits) {
    for (uint32_t e = 0; e < numEndpoints; ++e) {
      for (uint32_t c = 0; c < 4; ++c) {
        endpoints[e][c] <<= 1;
      }
    }
    if (mode == 1) {
      // One p-bit shared by the first two endpoints of each channel pair.
      const uint32_t first = stream.ReadBit();
      const uint32_t second = stream.ReadBit();
      for (uint32_t c = 0; c < 3; ++c) {
        endpoints[0][c] |= first;
        endpoints[1][c] |= first;
        endpoints[2][c] |= second;
        endpoints[3][c] |= second;
      }
    } else if ((kModeHasPbits & (1u << mode)) != 0) {
      for (uint32_t e = 0; e < numEndpoints; ++e) {
        const uint32_t bit = stream.ReadBit();
        for (uint32_t c = 0; c < 4; ++c) {
          endpoints[e][c] |= bit;
        }
      }
    }
  }

  // Widen each component to 8 bits by putting its top bit at bit 7 and
  // replicating that bit into the bits the shift revealed.
  for (uint32_t e = 0; e < numEndpoints; ++e) {
    uint32_t bits = kBc7ActualBits[0][mode] + (hasPbits ? 1 : 0);
    for (uint32_t c = 0; c < 3; ++c) {
      endpoints[e][c] <<= 8 - bits;
      endpoints[e][c] |= endpoints[e][c] >> bits;
    }
    bits = kBc7ActualBits[1][mode] + (hasPbits ? 1 : 0);
    endpoints[e][3] <<= 8 - bits;
    endpoints[e][3] |= endpoints[e][3] >> bits;
  }
  if (kBc7ActualBits[1][mode] == 0) {
    for (uint32_t e = 0; e < numEndpoints; ++e) {
      endpoints[e][3] = 0xFF;
    }
  }

  uint32_t indexBits = (mode == 0 || mode == 1) ? 3 : (mode == 6 ? 4 : 2);
  const uint32_t indexBits2 = mode == 4 ? 3 : (mode == 5 ? 2 : 0);
  const uint32_t* weights =
      indexBits == 2 ? weight2 : (indexBits == 3 ? weight3 : weight4);
  const uint32_t* weights2 = indexBits2 == 2 ? weight2 : weight3;

  // The indices of a row are not interleaved with the next row's, so they are
  // all read first and then turned into pixels.
  uint8_t indices[4][4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    for (uint32_t j = 0; j < 4; ++j) {
      const uint32_t set = numPartitions == 1
                               ? ((i | j) != 0 ? 0u : 128u)
                               : uint32_t(kBc7PartitionSets[numPartitions - 2][partition][i][j]);
      uint32_t bits = (mode == 0 || mode == 1) ? 3 : (mode == 6 ? 4 : 2);
      if ((set & 0x80) != 0) {
        bits -= 1;
      }
      indices[i][j] = uint8_t(stream.ReadBits(bits));
    }
  }

  for (uint32_t i = 0; i < 4; ++i) {
    for (uint32_t j = 0; j < 4; ++j) {
      uint32_t set = numPartitions == 1 ? ((i | j) != 0 ? 0u : 128u)
                                       : uint32_t(kBc7PartitionSets[numPartitions - 2][partition]
                                                                          [i][j]);
      set &= 3;
      const uint32_t weight = weights[indices[i][j]];
      uint32_t r, g, b, a;
      if (indexBits2 == 0) {
        r = Interpolate(endpoints[set * 2][0], endpoints[set * 2 + 1][0], weight);
        g = Interpolate(endpoints[set * 2][1], endpoints[set * 2 + 1][1], weight);
        b = Interpolate(endpoints[set * 2][2], endpoints[set * 2 + 1][2], weight);
        a = Interpolate(endpoints[set * 2][3], endpoints[set * 2 + 1][3], weight);
      } else {
        const uint32_t index2 = stream.ReadBits((i | j) != 0 ? indexBits2 : indexBits2 - 1);
        const uint32_t weight2v = weights2[index2];
        // The index selection bit says which of the two index sets drives the
        // colour and which drives the alpha.
        if (indexSelectionBit == 0) {
          r = Interpolate(endpoints[set * 2][0], endpoints[set * 2 + 1][0], weight);
          g = Interpolate(endpoints[set * 2][1], endpoints[set * 2 + 1][1], weight);
          b = Interpolate(endpoints[set * 2][2], endpoints[set * 2 + 1][2], weight);
          a = Interpolate(endpoints[set * 2][3], endpoints[set * 2 + 1][3], weight2v);
        } else {
          r = Interpolate(endpoints[set * 2][0], endpoints[set * 2 + 1][0], weight2v);
          g = Interpolate(endpoints[set * 2][1], endpoints[set * 2 + 1][1], weight2v);
          b = Interpolate(endpoints[set * 2][2], endpoints[set * 2 + 1][2], weight2v);
          a = Interpolate(endpoints[set * 2][3], endpoints[set * 2 + 1][3], weight);
        }
      }
      switch (rotation) {
        case 1:
          // Scalar(R), vector(AGB).
          std::swap(a, r);
          break;
        case 2:
          // Scalar(G), vector(RAB).
          std::swap(a, g);
          break;
        case 3:
          // Scalar(B), vector(RGA).
          std::swap(a, b);
          break;
        default:
          break;
      }
      dst[(i * 4 + j) * 4 + 0] = uint8_t(r);
      dst[(i * 4 + j) * 4 + 1] = uint8_t(g);
      dst[(i * 4 + j) * 4 + 2] = uint8_t(b);
      dst[(i * 4 + j) * 4 + 3] = uint8_t(a);
    }
  }
}

}  // namespace TxtrDetail
}  // namespace PortRemastered
