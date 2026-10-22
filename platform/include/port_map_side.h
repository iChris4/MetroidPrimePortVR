#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "port_map_marks.h"
#include "port_hash.h"
#include "port_map_pickups.h"

// The seen pickups and room marks of a save slot, kept in a small file in the
// port's user folder instead of only in the slot: a USA slot has no room for
// them next to an Archipelago trailer (the in-save "PKSN"/"RMMK" trailers are
// cut or dropped there), and even without one the "RMMK" trailer carries at
// most PortMapMarks::kTrailerMarks marks. One file per slot of the memory card
// in use: <user folder>/mapdata/<card key>-<slot>.bin.
//
// The key is the card's identity (CardIdentity): the directory the card
// actually resolves to (absolute and normalised, so two spellings of one
// directory agree), the game id and maker its files are created with, and the
// card port. A PAL and a USA card under one directory, or two channels, are
// therefore different cards here and keep their own files; the data never goes
// into the card's folder (or the emulator's), and it follows the port's own
// user folder.
//
// A file left under another card key (a user folder copied to a new install
// folder, so the card's directory differs) is still used when its fingerprint
// matches this slot: Apply scans the folder for that slot's name with any key,
// and the next save writes it under the current one.
//
// Layout, little endian: "MPMD", version 1, a 64-bit fingerprint, the seen
// word count and the words, the mark count and the 16-bit packed marks. The
// whole file is at most kMaxBytes.
//
// The fingerprint is FNV-1a over the whole slot buffer as written to the card
// (the bytes SGameFileSlot::PutTo writes, trailers included): the file is only
// used when the slot read back hashes the same, so a save replaced elsewhere
// (Dolphin, an import, a conversion, an older port build) ignores stale data
// and falls back to the slot's own trailers. The trailers are inside it because
// those are exactly the bytes between write and read; the in-save bytes change
// whenever the game state does, so the hash also follows the progress.
//
// Only the port's own saving keeps these files: the front end has no slot-copy
// feature, and an import or a save conversion writes card files directly, so a
// save brought in that way carries what its own "PKSN"/"RMMK" trailers hold
// until the game next saves it (which stages its own file).
namespace PortMapSide {

constexpr uint32_t kMagic = 0x444D504D; // "MPMD" read as a little endian word
constexpr uint32_t kVersion = 1;
constexpr size_t kSeenWords = PortMapPickups::kSeenWords;
// The header (magic, version, fingerprint, seen count, mark count) and the
// longest body the version can hold; a file longer than this is not ours.
constexpr size_t kHeaderBytes = 24;
constexpr size_t kMaxBytes = kHeaderBytes + kSeenWords * 4 + PortMapMarks::kMaxMarks * 2;

struct Data {
  uint32_t seen[kSeenWords] = {};
  PortMapMarks::List marks;
};

inline uint64_t Fingerprint(const uint8_t* bytes, size_t size) {
  return port::Fnv1a64(bytes, size);
}

// UTF-8 text to and from paths (std::filesystem's u8 forms are char8_t), as
// PortPaths::detail and PortGci keep them; inline here so the unit test needs
// neither of those.
inline std::filesystem::path PathFromUtf8(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
inline std::string PathToUtf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

// One directory, one spelling: absolute, lexically normalised and with symlinks
// resolved, so a relative path, a trailing separator, a "." or ".." and a
// symlink to the same directory all key the same files. Falls back to the path
// as given when it cannot be resolved (a permission error, a path that cannot
// be made absolute).
inline std::string NormalizedDirectory(const std::string& directory) {
  if (directory.empty()) {
    return {};
  }
  std::error_code error;
  const std::filesystem::path resolved =
      std::filesystem::weakly_canonical(PathFromUtf8(directory), error);
  return error ? directory : PathToUtf8(resolved);
}

// The identity of a card, as the key's input: the directory the card resolves
// to, the game id and maker its files are created with (what CARDInit was
// given, e.g. "GM8E01") and the card port. Two cards that differ in any of the
// three get their own files even when they share a directory; the same card
// seen through two spellings of its directory gets the same ones.
inline std::string CardIdentity(const std::string& directory, const std::string& idMaker, int cardPort) {
  std::string identity = NormalizedDirectory(directory);
  identity += '|';
  identity += idMaker;
  identity += '|';
  identity += std::to_string(cardPort);
  return identity;
}

// The file name part of a card: the hash of its identity, 16 hex digits.
inline uint64_t CardKey(const std::string& identity) {
  return Fingerprint(reinterpret_cast< const uint8_t* >(identity.data()), identity.size());
}

// Whether name is prefix, a 16 lowercase hex digit card key and suffix.
inline bool HasKeyedName(const std::string& name, const std::string& prefix, const std::string& suffix) {
  if (name.size() != prefix.size() + 16 + suffix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
      name.compare(prefix.size() + 16, std::string::npos, suffix) != 0) {
    return false;
  }
  for (size_t i = prefix.size(); i < prefix.size() + 16; ++i) {
    const char c = name[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// The regular files of the folder (not recursive) named prefix, any card key and
// suffix, in name order, except skip. Errors give a shorter list, never a throw.
inline std::vector< std::filesystem::path > KeyedFiles(const std::filesystem::path& folder,
                                                       const std::string& prefix, const std::string& suffix,
                                                       const std::filesystem::path& skip) {
  std::vector< std::filesystem::path > found;
  std::error_code error;
  std::filesystem::directory_iterator it(folder, error);
  const std::filesystem::directory_iterator end;
  while (!error && it != end) {
    const std::filesystem::path path = it->path();
    std::error_code fileError;
    if (path != skip && it->is_regular_file(fileError) && !fileError &&
        HasKeyedName(PathToUtf8(path.filename()), prefix, suffix)) {
      found.push_back(path);
    }
    it.increment(error);
  }
  std::sort(found.begin(), found.end());
  return found;
}

inline void PutU32(std::vector< uint8_t >& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast< uint8_t >(v >> (8 * i)));
  }
}

inline std::vector< uint8_t > Encode(uint64_t fingerprint, const Data& data) {
  std::vector< uint8_t > out;
  out.reserve(kMaxBytes);
  PutU32(out, kMagic);
  PutU32(out, kVersion);
  PutU32(out, static_cast< uint32_t >(fingerprint));
  PutU32(out, static_cast< uint32_t >(fingerprint >> 32));
  PutU32(out, static_cast< uint32_t >(kSeenWords));
  for (size_t i = 0; i < kSeenWords; ++i) {
    PutU32(out, data.seen[i]);
  }
  PutU32(out, data.marks.count);
  for (uint32_t i = 0; i < data.marks.count; ++i) {
    out.push_back(static_cast< uint8_t >(data.marks.packed[i]));
    out.push_back(static_cast< uint8_t >(data.marks.packed[i] >> 8));
  }
  return out;
}

// False (and data untouched) for a wrong magic/version, another fingerprint,
// a different seen size, a mark count above the cap or a truncated or longer
// file. Marks out of range or repeated are dropped.
inline bool Decode(const uint8_t* bytes, size_t size, uint64_t fingerprint, Data& data) {
  size_t pos = 0;
  bool ok = true;
  auto u32 = [&]() -> uint32_t {
    if (pos + 4 > size) {
      ok = false;
      return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= static_cast< uint32_t >(bytes[pos + i]) << (8 * i);
    }
    pos += 4;
    return v;
  };
  Data result;
  if (u32() != kMagic || u32() != kVersion) {
    return false;
  }
  const uint64_t lo = u32();
  const uint64_t hi = u32();
  if (!ok || (lo | (hi << 32)) != fingerprint || u32() != kSeenWords) {
    return false;
  }
  for (size_t i = 0; i < kSeenWords; ++i) {
    result.seen[i] = u32();
  }
  const uint32_t count = u32();
  if (!ok || count > PortMapMarks::kMaxMarks || pos + size_t(count) * 2 != size) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint16_t mark = static_cast< uint16_t >(bytes[pos] | (bytes[pos + 1] << 8));
    pos += 2;
    if (PortMapMarks::Valid(PortMapMarks::WorldOf(mark), PortMapMarks::AreaOf(mark)) &&
        !result.marks.Has(mark)) {
      result.marks.packed[result.marks.count++] = mark;
    }
  }
  data = result;
  return true;
}

// The file of one slot, read whole: false (and data untouched) when it is
// missing, unreadable, longer than kMaxBytes or does not match the fingerprint.
bool ReadFile(const std::filesystem::path& path, uint64_t fingerprint, Data& data);

// Writes one file whole, through a temporary beside it and a rename that
// replaces an existing file in one step (see ReplaceWithTemporary), so a crash, a full
// disk or a failed rename leaves the previous file (or none) in place: the old
// file is never removed first. False (and the previous file untouched) when the
// temporary could not be written or closed or the replacement failed; the
// temporary is removed either way.
bool WriteFile(const std::filesystem::path& path, uint64_t fingerprint, const Data& data);

// Puts the temporary in place of the file, replacing one that is there, in one
// step (also used by the beam door side files).
bool ReplaceWithTemporary(const std::filesystem::path& temporary, const std::filesystem::path& path);

// Removes the file of one slot; true when it is gone (also when it never was).
bool RemoveFile(const std::filesystem::path& path);

// Where the files go, from the game: the port's user folder and the identity of
// the card it has mounted (see CardIdentity). Resolved in
// platform/port_map_side_paths.cpp from PortPaths::UserFolder() and
// CMemoryCardSys::PortCard*() (nothing else implements this, so the unit test
// resolves its own directory and identity without the engine). The identity is
// empty before the card is set up, and no file is written or read then.
void Location(std::string& userFolder, std::string& cardIdentity);

#ifdef TARGET_PC
// A slot of the file the memory card driver just wrote, as SGameFileSlot holds
// it: bytes null when the file has no such slot.
struct SlotBytes {
  const uint8_t* bytes = nullptr;
  size_t size = 0;
};

// Called where a slot's buffer is rebuilt from the live game (not yet on the
// card): remembers what the file should say for that slot, and which card it is
// for.
void Stage(int slot, const uint8_t* slotBytes, size_t size, const Data& data);
// The slot is being erased from the card.
void StageErase(int slot);
// After the card write finished and the file was renamed into place: writes (or
// deletes) the file of each staged slot and drops the staging. Only a slot of
// the card that is mounted now, whose bytes in the written file are the ones
// that were staged, is written, so a save that was built and then abandoned, or
// a card that changed in between (an Archipelago room's card, the emulator-card
// setting), touches nothing. An erase only removes the file once the written
// file really has no such slot, so an erase that failed or was cancelled keeps
// the map data of the slot that is still there. Returns how many files were
// written or removed. Never call this for a write that failed.
int Flush(const SlotBytes* slots, int slotCount);
// After a slot was read back from the card and loaded into the live game:
// replaces the game's seen set and marks with the file's when it matches.
bool Apply(int slot, const uint8_t* slotBytes, size_t size, uint32_t* seen,
           PortMapMarks::List& marks);
#endif

} // namespace PortMapSide
