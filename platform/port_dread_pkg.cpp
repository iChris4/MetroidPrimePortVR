// Metroid Dread .pkg reader (port_dread_pkg.h). Written from the format notes
// (kb format/dread-pkg.md); the CRC-64 is the standard reflected table-driven form with the
// ECMA-182 polynomial, the table generated here instead of stored.

#include "port_dread_pkg.h"

#include <algorithm>
#include <array>

namespace PortDread {

namespace {

constexpr uint64_t kPoly = 0xC96C5795D7870F42ull; // reflected ECMA-182

const std::array<uint64_t, 256>& Table() {
  static const std::array<uint64_t, 256> table = [] {
    std::array<uint64_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint64_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1) ? (crc >> 1) ^ kPoly : crc >> 1;
      }
      t[i] = crc;
    }
    return t;
  }();
  return table;
}

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | uint64_t(Le32(p + 4)) << 32; }

} // namespace

uint64_t PathCrc64(std::string_view path) {
  const auto& table = Table();
  uint64_t crc = ~uint64_t(0);
  for (char c : path) {
    uint8_t b = uint8_t(c);
    if (b >= 'A' && b <= 'Z') {
      b = uint8_t(b + ('a' - 'A'));
    }
    crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

bool PkgHeaderBytes(const uint8_t* data, size_t size, size_t& bytes) {
  if (data == nullptr || size < 12) {
    return false;
  }
  const uint32_t count = Le32(data + 8);
  if (count > kMaxPkgEntries) {
    return false;
  }
  bytes = 12 + size_t(count) * 16;
  return true;
}

bool Pkg::Parse(const uint8_t* data, size_t size, std::string& error, uint64_t fileSize) {
  m_entries.clear();
  m_sorted.clear();
  size_t tableBytes = 0;
  if (!PkgHeaderBytes(data, size, tableBytes)) {
    error = size < 12 ? "pkg: truncated header" : "pkg: implausible entry count";
    return false;
  }
  if (size < tableBytes) {
    error = "pkg: entry table is cut short";
    return false;
  }
  if (fileSize == 0) {
    fileSize = size;
  }
  if (fileSize > 0xFFFFFFFFull) {
    error = "pkg: file larger than 4 GiB (offsets are 32-bit)";
    return false;
  }
  const uint32_t count = Le32(data + 8);
  std::vector<PkgEntry> entries(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = data + 12 + size_t(i) * 16;
    entries[i].assetId = Le64(e);
    entries[i].start = Le32(e + 8);
    entries[i].end = Le32(e + 12);
    if (entries[i].start < tableBytes || entries[i].start > entries[i].end || entries[i].end > fileSize) {
      error = "pkg: entry " + std::to_string(i) + " lies outside the file";
      return false;
    }
  }
  std::vector<uint32_t> sorted(count);
  for (uint32_t i = 0; i < count; ++i) {
    sorted[i] = i;
  }
  std::sort(sorted.begin(), sorted.end(), [&](uint32_t a, uint32_t b) {
    return entries[a].assetId != entries[b].assetId ? entries[a].assetId < entries[b].assetId : a < b;
  });
  m_entries = std::move(entries);
  m_sorted = std::move(sorted);
  return true;
}

const PkgEntry* Pkg::Find(uint64_t assetId) const {
  auto it = std::lower_bound(m_sorted.begin(), m_sorted.end(), assetId,
                             [&](uint32_t index, uint64_t id) { return m_entries[index].assetId < id; });
  if (it == m_sorted.end() || m_entries[*it].assetId != assetId) {
    return nullptr;
  }
  return &m_entries[*it];
}

} // namespace PortDread
