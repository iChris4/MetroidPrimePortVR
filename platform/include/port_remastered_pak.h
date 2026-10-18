#pragma once

// Reads a Metroid Prime Remastered .pak (a "PACK" form) and hands out its assets.
//
// A pak is a PACK form holding a TOCC table (the asset directory ADIR, the
// metadata META and the name table STRG) followed by the assets themselves,
// each compressed on its own. The reader is a faithful port of retrotool's
// retrolib (build/mpr-tools/retrotool/lib/src/format/pack.rs), because that is
// what the game's own extractors and the .nsp tooling agree with: an extracted
// asset is the asset's bytes plus a "FOOT" footer retrotool appends, and
// byte-for-byte equality with it is what makes the port's assets interchangeable
// with retrotool's.
//
// Bytes are served through a ReadFn rather than read from a path, so the caller
// decides where they come from: a plain file, a window in a mounted pak inside
// the game's virtual disc, or (later) straight out of an encrypted image.
// Open() touches only the TOCC at the front, ReadAsset() only the one asset it
// is asked for, so a several-hundred-MB pak never lands in memory whole.
//
// Compression is the four modes the pak format uses: 0 (stored) and 1..3, the
// LZSS variants with 1, 2 and 4 byte groups. All are implemented here; the pak
// format needs no zlib, zstd or gdeflate.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "port_hash.h"

namespace PortRemastered {

// One asset directory entry (PACK::TOCC::ADIR). The three versions are the
// asset's own RFRM header values, kept so ReadAsset can check what it
// decompressed against the directory (retrotool does the same).
struct PakAsset {
  std::array<uint8_t, 16> id{};  // asset UUID, as printed by IdToString()
  uint32_t type = 0;             // FourCC, big-endian packed: 'CMDL' = 0x434D444C
  uint32_t readerVersion = 0;    // RFRM reader version
  uint32_t writerVersion = 0;    // RFRM writer version (ADIR calls it other_version)
  uint64_t offset = 0;           // where the asset's bytes start in the pak
  uint64_t size = 0;             // bytes on disc
  uint64_t decompressedSize = 0; // bytes once decompressed
  // The pak's metadata blob for this asset, if it has one: where its little
  // endian length sits in the pak, so the blob itself is read only when the
  // asset is. Metadata lives in the TOCC, not in the asset, and the game's
  // resource readers want it back in the extracted file.
  uint64_t metaLengthOffset = 0;
  bool hasMeta = false;
  // Names for this asset from the string table, in pak order. The first one is
  // the file name retrotool extracts to; all of them go into the footer.
  std::vector<std::string> names;
};

// Serves `size` bytes at `offset` in the pak into `out`. False when the range
// is not readable.
using ReadFn = std::function<bool(uint64_t offset, void* out, size_t size)>;

// std::hash has no standard specialisation for std::array, and assets are looked
// up by id, so one is provided here (FNV-1a; the ids are random, so the hash
// only has to spread them).
struct PakIdHash {
  size_t operator()(const std::array<uint8_t, 16>& id) const {
    return size_t(port::Fnv1a64(id.data(), id.size()));
  }
};

// How assets are stored. Not copyable: it holds the caller's read callback.
class Pak {
public:
  Pak() = default;
  Pak(const Pak&) = delete;
  Pak& operator=(const Pak&) = delete;

  // Parses the asset directory only. `size` is the pak's length, used to keep
  // every offset read from it inside the file. No asset data is read.
  bool Open(ReadFn read, uint64_t size, std::string& error);
  bool IsOpen() const { return m_open; }

  // In directory order, with duplicates kept (retrotool's read_full does not
  // drop them either), so extracting walks the same sequence.
  const std::vector<PakAsset>& Assets() const { return m_assets; }
  // The first asset with this id, or null.
  const PakAsset* Find(const std::array<uint8_t, 16>& id) const;

  // The asset's complete file: its decompressed RFRM bytes with retrotool's
  // FOOT footer (AINF, then META if the pak has it, then NAME per name)
  // appended, byte-identical to what `retrotool pak extract` writes.
  bool ReadAsset(const PakAsset& asset, std::vector<uint8_t>& out, std::string& error) const;

private:
  // Reads into `out`, checking the range against the pak's length first.
  bool ReadAt(uint64_t offset, size_t size, void* out, std::string& error) const;
  // Dispatches one TOCC chunk to the table parser below.
  bool ParseToccChunk(uint32_t chunkId, uint64_t body, uint64_t chunkSize,
                      std::unordered_map<std::array<uint8_t, 16>, uint64_t, PakIdHash>& metaOffsets,
                      std::unordered_map<std::array<uint8_t, 16>, std::vector<std::string>, PakIdHash>& names,
                      std::string& error);
  // ADIR: the asset directory, appended to m_assets.
  bool ParseAdir(uint64_t body, uint64_t chunkSize, std::string& error);
  // META: where each asset's metadata length sits in the pak.
  bool ParseMeta(uint64_t body, uint64_t chunkSize,
                 std::unordered_map<std::array<uint8_t, 16>, uint64_t, PakIdHash>& metaOffsets,
                 std::string& error) const;
  // STRG: the names per asset, in pak order.
  bool ParseStrg(uint64_t body, uint64_t chunkSize,
                 std::unordered_map<std::array<uint8_t, 16>, std::vector<std::string>, PakIdHash>& names,
                 std::string& error) const;

  ReadFn m_read;
  uint64_t m_size = 0;
  bool m_open = false;
  std::vector<PakAsset> m_assets;
  std::unordered_map<std::array<uint8_t, 16>, size_t, PakIdHash> m_byId;
};

// The UUID as retrotool prints it in file names: lower case hex, hyphenated
// 8-4-4-4-12. Takes the id in the same byte order PakAsset::id holds.
std::string IdToString(const std::array<uint8_t, 16>& id);
// A FourCC the same way: 'CMDL'.
std::string FourCCString(uint32_t type);

} // namespace PortRemastered