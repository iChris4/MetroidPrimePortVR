#include "port_map_side.h"

#include <cstdio>
#include <map>

#include "port_log.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace PortMapSide {
namespace {

namespace fs = std::filesystem;

// What one card write should leave behind, held from the moment the slot's
// buffer is rebuilt until the write finished: the card the buffer and the data
// belong to, and the file to write for it. Both are taken at Stage, so a card
// that changes in between cannot make this write land on another card.
struct Staged {
  bool erase = false;
  uint64_t fingerprint = 0;
  Data data;
  std::string identity;
  fs::path path;
};

std::map< int, Staged > sStaged;

fs::path SlotPath(const std::string& userFolder, const std::string& identity, int slot) {
  if (userFolder.empty() || identity.empty()) {
    return {};
  }
  char name[48];
  std::snprintf(name, sizeof(name), "%016llx-%d.bin",
                static_cast< unsigned long long >(CardKey(identity)), slot);
  return PathFromUtf8(userFolder + "mapdata") / name;
}

// The staged record of a slot of the card in use now, or a path-less one when
// there is no such card.
Staged StagedFor(int slot, bool erase) {
  Staged staged;
  staged.erase = erase;
  std::string userFolder;
  Location(userFolder, staged.identity);
  staged.path = SlotPath(userFolder, staged.identity, slot);
  return staged;
}

} // namespace

// Puts the temporary in place of the file, replacing one that is there. The C
// rename replaces an existing destination on POSIX; on Windows it does not, and
// removing the old file first would leave a moment with neither file (the same
// as port_ap_protocol.cpp's writer): MoveFileExW replaces it in one step and
// writes it through.
bool ReplaceWithTemporary(const fs::path& temporary, const fs::path& path) {
  if (std::rename(PathToUtf8(temporary).c_str(), PathToUtf8(path).c_str()) == 0) {
    return true;
  }
#ifdef _WIN32
  return MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return false;
#endif
}

bool ReadFile(const fs::path& path, uint64_t fingerprint, Data& data) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::vector< uint8_t > bytes;
  bytes.reserve(kMaxBytes);
  // One byte more than a file of this version can be, so a longer one is
  // rejected without reading all of it.
  char buffer[kMaxBytes + 1];
  while (bytes.size() <= kMaxBytes && in.read(buffer, sizeof(buffer)).gcount() != 0) {
    const std::streamsize got = in.gcount();
    bytes.insert(bytes.end(), buffer, buffer + got);
  }
  Data decoded;
  if (bytes.size() > kMaxBytes || !Decode(bytes.data(), bytes.size(), fingerprint, decoded)) {
    return false;
  }
  data = decoded;
  return true;
}

bool WriteFile(const fs::path& path, uint64_t fingerprint, const Data& data) {
  std::error_code ec;
  if (!path.parent_path().empty()) {
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
      PortLog::Write("map data: couldn't make %s: %s\n", path.parent_path().string().c_str(),
                     ec.message().c_str());
      return false;
    }
  }
  const std::vector< uint8_t > bytes = Encode(fingerprint, data);
  fs::path temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast< const char* >(bytes.data()),
              static_cast< std::streamsize >(bytes.size()));
    // A full disk shows up at the flush or at the close, where the flush's last
    // buffer goes, and the rename only follows a close that worked: a half
    // written temporary must never take the previous file's place.
    out.flush();
    if (!out) {
      PortLog::Write("map data: couldn't write %s\n", temp.string().c_str());
      out.close();
      fs::remove(temp, ec);
      return false;
    }
    out.close();
    if (!out) {
      PortLog::Write("map data: couldn't finish %s\n", temp.string().c_str());
      fs::remove(temp, ec);
      return false;
    }
  }
  if (!ReplaceWithTemporary(temp, path)) {
    PortLog::Write("map data: couldn't replace %s\n", path.string().c_str());
    fs::remove(temp, ec);
    return false;
  }
  return true;
}

bool RemoveFile(const fs::path& path) {
  std::error_code ec;
  fs::remove(path, ec);
  if (ec) {
    PortLog::Write("map data: couldn't delete %s: %s\n", path.string().c_str(),
                   ec.message().c_str());
    return false;
  }
  return true;
}

#ifdef TARGET_PC
void Stage(int slot, const uint8_t* slotBytes, size_t size, const Data& data) {
  Staged staged = StagedFor(slot, false);
  staged.fingerprint = Fingerprint(slotBytes, size);
  staged.data = data;
  sStaged[slot] = staged;
}

void StageErase(int slot) { sStaged[slot] = StagedFor(slot, true); }

int Flush(const SlotBytes* slots, int slotCount) {
  std::string userFolder;
  std::string identity;
  Location(userFolder, identity);
  int written = 0;
  for (const auto& entry : sStaged) {
    const int slot = entry.first;
    if (entry.second.path.empty()) {
      continue;
    }
    if (entry.second.identity != identity) {
      // Staged for another card (a switch while a save was in flight): its
      // files are not ours to write or delete now.
      PortLog::Write("map data: slot %d was staged for another card; leaving it\n", slot);
      continue;
    }
    const bool inFile = slot < slotCount && slots[slot].bytes != nullptr;
    if (entry.second.erase) {
      // Only once the file that was just written really has no such slot: an
      // erase that failed or was cancelled leaves the slot, and its map data,
      // in place.
      if (inFile) {
        continue;
      }
      written += RemoveFile(entry.second.path) ? 1 : 0;
      continue;
    }
    if (!inFile) {
      // The file written has no such slot (a save that was built and then
      // abandoned), so the card does not hold these bytes.
      continue;
    }
    if (Fingerprint(slots[slot].bytes, slots[slot].size) != entry.second.fingerprint) {
      PortLog::Write("map data: slot %d was not the save that was staged; leaving it\n", slot);
      continue;
    }
    written += WriteFile(entry.second.path, entry.second.fingerprint, entry.second.data) ? 1 : 0;
  }
  sStaged.clear();
  return written;
}

bool Apply(int slot, const uint8_t* slotBytes, size_t size, uint32_t* seen,
           PortMapMarks::List& marks) {
  std::string userFolder;
  std::string identity;
  Location(userFolder, identity);
  const fs::path path = SlotPath(userFolder, identity, slot);
  if (path.empty()) {
    return false;
  }
  const uint64_t fingerprint = Fingerprint(slotBytes, size);
  Data data;
  if (!ReadFile(path, fingerprint, data)) {
    // The user folder may have moved, which changes the card key: take a file of
    // this slot left under another key when it is for these slot bytes.
    bool adopted = false;
    for (const fs::path& other :
         KeyedFiles(path.parent_path(), "", "-" + std::to_string(slot) + ".bin", path)) {
      if (ReadFile(other, fingerprint, data)) {
        PortLog::Write("map data: slot %d takes its data from %s (another card path)\n", slot,
                       PathToUtf8(other).c_str());
        adopted = true;
        break;
      }
    }
    if (!adopted) {
      return false;
    }
  }
  for (size_t i = 0; i < kSeenWords; ++i) {
    seen[i] = data.seen[i];
  }
  marks = data.marks;
  return true;
}
#endif

} // namespace PortMapSide
