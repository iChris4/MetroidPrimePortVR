#pragma once

// Reader for Metroid Dread's .bctex textures (kb format/dread-bctex.md), read out of the
// user's own game image.
//
//   "MTXT"; u32 flags; a gzip stream whose last 4 bytes (also the file's) are the inflated size
//   inflated: u64; u32 width, height, mips, flag; u32 name_off; u32; u32 xtx_off; u32; u32 xtx_size
//   XTX at xtx_off - 8: "DFvN" {u32 hdr_size, 1, 1}, then "HBvN" blocks {u32 blk_size; u64 data_size;
//   i64 data_off (from the block start); u32 type (2 = texture header, 3 = data); u32; u32}
//   texture header: u64 size, u32 align, width, height, depth, target, format, mips, slice size,
//   u32 mip_offset[17], u32 layout1, u32 layout2, u32
//
// The surface is Tegra X1 block-linear; Deswizzle() untiles a mip to linear blocks with the
// port's tegra_swizzle port and DecodeRgba8() expands those to RGBA8.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PortDread {

// The XTX format codes seen in Dread's Samus textures.
enum BctexFormat : uint32_t {
  kBctexR8 = 0x01,
  kBctexRg8 = 0x0D,
  kBctexRgba8 = 0x25,
  kBctexBc1 = 0x42,
  kBctexBc3 = 0x44,
  kBctexBc5 = 0x4B,
  kBctexBc6h = 0x50,
  kBctexBgra8 = 0x6D,
};

struct BctexInfo {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mipCount = 0;
  uint32_t format = 0; // BctexFormat
  uint32_t depth = 1;
  uint32_t layout1 = 0;
  uint32_t layout2 = 0;
  // Block height of mip 0, in GOBs (1 << (layout1 & 7)); smaller mips shrink it.
  uint32_t blockHeightGobs = 1;
  // Bytes per 4x4 block (compressed) or per pixel (uncompressed).
  uint32_t bytesPerBlock = 0;
  bool compressed = false;
  // Each mip's start in the surface data.
  uint32_t mipOffset[17] = {};
};

class Bctex {
public:
  // Inflates and parses the file. On failure the object is empty and `error` is set
  // ("bctex: ..."), including "unsupported format 0x..." for formats whose block size is unknown.
  bool Parse(const uint8_t* data, size_t size, std::string& error);
  bool Valid() const { return m_valid; }
  const BctexInfo& Info() const { return m_info; }

  // Mip `mip`'s size in texels, at least 1x1.
  uint32_t MipWidth(uint32_t mip) const;
  uint32_t MipHeight(uint32_t mip) const;

  // Mip `mip` untiled: rows of blocks (compressed) or pixels, tightly packed,
  // ceil(w/4) * ceil(h/4) * bytesPerBlock bytes for BC, w * h * bytesPerBlock otherwise.
  bool Deswizzle(uint32_t mip, std::vector<uint8_t>& linear, std::string& error) const;

  // Mip `mip` as RGBA8, MipWidth x MipHeight, top row first. BC1/BC3: colour (and alpha);
  // BC5: R and G from the two channels, B = 0, A = 255; R8: grey; RG8: R, G, B = 0;
  // RGBA8/BGRA8 as stored. Other formats fail with "bctex: cannot decode format 0x...".
  bool DecodeRgba8(uint32_t mip, std::vector<uint8_t>& rgba, std::string& error) const;

private:
  bool m_valid = false;
  BctexInfo m_info;
  std::vector<uint8_t> m_surface; // the swizzled data block
};

} // namespace PortDread
