#pragma once

// The port's non-cryptographic hashes: FNV-1a in 32 and 64 bits and the zlib
// CRC-32. Each used to be a file-local copy; several of the results are
// written to files (resource ids, save fingerprints, the import manifest), so
// these must give exactly the bits the copies did: FNV-1a over bytes in the
// order given, the standard offset bases and primes, and the reflected CRC-32
// (polynomial 0xEDB88320, initial and final value 0xFFFFFFFF).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace port {

constexpr uint32_t kFnv32Basis = 0x811C9DC5u; // 2166136261
constexpr uint32_t kFnv32Prime = 0x01000193u; // 16777619
constexpr uint64_t kFnv64Basis = 0xCBF29CE484222325ull;
constexpr uint64_t kFnv64Prime = 0x100000001B3ull;

// One byte into a running FNV-1a hash, for callers that mix as they go.
constexpr uint32_t Fnv1a32Step(uint32_t hash, uint8_t byte) { return (hash ^ byte) * kFnv32Prime; }
constexpr uint64_t Fnv1a64Step(uint64_t hash, uint8_t byte) { return (hash ^ byte) * kFnv64Prime; }

// `size` bytes at `bytes`, continuing from `hash` (a salted basis, or a hash
// of what came before).
inline uint32_t Fnv1a32(const uint8_t* bytes, size_t size, uint32_t hash = kFnv32Basis) {
  for (size_t i = 0; i < size; ++i) {
    hash = Fnv1a32Step(hash, bytes[i]);
  }
  return hash;
}

constexpr uint32_t Fnv1a32(std::string_view text, uint32_t hash = kFnv32Basis) {
  for (const char c : text) {
    hash = Fnv1a32Step(hash, uint8_t(c));
  }
  return hash;
}

inline uint64_t Fnv1a64(const uint8_t* bytes, size_t size, uint64_t hash = kFnv64Basis) {
  for (size_t i = 0; i < size; ++i) {
    hash = Fnv1a64Step(hash, bytes[i]);
  }
  return hash;
}

constexpr uint64_t Fnv1a64(std::string_view text, uint64_t hash = kFnv64Basis) {
  for (const char c : text) {
    hash = Fnv1a64Step(hash, uint8_t(c));
  }
  return hash;
}

namespace detail {
// Built at compile time, so concurrent first calls (the importer's threads)
// have nothing to race on.
constexpr std::array<uint32_t, 256> kCrc32Table = [] {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    table[i] = c;
  }
  return table;
}();
} // namespace detail

// The CRC-32 of zlib, PNG and the GameCube memory card (check value
// 0xCBF43926 for "123456789").
inline uint32_t Crc32(const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc = detail::kCrc32Table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

inline uint32_t Crc32(std::string_view text) { return Crc32(text.data(), text.size()); }

} // namespace port
