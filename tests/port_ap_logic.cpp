#include "port_ap_logic.h"

#include "port_ap_metroidprime.h"
#include "port_test.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {

using PortApLogic::Items;
using PortApLogic::Level;
using PortApLogic::Options;

constexpr int64_t kBase = PortAp::MetroidPrime::kItemBase;

Level At(const Options& options, const Items& items, const char* room, const char* section) {
  size_t count = 0;
  const PortApLogic::Check* checks = PortApLogic::Checks(count);
  const std::vector< Level > levels = PortApLogic::Evaluate(options, items);
  for (size_t i = 0; i < count; ++i) {
    if (std::strcmp(checks[i].room, room) == 0 && std::strcmp(checks[i].section, section) == 0)
      return levels[i];
  }
  REQUIRE(false);
  return Level::None;
}

size_t CountOf(const Options& options, const Items& items, Level level) {
  size_t out = 0;
  for (Level each : PortApLogic::Evaluate(options, items))
    out += each == level ? 1 : 0;
  return out;
}
} // namespace

int main() {
  // One check per AP location, in the same order.
  size_t count = 0;
  const PortApLogic::Check* checks = PortApLogic::Checks(count);
  size_t locationCount = 0;
  const PortAp::MetroidPrime::Location* locations = PortAp::MetroidPrime::Locations(locationCount);
  REQUIRE(count == 100 && locationCount == count);
  for (size_t i = 0; i < count; ++i) {
    REQUIRE(checks[i].id == locations[i].id);
    REQUIRE(checks[i].area[0] != 0 && checks[i].room[0] != 0 && checks[i].section != nullptr);
  }

  // With nothing, the only thing in sight is the Landing Site pickup, behind
  // its Morph Ball tunnel: the tracker shows it blue.
  {
    const Options options;
    const Items items;
    REQUIRE(PortApLogic::Evaluate(options, items).size() == count);
    REQUIRE(CountOf(options, items, Level::Normal) == 0);
    REQUIRE(At(options, items, "Landing Site", "Morph Ball tunnel") == Level::Inspect);
    REQUIRE(At(options, items, "Elite Quarters", "Omega Pirate") == Level::None);
  }

  // The Morph Ball puts it in logic; the Hive Totem needs a way to Chozo Ruins.
  {
    const Options options;
    Items items{{kBase + 16, 1}};
    REQUIRE(At(options, items, "Landing Site", "Morph Ball tunnel") == Level::Normal);
    REQUIRE(At(options, items, "Hive Totem", "Hive Mecha") == Level::None);
    items = {{kBase + 0, 1}, {kBase + 5, 1}};
    REQUIRE(At(options, items, "Hive Totem", "Hive Mecha") == Level::Normal);
    // Alcove without Space Jump is a trick: a sequence break with no tricks in logic,
    REQUIRE(At(options, items, "Alcove", "") == Level::SequenceBreak);
    // in logic once the seed allows easy tricks, or the trick by name,
    Options easy;
    easy.trickDifficulty = 0;
    REQUIRE(At(easy, items, "Alcove", "") == Level::Normal);
    Options allowed;
    allowed.trickAllow = {"Alcove Escape"};
    // (one trick is not enough: the way in is the Landing Site scan dash)
    REQUIRE(At(allowed, items, "Alcove", "") == Level::SequenceBreak);
    allowed.trickAllow.push_back("Landing Site Scan Dash");
    REQUIRE(At(allowed, items, "Alcove", "") == Level::Normal);
    // and out again when the seed denies it; the allow list wins over the deny list.
    easy.trickDeny = {"Alcove Escape"};
    REQUIRE(At(easy, items, "Alcove", "") == Level::SequenceBreak);
    easy.trickAllow = {"Alcove Escape"};
    REQUIRE(At(easy, items, "Alcove", "") == Level::Normal);
  }

  // A Missile Expansion is five missiles, and counts for nothing without the
  // launcher when the seed has one.
  {
    Options options;
    Items items{{kBase + 0, 1}, {kBase + 5, 1}, {kBase + 16, 1}, {kBase + 4, 1}};
    REQUIRE(At(options, items, "Ruined Gallery", "Missile Wall") == Level::Normal);
    options.mainMissile = true;
    REQUIRE(At(options, items, "Ruined Gallery", "Missile Wall") != Level::Normal);
    items[kBase + 43] = 1;
    items.erase(kBase + 4);
    REQUIRE(At(options, items, "Ruined Gallery", "Missile Wall") == Level::Normal);
  }

  // Everything is in logic with every item, whichever way the beams come.
  {
    Options options;
    Items items;
    for (int i = 0; i <= 45; ++i)
      items[kBase + i] = 20;
    REQUIRE(CountOf(options, items, Level::Normal) == count);
    // Without the X-Ray Visor the Omega Pirate is out, whatever the seed removes.
    items.erase(kBase + 13);
    options.removeXray = 2;
    REQUIRE(At(options, items, "Elite Quarters", "Omega Pirate") == Level::None);

    options = Options();
    options.progressiveBeams = true;
    items.clear();
    for (int i = 4; i <= 45; ++i) {
      if (i != 10 && i != 11 && i != 14 && i != 8 && i != 28)
        items[kBase + i] = 20;
    }
    for (int beam : {49, 51, 52, 53})
      items[kBase + beam] = 3;
    REQUIRE(CountOf(options, items, Level::Normal) == count);
    // The second Power Beam is its charge, the third the Super Missile.
    items[kBase + 49] = 2;
    const size_t charged = CountOf(options, items, Level::Normal);
    REQUIRE(charged < count);
    items[kBase + 49] = 1;
    REQUIRE(CountOf(options, items, Level::Normal) <= charged);
  }

  // The seed's layout. A game that starts in the Ruined Fountain has no way
  // to the Landing Site with nothing, and its own room in reach.
  {
    Options options;
    const Items items{{kBase + 16, 1}, {kBase + 19, 1}};
    REQUIRE(At(options, items, "Ruined Fountain", "Spider tracks") == Level::None);
    options.startRoom = "Ruined Fountain";
    REQUIRE(At(options, items, "Landing Site", "Morph Ball tunnel") == Level::None);
    REQUIRE(At(options, items, "Ruined Fountain", "Spider tracks") != Level::None);
    // A room the pack has no rule for is in reach all the same.
    options.startRoom = "Hive Totem";
    REQUIRE(At(options, items, "Hive Totem", "Hive Mecha") != Level::None);
    REQUIRE(At(Options(), items, "Hive Totem", "Hive Mecha") == Level::None);
  }

  // Elevators: with the Landing Site's west elevator leading to Magmoor, the
  // Hive Totem is no longer a scan away, and is again from an elevator that
  // leads to its room.
  {
    Options options;
    const Items items{{kBase + 0, 1}, {kBase + 5, 1}};
    REQUIRE(At(options, items, "Hive Totem", "Hive Mecha") == Level::Normal);
    options.elevators["Tallon Overworld"]["Transport to Chozo Ruins West"] = "Transport to Tallon Overworld West";
    options.elevators["Magmoor Caverns"]["Transport to Tallon Overworld West"] = "Transport to Chozo Ruins West";
    options.elevators["Chozo Ruins"]["Transport to Tallon Overworld North"] =
        "Phazon Mines: Transport to Tallon Overworld South";
    options.elevators["Phazon Mines"]["Transport to Tallon Overworld South"] =
        "Transport to Tallon Overworld North";
    REQUIRE(At(options, items, "Hive Totem", "Hive Mecha") == Level::None);
    options.elevators["Tallon Overworld"]["Transport to Chozo Ruins West"] = "Transport to Tallon Overworld North";
    REQUIRE(At(options, items, "Hive Totem", "Hive Mecha") == Level::Normal);
  }

  // Transport Tunnel E reaches Hydro Access Tunnel over Great Tree Hall's
  // upper level, as in the apworld: no Wave Beam or Thermal Visor needed.
  {
    Options options;
    options.startRoom = "Transport to Phazon Mines East";
    Items items{{kBase + 6, 1}, {kBase + 16, 1}, {kBase + 21, 1}};
    REQUIRE(At(options, items, "Hydro Access Tunnel", "Underwater bomb puzzle") == Level::None);
    items[kBase + 1] = 1;
    REQUIRE(At(options, items, "Hydro Access Tunnel", "Underwater bomb puzzle") == Level::Normal);
  }

  // Doors: the Landing Site's door to the Alcove behind a blast shield, a
  // disabled door, or another colour.
  {
    Options options;
    options.trickDifficulty = 0;
    Items items{{kBase + 0, 1}, {kBase + 5, 1}};
    REQUIRE(At(options, items, "Alcove", "") == Level::Normal);
    options.doors["Tallon Overworld|Landing Site|Alcove"] = {"Blue", "Power Bomb"};
    REQUIRE(At(options, items, "Alcove", "") == Level::None);
    items[kBase + 16] = 1;
    items[kBase + 7] = 1;
    REQUIRE(At(options, items, "Alcove", "") == Level::Normal);
    options.doors["Tallon Overworld|Landing Site|Alcove"] = {"Disabled", ""};
    REQUIRE(At(options, items, "Alcove", "") == Level::None);
    options.doors["Tallon Overworld|Landing Site|Alcove"] = {"Ice Beam", ""};
    REQUIRE(At(options, items, "Alcove", "") == Level::None);
    items[kBase + 1] = 1;
    REQUIRE(At(options, items, "Alcove", "") == Level::Normal);
  }

  // A recoloured area: every item but the Wave Beam reaches everything once
  // the Wave doors are Ice doors, and not before.
  {
    Options options;
    Items items;
    for (int i = 0; i <= 45; ++i)
      items[kBase + i] = 20;
    items.erase(kBase + 2);
    const size_t before = CountOf(options, items, Level::Normal);
    for (const char* area : {"Tallon Overworld", "Chozo Ruins", "Magmoor Caverns", "Phendrana Drifts", "Phazon Mines"})
      options.doorColors[area]["Wave Beam"] = "Ice Beam";
    REQUIRE(CountOf(options, items, Level::Normal) > before);
  }

  // The Hive Mecha fight needs nothing once the seed removes it, and the
  // lower mines open from their far end only when the seed says so.
  {
    Options options;
    Items items;
    for (int i = 0; i <= 45; ++i)
      items[kBase + i] = 20;
    const size_t all = CountOf(options, items, Level::Normal);
    options.removeHiveMecha = true;
    options.backwardsLowerMines = true;
    REQUIRE(CountOf(options, items, Level::Normal) == all);
    Options other;
    REQUIRE(options != other);
    other.removeHiveMecha = other.backwardsLowerMines = true;
    REQUIRE(options == other);
    other.doors["a|b|c"] = {"Blue", "Missile"};
    REQUIRE(options != other);
  }

  // Tricks are named as the seed's allow and deny lists name them.
  REQUIRE(PortApLogic::TrickName(0) != nullptr && PortApLogic::TrickName(100000) == nullptr);
  bool found = false;
  for (size_t i = 0; PortApLogic::TrickName(i) != nullptr; ++i)
    found = found || std::strcmp(PortApLogic::TrickName(i), "Alcove Escape") == 0;
  REQUIRE(found);

  Options a, b;
  REQUIRE(a == b);
  b.trickDeny = {"Alcove Escape"};
  REQUIRE(a != b);

  // A pickup plays the jingle of the item placed there (randomprime's
  // attainment_audio_file_name): artifacts, upgrades, small items, other games.
  {
    namespace Mp = PortAp::MetroidPrime;
    constexpr const char* kShort = "/audio/itm_x_short_02.dsp";
    constexpr const char* kItem = "audio/jin_itemattain.dsp";
    constexpr const char* kArtifact = "audio/jin_artifact.dsp";
    for (int64_t local = Mp::kArtifactTruth; local <= Mp::kArtifactNewborn; ++local)
      REQUIRE(std::strcmp(Mp::JingleFile(kBase + local, true), kArtifact) == 0);
    for (int64_t local : std::initializer_list< int64_t >{
             0, Mp::kMissileExpansion, Mp::kPowerBombExpansion, Mp::kEnergyTank,
             Mp::kUnlimitedMissiles, Mp::kUnlimitedPowerBombs, 46})
      REQUIRE(std::strcmp(Mp::JingleFile(kBase + local, true), kShort) == 0);
    for (int64_t local : std::initializer_list< int64_t >{
             1, 6, 16, 22, Mp::kMissileLauncher, Mp::kMainPowerBomb, Mp::kSpringBall, 49, 54})
      REQUIRE(std::strcmp(Mp::JingleFile(kBase + local, true), kItem) == 0);
    // Another game's item, whatever its id, is the short jingle.
    REQUIRE(std::strcmp(Mp::JingleFile(kBase + 29, false), kShort) == 0);
    REQUIRE(std::strcmp(Mp::JingleFile(123, false), kShort) == 0);
    // An offline seed's item type picks the same jingle as the AP item id.
    REQUIRE(std::strcmp(Mp::JingleForItemType(29), kArtifact) == 0);
    REQUIRE(std::strcmp(Mp::JingleForItemType(4), kShort) == 0);
    REQUIRE(std::strcmp(Mp::JingleForItemType(22), kItem) == 0);
    REQUIRE(Mp::JingleForItemType(-1) == nullptr && Mp::JingleForItemType(41) == nullptr);
    // Every location names its jingle object, and the two sides find each other.
    for (size_t i = 0; i < locationCount; ++i) {
      REQUIRE(locations[i].audio != 0 && locations[i].audio != locations[i].pickup);
      REQUIRE(Mp::FindAudio(locations[i].world, locations[i].area, locations[i].audio) ==
              &locations[i]);
    }
  }

  std::puts("AP logic tests passed");
  return 0;
}
