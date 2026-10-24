// Metroid Dread .bctex reader (port_dread_bctex.h), written from the format notes
// (kb format/dread-bctex.md). Untiling and the BC block decoders are the port's own
// (port_remastered_tegra_swizzle.cpp, port_remastered_bcdec.cpp).

#include "port_dread_bctex.h"

#include "port_remastered_txtr_codecs.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace PortDread {

using namespace PortRemastered::TxtrDetail;

namespace {

constexpr size_t kMaxInflated = size_t(512) << 20;
constexpr size_t kMaxDimension = 16384;

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | uint64_t(Le32(p + 4)) << 32; }

std::string Hex(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%x", v);
  return buf;
}

bool Inflate(const uint8_t* in, size_t inSize, size_t outSize, std::vector<uint8_t>& out, std::string& error) {
  out.assign(outSize, 0);
  z_stream zs = {};
  if (inflateInit2(&zs, 47) != Z_OK) { // zlib or gzip header
    error = "bctex: zlib init failed";
    return false;
  }
  zs.next_in = const_cast<Bytef*>(in);
  zs.avail_in = uInt(inSize);
  zs.next_out = out.data();
  zs.avail_out = uInt(outSize);
  const int rc = inflate(&zs, Z_FINISH);
  const size_t produced = outSize - zs.avail_out;
  inflateEnd(&zs);
  if (rc != Z_STREAM_END || produced != outSize) {
    error = "bctex: the gzip stream is corrupt or does not match its declared size";
    return false;
  }
  return true;
}

bool FormatBytes(uint32_t format, uint32_t& bytes, bool& compressed) {
  compressed = false;
  switch (format) {
    case kBctexR8: bytes = 1; return true;
    case kBctexRg8: bytes = 2; return true;
    case kBctexRgba8:
    case kBctexBgra8: bytes = 4; return true;
    case kBctexBc1: bytes = 8; compressed = true; return true;
    case kBctexBc3:
    case kBctexBc5:
    case kBctexBc6h: bytes = 16; compressed = true; return true;
    default: return false;
  }
}

} // namespace

uint32_t Bctex::MipWidth(uint32_t mip) const { return std::max(m_info.width >> std::min(mip, 31u), 1u); }
uint32_t Bctex::MipHeight(uint32_t mip) const { return std::max(m_info.height >> std::min(mip, 31u), 1u); }

bool Bctex::Parse(const uint8_t* data, size_t size, std::string& error) {
  m_valid = false;
  m_info = BctexInfo();
  m_surface.clear();
  if (data == nullptr || size < 8 + 18 || std::memcmp(data, "MTXT", 4) != 0) {
    error = "bctex: not an MTXT file";
    return false;
  }
  const size_t inflatedSize = Le32(data + size - 4);
  if (inflatedSize < 0x30 || inflatedSize > kMaxInflated) {
    error = "bctex: implausible inflated size";
    return false;
  }
  if (data[8] != 0x1F || data[9] != 0x8B) {
    error = "bctex: the payload is not a gzip stream";
    return false;
  }
  std::vector<uint8_t> inf;
  if (!Inflate(data + 8, size - 8, inflatedSize, inf, error)) {
    return false;
  }

  // Outer header.
  const uint32_t xtxOff = Le32(inf.data() + 0x20);
  const uint32_t xtxSize = Le32(inf.data() + 0x28);
  if (xtxOff < 8 || size_t(xtxOff) - 8 > inf.size() || xtxSize > inf.size() - (size_t(xtxOff) - 8) ||
      xtxSize < 16) {
    error = "bctex: the XTX lies outside the file";
    return false;
  }
  const uint8_t* xtx = inf.data() + (xtxOff - 8);
  const size_t xtxLen = xtxSize;
  if (std::memcmp(xtx, "DFvN", 4) != 0) {
    error = "bctex: no XTX container";
    return false;
  }
  const uint32_t xtxHeader = Le32(xtx + 4);
  if (xtxHeader < 16 || xtxHeader > xtxLen) {
    error = "bctex: bad XTX header size";
    return false;
  }

  // Blocks: the texture header (type 2) and the surface data (type 3).
  const uint8_t* texHdr = nullptr;
  size_t texHdrSize = 0;
  const uint8_t* surf = nullptr;
  size_t surfSize = 0;
  size_t at = xtxHeader;
  while (at + 36 <= xtxLen && std::memcmp(xtx + at, "HBvN", 4) == 0) {
    const uint8_t* b = xtx + at;
    const uint32_t blockSize = Le32(b + 4);
    const uint64_t dataSize = Le64(b + 8);
    const int64_t dataOff = int64_t(Le64(b + 16));
    const uint32_t type = Le32(b + 24);
    if (blockSize < 36 || dataOff < 0 || uint64_t(dataOff) > xtxLen - at || dataSize > xtxLen - at - uint64_t(dataOff)) {
      error = "bctex: an XTX block lies outside the XTX";
      return false;
    }
    if (type == 2 && !texHdr) {
      texHdr = b + dataOff;
      texHdrSize = size_t(dataSize);
    } else if (type == 3 && !surf) {
      surf = b + dataOff;
      surfSize = size_t(dataSize);
    }
    const uint64_t next = uint64_t(at) + uint64_t(dataOff) + dataSize;
    if (next <= at || next > xtxLen) {
      break;
    }
    at = size_t(next);
  }
  if (!texHdr || !surf) {
    error = "bctex: the XTX lacks a texture header or data block";
    return false;
  }
  // u64 size, u32 align, width, height, depth, target, format, mips, slice size, u32 mip_offset[17],
  // u32 layout1, layout2.
  constexpr size_t kHdrNeeded = 8 + 4 * 8 + 17 * 4 + 8;
  if (texHdrSize < kHdrNeeded) {
    error = "bctex: the texture header is short";
    return false;
  }
  BctexInfo info;
  info.width = Le32(texHdr + 12);
  info.height = Le32(texHdr + 16);
  info.depth = Le32(texHdr + 20);
  info.format = Le32(texHdr + 28);
  info.mipCount = Le32(texHdr + 32);
  for (int i = 0; i < 17; ++i) {
    info.mipOffset[i] = Le32(texHdr + 40 + 4 * i);
  }
  info.layout1 = Le32(texHdr + 40 + 17 * 4);
  info.layout2 = Le32(texHdr + 44 + 17 * 4);
  info.blockHeightGobs = 1u << (info.layout1 & 7);
  if (info.width == 0 || info.height == 0 || info.width > kMaxDimension || info.height > kMaxDimension ||
      info.mipCount == 0 || info.mipCount > 17 || info.depth > 1) {
    error = "bctex: unsupported or implausible dimensions (" + std::to_string(info.width) + "x" +
            std::to_string(info.height) + ", " + std::to_string(info.mipCount) + " mips, depth " +
            std::to_string(info.depth) + ")";
    return false;
  }
  if (!FormatBytes(info.format, info.bytesPerBlock, info.compressed)) {
    error = "bctex: unsupported format " + Hex(info.format);
    return false;
  }
  m_info = info;
  m_surface.assign(surf, surf + surfSize);
  m_valid = true;
  return true;
}

bool Bctex::Deswizzle(uint32_t mip, std::vector<uint8_t>& linear, std::string& error) const {
  if (!m_valid || mip >= m_info.mipCount) {
    error = "bctex: no such mip";
    return false;
  }
  const size_t w = m_info.compressed ? DivRoundUp(MipWidth(mip), 4) : MipWidth(mip);
  const size_t h = m_info.compressed ? DivRoundUp(MipHeight(mip), 4) : MipHeight(mip);
  const uint32_t blockHeight = MipBlockHeight(h, m_info.blockHeightGobs);
  const size_t swizzled = SwizzledMipSize(w, h, 1, blockHeight, m_info.bytesPerBlock);
  const size_t offset = m_info.mipOffset[mip];
  if (swizzled > m_surface.size() || offset > m_surface.size() - swizzled) {
    error = "bctex: mip " + std::to_string(mip) + " lies outside the surface";
    return false;
  }
  linear.assign(w * h * m_info.bytesPerBlock, 0);
  DeswizzleMip(w, h, 1, blockHeight, 1, m_info.bytesPerBlock, m_surface.data(), offset, linear.data());
  return true;
}

bool Bctex::DecodeRgba8(uint32_t mip, std::vector<uint8_t>& rgba, std::string& error) const {
  if (!m_valid) {
    error = "bctex: no texture";
    return false;
  }
  const uint32_t f = m_info.format;
  if (f == kBctexBc6h) {
    error = "bctex: cannot decode format " + Hex(f);
    return false;
  }
  std::vector<uint8_t> lin;
  if (!Deswizzle(mip, lin, error)) {
    return false;
  }
  const size_t w = MipWidth(mip), h = MipHeight(mip);
  rgba.assign(w * h * 4, 0);
  if (!m_info.compressed) {
    for (size_t i = 0; i < w * h; ++i) {
      uint8_t* d = &rgba[i * 4];
      const uint8_t* s = &lin[i * m_info.bytesPerBlock];
      switch (f) {
        case kBctexR8: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
        case kBctexRg8: d[0] = s[0]; d[1] = s[1]; d[2] = 0; d[3] = 255; break;
        case kBctexRgba8: std::memcpy(d, s, 4); break;
        default: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; break; // BGRA8
      }
    }
    return true;
  }
  const size_t blocksX = DivRoundUp(w, 4), blocksY = DivRoundUp(h, 4);
  uint8_t block[4 * 4 * 4];
  for (size_t by = 0; by < blocksY; ++by) {
    for (size_t bx = 0; bx < blocksX; ++bx) {
      const uint8_t* src = &lin[(by * blocksX + bx) * m_info.bytesPerBlock];
      std::memset(block, 255, sizeof(block));
      if (f == kBctexBc1) {
        DecodeColorBlock(src, block, false);
      } else if (f == kBctexBc3) {
        DecodeColorBlock(src + 8, block, true);
        DecodeSmoothAlpha(src, block, 4, 3);
      } else { // BC5
        DecodeSmoothAlpha(src, block, 4, 0);
        DecodeSmoothAlpha(src + 8, block, 4, 1);
        for (size_t i = 0; i < 16; ++i) {
          block[i * 4 + 2] = 0;
          block[i * 4 + 3] = 255;
        }
      }
      // Copy in, clipping the partial blocks at the right and bottom edge.
      for (size_t y = 0; y < 4 && by * 4 + y < h; ++y) {
        for (size_t x = 0; x < 4 && bx * 4 + x < w; ++x) {
          std::memcpy(&rgba[((by * 4 + y) * w + bx * 4 + x) * 4], &block[(y * 4 + x) * 4], 4);
        }
      }
    }
  }
  return true;
}

} // namespace PortDread
