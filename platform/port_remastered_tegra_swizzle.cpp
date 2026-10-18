// tegra_swizzle 0.3.2 (surface.rs, swizzle.rs, blockheight.rs, blockdepth.rs,
// arrays.rs), ported for the Remastered TXTR reader (port_remastered_txtr.cpp):
// the Tegra X1 block linear untiling, kept bit exact with the Rust.
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
// tegra_swizzle 0.3.2: block linear untiling
// ---------------------------------------------------------------------------


// The block heights a Tegra surface can use, in GOBs (8 rows each).
uint32_t BlockHeightMip0(size_t heightInBlocks) {
  const size_t heightAndHalf = heightInBlocks + heightInBlocks / 2;
  if (heightAndHalf >= 128) {
    return 16;
  }
  if (heightAndHalf >= 64) {
    return 8;
  }
  if (heightAndHalf >= 32) {
    return 4;
  }
  if (heightAndHalf >= 16) {
    return 2;
  }
  return 1;
}

uint32_t MipBlockHeight(size_t mipHeight, uint32_t blockHeightMip0) {
  uint32_t blockHeight = blockHeightMip0;
  while (mipHeight <= (blockHeight / 2) * 8 && blockHeight > 1) {
    blockHeight /= 2;
  }
  return blockHeight;
}

// Blocks are always one GOB wide, so the depth in blocks is the whole story.
size_t BlockDepth(size_t depth) {
  const size_t depthAndHalf = depth + depth / 2;
  if (depthAndHalf >= 16) {
    return 16;
  }
  if (depthAndHalf >= 8) {
    return 8;
  }
  if (depthAndHalf >= 4) {
    return 4;
  }
  if (depthAndHalf >= 2) {
    return 2;
  }
  return 1;
}

size_t MipBlockDepth(size_t mipDepth, size_t blockDepthMip0) {
  size_t blockDepth = blockDepthMip0;
  while (mipDepth <= blockDepth / 2 && blockDepth > 1) {
    blockDepth /= 2;
  }
  return blockDepth;
}

// The tiled size of one mip, which is padded up to whole GOBs. A block is one
// GOB wide, so the width in GOBs follows from the bytes per pixel.
size_t SwizzledMipSize(size_t width, size_t height, size_t depth, uint32_t blockHeight,
                       size_t bytesPerPixel) {
  const size_t widthInGobs = DivRoundUp(width * bytesPerPixel, kGobWidthBytes);
  const size_t heightInBlocks = DivRoundUp(height, size_t(blockHeight) * kGobHeightBytes);
  const size_t heightInGobs = heightInBlocks * size_t(blockHeight);
  const size_t depthUnit = BlockDepth(depth);
  const size_t depthInGobs = DivRoundUp(depth, depthUnit) * depthUnit;
  return widthInGobs * heightInGobs * depthInGobs * kGobSizeBytes;
}

// Offset of the byte at (x, y) inside one GOB, from the Tegra TRM p. 1218.
size_t GobOffset(size_t x, size_t y) {
  return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 +
         (x % 16);
}

// The row offsets of a GOB's eight 16 byte rows, which is what makes the whole
// GOB contiguous: 0, 16, 64, 80, 128, 144, 192, 208.
constexpr size_t kGobRowOffsets[8] = {0, 16, 64, 80, 128, 144, 192, 208};

// Untiles one mip into `dst`, which must hold width * height * depth * bpp
// bytes, reading from `src` at `srcOffset`. This is swizzle.rs's
// swizzle_inner<true> with only the untiling direction kept.
void DeswizzleMip(size_t width, size_t height, size_t depth, uint32_t blockHeight,
                  size_t blockDepth, size_t bytesPerPixel, const uint8_t* src, size_t srcOffset,
                  uint8_t* dst) {
  const size_t rowBytes = width * bytesPerPixel;
  const size_t widthInGobs = DivRoundUp(rowBytes, kGobWidthBytes);
  const size_t blockHeightInBytes = kGobHeightBytes * blockHeight;
  const size_t blockSizeInBytes = kGobSizeBytes * blockHeight * blockDepth;
  // Rows are grouped into blocks of `blockHeight` GOBs, and blocks into slices.
  const size_t sliceSize =
      DivRoundUp(height, blockHeight * kGobHeightBytes) * kGobSizeBytes * blockHeight *
      blockDepth * widthInGobs;

  for (size_t z = 0; z < depth; ++z) {
    const size_t offsetZ =
        (z / blockDepth) * sliceSize + (z & (blockDepth - 1)) * kGobSizeBytes * blockHeight;
    for (size_t y0 = 0; y0 < height; y0 += kGobHeightBytes) {
      const size_t blockY = y0 / blockHeightInBytes;
      const size_t blockInnerRow = (y0 % blockHeightInBytes) / kGobHeightBytes;
      const size_t offsetY =
          blockY * blockSizeInBytes * widthInGobs + blockInnerRow * kGobSizeBytes;
      for (size_t x0 = 0; x0 < rowBytes; x0 += kGobWidthBytes) {
        const size_t offsetX = (x0 / kGobWidthBytes) * blockSizeInBytes;
        const size_t gobAddress = srcOffset + offsetZ + offsetY + offsetX;
        if (x0 + kGobWidthBytes < rowBytes && y0 + kGobHeightBytes < height) {
          // A whole GOB: copy its eight rows, each sixteen bytes at a time.
          for (size_t row = 0; row < kGobHeightBytes; ++row) {
            uint8_t* out = dst + (y0 + row) * rowBytes + x0;
            const uint8_t* in = src + gobAddress + kGobRowOffsets[row];
            for (size_t chunk = 0; chunk < 4; ++chunk) {
              // The eight 16 byte pieces of a row, in the order the GOB holds
              // them: halves, then quarters, then the two eighths.
              static const size_t kDstOffsets[4] = {0, 16, 32, 48};
              static const size_t kSrcOffsets[4] = {0, 32, 256, 288};
              std::memcpy(out + kDstOffsets[chunk], in + kSrcOffsets[chunk], 16);
            }
          }
        } else {
          // A partly filled GOB along the right or bottom edge, byte by byte.
          for (size_t y = 0; y < kGobHeightBytes; ++y) {
            for (size_t x = 0; x < kGobWidthBytes; ++x) {
              if (y0 + y < height && x0 + x < rowBytes) {
                dst[(y0 + y) * rowBytes + x0 + x] = src[gobAddress + GobOffset(x, y)];
              }
            }
          }
        }
      }
    }
  }
}

}  // namespace TxtrDetail
}  // namespace PortRemastered
