// Metroid Prime Remastered pak reader (port_remastered_pak.h): a port of
// retrotool's retrolib pak code, with its LZSS decompressor written out here.
//
// Structure of a pak, all little endian:
//   RFRM form   size, "PACK", reader 1         <- the pak form
//     RFRM form size, "TOCC", reader 3         <- the table of contents
//       ADIR chunk  count + count * 52 bytes    <- the asset directory
//       META chunk  entries + length-prefixed blobs
//       STRG chunk  entries + length-prefixed names
//     asset data, one blob per ADIR entry
// Forms are a 32 byte header (magic, size, unknown, id, reader, writer) and
// chunks a 24 byte one (id, size, unknown, skip), both of which may carry a
// `skip` of padding before their data.

#include "port_remastered_pak.h"
#include "port_bytes.h"
#include "port_remastered_lzss.h"

#include <cstring>
#include <string>

namespace PortRemastered {
namespace {

using port::FourCC;

constexpr uint32_t kFormRFRM = FourCC('R', 'F', 'R', 'M'); // the magic of every form
constexpr uint32_t kFormPACK = FourCC('P', 'A', 'C', 'K');
constexpr uint32_t kFormTOCC = FourCC('T', 'O', 'C', 'C');
constexpr uint32_t kFormFOOT = FourCC('F', 'O', 'O', 'T'); // retrotool's own footer
constexpr uint32_t kChunkADIR = FourCC('A', 'D', 'I', 'R');
constexpr uint32_t kChunkMETA = FourCC('M', 'E', 'T', 'A');
constexpr uint32_t kChunkSTRG = FourCC('S', 'T', 'R', 'G');
constexpr uint32_t kChunkAINF = FourCC('A', 'I', 'N', 'F'); // in the footer
constexpr uint32_t kChunkNAME = FourCC('N', 'A', 'M', 'E'); // in the footer

constexpr size_t kFormSize = 32;
constexpr size_t kChunkSize = 24;
constexpr size_t kAdirEntrySize = 52;  // type 4 + id 16 + version 4 + other 4 + 3 * 8
constexpr size_t kMetaEntrySize = 20;  // id 16 + offset 4
constexpr size_t kStrgEntrySize = 24;  // kind 4 + id 16 + name length 4
constexpr size_t kAssetInfoSize = 28;   // id 16 + mode 4 + offset 8

// An asset's directory entry, as it is on disc (little endian).
struct AdirEntry {
  uint32_t type = 0;
  std::array<uint8_t, 16> id{};
  uint32_t readerVersion = 0;
  uint32_t writerVersion = 0;
  uint64_t offset = 0;
  uint64_t decompressedSize = 0;
  uint64_t size = 0;
};

// A form header: magic, size, unknown, id, reader version, writer version.
struct Form {
  uint32_t magic = 0;
  uint64_t size = 0;
  uint32_t id = 0;
  uint32_t readerVersion = 0;
  uint32_t writerVersion = 0;
};

// A chunk header: id, size, unknown, skip (bytes of padding before the data).
struct Chunk {
  uint32_t id = 0;
  uint64_t size = 0;
  uint32_t unk = 0;
  uint64_t skip = 0;
};

// FourCCs are byte arrays in the format, and this port packs them big endian so
// they can be compared and printed like the retail PAK's resource types.
using port::ReadBE32;
using port::ReadLE32;
using port::ReadLE64;

Form ParseForm(const uint8_t* p) {
  Form form;
  form.magic = ReadBE32(p);
  form.size = ReadLE64(p + 4);
  form.id = ReadBE32(p + 20);
  form.readerVersion = ReadLE32(p + 24);
  form.writerVersion = ReadLE32(p + 28);
  return form;
}

Chunk ParseChunk(const uint8_t* p) {
  Chunk chunk;
  chunk.id = ReadBE32(p);
  chunk.size = ReadLE64(p + 4);
  chunk.unk = ReadLE32(p + 12);
  chunk.skip = ReadLE64(p + 16);
  return chunk;
}

// FourCCs are byte strings in the format; the constants here are packed big
// endian so they can be compared as numbers, so they are written back that way.
void AppendFourCC(std::vector<uint8_t>& out, uint32_t fourCC) {
  for (int i = 3; i >= 0; --i) {
    out.push_back(uint8_t(fourCC >> (8 * i)));
  }
}

void AppendLE32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (8 * i)));
  }
}

void AppendLE64(std::vector<uint8_t>& out, uint64_t value) {
  AppendLE32(out, uint32_t(value));
  AppendLE32(out, uint32_t(value >> 32));
}

// The pak stores UUIDs little endian (retrotool's Uuid::from_bytes_le): each of
// the first three fields is byte swapped relative to the printed form. Ids are
// kept printed, so only this direction has to swap.
std::array<uint8_t, 16> UuidFromLe(const uint8_t* p) {
  std::array<uint8_t, 16> id{};
  id[0] = p[3];
  id[1] = p[2];
  id[2] = p[1];
  id[3] = p[0];
  id[4] = p[5];
  id[5] = p[4];
  id[6] = p[7];
  id[7] = p[6];
  for (int i = 8; i < 16; ++i) {
    id[i] = p[i];
  }
  return id;
}

std::array<uint8_t, 16> UuidToLe(const std::array<uint8_t, 16>& id) {
  std::array<uint8_t, 16> out{};
  out[0] = id[3];
  out[1] = id[2];
  out[2] = id[1];
  out[3] = id[0];
  out[4] = id[5];
  out[5] = id[4];
  out[6] = id[7];
  out[7] = id[6];
  for (int i = 8; i < 16; ++i) {
    out[i] = id[i];
  }
  return out;
}

std::string Hex(uint8_t byte) {
  static const char kDigits[] = "0123456789abcdef";
  std::string text(2, '0');
  text[0] = kDigits[byte >> 4];
  text[1] = kDigits[byte & 0x0F];
  return text;
}

// retrotool's decompress_into (util/compression.rs) over a whole asset blob.
// `mode` is reported back because it goes into the extracted file's AINF chunk.
bool DecompressAsset(const uint8_t* in, size_t inSize, std::vector<uint8_t>& out, uint32_t& mode,
                     std::string& error) {
  if (inSize < 4) {
    error = "remastered pak: compressed asset is shorter than its mode";
    return false;
  }
  mode = ReadLE32(in);
  const uint8_t* data = in + 4;
  const size_t dataSize = inSize - 4;
  switch (mode) {
    case 0:
      if (dataSize != out.size()) {
        error = "remastered pak: stored asset is " + std::to_string(dataSize) + " bytes, the directory says " +
                std::to_string(out.size());
        return false;
      }
      std::memcpy(out.data(), data, dataSize);
      return true;
    case 1:
    case 2:
    case 3:
      if (!LzssDecompress(mode, data, dataSize, out.data(), out.size())) {
        error = "remastered pak: LZSS mode " + std::to_string(mode) + " decompression failed";
        return false;
      }
      return true;
    default:
      error = "remastered pak: unsupported compression mode " + std::to_string(mode);
      return false;
  }
}

bool RangeFits(uint64_t offset, uint64_t length, uint64_t limit) {
  return offset <= limit && length <= limit - offset;
}

} // namespace

std::string IdToString(const std::array<uint8_t, 16>& id) {
  static const int kGroupStart[] = {0, 4, 6, 8, 10};
  static const int kGroupLength[] = {4, 2, 2, 2, 6};
  std::string text;
  text.reserve(36);
  for (int group = 0; group < 5; ++group) {
    if (group > 0) {
      text += '-';
    }
    for (int i = 0; i < kGroupLength[group]; ++i) {
      text += Hex(id[size_t(kGroupStart[group] + i)]);
    }
  }
  return text;
}

std::string FourCCString(uint32_t type) {
  std::string text;
  text.reserve(4);
  for (int i = 3; i >= 0; --i) {
    const char c = char(type >> (8 * i));
    // The game has no odd FourCCs; anything unprintable becomes '?' so a stray
    // one cannot put a control character in a file name.
    text += (c >= 32 && c < 127) ? c : '?';
  }
  return text;
}

bool Pak::Open(ReadFn read, uint64_t size, std::string& error) {
  m_read = std::move(read);
  m_size = size;
  m_assets.clear();
  m_byId.clear();
  m_open = false;
  if (!m_read) {
    error = "remastered pak: no read callback";
    return false;
  }
  if (size < kFormSize) {
    error = "remastered pak: file is too small to hold a form";
    return false;
  }

  uint8_t header[kFormSize];
  if (!ReadAt(0, kFormSize, header, error)) {
    return false;
  }
  const Form pack = ParseForm(header);
  if (pack.magic != kFormRFRM) {
    error = "remastered pak: not an RFRM pak (magic is " + FourCCString(pack.magic) + ")";
    return false;
  }
  if (pack.id != kFormPACK || pack.readerVersion != 1) {
    error = "remastered pak: expected a PACK form of reader version 1, got " + FourCCString(pack.id) + " " +
            std::to_string(pack.readerVersion);
    return false;
  }

  // The TOCC sits right at the front of the pak's contents.
  if (pack.size < kFormSize || !RangeFits(kFormSize, pack.size, m_size)) {
    error = "remastered pak: PACK form runs past the end of the file";
    return false;
  }
  if (!ReadAt(kFormSize, kFormSize, header, error)) {
    return false;
  }
  const Form tocc = ParseForm(header);
  if (tocc.id != kFormTOCC || tocc.readerVersion != 3) {
    error = "remastered pak: expected a TOCC form of reader version 3, got " + FourCCString(tocc.id) + " " +
            std::to_string(tocc.readerVersion);
    return false;
  }

  // Metadata and names are keyed by id and may come before ADIR, so they are
  // collected first and applied to the directory afterwards.
  std::unordered_map<std::array<uint8_t, 16>, uint64_t, PakIdHash> metaOffsets;
  std::unordered_map<std::array<uint8_t, 16>, std::vector<std::string>, PakIdHash> names;

  const uint64_t toccStart = kFormSize + kFormSize;
  if (!RangeFits(toccStart, tocc.size, m_size)) {
    error = "remastered pak: TOCC form runs past the end of the file";
    return false;
  }
  uint64_t at = toccStart;
  const uint64_t toccEnd = toccStart + tocc.size;
  while (at < toccEnd) {
    uint8_t chunkHeader[kChunkSize];
    if (!ReadAt(at, kChunkSize, chunkHeader, error)) {
      return false;
    }
    const Chunk chunk = ParseChunk(chunkHeader);
    if (!RangeFits(at, uint64_t(kChunkSize) + chunk.skip, m_size) ||
        !RangeFits(at + kChunkSize + chunk.skip, chunk.size, toccEnd)) {
      error = "remastered pak: TOCC chunk " + FourCCString(chunk.id) + " runs past the end of the TOCC";
      return false;
    }
    const uint64_t body = at + uint64_t(kChunkSize) + chunk.skip;
    if (!ParseToccChunk(chunk.id, body, chunk.size, metaOffsets, names, error)) {
      return false;
    }
    at = body + chunk.size;
  }

  if (m_assets.empty()) {
    error = "remastered pak: no asset directory in the table of contents";
    return false;
  }
  for (PakAsset& asset : m_assets) {
    const auto meta = metaOffsets.find(asset.id);
    if (meta != metaOffsets.end()) {
      asset.hasMeta = true;
      asset.metaLengthOffset = meta->second;
    }
    const auto entry = names.find(asset.id);
    if (entry != names.end()) {
      asset.names = entry->second;
    }
    // First entry wins for a repeated id, the way retrotool's read_asset finds
    // its asset.
    m_byId.emplace(asset.id, size_t(&asset - m_assets.data()));
  }
  m_open = true;
  return true;
}

bool Pak::ParseToccChunk(uint32_t chunkId, uint64_t body, uint64_t chunkSize,
                         std::unordered_map<std::array<uint8_t, 16>, uint64_t, PakIdHash>& metaOffsets,
                         std::unordered_map<std::array<uint8_t, 16>, std::vector<std::string>, PakIdHash>& names,
                         std::string& error) {
  switch (chunkId) {
    case kChunkADIR:
      return ParseAdir(body, chunkSize, error);
    case kChunkMETA:
      return ParseMeta(body, chunkSize, metaOffsets, error);
    case kChunkSTRG:
      return ParseStrg(body, chunkSize, names, error);
    default:
      // retrotool refuses a TOCC chunk it does not know, so a pak laid out
      // differently is reported rather than half read.
      error = "remastered pak: unknown table of contents chunk " + FourCCString(chunkId);
      return false;
  }
}

bool Pak::ParseAdir(uint64_t body, uint64_t chunkSize, std::string& error) {
  if (chunkSize < 4) {
    error = "remastered pak: asset directory is cut short";
    return false;
  }
  uint8_t countBytes[4];
  if (!ReadAt(body, 4, countBytes, error)) {
    return false;
  }
  const uint64_t count = ReadLE32(countBytes);
  if (count > (chunkSize - 4) / kAdirEntrySize) {
    error = "remastered pak: asset directory holds " + std::to_string(count) + " entries but only " +
            std::to_string((chunkSize - 4) / kAdirEntrySize) + " fit";
    return false;
  }
  std::vector<uint8_t> entries(size_t(count) * kAdirEntrySize);
  if (!entries.empty() && !ReadAt(body + 4, entries.size(), entries.data(), error)) {
    return false;
  }
  m_assets.reserve(m_assets.size() + size_t(count));
  for (size_t i = 0; i < size_t(count); ++i) {
    const uint8_t* p = entries.data() + i * kAdirEntrySize;
    AdirEntry entry;
    entry.type = ReadBE32(p);
    entry.id = UuidFromLe(p + 4);
    entry.readerVersion = ReadLE32(p + 20);
    entry.writerVersion = ReadLE32(p + 24);
    entry.offset = ReadLE64(p + 28);
    entry.decompressedSize = ReadLE64(p + 36);
    entry.size = ReadLE64(p + 44);
    PakAsset asset;
    asset.id = entry.id;
    asset.type = entry.type;
    asset.readerVersion = entry.readerVersion;
    asset.writerVersion = entry.writerVersion;
    asset.offset = entry.offset;
    asset.size = entry.size;
    asset.decompressedSize = entry.decompressedSize;
    m_assets.push_back(std::move(asset));
  }
  return true;
}

bool Pak::ParseMeta(uint64_t body, uint64_t chunkSize,
                    std::unordered_map<std::array<uint8_t, 16>, uint64_t, PakIdHash>& metaOffsets,
                    std::string& error) const {
  if (chunkSize < 4) {
    error = "remastered pak: metadata table is cut short";
    return false;
  }
  uint8_t countBytes[4];
  if (!ReadAt(body, 4, countBytes, error)) {
    return false;
  }
  const uint64_t count = ReadLE32(countBytes);
  if (count > (chunkSize - 4) / kMetaEntrySize) {
    error = "remastered pak: metadata table is shorter than its entry count";
    return false;
  }
  std::vector<uint8_t> entries(size_t(count) * kMetaEntrySize);
  if (!entries.empty() && !ReadAt(body + 4, entries.size(), entries.data(), error)) {
    return false;
  }
  for (size_t i = 0; i < size_t(count); ++i) {
    const uint8_t* p = entries.data() + i * kMetaEntrySize;
    // The entry's offset is from the start of the chunk data, and points at the
    // metadata's own little endian length. Last entry wins for a repeated id,
    // as in retrotool's read_full.
    const uint64_t offset = ReadLE32(p + 16);
    if (!RangeFits(offset, 4, chunkSize)) {
      error = "remastered pak: metadata entry points outside its table";
      return false;
    }
    metaOffsets[UuidFromLe(p)] = body + offset;
  }
  return true;
}

bool Pak::ParseStrg(uint64_t body, uint64_t chunkSize,
                    std::unordered_map<std::array<uint8_t, 16>, std::vector<std::string>, PakIdHash>& names,
                    std::string& error) const {
  if (chunkSize < 4) {
    error = "remastered pak: string table is cut short";
    return false;
  }
  uint8_t countBytes[4];
  if (!ReadAt(body, 4, countBytes, error)) {
    return false;
  }
  const uint64_t count = ReadLE32(countBytes);
  // Entries are variable length (each carries its own name), so they are read
  // one at a time rather than as an array.
  uint64_t at = body + 4;
  uint8_t entry[kStrgEntrySize];
  for (uint64_t i = 0; i < count; ++i) {
    if (!RangeFits(at, kStrgEntrySize, body + chunkSize) || !ReadAt(at, kStrgEntrySize, entry, error)) {
      error = "remastered pak: string table entry runs past the end of the table";
      return false;
    }
    const std::array<uint8_t, 16> id = UuidFromLe(entry + 4);
    const uint64_t nameLength = ReadLE32(entry + 20);
    at += kStrgEntrySize;
    if (!RangeFits(at, nameLength, body + chunkSize)) {
      error = "remastered pak: string table entry runs past the end of the table";
      return false;
    }
    std::vector<uint8_t> name;
    name.resize(size_t(nameLength));
    if (!name.empty() && !ReadAt(at, name.size(), name.data(), error)) {
      return false;
    }
    at += nameLength;
    // An asset can have several names; the first is retrotool's file name and
    // all of them go into the footer.
    names[id].emplace_back(reinterpret_cast<const char*>(name.data()), name.size());
  }
  return true;
}

const PakAsset* Pak::Find(const std::array<uint8_t, 16>& id) const {
  const auto entry = m_byId.find(id);
  if (entry == m_byId.end()) {
    return nullptr;
  }
  return &m_assets[entry->second];
}

bool Pak::ReadAt(uint64_t offset, size_t size, void* out, std::string& error) const {
  if (!RangeFits(offset, size, m_size)) {
    error = "remastered pak: read of " + std::to_string(size) + " bytes at " + std::to_string(offset) +
            " runs past the end of the " + std::to_string(m_size) + " byte pak";
    return false;
  }
  if (!m_read(offset, out, size)) {
    error = "remastered pak: read of " + std::to_string(size) + " bytes at " + std::to_string(offset) + " failed";
    return false;
  }
  return true;
}

bool Pak::ReadAsset(const PakAsset& asset, std::vector<uint8_t>& out, std::string& error) const {
  out.clear();
  if (!m_open) {
    error = "remastered pak: the pak is not open";
    return false;
  }
  if (!RangeFits(asset.offset, asset.size, m_size)) {
    error = "remastered pak: asset " + IdToString(asset.id) + " runs past the end of the pak";
    return false;
  }
  // Refuse a decompressed size that no real asset has before reserving for it.
  constexpr uint64_t kMaxAssetSize = uint64_t(1) << 32;
  if (asset.decompressedSize > kMaxAssetSize) {
    error = "remastered pak: asset " + IdToString(asset.id) + " claims an impossible size";
    return false;
  }

  std::vector<uint8_t> stored(size_t(asset.size));
  if (!stored.empty() && !ReadAt(asset.offset, stored.size(), stored.data(), error)) {
    return false;
  }

  // The asset's bytes as the game's readers see them: compressed, prefixed with
  // a four byte mode, unless the directory says it is stored as it is.
  uint32_t mode = 0;
  if (asset.size == asset.decompressedSize) {
    out = std::move(stored);
  } else {
    if (stored.size() < 4) {
      error = "remastered pak: asset " + IdToString(asset.id) + " is shorter than its compression mode";
      return false;
    }
    mode = ReadLE32(stored.data());
    if (mode == 0) {
      // A zero mode means the rest of the blob is the asset as it is.
      out.assign(stored.begin() + 4, stored.end());
    } else {
      // A back reference is two bytes plus a header bit and copies at most 64 (about
      // 30 times its size), so anything claiming 40 times its stored size is corrupt, and
      // is refused before the buffer is allocated.
      if (asset.decompressedSize > stored.size() * 40) {
        error = "remastered pak: asset " + IdToString(asset.id) + " claims more than its data can hold";
        return false;
      }
      out.assign(size_t(asset.decompressedSize), 0);
      if (!DecompressAsset(stored.data(), stored.size(), out, mode, error)) {
        return false;
      }
    }
  }

  // Check the asset against the directory, the way retrotool does: the form's
  // id and versions must be the entry's, and its size must be what the
  // directory says minus the 32 byte form header.
  if (out.size() < kFormSize) {
    error = "remastered pak: asset " + IdToString(asset.id) + " is too small to be a resource";
    return false;
  }
  const Form form = ParseForm(out.data());
  if (form.magic != kFormRFRM || form.id != asset.type || form.readerVersion != asset.readerVersion ||
      form.writerVersion != asset.writerVersion || form.size + kFormSize != asset.decompressedSize) {
    error = "remastered pak: asset " + IdToString(asset.id) + " does not match its directory entry";
    return false;
  }

  // Append retrotool's footer: the FOOT form, then AINF (what the extracted
  // file records about where it came from), then the pak's metadata, then a
  // NAME chunk per name. Retrotool's own extractors look for these, and an
  // asset repackaged with them has to look identical.
  const size_t footerStart = out.size();
  AppendFourCC(out, kFormRFRM); // forms are written magic first, id in the header
  AppendLE64(out, 0);          // size, backpatched below
  AppendLE64(out, 0);          // the form header's unknown
  AppendFourCC(out, kFormFOOT);
  AppendLE32(out, 1); // reader version
  AppendLE32(out, 1); // writer version

  AppendFourCC(out, kChunkAINF);
  AppendLE64(out, kAssetInfoSize);
  AppendLE32(out, 0); // unknown
  AppendLE64(out, 0); // skip
  const std::array<uint8_t, 16> littleId = UuidToLe(asset.id);
  out.insert(out.end(), littleId.begin(), littleId.end());
  AppendLE32(out, mode);
  AppendLE64(out, asset.offset);

  if (asset.hasMeta) {
    uint8_t lengthBytes[4];
    if (!ReadAt(asset.metaLengthOffset, 4, lengthBytes, error)) {
      return false;
    }
    const uint64_t metaLength = ReadLE32(lengthBytes);
    if (!RangeFits(asset.metaLengthOffset + 4, metaLength, m_size)) {
      error = "remastered pak: metadata for " + IdToString(asset.id) + " runs past the end of the pak";
      return false;
    }
    std::vector<uint8_t> meta;
    meta.resize(size_t(metaLength));
    if (!meta.empty() && !ReadAt(asset.metaLengthOffset + 4, meta.size(), meta.data(), error)) {
      return false;
    }
    AppendFourCC(out, kChunkMETA);
    AppendLE64(out, metaLength);
    AppendLE32(out, 0);
    AppendLE64(out, 0);
    out.insert(out.end(), meta.begin(), meta.end());
  }

  for (const std::string& name : asset.names) {
    AppendFourCC(out, kChunkNAME);
    AppendLE64(out, name.size());
    AppendLE32(out, 0);
    AppendLE64(out, 0);
    out.insert(out.end(), name.begin(), name.end());
  }

  // Backpatch the footer's size: everything after its own header.
  const uint64_t footerSize = out.size() - footerStart - kFormSize;
  for (int i = 0; i < 8; ++i) {
    out[footerStart + 4 + size_t(i)] = uint8_t(footerSize >> (8 * i));
  }
  return true;
}

} // namespace PortRemastered