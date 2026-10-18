// astc-decode 0.3.1, ported for the Remastered TXTR reader
// (port_remastered_txtr.cpp): the ASTC block decoder, kept bit exact with the
// Rust. Its asserts on a malformed block become a failed decode that fills the
// block with astc's error colour, because this has to return, not panic.
//
// astc-decode is Apache 2.0 licensed, Copyright 2021 Weiyi Wang, and is itself
// derived from an Apache 2.0 decoder at UNC Chapel Hill.

#include "port_remastered_txtr_codecs.h"

#include <algorithm>
#include <cstring>

namespace PortRemastered {
namespace TxtrDetail {

// ---------------------------------------------------------------------------
// astc-decode 0.3.1: the ASTC block decoder
// ---------------------------------------------------------------------------

namespace {

// A 128 bit little endian bit stream, which is what ASTC blocks are: 128 bits
// of configuration and data in the order the format defines them.
struct AstcBitStream {
  uint64_t lo = 0;
  uint64_t hi = 0;
  uint32_t bitsRead = 0;

  U128 ReadBits128(uint32_t numBits) {
    if (numBits == 0) {
      return U128{};
    }
    U128 out{};
    if (numBits < 64) {
      out.lo = lo & ((uint64_t(1) << numBits) - 1);
    } else {
      out.lo = lo;
      if (numBits > 64) {
        const uint32_t top = numBits - 64;
        out.hi = top >= 64 ? hi : (hi & ((uint64_t(1) << top) - 1));
      }
    }
    if (numBits >= 64) {
      const uint32_t shift = numBits - 64;
      lo = shift == 0 ? hi : (hi >> shift);
      hi = 0;
    } else {
      lo = (lo >> numBits) | (hi << (64 - numBits));
      hi >>= numBits;
    }
    bitsRead += numBits;
    return out;
  }

  uint32_t ReadBits(uint32_t numBits) { return uint32_t(ReadBits128(numBits).lo); }
  uint32_t ReadBit() { return ReadBits(1); }
};

uint64_t ReverseBits64(uint64_t value) {
  uint64_t out = 0;
  for (int i = 0; i < 64; ++i) {
    out = (out << 1) | ((value >> i) & 1);
  }
  return out;
}

// How many bits are set, which is how the spec's tables spell "the log2 of this
// power of two".
uint32_t CountOnes(uint32_t value) {
  uint32_t count = 0;
  while (value != 0) {
    count += value & 1;
    value >>= 1;
  }
  return count;
}

enum EncodingType { kJustBits, kQuint, kTrit };

struct IntegerEncoding {
  EncodingType encoding = kJustBits;
  uint32_t numBits = 0;
};

// How many bits an integer sequence of n values takes in this encoding.
uint32_t EncodedBitLength(const IntegerEncoding& enc, uint32_t nValues) {
  uint32_t total = enc.numBits * nValues;
  if (enc.encoding == kTrit) {
    total += (nValues * 8 + 4) / 5;
  } else if (enc.encoding == kQuint) {
    total += (nValues * 7 + 2) / 3;
  }
  return total;
}

// The smallest encoding that can hold every value from 0 to maxVal: plain bits
// when that is a power of two, otherwise trits or quints.
IntegerEncoding CreateEncoding(uint32_t maxVal) {
  while (maxVal > 0) {
    const uint32_t check = maxVal + 1;
    if ((check & (check - 1)) == 0) {
      return IntegerEncoding{kJustBits, uint32_t(CountOnes(maxVal))};
    }
    if (check % 3 == 0 && ((check / 3) & ((check / 3) - 1)) == 0) {
      return IntegerEncoding{kTrit, uint32_t(CountOnes(check / 3 - 1))};
    }
    if (check % 5 == 0 && ((check / 5) & ((check / 5) - 1)) == 0) {
      return IntegerEncoding{kQuint, uint32_t(CountOnes(check / 5 - 1))};
    }
    maxVal -= 1;
  }
  return IntegerEncoding{kJustBits, 0};
}

// One table per possible value count, and the deduplicated sequence of
// encodings, which is what the spec's "pick the first that fits" search walks.
// Both are built once, since a table lookup would otherwise cost more than the
// search it is for.
struct EncodingTables {
  IntegerEncoding byValue[256] = {};
  IntegerEncoding sequence[256] = {};
  uint32_t sequenceLength = 0;
};

const EncodingTables& GetEncodingTables() {
  static const EncodingTables tables = [] {
    EncodingTables t{};
    for (uint32_t i = 0; i < 256; ++i) {
      t.byValue[i] = CreateEncoding(i);
    }
    t.sequence[0] = t.byValue[0];
    t.sequenceLength = 1;
    for (uint32_t i = 1; i < 256; ++i) {
      const IntegerEncoding& enc = t.byValue[i];
      const IntegerEncoding& previous = t.sequence[t.sequenceLength - 1];
      if (enc.encoding != previous.encoding || enc.numBits != previous.numBits) {
        t.sequence[t.sequenceLength] = enc;
        t.sequenceLength += 1;
      }
    }
    return t;
  }();
  return tables;
}

uint32_t BitsGet(uint32_t value, uint32_t pos) { return (value >> pos) & 1; }

uint32_t BitsRange(uint32_t value, uint32_t start, uint32_t end) {
  const uint32_t mask = (uint32_t(1) << (end - start + 1)) - 1;
  return (value >> start) & mask;
}

// A trit or quint holds two fields: a value in 0..2 or 0..4, and a bit field
// whose width depends on the value. Each of the four cases below is the spec's
// table (C.2.8) expanded, returning the value's contribution to the final 8 bit
// component or 6 bit weight: `(a & mask) | ((d * c + b) ^ a) >> 2`, where `d` is
// the value, `a` the low bit of the bit field scaled, and `b` and `c` come from
// the rest of the bit field through the table.
uint32_t DecodeTritColor(uint32_t value, uint32_t bitValue, uint32_t numBits) {
  const uint32_t a = (bitValue & 1) * 0x1FF;
  const uint32_t x = bitValue >> 1;
  uint32_t b, c;
  switch (numBits) {
    case 1: c = 204; b = 0; break;
    case 2: c = 93; b = (x << 8) | (x << 4) | (x << 2) | (x << 1); break;
    case 3: c = 44; b = (x << 7) | (x << 2) | x; break;
    case 4: c = 22; b = (x << 6) | x; break;
    case 5: c = 11; b = (x << 5) | (x >> 2); break;
    case 6: c = 5; b = (x << 4) | (x >> 4); break;
    default: return 0;
  }
  return (a & 0x80) | (((value * c + b) ^ a) >> 2);
}

uint32_t DecodeTritWeight(uint32_t value, uint32_t bitValue, uint32_t numBits) {
  if (numBits == 0) {
    // No bit field, so the value stands on its own.
    static const uint32_t kWeights[3] = {0, 32, 63};
    return kWeights[value < 3 ? value : 2];
  }
  const uint32_t a = (bitValue & 1) * 0x7F;
  const uint32_t x = bitValue >> 1;
  uint32_t b, c;
  switch (numBits) {
    case 1: c = 50; b = 0; break;
    case 2: c = 23; b = (x << 6) | (x << 2) | x; break;
    case 3: c = 11; b = (x << 5) | x; break;
    default: return 0;
  }
  return (a & 0x20) | (((value * c + b) ^ a) >> 2);
}

uint32_t DecodeQuintColor(uint32_t value, uint32_t bitValue, uint32_t numBits) {
  const uint32_t a = (bitValue & 1) * 0x1FF;
  const uint32_t x = bitValue >> 1;
  uint32_t b, c;
  switch (numBits) {
    case 1: c = 113; b = 0; break;
    case 2: c = 54; b = (x << 8) | (x << 3) | (x << 2); break;
    case 3: c = 26; b = (x << 7) | (x << 1) | (x >> 1); break;
    case 4: c = 13; b = (x << 6) | (x >> 1); break;
    case 5: c = 6; b = (x << 5) | (x >> 3); break;
    default: return 0;
  }
  return (a & 0x80) | (((value * c + b) ^ a) >> 2);
}

uint32_t DecodeQuintWeight(uint32_t value, uint32_t bitValue, uint32_t numBits) {
  if (numBits == 0) {
    static const uint32_t kWeights[5] = {0, 16, 32, 47, 63};
    return kWeights[value < 5 ? value : 4];
  }
  const uint32_t a = (bitValue & 1) * 0x7F;
  const uint32_t x = bitValue >> 1;
  uint32_t b, c;
  switch (numBits) {
    case 1: c = 28; b = 0; break;
    case 2: c = 13; b = (x << 6) | (x << 1); break;
    default: return 0;
  }
  return (a & 0x20) | (((value * c + b) ^ a) >> 2);
}

// One trit or quint: a value plus the bit field it shares with its neighbours.
struct IntSequence {
  uint32_t value = 0;
  uint32_t bitValue = 0;
  bool valid = false;
};

// Reads five trit encoded values, spec section C.2.12.
void DecodeTritBlock(AstcBitStream& bits, uint32_t bitsPerValue, IntSequence out[5]) {
  uint32_t m[5];
  uint32_t t[5] = {};
  m[0] = bits.ReadBits(bitsPerValue);
  uint32_t tt = bits.ReadBits(2);
  m[1] = bits.ReadBits(bitsPerValue);
  tt |= bits.ReadBits(2) << 2;
  m[2] = bits.ReadBits(bitsPerValue);
  tt |= bits.ReadBit() << 4;
  m[3] = bits.ReadBits(bitsPerValue);
  tt |= bits.ReadBits(2) << 5;
  m[4] = bits.ReadBits(bitsPerValue);
  tt |= bits.ReadBit() << 7;

  uint32_t c;
  if (BitsRange(tt, 2, 4) == 7) {
    c = (BitsRange(tt, 5, 7) << 2) | BitsRange(tt, 0, 1);
    t[3] = 2;
    t[4] = 2;
  } else {
    c = BitsRange(tt, 0, 4);
    if (BitsRange(tt, 5, 6) == 3) {
      t[4] = 2;
      t[3] = BitsGet(tt, 7);
    } else {
      t[4] = BitsGet(tt, 7);
      t[3] = BitsRange(tt, 5, 6);
    }
  }
  if (BitsRange(c, 0, 1) == 3) {
    t[2] = 2;
    t[1] = BitsGet(c, 4);
    t[0] = (BitsGet(c, 3) << 1) | (BitsGet(c, 2) & ~BitsGet(c, 3));
  } else if (BitsRange(c, 2, 3) == 3) {
    t[2] = 2;
    t[1] = 2;
    t[0] = BitsRange(c, 0, 1);
  } else {
    t[2] = BitsGet(c, 4);
    t[1] = BitsRange(c, 2, 3);
    t[0] = (BitsGet(c, 1) << 1) | (BitsGet(c, 0) & ~BitsGet(c, 1));
  }
  for (uint32_t i = 0; i < 5; ++i) {
    out[i].bitValue = m[i];
    out[i].value = t[i];
    out[i].valid = true;
  }
}

// Reads three quint encoded values, spec section C.2.12.
void DecodeQuintBlock(AstcBitStream& bits, uint32_t bitsPerValue, IntSequence out[3]) {
  uint32_t m[3];
  uint32_t q[3] = {};
  m[0] = bits.ReadBits(bitsPerValue);
  uint32_t qq = bits.ReadBits(3);
  m[1] = bits.ReadBits(bitsPerValue);
  qq |= bits.ReadBits(2) << 3;
  m[2] = bits.ReadBits(bitsPerValue);
  qq |= bits.ReadBits(2) << 5;

  uint32_t c;
  if (BitsRange(qq, 1, 2) == 3 && BitsRange(qq, 5, 6) == 0) {
    q[0] = 4;
    q[1] = 4;
    q[2] = (BitsGet(qq, 0) << 2) | ((BitsGet(qq, 4) & ~BitsGet(qq, 0)) << 1) |
           (BitsGet(qq, 3) & ~BitsGet(qq, 0));
  } else {
    if (BitsRange(qq, 1, 2) == 3) {
      q[2] = 4;
      c = (BitsRange(qq, 3, 4) << 3) | (((~BitsRange(qq, 5, 6)) & 3) << 1) | BitsGet(qq, 0);
    } else {
      q[2] = BitsRange(qq, 5, 6);
      c = BitsRange(qq, 0, 4);
    }
    if (BitsRange(c, 0, 2) == 5) {
      q[1] = 4;
      q[0] = BitsRange(c, 3, 4);
    } else {
      q[1] = BitsRange(c, 3, 4);
      q[0] = BitsRange(c, 0, 2);
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    out[i].bitValue = m[i];
    out[i].value = q[i];
    out[i].valid = true;
  }
}

// Spreads a value's low bits up to `toBit`, so a 5 bit value becomes 8 bits.
uint32_t Replicate(uint32_t value, uint32_t numBits, uint32_t toBit) {
  if (numBits == 0 || toBit == 0) {
    return 0;
  }
  uint32_t result = value << (toBit - numBits);
  uint32_t shift = numBits;
  for (int i = 0; i < 8 && shift < 64; ++i) {
    const uint32_t next = result >> shift;
    if (next == 0) {
      break;
    }
    result |= next;
    shift *= 2;
  }
  return result;
}

struct TexelWeightParams {
  uint32_t width = 0;
  uint32_t height = 0;
  bool dualPlane = false;
  uint32_t maxWeight = 0;
  bool isError = false;
  bool voidExtentLdr = false;
  bool voidExtentHdr = false;

  uint32_t NumWeightValues() const { return width * height * (dualPlane ? 2 : 1); }

  uint32_t PackedBitSize() const {
    return EncodedBitLength(GetEncodingTables().byValue[maxWeight], NumWeightValues());
  }
};

// The first eleven bits of a block are the block mode, which fixes the weight
// grid size, the number of bits per weight and whether there are two planes.
TexelWeightParams DecodeBlockInfo(AstcBitStream& stream) {
  TexelWeightParams params;
  const uint32_t modeBits = stream.ReadBits(11);
  if ((modeBits & 0x1FF) == 0x1FC) {
    if ((modeBits & 0x200) != 0) {
      params.voidExtentHdr = true;
    } else {
      params.voidExtentLdr = true;
    }
    if ((modeBits & 0x400) == 0 || stream.ReadBit() == 0) {
      params.isError = true;
    }
    return params;
  }
  if ((modeBits & 0xF) == 0 || ((modeBits & 0x3) == 0 && (modeBits & 0x1C0) == 0x1C0)) {
    params.isError = true;
    return params;
  }

  uint32_t layout;
  if ((modeBits & 0x1) != 0 || (modeBits & 0x2) != 0) {
    if ((modeBits & 0x8) != 0) {
      if ((modeBits & 0x4) != 0) {
        layout = (modeBits & 0x100) != 0 ? 4 : 3;
      } else {
        layout = 2;
      }
    } else {
      layout = (modeBits & 0x4) != 0 ? 1 : 0;
    }
  } else {
    if ((modeBits & 0x100) != 0) {
      if ((modeBits & 0x80) != 0) {
        layout = (modeBits & 0x20) != 0 ? 8 : 7;
      } else {
        layout = 9;
      }
    } else {
      layout = (modeBits & 0x80) != 0 ? 6 : 5;
    }
  }

  uint32_t r = (modeBits & 0x10) >> 4;
  if (layout < 5) {
    r |= (modeBits & 0x3) << 1;
  } else {
    r |= (modeBits & 0xC) >> 1;
  }
  if (r < 2 || r > 7) {
    // Not a real mode; the Rust asserts here and gives up on the whole file.
    params.isError = true;
    return params;
  }

  const uint32_t a = (modeBits >> 5) & 0x3;
  const uint32_t b = (modeBits >> 7) & 0x3;
  switch (layout) {
    case 0: params.width = b + 4; params.height = a + 2; break;
    case 1: params.width = b + 8; params.height = a + 2; break;
    case 2: params.width = a + 2; params.height = b + 8; break;
    case 3: params.width = a + 2; params.height = ((modeBits >> 7) & 0x1) + 6; break;
    case 4: params.width = ((modeBits >> 7) & 0x1) + 2; params.height = a + 2; break;
    case 5: params.width = 12; params.height = a + 2; break;
    case 6: params.width = a + 2; params.height = 12; break;
    case 7: params.width = 6; params.height = 10; break;
    case 8: params.width = 10; params.height = 6; break;
    case 9: params.width = a + 6; params.height = ((modeBits >> 9) & 0x3) + 6; break;
    default: break;
  }

  params.dualPlane = layout != 9 && (modeBits & 0x400) != 0;
  const bool highPrecision = layout != 9 && (modeBits & 0x200) != 0;
  const uint32_t maxWeights[6] = {9, 11, 15, 19, 23, 31};
  const uint32_t lowWeights[6] = {1, 2, 3, 4, 5, 7};
  params.maxWeight = highPrecision ? maxWeights[r - 2] : lowWeights[r - 2];
  return params;
}

uint8_t ClampByte(int32_t value) { return uint8_t(value < 0 ? 0 : (value > 255 ? 255 : value)); }

// One bit moved between two fields of an integer sequence, spec C.2.14. The
// fields are signed: the value keeps its sign, which is what decides between
// the two ways the endpoints can be read.
void BitTransfer(int32_t& a, int32_t& b) {
  b >>= 1;
  b |= a & 0x80;
  a >>= 1;
  a &= 0x3F;
  if ((a & 0x20) != 0) {
    a -= 0x40;
  }
}

// Section C.2.14: the blue channel gets the extra precision when the sum of
// the odd endpoints is below the sum of the even ones.
void BlueContract(int32_t r, int32_t g, int32_t b, int32_t a, uint8_t out[4]) {
  out[0] = ClampByte((r + b) >> 1);
  out[1] = ClampByte((g + b) >> 1);
  out[2] = ClampByte(b);
  out[3] = ClampByte(a);
}

// Turns the decoded colour values into the two endpoints of one partition,
// following spec C.2.14's endpoint modes.
void ComputeEndpoints(const uint8_t* values, uint32_t* used, uint32_t mode, uint8_t ep[2][4]) {
  auto take = [&](uint32_t count, int32_t* dst) {
    for (uint32_t i = 0; i < count; ++i) {
      dst[i] = values[*used];
      *used += 1;
    }
  };
  // Signed, because the endpoint modes that move a bit between fields leave a
  // negative value behind and the mode's own comparison reads that sign.
  int32_t v[8] = {};
  switch (mode) {
    case 0: {
      take(2, v);
      ep[0][0] = ep[0][1] = ep[0][2] = ClampByte(v[0]);
      ep[0][3] = 255;
      ep[1][0] = ep[1][1] = ep[1][2] = ClampByte(v[1]);
      ep[1][3] = 255;
      break;
    }
    case 1: {
      take(2, v);
      const int32_t low = (v[0] >> 2) | (v[1] & 0xC0);
      const int32_t high = low + (v[1] & 0x3F) > 255 ? 255 : low + (v[1] & 0x3F);
      ep[0][0] = ep[0][1] = ep[0][2] = ClampByte(low);
      ep[0][3] = 255;
      ep[1][0] = ep[1][1] = ep[1][2] = ClampByte(high);
      ep[1][3] = 255;
      break;
    }
    case 4: {
      take(4, v);
      ep[0][0] = ep[0][1] = ep[0][2] = ClampByte(v[0]);
      ep[0][3] = ClampByte(v[2]);
      ep[1][0] = ep[1][1] = ep[1][2] = ClampByte(v[1]);
      ep[1][3] = ClampByte(v[3]);
      break;
    }
    case 5: {
      take(4, v);
      BitTransfer(v[1], v[0]);
      BitTransfer(v[3], v[2]);
      ep[0][0] = ep[0][1] = ep[0][2] = ClampByte(v[0]);
      ep[0][3] = ClampByte(v[2]);
      const uint8_t sum = ClampByte(v[0] + v[1]);
      ep[1][0] = ep[1][1] = ep[1][2] = sum;
      ep[1][3] = ClampByte(int32_t(v[2] + v[3]));
      break;
    }
    case 6: {
      take(4, v);
      ep[0][0] = ClampByte((v[0] * v[3]) >> 8);
      ep[0][1] = ClampByte((v[1] * v[3]) >> 8);
      ep[0][2] = ClampByte((v[2] * v[3]) >> 8);
      ep[0][3] = 255;
      ep[1][0] = ClampByte(v[0]);
      ep[1][1] = ClampByte(v[1]);
      ep[1][2] = ClampByte(v[2]);
      ep[1][3] = 255;
      break;
    }
    case 8: {
      take(6, v);
      if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) {
        ep[0][0] = ClampByte(v[0]);
        ep[0][1] = ClampByte(v[2]);
        ep[0][2] = ClampByte(v[4]);
        ep[0][3] = 255;
        ep[1][0] = ClampByte(v[1]);
        ep[1][1] = ClampByte(v[3]);
        ep[1][2] = ClampByte(v[5]);
        ep[1][3] = 255;
      } else {
        BlueContract(v[1], v[3], v[5], 255, ep[0]);
        BlueContract(v[0], v[2], v[4], 255, ep[1]);
      }
      break;
    }
    case 9: {
      take(6, v);
      BitTransfer(v[1], v[0]);
      BitTransfer(v[3], v[2]);
      BitTransfer(v[5], v[4]);
      if (v[1] + v[3] + v[5] >= 0) {
        ep[0][0] = ClampByte(v[0]);
        ep[0][1] = ClampByte(v[2]);
        ep[0][2] = ClampByte(v[4]);
        ep[0][3] = 255;
        ep[1][0] = ClampByte(int32_t(v[0] + v[1]));
        ep[1][1] = ClampByte(int32_t(v[2] + v[3]));
        ep[1][2] = ClampByte(int32_t(v[4] + v[5]));
        ep[1][3] = 255;
      } else {
        BlueContract(v[0] + v[1], v[2] + v[3], v[4] + v[5], 255, ep[0]);
        BlueContract(v[0], v[2], v[4], 255, ep[1]);
      }
      break;
    }
    case 10: {
      take(6, v);
      ep[0][0] = ClampByte((v[0] * v[3]) >> 8);
      ep[0][1] = ClampByte((v[1] * v[3]) >> 8);
      ep[0][2] = ClampByte((v[2] * v[3]) >> 8);
      ep[0][3] = ClampByte(v[4]);
      ep[1][0] = ClampByte(v[0]);
      ep[1][1] = ClampByte(v[1]);
      ep[1][2] = ClampByte(v[2]);
      ep[1][3] = ClampByte(v[5]);
      break;
    }
    case 12: {
      take(8, v);
      if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) {
        ep[0][0] = ClampByte(v[0]);
        ep[0][1] = ClampByte(v[2]);
        ep[0][2] = ClampByte(v[4]);
        ep[0][3] = ClampByte(v[6]);
        ep[1][0] = ClampByte(v[1]);
        ep[1][1] = ClampByte(v[3]);
        ep[1][2] = ClampByte(v[5]);
        ep[1][3] = ClampByte(v[7]);
      } else {
        BlueContract(v[1], v[3], v[5], v[7], ep[0]);
        BlueContract(v[0], v[2], v[4], v[6], ep[1]);
      }
      break;
    }
    case 13: {
      take(8, v);
      BitTransfer(v[1], v[0]);
      BitTransfer(v[3], v[2]);
      BitTransfer(v[5], v[4]);
      BitTransfer(v[7], v[6]);
      if (v[1] + v[3] + v[5] >= 0) {
        ep[0][0] = ClampByte(v[0]);
        ep[0][1] = ClampByte(v[2]);
        ep[0][2] = ClampByte(v[4]);
        ep[0][3] = ClampByte(v[6]);
        ep[1][0] = ClampByte(int32_t(v[0] + v[1]));
        ep[1][1] = ClampByte(int32_t(v[2] + v[3]));
        ep[1][2] = ClampByte(int32_t(v[4] + v[5]));
        ep[1][3] = ClampByte(int32_t(v[6] + v[7]));
      } else {
        BlueContract(v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7], ep[0]);
        BlueContract(v[0], v[2], v[4], v[6], ep[1]);
      }
      break;
    }
    default: {
      // The HDR endpoint modes, which are not used and are not decoded.
      const uint8_t error[4] = {0xFF, 0, 0xFF, 0xFF};
      std::memcpy(ep[0], error, 4);
      std::memcpy(ep[1], error, 4);
      break;
    }
  }
}

uint32_t Hash52(uint32_t p) {
  p ^= p >> 15;
  p -= p << 17;
  p += p << 7;
  p += p << 4;
  p ^= p >> 5;
  p += p << 16;
  p ^= p >> 7;
  p ^= p >> 3;
  p ^= p << 6;
  p ^= p >> 17;
  return p;
}

// Which of up to four partitions a texel belongs to, spec C.2.21: four hashed
// seeds vote with a weighted sum of the texel's position.
uint32_t SelectPartition(uint32_t seed, uint32_t x, uint32_t y, uint32_t z, uint32_t count,
                         bool smallBlock) {
  if (count == 1) {
    return 0;
  }
  if (smallBlock) {
    x <<= 1;
    y <<= 1;
    z <<= 1;
  }
  seed += (count - 1) * 1024;
  const uint32_t rnum = Hash52(seed);
  uint32_t s[12];
  for (uint32_t i = 0; i < 8; ++i) {
    s[i] = (rnum >> (4 * i)) & 0xF;
  }
  s[8] = (rnum >> 18) & 0xF;
  s[9] = (rnum >> 22) & 0xF;
  s[10] = (rnum >> 26) & 0xF;
  s[11] = ((rnum >> 30) | (rnum << 2)) & 0xF;
  for (uint32_t i = 0; i < 12; ++i) {
    s[i] = s[i] * s[i];
  }
  uint32_t sh1, sh2;
  if ((seed & 1) != 0) {
    sh1 = (seed & 2) != 0 ? 4 : 5;
    sh2 = count == 3 ? 6 : 5;
  } else {
    sh1 = count == 3 ? 6 : 5;
    sh2 = (seed & 2) != 0 ? 4 : 5;
  }
  const uint32_t sh3 = (seed & 0x10) != 0 ? sh1 : sh2;
  s[0] >>= sh1; s[1] >>= sh2; s[2] >>= sh1; s[3] >>= sh2;
  s[4] >>= sh1; s[5] >>= sh2; s[6] >>= sh1; s[7] >>= sh2;
  s[8] >>= sh3; s[9] >>= sh3; s[10] >>= sh3; s[11] >>= sh3;

  uint32_t a = s[0] * x + s[1] * y + s[10] * z + (rnum >> 14);
  uint32_t b = s[2] * x + s[3] * y + s[11] * z + (rnum >> 10);
  uint32_t c = s[4] * x + s[5] * y + s[8] * z + (rnum >> 6);
  uint32_t d = s[6] * x + s[7] * y + s[9] * z + (rnum >> 2);
  a &= 0x3F;
  b &= 0x3F;
  c &= 0x3F;
  d &= 0x3F;
  if (count < 4) {
    d = 0;
  }
  if (count < 3) {
    c = 0;
  }
  if (a >= b && a >= c && a >= d) {
    return 0;
  }
  if (b >= c && b >= d) {
    return 1;
  }
  if (c >= d) {
    return 2;
  }
  return 3;
}

// Decodes the weight grid of one block: the packed weights are expanded, then
// bilinearly interpolated up to the block's footprint (spec C.2.18).
void UnquantizeTexelWeights(AstcBitStream& stream, const TexelWeightParams& params,
                            uint32_t blockWidth, uint32_t blockHeight, uint32_t out[2][144],
                            bool& ok) {
  const uint32_t planeScale = params.dualPlane ? 2 : 1;
  const uint32_t count = params.NumWeightValues();
  uint32_t unquantized[96] = {};
  const IntegerEncoding& enc = GetEncodingTables().byValue[params.maxWeight];
  uint32_t index = 0;
  IntSequence decoded[5];
  if (enc.encoding == kJustBits) {
    for (index = 0; index < count; ++index) {
      unquantized[index] = Replicate(stream.ReadBits(enc.numBits), enc.numBits, 6);
    }
  } else if (enc.encoding == kTrit) {
    while (index < count) {
      DecodeTritBlock(stream, enc.numBits, decoded);
      for (uint32_t i = 0; i < 5 && index < count; ++i, ++index) {
        unquantized[index] = DecodeTritWeight(decoded[i].value, decoded[i].bitValue, enc.numBits);
      }
    }
  } else {
    while (index < count) {
      DecodeQuintBlock(stream, enc.numBits, decoded);
      for (uint32_t i = 0; i < 3 && index < count; ++i, ++index) {
        unquantized[index] = DecodeQuintWeight(decoded[i].value, decoded[i].bitValue, enc.numBits);
      }
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    // A weight of exactly 32 is left out of the encoding, so the values above
    // it shift up one: that is what turns the replicated 42 and 63 into the
    // 43 and 64 the spec's weight tables use. 64 is legal, a weight that was
    // already 64 is not.
    if (unquantized[i] >= 64) {
      ok = false;
      return;
    }
    if (unquantized[i] > 32) {
      unquantized[i] += 1;
    }
  }

  const uint32_t ds = (1024 + (blockWidth / 2)) / (blockWidth - 1);
  const uint32_t dt = (1024 + (blockHeight / 2)) / (blockHeight - 1);
  for (uint32_t plane = 0; plane < planeScale; ++plane) {
    for (uint32_t t = 0; t < blockHeight; ++t) {
      for (uint32_t s = 0; s < blockWidth; ++s) {
        const uint32_t gs = (ds * s * (params.width - 1) + 32) >> 6;
        const uint32_t gt = (dt * t * (params.height - 1) + 32) >> 6;
        const uint32_t js = gs >> 4, fs = gs & 0xF;
        const uint32_t jt = gt >> 4, ft = gt & 0x0F;
        const uint32_t w11 = (fs * ft + 8) >> 4;
        const uint32_t w10 = ft - w11;
        const uint32_t w01 = fs - w11;
        const uint32_t w00 = 16 + w11 - fs - ft;
        const uint32_t v0 = js + jt * params.width;
        uint32_t p00 = 0, p01 = 0, p10 = 0, p11 = 0;
        const uint32_t gridSize = params.width * params.height;
        if (v0 < gridSize) {
          p00 = unquantized[plane + planeScale * v0];
        }
        if (v0 + 1 < gridSize) {
          p01 = unquantized[plane + planeScale * (v0 + 1)];
        }
        if (v0 + params.width < gridSize) {
          p10 = unquantized[plane + planeScale * (v0 + params.width)];
        }
        if (v0 + params.width + 1 < gridSize) {
          p11 = unquantized[plane + planeScale * (v0 + params.width + 1)];
        }
        out[plane][t * blockWidth + s] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
      }
    }
  }
}

// The colour values of every endpoint in the block, in the order the format
// packs them.
uint32_t DecodeColorValues(U128 data, uint32_t nValues, uint32_t bitsForColor,
                           uint8_t out[18]) {
  const EncodingTables& tables = GetEncodingTables();
  uint32_t index = 0;
  while (index < tables.sequenceLength &&
         EncodedBitLength(tables.sequence[index], nValues) <= bitsForColor) {
    index += 1;
  }
  // The Rust indexes one before the first encoding that does not fit, and
  // wraps around when nothing fits at all.
  const IntegerEncoding enc = tables.sequence[index == 0 ? 0 : index - 1];
  AstcBitStream stream;
  stream.lo = data.lo;
  stream.hi = data.hi;
  IntSequence decoded[5];
  if (enc.encoding == kJustBits) {
    for (uint32_t i = 0; i < nValues; ++i) {
      out[i] = uint8_t(Replicate(stream.ReadBits(enc.numBits), enc.numBits, 8));
    }
  } else if (enc.encoding == kTrit) {
    uint32_t i = 0;
    while (i < nValues) {
      DecodeTritBlock(stream, enc.numBits, decoded);
      for (uint32_t k = 0; k < 5 && i < nValues; ++k, ++i) {
        const uint32_t value = DecodeTritColor(decoded[k].value, decoded[k].bitValue, enc.numBits);
        out[i] = uint8_t(value > 255 ? 255 : value);
      }
    }
  } else {
    uint32_t i = 0;
    while (i < nValues) {
      DecodeQuintBlock(stream, enc.numBits, decoded);
      for (uint32_t k = 0; k < 3 && i < nValues; ++k, ++i) {
        const uint32_t value = DecodeQuintColor(decoded[k].value, decoded[k].bitValue, enc.numBits);
        out[i] = uint8_t(value > 255 ? 255 : value);
      }
    }
  }
  return nValues;
}

}  // namespace

// Decodes one ASTC block into `dst`, which is blockWidth * blockHeight RGBA
// texels in row order. False means the block is malformed and `dst` holds
// astc's error colour instead.
bool DecodeAstcBlock(const uint8_t* block, uint32_t blockWidth, uint32_t blockHeight,
                    uint8_t* dst) {
  AstcBitStream stream;
  for (size_t i = 0; i < 8; ++i) {
    stream.lo |= uint64_t(block[i]) << (8 * i);
    stream.hi |= uint64_t(block[i + 8]) << (8 * i);
  }
  auto fillError = [&]() {
    for (uint32_t j = 0; j < blockHeight; ++j) {
      for (uint32_t i = 0; i < blockWidth; ++i) {
        uint8_t* p = dst + (j * blockWidth + i) * 4;
        p[0] = 0xFF;
        p[1] = 0;
        p[2] = 0xFF;
        p[3] = 0xFF;
      }
    }
  };

  const TexelWeightParams params = DecodeBlockInfo(stream);
  if (params.isError) {
    fillError();
    return false;
  }
  if (params.voidExtentLdr) {
    // A constant colour block: the void extent is skipped and the four 16 bit
    // colour components are the high bytes of the image.
    for (uint32_t i = 0; i < 4; ++i) {
      stream.ReadBits(13);
    }
    const uint8_t color[4] = {uint8_t(stream.ReadBits(16) >> 8), uint8_t(stream.ReadBits(16) >> 8),
                              uint8_t(stream.ReadBits(16) >> 8), uint8_t(stream.ReadBits(16) >> 8)};
    for (uint32_t j = 0; j < blockHeight; ++j) {
      for (uint32_t i = 0; i < blockWidth; ++i) {
        std::memcpy(dst + (j * blockWidth + i) * 4, color, 4);
      }
    }
    return true;
  }
  if (params.voidExtentHdr || params.width > blockWidth || params.height > blockHeight ||
      params.NumWeightValues() > 64) {
    fillError();
    return false;
  }
  const uint32_t nWeightBits = params.PackedBitSize();
  if (nWeightBits < 24 || nWeightBits > 96) {
    fillError();
    return false;
  }

  const uint32_t nPartitions = stream.ReadBits(2) + 1;
  if (nPartitions == 4 && params.dualPlane) {
    fillError();
    return false;
  }

  uint32_t endpointMods[4] = {};
  uint32_t partitionIndex = 0;
  uint32_t baseCem = 0;
  if (nPartitions == 1) {
    endpointMods[0] = stream.ReadBits(4);
  } else {
    partitionIndex = stream.ReadBits(10);
    baseCem = stream.ReadBits(6);
  }
  const uint32_t baseMode = baseCem & 3;

  uint32_t nonColorBits = nWeightBits + stream.bitsRead;
  uint32_t extraCemBits = 0;
  if (baseMode != 0) {
    if (nPartitions == 2) {
      extraCemBits = 2;
    } else if (nPartitions == 3) {
      extraCemBits = 5;
    } else if (nPartitions == 4) {
      extraCemBits = 8;
    }
  }
  nonColorBits += extraCemBits;
  const uint32_t planeSelectorBits = params.dualPlane ? 2 : 0;
  nonColorBits += planeSelectorBits;
  if (nonColorBits >= 128) {
    fillError();
    return false;
  }

  const U128 colorData = stream.ReadBits128(128 - nonColorBits);
  const uint32_t planeIdx = stream.ReadBits(planeSelectorBits);
  if (baseMode != 0) {
    const uint32_t extraCem = stream.ReadBits(extraCemBits);
    uint32_t cem = (extraCem << 6) | baseCem;
    cem >>= 2;
    bool switches[4] = {};
    for (uint32_t i = 0; i < nPartitions; ++i) {
      switches[i] = (cem & 1) != 0;
      cem >>= 1;
    }
    uint32_t multiplies[4] = {};
    for (uint32_t i = 0; i < nPartitions; ++i) {
      multiplies[i] = cem & 3;
      cem >>= 2;
    }
    for (uint32_t i = 0; i < nPartitions; ++i) {
      endpointMods[i] = baseMode - (switches[i] ? 0u : 1u);
      endpointMods[i] = (endpointMods[i] << 2) | multiplies[i];
    }
  } else if (nPartitions > 1) {
    const uint32_t cem = baseCem >> 2;
    for (uint32_t i = 0; i < nPartitions; ++i) {
      endpointMods[i] = cem;
    }
  }
  // The block's configuration, colour data and plane selector together must be
  // exactly the 128 bits the weights do not use.
  if (stream.bitsRead + nWeightBits != 128) {
    fillError();
    return false;
  }

  uint32_t nValues = 0;
  for (uint32_t i = 0; i < nPartitions; ++i) {
    nValues += ((endpointMods[i] >> 2) + 1) << 1;
  }
  if (nValues > 18 || (nValues * 13 + 4) / 5 > 128 - nonColorBits) {
    fillError();
    return false;
  }

  uint8_t colorValues[18] = {};
  DecodeColorValues(colorData, nValues, 128 - nonColorBits, colorValues);
  uint8_t endpoints[4][2][4] = {};
  uint32_t used = 0;
  for (uint32_t i = 0; i < nPartitions; ++i) {
    ComputeEndpoints(colorValues, &used, endpointMods[i], endpoints[i]);
  }

  // The weights are the block's bits from the other end, so their stream starts
  // with the block's 128 bits reversed.
  U128 blockBits{};
  for (size_t i = 0; i < 8; ++i) {
    blockBits.lo |= uint64_t(block[i]) << (8 * i);
    blockBits.hi |= uint64_t(block[i + 8]) << (8 * i);
  }
  // Reversing the whole 128 bit value swaps the halves as well as the bits
  // inside them, since the low half of the result is the top half of the block.
  U128 reversed;
  reversed.lo = ReverseBits64(blockBits.hi);
  reversed.hi = ReverseBits64(blockBits.lo);
  // Keep only the low nWeightBits of the reversed value, which is how many bits
  // the weights take: the weight stream is the tail of the block.
  if (nWeightBits < 64) {
    reversed.lo &= (uint64_t(1) << nWeightBits) - 1;
    reversed.hi = 0;
  } else if (nWeightBits < 128) {
    reversed.hi &= (uint64_t(1) << (nWeightBits - 64)) - 1;
  }
  AstcBitStream weightStream;
  weightStream.lo = reversed.lo;
  weightStream.hi = reversed.hi;
  uint32_t weights[2][144] = {};
  bool ok = true;
  UnquantizeTexelWeights(weightStream, params, blockWidth, blockHeight, weights, ok);
  if (!ok) {
    fillError();
    return false;
  }

  const bool smallBlock = (blockHeight * blockWidth) < 32;
  for (uint32_t j = 0; j < blockHeight; ++j) {
    for (uint32_t i = 0; i < blockWidth; ++i) {
      const uint32_t partition =
          SelectPartition(partitionIndex, i, j, 0, nPartitions, smallBlock);
      if (partition >= nPartitions) {
        fillError();
        return false;
      }
      uint8_t* p = dst + (j * blockWidth + i) * 4;
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t c0 = uint32_t(endpoints[partition][0][c]) * 0x101;
        const uint32_t c1 = uint32_t(endpoints[partition][1][c]) * 0x101;
        const uint32_t plane = (params.dualPlane && (planeIdx & 3) == c) ? 1 : 0;
        const uint32_t weight = weights[plane][j * blockWidth + i];
        const uint32_t color = (c0 * (64 - weight) + c1 * weight + 32) / 64;
        p[c] = uint8_t((color * 255 + 32767) / 65536);
      }
    }
  }
  return true;
}

}  // namespace TxtrDetail
}  // namespace PortRemastered
