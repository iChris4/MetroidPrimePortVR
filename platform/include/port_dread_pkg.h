#pragma once

// Reader for Metroid Dread's .pkg archives (kb format/dread-pkg.md), read out of the
// user's own game image. A pkg is an uncompressed table of assets keyed by the CRC-64
// of the lower-case asset path; the files sit in it raw.
//
//   u32 header_size    bytes after this field up to the 128-aligned data start
//   u32 data_size
//   u32 count
//   count * { u64 asset_id; u32 start; u32 end }    offsets from the FILE start, end exclusive
//
// The parser works on bytes already in memory, and needs only the header (use
// PkgHeaderBytes() to know how much to read first when the pkg is big).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace PortDread {

// CRC-64 of `path` as the pkg tables key it: init ~0, reflected table-driven, no final xor,
// over the path's bytes lower-cased (ASCII).
uint64_t PathCrc64(std::string_view path);

struct PkgEntry {
  uint64_t assetId = 0;
  uint32_t start = 0; // from the start of the pkg file
  uint32_t end = 0;   // exclusive
  uint32_t Size() const { return end - start; }
};

// How many bytes of the pkg (12 + 16 * count) hold the entry table, from its first
// 12 bytes. False when `size` < 12 or the count is implausible (> kMaxPkgEntries).
constexpr uint32_t kMaxPkgEntries = 1u << 20;
bool PkgHeaderBytes(const uint8_t* data, size_t size, size_t& bytes);

class Pkg {
public:
  // Parses the table from the first `size` bytes of a pkg that is `fileSize` bytes long
  // (0 = `size`: the whole file is in memory). Every entry must satisfy
  // table end <= start <= end <= fileSize. On failure the Pkg is empty and `error` is set.
  bool Parse(const uint8_t* data, size_t size, std::string& error, uint64_t fileSize = 0);

  // In file order.
  const std::vector<PkgEntry>& Entries() const { return m_entries; }
  size_t Count() const { return m_entries.size(); }
  // nullptr when absent. With duplicate ids, the first in file order.
  const PkgEntry* Find(uint64_t assetId) const;
  const PkgEntry* Find(std::string_view path) const { return Find(PathCrc64(path)); }

private:
  std::vector<PkgEntry> m_entries;
  std::vector<uint32_t> m_sorted; // indices into m_entries, by (assetId, index)
};

} // namespace PortDread
