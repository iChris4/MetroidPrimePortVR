#include "port_beam_doors.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

#include "port_log.h"
#include "port_map_side.h"

// The per-slot files of the open beam doorways; see port_beam_doors.h. They share
// the map side files' folder, card key and slot fingerprint (port_map_side.h) and
// follow the same stage / flush / apply moments around the card write.
namespace PortBeamDoors {
namespace {

namespace fs = std::filesystem;

struct Staged {
  bool erase = false;
  uint64_t fingerprint = 0;
  uint32_t bits[kWords] = {};
  std::string identity;
  fs::path path;
};

std::map< int, Staged > sStaged;

Staged StagedFor(int slot, bool erase) {
  Staged staged;
  staged.erase = erase;
  std::string userFolder;
  PortMapSide::Location(userFolder, staged.identity);
  staged.path = SidePath(userFolder, staged.identity, slot);
  return staged;
}

void WarnOnce(const char* what, const fs::path& path) {
  static bool sWarned = false;
  if (!sWarned) {
    sWarned = true;
    PortLog::Write("beam doors: %s (%s); the slot starts with no unlocked doorways\n", what,
                   path.string().c_str());
  }
}

bool WriteSide(const fs::path& path, uint64_t fingerprint, const uint32_t* bits) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    PortLog::Write("beam doors: couldn't make %s: %s\n", path.parent_path().string().c_str(),
                   ec.message().c_str());
    return false;
  }
  const std::vector< uint8_t > bytes = Encode(fingerprint, bits);
  fs::path temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast< const char* >(bytes.data()), static_cast< std::streamsize >(bytes.size()));
    out.flush();
    const bool wrote = static_cast< bool >(out);
    out.close();
    if (!wrote || !out) {
      PortLog::Write("beam doors: couldn't write %s\n", temp.string().c_str());
      fs::remove(temp, ec);
      return false;
    }
  }
  if (!PortMapSide::ReplaceWithTemporary(temp, path)) {
    PortLog::Write("beam doors: couldn't replace %s\n", path.string().c_str());
    fs::remove(temp, ec);
    return false;
  }
  return true;
}

// Reads and decodes one side file for the fingerprint; warn tells a problem
// with it (once), as the file of the current key is expected to be good.
bool ReadSide(const fs::path& path, uint64_t fingerprint, uint32_t* decoded, bool warn) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (warn)
      WarnOnce("couldn't open the unlock file", path);
    return false;
  }
  // One byte past the longest valid file, so a longer one is rejected unread.
  std::vector< uint8_t > bytes(kSideMaxBytes + 1);
  in.read(reinterpret_cast< char* >(bytes.data()), static_cast< std::streamsize >(bytes.size()));
  bytes.resize(static_cast< size_t >(in.gcount()));
  if (!Decode(bytes.data(), bytes.size(), fingerprint, decoded)) {
    if (warn)
      WarnOnce("the unlock file is damaged, of another version, or belongs to another save", path);
    return false;
  }
  return true;
}

} // namespace

std::string SidePath(const std::string& userFolder, const std::string& identity, int slot) {
  if (userFolder.empty() || identity.empty())
    return {};
  char name[64];
  std::snprintf(name, sizeof(name), "beam-doors-%016llx-slot%d.bin",
                static_cast< unsigned long long >(PortMapSide::CardKey(identity)), slot);
  return PortMapSide::PathToUtf8(PortMapSide::PathFromUtf8(userFolder + "mapdata") / name);
}

void Stage(int slot, const uint8_t* slotBytes, size_t size, const uint32_t* bits) {
  Staged staged = StagedFor(slot, false);
  staged.fingerprint = PortMapSide::Fingerprint(slotBytes, size);
  for (int i = 0; i < kWords; ++i)
    staged.bits[i] = bits[i];
  sStaged[slot] = staged;
}

void StageErase(int slot) { sStaged[slot] = StagedFor(slot, true); }

int Flush(const SlotBytes* slots, int slotCount) {
  std::string userFolder;
  std::string identity;
  PortMapSide::Location(userFolder, identity);
  int touched = 0;
  for (const auto& entry : sStaged) {
    const int slot = entry.first;
    const Staged& staged = entry.second;
    if (staged.path.empty() || staged.identity != identity)
      continue; // no card, or staged for another card: not ours to touch now
    const bool inFile = slot < slotCount && slots[slot].bytes != nullptr;
    std::error_code ec;
    if (staged.erase) {
      if (!inFile && fs::remove(staged.path, ec))
        ++touched;
      continue;
    }
    if (!inFile || PortMapSide::Fingerprint(slots[slot].bytes, slots[slot].size) != staged.fingerprint)
      continue; // the card does not hold the bytes that were staged
    if (UsedBits(staged.bits) == 0) {
      if (fs::remove(staged.path, ec))
        ++touched;
    } else if (WriteSide(staged.path, staged.fingerprint, staged.bits)) {
      ++touched;
    }
  }
  sStaged.clear();
  return touched;
}

bool Apply(int slot, const uint8_t* slotBytes, size_t size, uint32_t* bits) {
  std::string userFolder;
  std::string identity;
  PortMapSide::Location(userFolder, identity);
  const fs::path path = SidePath(userFolder, identity, slot);
  if (path.empty())
    return false;
  const uint64_t fingerprint = PortMapSide::Fingerprint(slotBytes, size);
  uint32_t decoded[kWords] = {};
  std::error_code ec;
  bool found = false;
  if (fs::exists(path, ec)) {
    found = ReadSide(path, fingerprint, decoded, true);
  }
  if (!found) {
    // The user folder may have moved, which changes the card key: take a file of
    // this slot left under another key when it is for these slot bytes.
    const std::string suffix = "-slot" + std::to_string(slot) + ".bin";
    for (const fs::path& other : PortMapSide::KeyedFiles(path.parent_path(), "beam-doors-", suffix, path)) {
      if (ReadSide(other, fingerprint, decoded, false)) {
        PortLog::Write("beam doors: slot %d takes its unlocks from %s (another card path)\n", slot,
                       PortMapSide::PathToUtf8(other).c_str());
        found = true;
        break;
      }
    }
  }
  if (!found)
    return false;
  for (int i = 0; i < kWords; ++i)
    bits[i] = decoded[i];
  return true;
}

} // namespace PortBeamDoors
