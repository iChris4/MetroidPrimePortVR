// The seen-pickup / room-mark side file: its format, its card key, its file
// layer and the staging the memory card driver goes through (see
// port_map_side.h). The engine's own wiring - the four call sites in
// CMemoryCardDriver.cpp and the card identity the files are keyed on - is
// checked by tests/port_map_side_wiring.py, so this test needs no card stack:
// it provides PortMapSide::Location itself.
#include "port_map_side.h"
#include "port_test.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

namespace fs = std::filesystem;

fs::path sDirectory;
std::string sIdentity;

std::string ReadRaw(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator< char >(in)), std::istreambuf_iterator< char >());
}

bool FileExists(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

int CountFiles(const fs::path& dir) {
  int count = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    count += fs::is_regular_file(entry.path(), ec) ? 1 : 0;
  }
  return count;
}

} // namespace

// What platform/port_map_side_paths.cpp does in the game: the port's user
// folder and the identity of the mounted card.
namespace PortMapSide {
void Location(std::string& userFolder, std::string& cardIdentity) {
  userFolder = sDirectory.string() + "/";
  cardIdentity = sIdentity;
}
} // namespace PortMapSide

int main() {
  using namespace PortMapSide;
  using PortMapMarks::Pack;

  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path root = fs::temp_directory_path(ec) / "port_map_side_tests";
  fs::remove_all(root, ec);
  fs::create_directories(root / "cards" / "usa", ec);
  sDirectory = root;

  uint8_t slot[940];
  for (size_t i = 0; i < sizeof(slot); ++i) {
    slot[i] = static_cast< uint8_t >(i * 7 + 3);
  }
  const uint64_t fp = Fingerprint(slot, sizeof(slot));

  Data in;
  in.seen[0] = 0x80000001u;
  in.seen[3] = 0xDEADBEEFu;
  // More marks than the in-save trailer carries.
  for (unsigned i = 0; i < PortMapMarks::kMaxMarks; ++i) {
    REQUIRE_MSG(in.marks.Toggle(Pack(i % 7, i)) == 1, "mark added");
  }
  REQUIRE_MSG(in.marks.Toggle(Pack(7, 127)) == 0 && in.marks.count == 64, "cap of 64");

  // --- the codec -------------------------------------------------------------
  const std::vector< uint8_t > bytes = Encode(fp, in);
  REQUIRE_MSG(bytes.size() <= kMaxBytes && bytes.size() == kMaxBytes, "bounded");
  Data out;
  REQUIRE_MSG(Decode(bytes.data(), bytes.size(), fp, out), "round trip");
  REQUIRE_MSG(out.seen[0] == in.seen[0] && out.seen[3] == in.seen[3] && out.seen[1] == 0, "seen words");
  REQUIRE_MSG(out.marks.count == 64, "all 64 marks");
  for (uint32_t i = 0; i < 64; ++i) {
    REQUIRE_MSG(out.marks.packed[i] == in.marks.packed[i], "mark order");
  }

  // The slot changed anywhere: the fingerprint no longer matches.
  slot[500] ^= 1;
  Data stale;
  REQUIRE_MSG(!Decode(bytes.data(), bytes.size(), Fingerprint(slot, sizeof(slot)), stale) &&
                  stale.marks.count == 0,
              "stale fingerprint rejected, data untouched");
  slot[500] ^= 1;

  // Empty data is valid.
  const std::vector< uint8_t > empty = Encode(fp, Data());
  Data e;
  e.marks.Toggle(Pack(1, 1));
  REQUIRE_MSG(Decode(empty.data(), empty.size(), fp, e) && e.marks.count == 0, "empty replaces");

  // Damage: truncation, extra bytes, bad magic/version, a count above the cap.
  Data d;
  for (size_t cut = 0; cut < bytes.size(); cut += 7) {
    REQUIRE_MSG(!Decode(bytes.data(), cut, fp, d), "truncated rejected");
  }
  std::vector< uint8_t > longer = bytes;
  longer.push_back(0);
  REQUIRE_MSG(!Decode(longer.data(), longer.size(), fp, d), "trailing byte rejected");
  std::vector< uint8_t > bad = bytes;
  bad[0] ^= 1;
  REQUIRE_MSG(!Decode(bad.data(), bad.size(), fp, d), "bad magic");
  bad = bytes;
  bad[4] = 2;
  REQUIRE_MSG(!Decode(bad.data(), bad.size(), fp, d), "bad version");
  bad = bytes;
  bad[16] = 8; // seen word count
  REQUIRE_MSG(!Decode(bad.data(), bad.size(), fp, d), "different seen size");
  std::vector< uint8_t > over = Encode(fp, Data());
  over[20] = 65; // mark count word
  over.resize(over.size() + 130, 0);
  REQUIRE_MSG(!Decode(over.data(), over.size(), fp, d), "count above cap");

  // Marks out of range drop, repeats drop.
  Data dup;
  dup.marks.count = 4;
  dup.marks.packed[0] = dup.marks.packed[1] = Pack(2, 9);
  dup.marks.packed[2] = Pack(3, 1);
  dup.marks.packed[3] = Pack(9, 1); // world 9 does not exist
  const std::vector< uint8_t > dupBytes = Encode(fp, dup);
  REQUIRE_MSG(Decode(dupBytes.data(), dupBytes.size(), fp, d) && d.marks.count == 2 &&
                  d.marks.Has(Pack(3, 1)),
              "repeat and out of range dropped");

  // --- the card key ----------------------------------------------------------
  const std::string usaDir = PathToUtf8(root / "cards" / "usa");
  const std::string usa = CardIdentity(usaDir, "GM8E01", 0);
  REQUIRE_MSG(CardKey(usa) == CardKey(CardIdentity(usaDir, "GM8E01", 0)), "the same card keys the same");
  // A PAL card in the USA card's directory is another card, and so is another
  // channel: neither may overwrite the other's map data.
  const std::string pal = CardIdentity(usaDir, "GM8P01", 0);
  REQUIRE_MSG(pal != usa && CardKey(pal) != CardKey(usa), "another game id in one directory");
  const std::string otherChannel = CardIdentity(usaDir, "GM8E01", 1);
  REQUIRE_MSG(CardKey(otherChannel) != CardKey(usa), "another channel in one directory");
  // Spellings of one directory agree: a trailing separator, a "." step, a ".."
  // step, a symlink and a relative path.
  REQUIRE_MSG(CardIdentity(usaDir + "/", "GM8E01", 0) == usa, "trailing separator");
  REQUIRE_MSG(CardIdentity(usaDir + "/./", "GM8E01", 0) == usa, "a dot step");
  REQUIRE_MSG(CardIdentity(usaDir + "/../usa", "GM8E01", 0) == usa, "a dot dot step");
  fs::create_directory_symlink(root / "cards" / "usa", root / "link", ec);
#ifdef _WIN32
  // Windows may refuse symlink creation without Developer Mode or the symlink privilege.
  const bool symlinkUnavailable = ec.value() == 1314 || // ERROR_PRIVILEGE_NOT_HELD
                                  ec == std::errc::permission_denied ||
                                  ec == std::errc::operation_not_permitted;
#else
  const bool symlinkUnavailable = false;
#endif
  if (symlinkUnavailable) {
    std::fprintf(stderr, "Skipping symlink identity check: %s\n", ec.message().c_str());
  } else {
    REQUIRE_MSG(!ec, "symlink created");
    REQUIRE_MSG(CardIdentity(PathToUtf8(root / "link"), "GM8E01", 0) == usa, "a symlink resolves");
  }
  {
    const fs::path here = fs::current_path(ec);
    fs::current_path(root, ec);
    REQUIRE_MSG(CardIdentity("cards/usa", "GM8E01", 0) == usa, "a relative path");
    REQUIRE_MSG(CardIdentity("./cards/usa/", "GM8E01", 0) == usa, "a relative path with a dot step");
    fs::current_path(here, ec);
  }
  REQUIRE_MSG(NormalizedDirectory("") == std::string() && CardKey("x") != CardKey("y"), "key input");

  // --- the file layer --------------------------------------------------------
  const fs::path file = root / "mapdata" / "file.bin";
  REQUIRE_MSG(!ReadFile(file, fp, d), "missing file reads false");
  REQUIRE_MSG(WriteFile(file, fp, in), "write makes the file");
  REQUIRE_MSG(FileExists(file) && !FileExists(file.string() + ".tmp"), "no temporary left");
  REQUIRE_MSG(ReadRaw(file).size() == bytes.size(), "written whole");
  Data back;
  REQUIRE_MSG(ReadFile(file, fp, back) && back.marks.count == 64 && back.seen[3] == in.seen[3],
              "read round trip");
  REQUIRE_MSG(!ReadFile(file, fp ^ 1, back), "another fingerprint reads false");
  // A second write replaces the first: the latest data is what comes back, and
  // the file is not the old one plus the new one.
  Data other;
  other.seen[1] = 7;
  other.marks.Toggle(Pack(6, 2));
  REQUIRE_MSG(WriteFile(file, fp, other), "write replaces");
  REQUIRE_MSG(ReadFile(file, fp, back) && back.marks.count == 1 && back.seen[0] == 0 &&
                  back.seen[1] == 7 && !back.marks.Has(Pack(0, 0)),
              "the latest data is the one read back");
  REQUIRE_MSG(ReadRaw(file).size() == Encode(fp, other).size(), "and nothing of the old file is left");
  REQUIRE_MSG(WriteFile(file, fp, in) && ReadFile(file, fp, back) && back.marks.count == 64,
              "and back again");
  // Every malformed shape the decoder rejects is rejected through the file too.
  {
    std::ofstream garbage(file, std::ios::binary | std::ios::trunc);
    garbage << ReadRaw(file).substr(0, 30) << "extra";
  }
  REQUIRE_MSG(!ReadFile(file, fp, back), "damaged file reads false");
  REQUIRE_MSG(back.marks.count == 64, "damaged file leaves the caller's data");
  // A file far longer than the format allows is refused without being read.
  {
    std::vector< char > huge(kMaxBytes * 4, 0);
    std::ofstream big(file, std::ios::binary | std::ios::trunc);
    big.write(huge.data(), static_cast< std::streamsize >(huge.size()));
  }
  REQUIRE_MSG(!ReadFile(file, fp, back), "oversized file reads false");
  // A write that fails leaves the previous file exactly as it was (the
  // temporary cannot be written, so nothing replaces it).
  Data previous;
  previous.marks.Toggle(Pack(4, 4));
  REQUIRE_MSG(WriteFile(file, fp, previous), "a previous file");
  const std::string previousBytes = ReadRaw(file);
  REQUIRE_MSG(fs::create_directories(file.string() + ".tmp", ec), "the temporary is blocked");
  {
    std::ofstream inside(file.string() + ".tmp/keep");
    inside << "x";
  }
  REQUIRE_MSG(!WriteFile(file, fp, in), "a failed write reports false");
  REQUIRE_MSG(FileExists(file) && ReadRaw(file) == previousBytes, "the previous file is untouched");
  REQUIRE_MSG(ReadFile(file, fp, back) && back.marks.count == 1 && back.marks.Has(Pack(4, 4)),
              "and still reads back");
  fs::remove_all(file.string() + ".tmp", ec);
  // Errors: a path whose parent is a file cannot be written or removed.
  const fs::path blocked = file / "inside.bin";
  REQUIRE_MSG(!WriteFile(blocked, fp, in) && !FileExists(blocked), "unwritable path refuses");
  REQUIRE_MSG(RemoveFile(file) && !FileExists(file), "removed");
  REQUIRE_MSG(RemoveFile(file), "removing a missing file is fine");
  // A directory that still holds something is not removed.
  const fs::path full = root / "full";
  fs::create_directories(full, ec);
  {
    std::ofstream keep(full / "keep.bin");
    keep << "x";
  }
  REQUIRE_MSG(!RemoveFile(full), "a non-empty directory reports failure");
  fs::remove_all(full, ec);

  // --- the staging the driver goes through ----------------------------------
  sIdentity = usa;
  // A rebuilt slot, then the card write finished: the file lands under mapdata,
  // keyed on the card (FNV-1a of the identity) and the slot.
  char name[48];
  std::snprintf(name, sizeof(name), "%016llx-0.bin",
                static_cast< unsigned long long >(CardKey(usa)));
  const fs::path slot0 = root / "mapdata" / name;
  const SlotBytes slots[3] = {{slot, sizeof(slot)}, {nullptr, 0}, {nullptr, 0}};

  REQUIRE_MSG(Flush(nullptr, 0) == 0, "nothing staged, nothing written");
  // No card set up: nothing is written and nothing is read, whatever is staged.
  sIdentity.clear();
  Stage(0, slot, sizeof(slot), in);
  REQUIRE_MSG(Flush(slots, 3) == 0 && CountFiles(root / "mapdata") == 0,
              "without a card nothing is written");
  uint32_t noCardSeen[kSeenWords] = {9, 9, 9, 9};
  PortMapMarks::List noCardMarks;
  REQUIRE_MSG(!Apply(0, slot, sizeof(slot), noCardSeen, noCardMarks) && noCardMarks.count == 0,
              "without a card nothing is read");
  sIdentity = usa;
  Stage(0, slot, sizeof(slot), in);
  // The slot the file does not hold writes nothing, and is dropped rather than
  // written by a later write.
  REQUIRE_MSG(Flush(slots + 1, 2) == 0 && CountFiles(root / "mapdata") == 0,
              "a slot the written file does not hold writes nothing");
  REQUIRE_MSG(Flush(slots, 3) == 0, "and is dropped, not written later");
  Stage(0, slot, sizeof(slot), in);
  // Bytes that are not the staged ones are refused: the file on the card and
  // the side file would not agree.
  uint8_t otherSlot[940];
  for (size_t i = 0; i < sizeof(otherSlot); ++i) {
    otherSlot[i] = static_cast< uint8_t >(i);
  }
  const SlotBytes staleSlots[3] = {{otherSlot, sizeof(otherSlot)}, {nullptr, 0}, {nullptr, 0}};
  REQUIRE_MSG(Flush(staleSlots, 3) == 0 && CountFiles(root / "mapdata") == 0, "stale slot refused");
  Stage(0, slot, sizeof(slot), in);
  REQUIRE_MSG(Flush(slots, 3) == 1, "staged slot written");
  REQUIRE_MSG(CountFiles(root / "mapdata") == 1 && !FileExists(slot0.string() + ".tmp"),
              "one file, no temporary");
  REQUIRE_MSG(FileExists(slot0), "named for the card and the slot");
  REQUIRE_MSG(Flush(slots, 3) == 0, "flushing again writes nothing");

  // The card changes while a save is in flight (an Archipelago room's card, the
  // emulator-card setting): what was staged for the old card is not written to
  // the new one, and the new card's files are left alone.
  sIdentity = CardIdentity(usaDir, "GM8P01", 0);
  Stage(0, slot, sizeof(slot), in);
  sIdentity = usa;
  Stage(0, slot, sizeof(slot), previous);
  REQUIRE_MSG(Flush(slots, 3) == 1 && ReadFile(slot0, fp, back) && back.marks.Has(Pack(4, 4)) &&
                  back.marks.count == 1,
              "the card that is mounted now gets its own staging");
  sIdentity = CardIdentity(usaDir, "GM8P01", 0);
  Stage(0, slot, sizeof(slot), in);
  sIdentity = usa;
  REQUIRE_MSG(Flush(slots, 3) == 0 && ReadFile(slot0, fp, back) && back.marks.count == 1,
              "another card's staging cannot touch this card's file");
  REQUIRE_MSG(Flush(nullptr, 0) == 0, "and is dropped, not written later");
  // An erase of a slot the written file still holds keeps its map data: the
  // erase did not get through, so there is still a save to explain.
  sIdentity = usa;
  StageErase(0);
  REQUIRE_MSG(Flush(slots, 3) == 0 && FileExists(slot0) && ReadFile(slot0, fp, back),
              "erasing a slot that is still in the file keeps its file");
  // Once the file really has no such slot, the erase removes it (and removing
  // one that is not there is not an error).
  StageErase(0);
  REQUIRE_MSG(Flush(slots + 1, 2) == 1 && !FileExists(slot0), "the erase deletes the file");
  StageErase(0);
  REQUIRE_MSG(Flush(slots + 1, 2) == 1, "erasing a file that is not there still counts as gone");
  // An erase staged for another card cannot delete it.
  Stage(0, slot, sizeof(slot), in);
  REQUIRE_MSG(Flush(slots, 3) == 1, "back for the erase test");
  sIdentity = CardIdentity(usaDir, "GM8P01", 0);
  StageErase(0);
  sIdentity = usa;
  REQUIRE_MSG(Flush(slots + 1, 2) == 0 && FileExists(slot0), "another card's erase cannot delete it");
  REQUIRE_MSG(Flush(slots + 1, 2) == 0 && FileExists(slot0), "and is dropped, not applied later");

  // Loading that slot: the side file replaces the game's seen set and marks.
  Stage(0, slot, sizeof(slot), in);
  REQUIRE_MSG(Flush(slots, 3) == 1, "staged for the read test");
  uint32_t seen[kSeenWords] = {1, 2, 3, 4};
  PortMapMarks::List marks;
  marks.Toggle(Pack(0, 0));
  REQUIRE_MSG(Apply(0, slot, sizeof(slot), seen, marks), "apply matches");
  REQUIRE_MSG(seen[0] == in.seen[0] && seen[3] == in.seen[3] && seen[1] == in.seen[1], "seen restored");
  REQUIRE_MSG(marks.count == 64 && marks.Has(Pack(1, 1)), "marks restored");
  // A slot the file was not written for, or one that changed since, or another
  // card: the caller keeps what the save's own trailers gave it.
  uint32_t untouched[kSeenWords] = {5, 6, 7, 8};
  PortMapMarks::List untouchedMarks;
  untouchedMarks.Toggle(Pack(4, 4));
  REQUIRE_MSG(!Apply(0, otherSlot, sizeof(otherSlot), untouched, untouchedMarks), "stale apply false");
  REQUIRE_MSG(untouched[0] == 5 && untouchedMarks.count == 1, "a stale file changes nothing");
  REQUIRE_MSG(!Apply(1, slot, sizeof(slot), untouched, untouchedMarks), "another slot has no file");
  REQUIRE_MSG(!Apply(2, slot, sizeof(slot), untouched, untouchedMarks), "nor the third");
  sIdentity = CardIdentity(usaDir, "GM8P01", 0);
  REQUIRE_MSG(Apply(0, slot, sizeof(slot), untouched, untouchedMarks) && untouchedMarks.count == 64,
              "another card path adopts the file for the same slot bytes");
  untouched[0] = 5;
  untouchedMarks = PortMapMarks::List();
  untouchedMarks.Toggle(Pack(4, 4));
  REQUIRE_MSG(!Apply(0, otherSlot, sizeof(otherSlot), untouched, untouchedMarks) && untouched[0] == 5 &&
                  untouchedMarks.count == 1,
              "but not for other slot bytes");
  // Names outside the slot's pattern are not adopted: a short key, an upper case
  // key, a temporary, and a beam doors file of the same slot.
  {
    const fs::path folder = root / "mapdata";
    std::vector< uint8_t > blob;
    {
      std::ifstream in(slot0, std::ios::binary);
      blob.assign(std::istreambuf_iterator< char >(in), {});
    }
    const char* names[] = {"0123456789abcde-2.bin", "0123456789ABCDEF-2.bin", "0123456789abcdef-2.bin.tmp",
                           "beam-doors-0123456789abcdef-slot2.bin"};
    for (const char* name : names) {
      std::ofstream out(folder / name, std::ios::binary);
      out.write(reinterpret_cast< const char* >(blob.data()), static_cast< std::streamsize >(blob.size()));
    }
    REQUIRE_MSG(!Apply(2, slot, sizeof(slot), untouched, untouchedMarks), "unrelated names are not adopted");
    for (const char* name : names) {
      fs::remove(folder / name, ec);
    }
  }
  sIdentity = usa;
  // Two slots of one card keep their own files.
  Data second;
  second.seen[2] = 0x1234;
  second.marks.Toggle(Pack(3, 3));
  Stage(1, slot, sizeof(slot), second);
  const SlotBytes two[3] = {{slot, sizeof(slot)}, {slot, sizeof(slot)}, {nullptr, 0}};
  REQUIRE_MSG(Flush(two, 3) == 1 && CountFiles(root / "mapdata") == 2, "one file per slot");
  REQUIRE_MSG(Apply(0, slot, sizeof(slot), seen, marks) && marks.count == 64, "slot 0 unchanged");
  REQUIRE_MSG(Apply(1, slot, sizeof(slot), seen, marks) && marks.count == 1 && seen[2] == 0x1234,
              "slot 1 own data");
  // Written files whose directory cannot be made are not fatal: the save on the
  // card is what matters.
  fs::remove_all(root / "mapdata", ec);
  {
    std::ofstream obstacle(root / "mapdata", std::ios::binary | std::ios::trunc);
    obstacle << "not a directory";
  }
  Stage(0, slot, sizeof(slot), in);
  REQUIRE_MSG(Flush(slots, 3) == 0, "an unwritable directory reports nothing written");
  REQUIRE_MSG(!Apply(0, slot, sizeof(slot), seen, marks), "and nothing is readable there");
  fs::remove(root / "mapdata", ec);
  REQUIRE_MSG(Flush(nullptr, 0) == 0, "and the staging is still emptied");
  fs::remove_all(root, ec);

  std::puts("port_map_side_tests ok");
  return 0;
}
