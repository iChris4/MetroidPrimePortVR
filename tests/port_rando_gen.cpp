#include "port_rando_gen.h"

#include "port_ap_logic.h"
#include "port_ap_metroidprime.h"
#include "port_ap_world.h"
#include "port_json.h"
#include "port_json_writer.h"
#include "port_test.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace MP = PortAp::MetroidPrime;

namespace {

using PortRandoGen::Seed;
using PortRandoGen::Settings;

Seed Make(const Settings& settings, const std::string& text) {
  Seed seed;
  std::string error;
  if (!PortRandoGen::Generate(settings, text, seed, error)) {
    std::fprintf(stderr, "Generate(%s) failed: %s\n", text.c_str(), error.c_str());
    std::abort();
  }
  return seed;
}

int64_t Int(const PortJson::Value& root, const char* key, int64_t fallback) {
  const PortJson::Value* v = root.Find(key);
  return v != nullptr ? v->AsInt(fallback) : fallback;
}

// The options as the AP client would build them from slot_data, written out
// again here rather than shared with the generator.
PortApLogic::Options OptionsFromSlotData(const PortJson::Value& root, PortApWorld::Layout& layout) {
  PortApWorld::Parse(root, layout);
  PortApLogic::Options options;
  PortApWorld::FillLogic(layout, options);
  options.trickDifficulty = static_cast< int >(Int(root, "trick_difficulty", -1));
  options.combatLogic = static_cast< int >(Int(root, "combat_logic_difficulty", 0));
  options.removeXray = static_cast< int >(Int(root, "remove_xray_requirements", 0));
  options.removeThermal = static_cast< int >(Int(root, "remove_thermal_requirements", 0));
  options.flaahgraPowerBombs = Int(root, "flaahgra_power_bombs", 0) != 0;
  options.progressiveBeams = Int(root, "progressive_beam_upgrades", 0) != 0;
  options.mainMissile = Int(root, "missile_launcher", 0) != 0;
  options.mainPowerBomb = Int(root, "main_power_bomb", 0) != 0;
  options.variaOnlyHeat = Int(root, "non_varia_heat_damage", 1) != 0;
  options.preScanElevators = Int(root, "pre_scan_elevators", 1) != 0;
  for (const char* key : {"trick_allow_list", "trick_deny_list"}) {
    const PortJson::Value* list = root.Find(key);
    REQUIRE(list != nullptr && list->IsArray());
    for (const PortJson::Value& name : list->AsArray())
      (key[6] == 'a' ? options.trickAllow : options.trickDeny).push_back(name.AsString());
  }
  return options;
}

// Walks the seed from its start items: every location must come into logic,
// Returns the number reached.
size_t Sweep(const Seed& seed, const PortApLogic::Options& options) {
  PortApLogic::Items items;
  for (const int64_t id : seed.startItems)
    ++items[id];
  size_t count = 0;
  size_t checkCount = 0;
  const PortApLogic::Check* checks = PortApLogic::Checks(checkCount);
  REQUIRE(checkCount == 100);
  std::vector< bool > taken(checkCount, false);
  for (bool changed = true; changed;) {
    changed = false;
    const std::vector< PortApLogic::Level > levels = PortApLogic::Evaluate(options, items);
    for (size_t i = 0; i < checkCount; ++i) {
      if (taken[i] || levels[i] != PortApLogic::Level::Normal)
        continue;
      taken[i] = true;
      changed = true;
      ++count;
      const auto it = seed.placements.find(checks[i].id);
      REQUIRE(it != seed.placements.end());
      ++items[it->second];
    }
  }
  return count;
}

std::map< int64_t, int64_t > Composition(const Seed& seed) {
  std::map< int64_t, int64_t > counts;
  for (const auto& entry : seed.placements)
    ++counts[entry.second - MP::kItemBase];
  for (const int64_t id : seed.startItems)
    ++counts[id - MP::kItemBase];
  return counts;
}

void CheckSeed(const Settings& settings, const std::string& text) {
  const Seed seed = Make(settings, text);
  REQUIRE(seed.name == text);
  REQUIRE(seed.placements.size() == 100);

  PortJson::Value root;
  size_t offset = 0;
  const char* reason = nullptr;
  REQUIRE(PortJson::Parse(seed.slotData, root, offset, &reason));
  REQUIRE(root.IsObject());
  // The slot data is canonical compact JSON: what the seed file's writer
  // makes of it again is the same text, so a saved seed reads back unchanged.
  port::JsonWriter canonical;
  canonical.Value(root);
  REQUIRE(canonical.Take() == seed.slotData);
  PortApWorld::Layout layout;
  const PortApLogic::Options options = OptionsFromSlotData(root, layout);
  REQUIRE(Sweep(seed, options) == 100);
  REQUIRE(Int(root, "required_artifacts", 0) == settings.requiredArtifacts);
  REQUIRE(Int(root, "final_bosses", -1) == settings.finalBosses);
  if (settings.elevatorRandomization)
    REQUIRE(layout.elevators != PortApWorld::Layout().elevators);
  REQUIRE(layout.hasDoorColors == (settings.doorColorRandomization != 0));
  REQUIRE(layout.etankCapacity == std::clamp(settings.etankCapacity, 10, 200));
  REQUIRE(!seed.spoiler.empty());

  // Pool: 12 artifacts, 100 locations, and the right beam family.
  const std::map< int64_t, int64_t > counts = Composition(seed);
  int64_t total = 0;
  for (const auto& entry : counts)
    total += entry.second;
  // Start items sit outside the 100 locations but are part of the pool.
  REQUIRE(total == 100 + static_cast< int64_t >(seed.startItems.size()));
  for (int a = 29; a <= 40; ++a)
    REQUIRE(counts.at(a) == 1);
  REQUIRE(counts.at(24) == 14);
  REQUIRE(counts.at(settings.missileLauncher ? 43 : 4) >= 1);
  REQUIRE(settings.missileLauncher ? counts.at(43) == 1 : counts.count(43) == 0);
  REQUIRE(settings.mainPowerBomb ? counts.at(44) == 1 : counts.count(44) == 0);
  if (settings.progressiveBeams) {
    for (const int id : {49, 51, 52, 53})
      REQUIRE(counts.at(id) == 3);
    REQUIRE(counts.count(10) == 0);
  } else {
    for (const int id : {0, 1, 2, 3, 10, 11, 14, 28, 8})
      REQUIRE(counts.at(id) == 1);
    REQUIRE(counts.count(49) == 0);
  }
  REQUIRE((counts.count(5) != 0 ? counts.at(5) : 0) == 1);
  REQUIRE(counts.count(41) == (settings.shuffleUnlimitedMissiles ? 1u : 0u));
  REQUIRE(counts.count(42) == (settings.shuffleUnlimitedPowerBombs ? 1u : 0u));
  if (settings.shuffleUnlimitedMissiles)
    REQUIRE(counts.at(41) == 1);
  if (settings.shuffleUnlimitedPowerBombs)
    REQUIRE(counts.at(42) == 1);
  // The beam, the start room's loadout, and the Scan Visor unless shuffled.
  REQUIRE(seed.startItems.size() >= (settings.shuffleScanVisor ? 1u : 2u));
  REQUIRE(std::set< int64_t >(seed.startItems.begin(), seed.startItems.end()).size() == seed.startItems.size());
  PortApWorld::Place place;
  REQUIRE(PortApWorld::StartRoom(layout, place));
  if (settings.startingRoom == 0)
    REQUIRE(layout.startRoom == "Landing Site" || layout.startRoom == "Save Station 1");
  REQUIRE(layout.hasShields == (settings.blastShieldRandomization != 0 || settings.lockedDoorCount > 0));
  int locked = 0;
  const std::set< std::string > types = {"Bomb",       "Charge Beam", "Flamethrower",  "Ice Spreader",
                                         "Wavebuster", "Power Bomb",  "Super Missile", "Missile", "Disabled"};
  for (const auto& area : layout.shields) {
    int combos = 0;
    for (const auto& room : area.second) {
      for (const auto& entry : room.second) {
        REQUIRE(types.count(entry.second) != 0);
        locked += entry.second == "Disabled" ? 1 : 0;
        combos += entry.second == "Flamethrower" || entry.second == "Ice Spreader" || entry.second == "Wavebuster";
        // replace_existing turns the vanilla Missile shields into something else.
        if (settings.blastShieldRandomization == 1)
          REQUIRE(entry.second != "Missile");
      }
    }
    REQUIRE(combos <= 1);
    if (settings.blastShieldAvailableTypes == 0)
      REQUIRE(combos == 0);
  }
  REQUIRE(locked == settings.lockedDoorCount);
  if (settings.blastShieldRandomization != 0)
    REQUIRE(!layout.shields.empty());
  const PortJson::Value* beam = root.Find("starting_beam");
  if (beam != nullptr)
    REQUIRE(beam->IsString() && (beam->AsString() == "Power Beam" || beam->AsString() == "Wave Beam" ||
                                 beam->AsString() == "Ice Beam" || beam->AsString() == "Plasma Beam"));
  if (!settings.randomizeStartingBeam && settings.doorColorRandomization == 0 && settings.startingRoom == 0)
    REQUIRE(beam == nullptr);

  const PortJson::Value* hints = root.Find("artifact_locations");
  if (settings.artifactHints) {
    REQUIRE(hints != nullptr && hints->IsObject() && hints->Size() == 12);
    for (const auto& member : hints->AsObject()) {
      REQUIRE(member.second.IsArray() && member.second.Size() == 2);
      const int64_t location = member.second.AsArray()[0].AsInt();
      REQUIRE(MP::ItemName(seed.placements.at(location)) == member.first);
    }
  } else {
    REQUIRE(hints == nullptr);
  }
}

} // namespace

int main() {
  // The port's item numbering is not the reference's.
  REQUIRE(std::string(MP::ItemName(MP::kItemBase + 22)) == "Varia Suit");
  REQUIRE(std::string(MP::ItemName(MP::kItemBase + 4)) == "Missile Expansion");
  REQUIRE(std::string(MP::ItemName(MP::kItemBase + 29)) == "Artifact of Truth");

  // Determinism.
  {
    Settings s;
    s.elevatorRandomization = true;
    s.doorColorRandomization = 2;
    const Seed a = Make(s, "determinism");
    const Seed b = Make(s, "determinism");
    REQUIRE(a.slotData == b.slotData && a.placements == b.placements && a.spoiler == b.spoiler &&
            a.startItems == b.startItems);
    const Seed c = Make(s, "determinism2");
    REQUIRE(c.placements != a.placements);
    Settings t = s;
    t.finalBosses = 1;
    REQUIRE(Make(t, "determinism").placements != a.placements);
    // A changed tank capacity is its own seed; outside 10..200 it is clamped.
    Settings e = s;
    e.etankCapacity = 50;
    REQUIRE(Make(e, "determinism").placements != a.placements);
    e.etankCapacity = 5;
    REQUIRE(Make(e, "determinism").slotData.find("\"etank_capacity\":10,") != std::string::npos);
    Settings u = s;
    u.shuffleUnlimitedMissiles = true;
    REQUIRE(Make(u, "determinism").placements != a.placements);
    const Seed r = Make(Settings(), "");
    REQUIRE(r.name.size() == 8);
  }

  // Golden output: the options at the head of slot_data, byte for byte, as
  // the hand-assembled writer made them before port::JsonWriter replaced it
  // (0/1 for toggles, the apworld's key order, escaped trick names).
  {
    Settings s;
    s.randomizeStartingBeam = true;
    s.trickDifficulty = 1;
    s.trickAllow = {"Wallcrawl", "Say \"hi\"\n\\"};
    s.trickDeny = {"Bomb Jump"};
    s.blastShieldRandomization = 2;
    s.etankCapacity = 37;
    const std::string slotData = Make(s, "golden").slotData;
    const std::string head = slotData.substr(0, slotData.find("\"starting_room_name\""));
    REQUIRE(head ==
        "{\"missile_launcher\":0,\"main_power_bomb\":0,\"progressive_beam_upgrades\":0,"
        "\"shuffle_scan_visor\":0,\"pre_scan_elevators\":1,\"elevator_randomization\":0,"
        "\"door_color_randomization\":0,\"trick_difficulty\":1,\"combat_logic_difficulty\":0,"
        "\"remove_xray_requirements\":0,\"remove_thermal_requirements\":0,"
        "\"flaahgra_power_bombs\":0,\"etank_capacity\":37,\"non_varia_heat_damage\":1,"
        "\"staggered_suit_damage\":1,\"backwards_lower_mines\":0,\"remove_hive_mecha\":1,"
        "\"spring_ball\":1,\"shuffle_unlimited_missiles\":0,\"shuffle_unlimited_power_bombs\":0,"
        "\"required_artifacts\":12,\"final_bosses\":0,\"artifact_hints\":1,\"starting_room\":0,"
        "\"randomize_starting_beam\":1,\"blast_shield_randomization\":2,"
        "\"blast_shield_frequency\":4,\"blast_shield_available_types\":0,\"locked_door_count\":0,"
        "\"include_power_beam_doors\":0,\"include_morph_ball_bomb_doors\":0,"
        "\"starting_beam\":\"Ice Beam\",\"trick_allow_list\":[\"Wallcrawl\",\"Say \\\"hi\\\"\\n\\\\\"],"
        "\"trick_deny_list\":[\"Bomb Jump\"],");
  }

  // Pool composition per combination, then a spread of seeds.
  int made = 0;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 240; ++i) {
    Settings s;
    s.requiredArtifacts = (i % 3 == 0) ? 1 : 12;
    s.finalBosses = i % 4;
    s.artifactHints = (i % 5) != 0;
    s.missileLauncher = (i & 1) != 0;
    s.mainPowerBomb = (i & 2) != 0;
    s.progressiveBeams = (i & 4) != 0;
    s.shuffleScanVisor = (i & 8) != 0;
    s.preScanElevators = (i % 7) != 0;
    s.elevatorRandomization = (i & 16) != 0;
    s.doorColorRandomization = (i / 32) % 3;
    s.trickDifficulty = (i % 11 == 0) ? 0 : (i % 13 == 0 ? 2 : -1);
    s.combatLogic = (i % 6 == 0) ? -1 : (i % 6 == 1 ? 1 : 0);
    s.removeXray = i % 3;
    s.removeThermal = (i / 3) % 3;
    s.removeHiveMecha = (i % 9) == 0;
    s.backwardsLowerMines = (i % 10) == 0;
    s.flaahgraPowerBombs = (i % 8) == 0;
    s.etankCapacity = (i % 5) == 0 ? 50 : (i % 5 == 1 ? 200 : 100);
    s.shuffleUnlimitedMissiles = (i % 3) == 1;
    s.shuffleUnlimitedPowerBombs = (i % 4) == 2;
    s.springBall = (i % 4) != 0;
    s.startingRoom = (i / 3) % 3;
    s.randomizeStartingBeam = (i % 5) == 1;
    s.blastShieldRandomization = (i / 2) % 3;
    s.blastShieldFrequency = (i % 3 == 0) ? 1 : (i % 3 == 1 ? 4 : 6);
    s.blastShieldAvailableTypes = (i / 4) % 2;
    s.lockedDoorCount = (i / 5) % 3;
    s.includePowerBeamDoors = (i % 6) >= 3;
    s.includeMorphBallBombDoors = (i % 7) >= 4;
    CheckSeed(s, "seed" + std::to_string(i));
    ++made;
  }
  const double ms = std::chrono::duration< double, std::milli >(std::chrono::steady_clock::now() - start).count();
  std::printf("%d seeds, %.1f ms each\n", made, ms / made);
  return 0;
}
