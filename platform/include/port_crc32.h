#pragma once

// The reflected CRC-32 (polynomial 0xEDB88320): zlib's crc32, and the GameCube card's
// CCrc32::Calculate. One table-driven copy for the disc check, the save converters and the
// Steam controller reports, which each used to carry their own.

#include <array>
#include <cstddef>
#include <cstdint>

namespace port {

// The bare register update, without zlib's inversions on the way in and out. The card's
// checksum is Crc32Register(0xFFFFFFFF, ...) (CCrc32::Calculate with that seed).
inline uint32_t Crc32Register(uint32_t reg, const void* data, size_t size) {
  static const std::array< uint32_t, 256 > table = [] {
    std::array< uint32_t, 256 > t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      t[i] = c;
    }
    return t;
  }();
  const auto* bytes = static_cast< const uint8_t* >(data);
  for (size_t i = 0; i < size; ++i) {
    reg = table[(reg ^ bytes[i]) & 0xFF] ^ (reg >> 8);
  }
  return reg;
}

// zlib's crc32(crc, data, size): 0 starts a checksum, a previous result continues it.
inline uint32_t Crc32(const void* data, size_t size, uint32_t crc = 0) {
  return ~Crc32Register(~crc, data, size);
}

} // namespace port
