// "Unlock beam doors once opened": the doorway numbering, the door patch it
// drives and the per-slot unlock file (platform/include/port_beam_doors.h).
#include "port_ap_world.h"
#include "port_beam_doors.h"
#include "port_json.h"
#include "port_test.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace {

std::filesystem::path sDirectory;
std::string sIdentity = "card-a|GM8E01|0";

std::vector< uint8_t > SlotData(uint8_t seed) {
  std::vector< uint8_t > bytes(940);
  for (size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast< uint8_t >(seed + i * 7);
  return bytes;
}

std::vector< uint8_t > ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector< uint8_t >(std::istreambuf_iterator< char >(in), {});
}

void WriteAll(const std::filesystem::path& path, const std::vector< uint8_t >& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast< const char* >(bytes.data()), static_cast< std::streamsize >(bytes.size()));
}

bool Same(const uint32_t* a, const uint32_t* b) {
  for (int i = 0; i < PortBeamDoors::kWords; ++i)
    if (a[i] != b[i])
      return false;
  return true;
}

// The format: round trip, and everything a reader must refuse.
void TestFormat() {
  using namespace PortBeamDoors;
  uint32_t none[kWords] = {};
  uint32_t some[kWords] = {};
  Set(some, 0);
  Set(some, 37);
  Set(some, 110);
  CHECK_EQ(UsedBits(none), size_t(0));
  CHECK_EQ(UsedBits(some), size_t(111));
  CHECK_MSG(!Test(some, 1) && Test(some, 37) && !Test(some, -1) && !Test(some, 128),
            "bit access stays in range");

  const uint64_t fp = 0x0123456789ABCDEFull;
  const std::vector< uint8_t > bytes = Encode(fp, some);
  CHECK_EQ(bytes.size(), kSideHeaderBytes + 14);
  uint32_t back[kWords] = {9, 9, 9, 9};
  CHECK_MSG(Decode(bytes.data(), bytes.size(), fp, back) && Same(back, some), "the bits round trip");
  CHECK_EQ(Encode(fp, none).size(), kSideHeaderBytes);
  uint32_t zero[kWords] = {9, 9, 9, 9};
  CHECK_MSG(Decode(Encode(fp, none).data(), kSideHeaderBytes, fp, zero) && Same(zero, none), "no bits round trips");

  uint32_t keep[kWords] = {1, 2, 3, 4};
  CHECK_MSG(!Decode(bytes.data(), bytes.size(), fp + 1, keep) && keep[0] == 1, "another fingerprint is refused");
  std::vector< uint8_t > bad = bytes;
  bad[0] ^= 1;
  CHECK_MSG(!Decode(bad.data(), bad.size(), fp, keep), "a bad magic is refused");
  bad = bytes;
  bad[4] = 2;
  CHECK_MSG(!Decode(bad.data(), bad.size(), fp, keep), "another version is refused");
  CHECK_MSG(!Decode(bytes.data(), bytes.size() - 1, fp, keep), "a truncated file is refused");
  CHECK_MSG(!Decode(bytes.data(), kSideHeaderBytes - 1, fp, keep), "a short header is refused");
  bad = bytes;
  bad.push_back(0);
  CHECK_MSG(!Decode(bad.data(), bad.size(), fp, keep), "a longer file is refused");
  bad = bytes;
  bad[16] = 200; // a bit count past kMaxBits
  bad[17] = 1;
  CHECK_MSG(!Decode(bad.data(), bad.size(), fp, keep) && keep[3] == 4, "a bit count over the limit is refused");
}

// The files of a slot around a card write.
void TestSidecar() {
  using namespace PortBeamDoors;
  namespace fs = std::filesystem;
  uint32_t some[kWords] = {};
  Set(some, 3);
  Set(some, 100);
  uint32_t none[kWords] = {};
  const std::vector< uint8_t > slot = SlotData(1);
  const SlotBytes written[3] = {{nullptr, 0}, {slot.data(), slot.size()}, {nullptr, 0}};
  const fs::path file = fs::path(SidePath(sDirectory.string() + "/", sIdentity, 1));
  CHECK_MSG(file.filename().string().find("slot1.bin") != std::string::npos, "the file is named for its slot");
  CHECK_MSG(SidePath("", sIdentity, 1).empty() && SidePath("x/", "", 1).empty(), "no card, no path");

  uint32_t got[kWords] = {};
  CHECK_MSG(!Apply(1, slot.data(), slot.size(), got), "an absent file applies nothing");

  // No bits: nothing is written.
  Stage(1, slot.data(), slot.size(), none);
  CHECK_EQ(Flush(written, 3), 0);
  CHECK_MSG(!fs::exists(file), "a save with no unlocks leaves no file");

  Stage(1, slot.data(), slot.size(), some);
  CHECK_EQ(Flush(written, 3), 1);
  CHECK_MSG(fs::exists(file) && !fs::exists(file.string() + ".tmp"), "written whole, no temporary left");
  CHECK_MSG(Apply(1, slot.data(), slot.size(), got) && Same(got, some), "the slot loads its doorways");

  // Another save in the slot: the fingerprint no longer matches.
  const std::vector< uint8_t > other = SlotData(2);
  uint32_t stale[kWords] = {5, 5, 5, 5};
  CHECK_MSG(!Apply(1, other.data(), other.size(), stale) && stale[0] == 5, "another save ignores the file");
  CHECK_MSG(!Apply(0, slot.data(), slot.size(), stale), "another slot has no file");

  // A moved user folder changes the card key: the file left under the old key is
  // still used for the same slot bytes, and only for those.
  const std::string home = sIdentity;
  sIdentity = "card-b|GM8E01|0";
  uint32_t moved[kWords] = {};
  CHECK_MSG(Apply(1, slot.data(), slot.size(), moved) && Same(moved, some),
            "a file under another card key is adopted");
  uint32_t kept[kWords] = {5, 5, 5, 5};
  CHECK_MSG(!Apply(1, other.data(), other.size(), kept) && kept[0] == 5,
            "another save does not adopt it");
  CHECK_MSG(!Apply(0, slot.data(), slot.size(), kept), "nor does another slot");
  // Names that are not this slot's pattern are not adopted: a short key, an upper
  // case key, a temporary and the map side file of the same slot.
  const fs::path folder = file.parent_path();
  const std::vector< uint8_t > blob = ReadAll(file);
  WriteAll(folder / "beam-doors-0123456789abcde-slot2.bin", blob);
  WriteAll(folder / "beam-doors-0123456789ABCDEF-slot2.bin", blob);
  WriteAll(folder / "beam-doors-0123456789abcdef-slot2.bin.tmp", blob);
  WriteAll(folder / "0123456789abcdef-2.bin", blob);
  CHECK_MSG(!Apply(2, slot.data(), slot.size(), kept), "unrelated names are not adopted");
  fs::remove(folder / "beam-doors-0123456789abcde-slot2.bin");
  fs::remove(folder / "beam-doors-0123456789ABCDEF-slot2.bin");
  fs::remove(folder / "beam-doors-0123456789abcdef-slot2.bin.tmp");
  fs::remove(folder / "0123456789abcdef-2.bin");
  sIdentity = home;

  // Staged but the card holds different bytes, or no such slot: nothing happens.
  Stage(1, other.data(), other.size(), none);
  CHECK_EQ(Flush(written, 3), 0);
  CHECK_MSG(fs::exists(file), "a write that did not leave the staged bytes touches nothing");
  const SlotBytes missing[3] = {};
  Stage(1, slot.data(), slot.size(), none);
  CHECK_EQ(Flush(missing, 3), 0);
  CHECK_MSG(fs::exists(file), "an abandoned save touches nothing");

  // Saving the slot with no unlocks left (a new game over it) removes the file.
  Stage(1, other.data(), other.size(), none);
  const SlotBytes next[3] = {{nullptr, 0}, {other.data(), other.size()}, {nullptr, 0}};
  CHECK_EQ(Flush(next, 3), 1);
  CHECK_MSG(!fs::exists(file), "an empty save deletes the stale file");

  // Erase: removed once the written file has no such slot, kept if it still does.
  Stage(1, slot.data(), slot.size(), some);
  Flush(written, 3);
  StageErase(1);
  CHECK_EQ(Flush(written, 3), 0);
  CHECK_MSG(fs::exists(file), "an erase that did not happen keeps the file");
  StageErase(1);
  CHECK_EQ(Flush(missing, 3), 1);
  CHECK_MSG(!fs::exists(file), "an erased slot loses its file");

  // Damaged files: no unlocks, no crash.
  Stage(1, slot.data(), slot.size(), some);
  Flush(written, 3);
  std::vector< uint8_t > bytes = ReadAll(file);
  CHECK_MSG(!bytes.empty(), "the file reads back");
  std::vector< uint8_t > cut(bytes.begin(), bytes.end() - 1);
  WriteAll(file, cut);
  CHECK_MSG(!Apply(1, slot.data(), slot.size(), stale), "a truncated file applies nothing");
  bytes[4] = 9;
  WriteAll(file, bytes);
  CHECK_MSG(!Apply(1, slot.data(), slot.size(), stale), "another version applies nothing");
  WriteAll(file, std::vector< uint8_t >());
  CHECK_MSG(!Apply(1, slot.data(), slot.size(), stale), "an empty file applies nothing");
  WriteAll(file, std::vector< uint8_t >(5000, 0xAA));
  CHECK_MSG(!Apply(1, slot.data(), slot.size(), stale), "garbage applies nothing");

  // Another card, same slot number: its own file.
  CHECK_MSG(SidePath(sDirectory.string() + "/", "card-b|GM8E01|0", 1) != file.string(), "cards keep their own files");
}

void TestNumbering() {
  using namespace PortApWorld;
  const int count = BeamDoorwayCount();
  std::printf("beam doorways: %d\n", count);
  CHECK_MSG(count > 90 && count <= kBeamDoorBits, "about 111 beam doorways, within the bitset");
  CHECK_EQ(count, 111);
  std::set< int > seen;
  int next = 0;
  bool paired = true, inOrder = true;
  int doors = 0;
  for (int bit = 0; bit < count; ++bit) {
    const std::vector< DoorChange > way = BeamDoorway(bit);
    for (const DoorChange& door : way) {
      ++doors;
      CHECK_EQ(BeamDoorwayBit(door.index), bit);
      CHECK_MSG(door.type.empty(), "BeamDoorway leaves the type empty");
      if (door.pair >= 0 && BeamDoorwayBit(door.pair) >= 0)
        paired = paired && BeamDoorwayBit(door.pair) == bit;
    }
    seen.insert(bit);
  }
  CHECK_MSG(paired, "both sides of a doorway share a bit");
  // Fungal Hall Access's doors are Plasma Beam on the disc (the apworld says
  // Blue, #174): they share the doorways of Elevator B's and Fungal Hall A's.
  CHECK_MSG(BeamDoorwayBit(404) >= 0 && BeamDoorwayBit(404) == BeamDoorwayBit(389) &&
                BeamDoorwayBit(403) >= 0 && BeamDoorwayBit(403) == BeamDoorwayBit(402),
            "Fungal Hall Access's doors are beam doors");
  for (int i = 0; i < 600; ++i) {
    const int bit = BeamDoorwayBit(i);
    if (bit < 0)
      continue;
    // A number appears first at its lowest door, so numbers count up in table order.
    if (bit > next)
      inOrder = false;
    if (bit == next)
      ++next;
  }
  CHECK_MSG(inOrder && next == count, "bits count up in table order");
  CHECK_MSG(BeamDoorwayBit(-1) < 0 && BeamDoorwayBit(100000) < 0, "out-of-range doors have none");
  CHECK_MSG(BeamDoorway(-1).empty() && BeamDoorway(count).empty(), "out-of-range doorways are empty");
  CHECK_MSG(doors > count, "most doorways have two doors");
}

PortApWorld::DoorChange* Find(std::vector< PortApWorld::DoorChange >& list, int index) {
  for (PortApWorld::DoorChange& change : list)
    if (change.index == index)
      return &change;
  return nullptr;
}

void TestDoors() {
  using namespace PortApWorld;
  const int count = BeamDoorwayCount();
  uint32_t all[kBeamDoorWords] = {~0u, ~0u, ~0u, ~0u};
  uint32_t none[kBeamDoorWords] = {};
  const Layout disc;

  // Setting off (no bitset), or nothing unlocked: the disc's rooms are untouched.
  int checked = 0;
  for (int bit = 0; bit < count; ++bit) {
    for (const DoorChange& door : BeamDoorway(bit)) {
      CHECK_MSG(Doors(disc, door.mrea).empty(), "no seed, no unlocks: no change");
      CHECK_MSG(Doors(disc, door.mrea, nullptr, none).empty(), "an empty set changes nothing");
      // One doorway unlocked: both sides, and only that doorway.
      uint32_t one[kBeamDoorWords] = {};
      one[bit / 32] |= 1u << (bit % 32);
      std::vector< DoorChange > changes = Doors(disc, door.mrea, nullptr, one);
      DoorChange* mine = Find(changes, door.index);
      CHECK_MSG(mine != nullptr && mine->type == "Blue" && mine->shield.empty() &&
                    mine->doorId == door.doorId && mine->forces[0] == door.forces[0],
                "an unlocked beam door is a plain blue one");
      for (const DoorChange& other : changes)
        CHECK_MSG(BeamDoorwayBit(other.index) == bit && other.type == "Blue",
                  "only the unlocked doorway changes");
      for (const DoorChange& side : BeamDoorway(bit)) {
        std::vector< DoorChange > there = Doors(disc, side.mrea, nullptr, one);
        CHECK_MSG(Find(there, side.index) != nullptr, "both sides turn blue");
      }
      ++checked;
    }
  }
  CHECK_MSG(checked > count, "every beam door was tried");

  // A seed's colours: the unlocked door is blue whatever the seed made it, but a
  // door the seed made Bomb or Missile (not a beam lock) is left alone.
  PortJson::Value data;
  size_t offset = 0;
  const char* reason = nullptr;
  CHECK_MSG(PortJson::Parse(R"({"door_color_randomization":1,"door_color_mapping":{
      "Chozo Ruins":{"area":"Chozo Ruins","type_mapping":{"Blue":"Ice Beam","Wave Beam":"Bomb",
      "Ice Beam":"Missile","Plasma Beam":"Wave Beam","Power Beam Only":"Plasma Beam"}}}})",
                            data, offset, &reason),
            "the colour fixture parses");
  Layout colours;
  Parse(data, colours);
  int bombed = 0, recoloured = 0, blueNow = 0;
  for (int bit = 0; bit < count; ++bit) {
    for (const DoorChange& door : BeamDoorway(bit)) {
      std::vector< DoorChange > plain = Doors(colours, door.mrea);
      std::vector< DoorChange > open = Doors(colours, door.mrea, nullptr, all);
      DoorChange* before = Find(plain, door.index);
      DoorChange* after = Find(open, door.index);
      CHECK_MSG(after != nullptr, "an unlocked door is listed");
      if (after == nullptr)
        continue;
      if (before != nullptr && before->type == "Blue")
        ++blueNow;
      if (before != nullptr && (before->type == "Bomb" || before->type == "Missile")) {
        CHECK_MSG(after->type == before->type, "a Bomb or Missile door stays as the seed made it");
        ++bombed;
      } else {
        CHECK_MSG(after->type == "Blue", "an unlocked beam door is blue under a seed too");
        if (before != nullptr && before->type != "Blue")
          ++recoloured;
      }
    }
  }
  CHECK_MSG(bombed > 0, "the seed fixture reaches a Bomb or Missile door");
  // Unlockable: the seed's lock decides.
  int unlockable = 0, refused = 0;
  for (int bit = 0; bit < count; ++bit) {
    for (const DoorChange& door : BeamDoorway(bit)) {
      CHECK_MSG(BeamDoorUnlockable(disc, door.mrea, door.index), "a disc beam door can be unlocked");
      const bool ok = BeamDoorUnlockable(colours, door.mrea, door.index);
      (ok ? unlockable : refused) += 1;
    }
  }
  CHECK_MSG(unlockable > 0 && refused == bombed + blueNow, "a Bomb, Missile or already-blue door is refused");
  CHECK_MSG(!BeamDoorUnlockable(disc, 0x18AB6106u, -1) && !BeamDoorUnlockable(disc, 0, 100000),
            "a door without a doorway is refused");

  CHECK_MSG(MapDoors(disc, 0x12345678u, nullptr, all).empty(), "no map area, no icons");

  DoorStyle style;
  CHECK_MSG(BlueDoorStyle(style) && style.cmdl == 0x0734977Au && style.cmdlVertical == 0x18D0AEE6u &&
                style.pattern0 == 0x544A9892u && style.color == 0x8A7F3683u,
            "the blue style is the disc's blue door");
  CHECK_MSG(BlueDoorVulnerability().size() == 4 + 15 * 4 + 4 + 4 + 5 * 4 + 4 + 5 * 4,
            "the blue weaknesses are a whole vulnerability");

  // The locked style: every beam doorway's doors have one, and under the
  // disc's layout it is the lock the disc gives them (never the blue one).
  int locked = 0;
  for (int bit = 0; bit < count; ++bit) {
    for (const DoorChange& door : BeamDoorway(bit)) {
      DoorStyle lockedStyle;
      std::vector< uint8_t > weakness;
      CHECK_MSG(LockedDoorStyle(disc, door.index, nullptr, lockedStyle, weakness) &&
                    weakness.size() == BlueDoorVulnerability().size(),
                "a beam door has a locked style");
      if (lockedStyle.color != style.color)
        ++locked;
    }
  }
  CHECK_MSG(locked > 0, "a locked beam door does not look blue");
  DoorStyle noStyle;
  std::vector< uint8_t > noWeakness;
  CHECK_MSG(!LockedDoorStyle(disc, -1, nullptr, noStyle, noWeakness), "no door, no style");
}

} // namespace

// What platform/port_map_side_paths.cpp does in the game: the port's user folder
// and the identity of the mounted card.
namespace PortMapSide {
void Location(std::string& userFolder, std::string& cardIdentity) {
  userFolder = sDirectory.string() + "/";
  cardIdentity = sIdentity;
}
} // namespace PortMapSide

int main() {
  std::error_code ec;
  sDirectory = std::filesystem::temp_directory_path(ec) / "port_beam_doors_tests";
  std::filesystem::remove_all(sDirectory, ec);
  std::filesystem::create_directories(sDirectory, ec);
  TestFormat();
  TestSidecar();
  TestNumbering();
  TestDoors();
  return PORT_TEST_RESULT();
}
