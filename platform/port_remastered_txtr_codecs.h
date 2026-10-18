// Internal to the Remastered TXTR reader (port_remastered_txtr.cpp): the entry
// points of the three decoders it vendors, each in a file of its own.
//
//   port_remastered_tegra_swizzle.cpp  tegra_swizzle 0.3.2, block linear untiling
//   port_remastered_bcdec.cpp          bcdec_rs 0.1.2, the BC1-BC7 block decoders
//   port_remastered_astc_decode.cpp    astc-decode 0.3.1, the ASTC block decoder
//
// Each file carries its crate's licence and attribution. Nothing outside the
// TXTR reader should include this.

#pragma once

#include <cstddef>
#include <cstdint>

namespace PortRemastered {
namespace TxtrDetail {

// tegra_swizzle: GOB geometry, which the TXTR reader uses for its own sizes too.
constexpr size_t kGobWidthBytes = 64;
constexpr size_t kGobHeightBytes = 8;
constexpr size_t kGobSizeBytes = 64 * 8;

inline size_t DivRoundUp(size_t x, size_t d) { return (x + d - 1) / d; }

uint32_t BlockHeightMip0(size_t heightInBlocks);
uint32_t MipBlockHeight(size_t mipHeight, uint32_t blockHeightMip0);
size_t BlockDepth(size_t depth);
size_t MipBlockDepth(size_t mipDepth, size_t blockDepthMip0);
size_t SwizzledMipSize(size_t width, size_t height, size_t depth, uint32_t blockHeight,
                       size_t bytesPerPixel);
size_t GobOffset(size_t x, size_t y);
void DeswizzleMip(size_t width, size_t height, size_t depth, uint32_t blockHeight,
                  size_t blockDepth, size_t bytesPerPixel, const uint8_t* src, size_t srcOffset,
                  uint8_t* dst);

// bcdec_rs and astc-decode.
// A 128 bit value as a pair of halves, because MSVC has no __int128 and this
// file has to build there.
struct U128 {
  uint64_t lo = 0;
  uint64_t hi = 0;
};

void DecodeColorBlock(const uint8_t* src, uint8_t* dst, bool onlyOpaqueMode);
void DecodeSharpAlpha(const uint8_t* src, uint8_t* dst);
void DecodeSmoothAlpha(const uint8_t* src, uint8_t* dst, size_t pixelSize, size_t channel);
void DecodeBc6h(const uint8_t* src, uint16_t* dst, bool isSigned);
void DecodeBc7(const uint8_t* src, uint8_t* dst);

bool DecodeAstcBlock(const uint8_t* block, uint32_t blockWidth, uint32_t blockHeight,
                     uint8_t* dst);

}  // namespace TxtrDetail
}  // namespace PortRemastered
