#pragma once

// retrotool's LZSS (util/lzss.rs), shared by the pak, texture and model readers.
// One variant per mode: mode is the log2 of the group's byte count (1, 2 or 4
// bytes), a clear header bit copies a group verbatim and a set bit copies a back
// reference. Unlike the Rust version this one checks every range instead of
// trusting the data, and reports failure rather than panicking.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace PortRemastered {

// Decompresses exactly `outSize` bytes; false when the data is corrupt, a
// reference points before the start or nowhere, or the sizes do not agree.
inline bool LzssDecompress(uint32_t mode, const uint8_t* in, size_t inSize, uint8_t* out, size_t outSize) {
  if (mode < 1 || mode > 3) {
    return false;
  }
  const size_t groupLen = size_t(1) << (mode - 1);
  size_t inCur = 0;
  size_t outCur = 0;
  uint8_t header = 0;
  int group = 0;
  while (inCur < inSize) {
    if (group == 0) {
      header = in[inCur++];
      group = 8;
    }
    if ((header & 0x80) == 0) {
      if (inSize - inCur < groupLen || outSize - outCur < groupLen) {
        return false;
      }
      std::memcpy(out + outCur, in + inCur, groupLen);
      inCur += groupLen;
      outCur += groupLen;
    } else {
      if (inSize - inCur < 2) {
        return false;
      }
      const size_t count = size_t(in[inCur] >> 4) + (4 - size_t(mode));
      const size_t length = (size_t(in[inCur] & 0x0F) * 256 + size_t(in[inCur + 1])) << (mode - 1);
      inCur += 2;
      // A distance of 0 would copy the bytes being written onto themselves: no
      // encoder writes one, so it is corrupt data, not a run.
      if (length == 0 || length > outCur) {
        return false;
      }
      const size_t total = count * groupLen;
      if (outSize - outCur < total) {
        return false;
      }
      // One byte at a time: a back reference may overlap the bytes it is writing,
      // which is how the format spells a run of the same byte.
      for (size_t i = 0; i < total; ++i) {
        out[outCur + i] = out[outCur - length + i];
      }
      outCur += total;
    }
    header = uint8_t(header << 1);
    group -= 1;
  }
  return outCur == outSize;
}

} // namespace PortRemastered
