#include "port_strings.h"
#include "port_rando_gen.h"

#include "port_json.h"
#include "port_json_writer.h"
#include "port_paths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

// Settings text and seed files for the built-in randomizer. The generator
// itself is in port_rando_gen.cpp; a seed is saved so that it can be played
// again, and its name is all the player needs to keep.
namespace PortRandoGen {
namespace {

constexpr int kSeedFileVersion = 1;

// slot_data is kept as JSON text, so a loaded value is written back out:
// compactly, with whole numbers as integers (item and location ids).
std::string Compact(const PortJson::Value& value) {
  port::JsonWriter json;
  json.Value(value);
  return json.Take();
}

// A number or a bool (the apworld writes toggles as 0/1), clamped.
int IntMember(const PortJson::Value& root, const char* name, int fallback, int low, int high) {
  const PortJson::Value* member = root.Find(name);
  if (member == nullptr)
    return fallback;
  int64_t value = fallback;
  if (member->IsBool())
    value = member->AsBool() ? 1 : 0;
  else if (member->IsNumber())
    value = member->AsInt(fallback);
  return static_cast< int >(std::clamp< int64_t >(value, low, high));
}

bool BoolMember(const PortJson::Value& root, const char* name, bool fallback) {
  return IntMember(root, name, fallback ? 1 : 0, 0, 1) != 0;
}

void NamesMember(const PortJson::Value& root, const char* name, std::vector< std::string >& out) {
  out.clear();
  const PortJson::Value* member = root.Find(name);
  if (member == nullptr || !member->IsArray())
    return;
  for (const PortJson::Value& entry : member->AsArray()) {
    if (entry.IsString())
      out.push_back(entry.AsString());
  }
}

void Names(port::JsonWriter& json, const char* key, const std::vector< std::string >& names) {
  json.Key(key).BeginArray();
  for (const std::string& name : names)
    json.String(name);
  json.EndArray();
}

// The settings object, under the apworld's option names; toggles are 0/1 as
// the apworld writes them.
void WriteSettings(port::JsonWriter& json, const Settings& s) {
  const auto flag = [](bool value) { return value ? 1 : 0; };
  json.BeginObject();
  json.Key("required_artifacts").Number(s.requiredArtifacts);
  json.Key("final_bosses").Number(s.finalBosses);
  json.Key("artifact_hints").Number(flag(s.artifactHints));
  json.Key("missile_launcher").Number(flag(s.missileLauncher));
  json.Key("main_power_bomb").Number(flag(s.mainPowerBomb));
  json.Key("shuffle_scan_visor").Number(flag(s.shuffleScanVisor));
  json.Key("pre_scan_elevators").Number(flag(s.preScanElevators));
  json.Key("elevator_randomization").Number(flag(s.elevatorRandomization));
  json.Key("door_color_randomization").Number(s.doorColorRandomization);
  json.Key("progressive_beam_upgrades").Number(flag(s.progressiveBeams));
  json.Key("non_varia_heat_damage").Number(flag(s.nonVariaHeatDamage));
  json.Key("staggered_suit_damage").Number(s.staggeredSuitDamage);
  json.Key("combat_logic_difficulty").Number(s.combatLogic);
  json.Key("trick_difficulty").Number(s.trickDifficulty);
  Names(json, "trick_allow_list", s.trickAllow);
  Names(json, "trick_deny_list", s.trickDeny);
  json.Key("flaahgra_power_bombs").Number(flag(s.flaahgraPowerBombs));
  json.Key("etank_capacity").Number(s.etankCapacity);
  json.Key("backwards_lower_mines").Number(flag(s.backwardsLowerMines));
  json.Key("remove_xray_requirements").Number(s.removeXray);
  json.Key("remove_thermal_requirements").Number(s.removeThermal);
  json.Key("remove_hive_mecha").Number(flag(s.removeHiveMecha));
  json.Key("spring_ball").Number(flag(s.springBall));
  json.Key("shuffle_unlimited_missiles").Number(flag(s.shuffleUnlimitedMissiles));
  json.Key("shuffle_unlimited_power_bombs").Number(flag(s.shuffleUnlimitedPowerBombs));
  json.Key("starting_room").Number(s.startingRoom);
  json.Key("randomize_starting_beam").Number(flag(s.randomizeStartingBeam));
  json.Key("blast_shield_randomization").Number(s.blastShieldRandomization);
  json.Key("blast_shield_frequency").Number(s.blastShieldFrequency);
  json.Key("blast_shield_available_types").Number(s.blastShieldAvailableTypes);
  json.Key("locked_door_count").Number(s.lockedDoorCount);
  json.Key("include_power_beam_doors").Number(flag(s.includePowerBeamDoors));
  json.Key("include_morph_ball_bomb_doors").Number(flag(s.includeMorphBallBombDoors));
  json.EndObject();
}

// The name is typed by the player and ends up in a file name.
std::string SafeName(const std::string& name) {
  std::string safe;
  for (const char c : name) {
    const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       c == '-' || c == '_' || c == '.';
    safe.push_back(plain ? c : '_');
  }
  // No hidden files, and "." / ".." must not name a directory.
  const size_t start = safe.find_first_not_of('.');
  safe = start == std::string::npos ? std::string() : safe.substr(start);
  if (safe.size() > 64)
    safe.resize(64);
  // <name>.state.json is the solo server's state of seed <name>.
  if (port::EndsWith(safe, ".state"))
    safe.back() = '_';
  return safe.empty() ? "seed" : safe;
}

bool ReadFile(const std::filesystem::path& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  out = port::ReadAllText(file);
  return !file.bad();
}

} // namespace

bool Settings::operator==(const Settings& other) const {
  return requiredArtifacts == other.requiredArtifacts && finalBosses == other.finalBosses &&
         artifactHints == other.artifactHints && missileLauncher == other.missileLauncher &&
         mainPowerBomb == other.mainPowerBomb && shuffleScanVisor == other.shuffleScanVisor &&
         preScanElevators == other.preScanElevators &&
         elevatorRandomization == other.elevatorRandomization &&
         doorColorRandomization == other.doorColorRandomization &&
         progressiveBeams == other.progressiveBeams &&
         nonVariaHeatDamage == other.nonVariaHeatDamage &&
         staggeredSuitDamage == other.staggeredSuitDamage && combatLogic == other.combatLogic &&
         trickDifficulty == other.trickDifficulty && trickAllow == other.trickAllow &&
         trickDeny == other.trickDeny && flaahgraPowerBombs == other.flaahgraPowerBombs &&
         etankCapacity == other.etankCapacity &&
         backwardsLowerMines == other.backwardsLowerMines && removeXray == other.removeXray &&
         removeThermal == other.removeThermal && removeHiveMecha == other.removeHiveMecha &&
         springBall == other.springBall && startingRoom == other.startingRoom &&
         shuffleUnlimitedMissiles == other.shuffleUnlimitedMissiles &&
         shuffleUnlimitedPowerBombs == other.shuffleUnlimitedPowerBombs &&
         randomizeStartingBeam == other.randomizeStartingBeam &&
         blastShieldRandomization == other.blastShieldRandomization &&
         blastShieldFrequency == other.blastShieldFrequency &&
         blastShieldAvailableTypes == other.blastShieldAvailableTypes &&
         lockedDoorCount == other.lockedDoorCount &&
         includePowerBeamDoors == other.includePowerBeamDoors &&
         includeMorphBallBombDoors == other.includeMorphBallBombDoors;
}

std::string SettingsText(const Settings& s) {
  port::JsonWriter json;
  WriteSettings(json, s);
  return json.Take();
}

namespace {

void ReadSettings(const PortJson::Value& root, Settings& out) {
  Settings s;
  s.requiredArtifacts = IntMember(root, "required_artifacts", s.requiredArtifacts, 1, 12);
  s.finalBosses = IntMember(root, "final_bosses", s.finalBosses, 0, 3);
  s.artifactHints = BoolMember(root, "artifact_hints", s.artifactHints);
  s.missileLauncher = BoolMember(root, "missile_launcher", s.missileLauncher);
  s.mainPowerBomb = BoolMember(root, "main_power_bomb", s.mainPowerBomb);
  s.shuffleScanVisor = BoolMember(root, "shuffle_scan_visor", s.shuffleScanVisor);
  s.preScanElevators = BoolMember(root, "pre_scan_elevators", s.preScanElevators);
  s.elevatorRandomization = BoolMember(root, "elevator_randomization", s.elevatorRandomization);
  s.doorColorRandomization =
      IntMember(root, "door_color_randomization", s.doorColorRandomization, 0, 2);
  s.progressiveBeams = BoolMember(root, "progressive_beam_upgrades", s.progressiveBeams);
  s.nonVariaHeatDamage = BoolMember(root, "non_varia_heat_damage", s.nonVariaHeatDamage);
  s.staggeredSuitDamage = IntMember(root, "staggered_suit_damage", s.staggeredSuitDamage, 0, 2);
  s.combatLogic = IntMember(root, "combat_logic_difficulty", s.combatLogic, -1, 1);
  s.trickDifficulty = IntMember(root, "trick_difficulty", s.trickDifficulty, -1, 2);
  NamesMember(root, "trick_allow_list", s.trickAllow);
  NamesMember(root, "trick_deny_list", s.trickDeny);
  s.flaahgraPowerBombs = BoolMember(root, "flaahgra_power_bombs", s.flaahgraPowerBombs);
  s.etankCapacity = IntMember(root, "etank_capacity", s.etankCapacity, 10, 200);
  s.backwardsLowerMines = BoolMember(root, "backwards_lower_mines", s.backwardsLowerMines);
  s.removeXray = IntMember(root, "remove_xray_requirements", s.removeXray, 0, 2);
  s.removeThermal = IntMember(root, "remove_thermal_requirements", s.removeThermal, 0, 2);
  s.removeHiveMecha = BoolMember(root, "remove_hive_mecha", s.removeHiveMecha);
  s.springBall = BoolMember(root, "spring_ball", s.springBall);
  s.shuffleUnlimitedMissiles = BoolMember(root, "shuffle_unlimited_missiles", s.shuffleUnlimitedMissiles);
  s.shuffleUnlimitedPowerBombs =
      BoolMember(root, "shuffle_unlimited_power_bombs", s.shuffleUnlimitedPowerBombs);
  s.startingRoom = IntMember(root, "starting_room", s.startingRoom, 0, 2);
  s.randomizeStartingBeam = BoolMember(root, "randomize_starting_beam", s.randomizeStartingBeam);
  s.blastShieldRandomization =
      IntMember(root, "blast_shield_randomization", s.blastShieldRandomization, 0, 2);
  s.blastShieldFrequency = IntMember(root, "blast_shield_frequency", s.blastShieldFrequency, 1, 6);
  s.blastShieldAvailableTypes =
      IntMember(root, "blast_shield_available_types", s.blastShieldAvailableTypes, 0, 1);
  s.lockedDoorCount = IntMember(root, "locked_door_count", s.lockedDoorCount, 0, 2);
  s.includePowerBeamDoors = BoolMember(root, "include_power_beam_doors", s.includePowerBeamDoors);
  s.includeMorphBallBombDoors =
      BoolMember(root, "include_morph_ball_bomb_doors", s.includeMorphBallBombDoors);
  out = std::move(s);
}

} // namespace

bool ParseSettings(const std::string& text, Settings& out) {
  PortJson::Value root;
  size_t offset = 0;
  const char* reason = nullptr;
  if (!PortJson::Parse(text, root, offset, &reason) || !root.IsObject())
    return false;
  ReadSettings(root, out);
  return true;
}

std::string SeedDirectory() {
  const std::string& user = PortPaths::UserFolder();
  return (std::filesystem::path(user.empty() ? std::string(".") : user) / "randomizer_seeds").string();
}

std::string SeedPath(const std::string& name) {
  return (std::filesystem::path(SeedDirectory()) / (SafeName(name) + ".json")).string();
}

bool Save(Seed& seed, std::string& error) {
  try {
    if (seed.name.empty()) {
      error = "the seed has no name";
      return false;
    }
    // Another seed (other options, or a name that comes out as the same file
    // name) may own this file already, and with it checked locations and a save
    // card that aren't this seed's, so this one is saved as <name>-2, -3, ...
    // The base is cut so the suffix survives SafeName's length limit.
    const std::string base = seed.name.substr(0, 56);
    for (int n = 2; std::filesystem::exists(SeedPath(seed.name)); ++n) {
      if (n > 999) {
        error = "too many seeds named " + base;
        return false;
      }
      Seed existing;
      std::string ignored;
      if (Load(SeedPath(seed.name), existing, ignored) && existing.settings == seed.settings &&
          existing.startItems == seed.startItems && existing.placements == seed.placements)
        break;
      seed.name = base + "-" + std::to_string(n);
    }
    std::string slotData = seed.slotData.empty() ? std::string("{}") : seed.slotData;
    // Checked here so a seed file is never written that Load would refuse.
    PortJson::Value slot;
    size_t offset = 0;
    const char* reason = nullptr;
    if (!PortJson::Parse(slotData, slot, offset, &reason) || !slot.IsObject()) {
      error = "the seed's slot data is not a JSON object";
      return false;
    }
    port::JsonWriter json;
    json.BeginObject();
    json.Key("version").Number(kSeedFileVersion);
    json.Key("name").String(seed.name);
    json.Key("settings");
    WriteSettings(json, seed.settings);
    json.Key("slot_data").Value(slot); // normalised, so the file has no stray whitespace
    json.Key("start_items").BeginArray();
    for (const int64_t item : seed.startItems)
      json.Number(item);
    json.EndArray();
    json.Key("placements").BeginObject();
    for (const auto& [location, item] : seed.placements)
      json.Key(std::to_string(location)).Number(item);
    json.EndObject();
    json.Key("spoiler").String(seed.spoiler);
    json.EndObject();
    const std::string out = json.Take() + "\n";

    const std::filesystem::path path = SeedPath(seed.name);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // A temporary beside it, renamed over it: a crash mid-write leaves the old
    // seed, and a seed that is being played is never half a file.
    const std::filesystem::path temp = path.string() + ".tmp";
    {
      std::ofstream file(temp, std::ios::binary | std::ios::trunc);
      file.write(out.data(), static_cast< std::streamsize >(out.size()));
      file.flush();
      if (!file) {
        error = "could not write " + temp.string();
        std::filesystem::remove(temp, ec);
        return false;
      }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
      error = "could not write " + path.string() + ": " + ec.message();
      std::filesystem::remove(temp, ec);
      return false;
    }
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

bool Load(const std::string& path, Seed& out, std::string& error) {
  try {
    std::string text;
    if (!ReadFile(path, text)) {
      error = "could not read " + path;
      return false;
    }
    PortJson::Value root;
    size_t offset = 0;
    const char* reason = nullptr;
    if (!PortJson::Parse(text, root, offset, &reason) || !root.IsObject()) {
      error = path + ": not a seed file" +
              (reason != nullptr ? std::string(" (") + reason + ")" : std::string());
      return false;
    }
    const PortJson::Value* version = root.Find("version");
    if (version == nullptr || version->AsInt(0) != kSeedFileVersion) {
      error = path + ": unsupported seed file version";
      return false;
    }
    const PortJson::Value* slot = root.Find("slot_data");
    const PortJson::Value* placements = root.Find("placements");
    if (slot == nullptr || !slot->IsObject() || placements == nullptr || !placements->IsObject()) {
      error = path + ": the seed file is missing its slot data or placements";
      return false;
    }
    Seed seed;
    seed.name = root.StringOr("name");
    if (seed.name.empty())
      seed.name = std::filesystem::path(path).stem().string();
    if (const PortJson::Value* settings = root.Find("settings"); settings != nullptr && settings->IsObject())
      ReadSettings(*settings, seed.settings);
    seed.slotData = Compact(*slot);
    if (const PortJson::Value* items = root.Find("start_items"); items != nullptr && items->IsArray()) {
      for (const PortJson::Value& item : items->AsArray()) {
        if (item.IsNumber())
          seed.startItems.push_back(item.AsInt());
      }
    }
    for (const auto& [key, item] : placements->AsObject()) {
      char* end = nullptr;
      const long long location = std::strtoll(key.c_str(), &end, 10);
      if (key.empty() || end == nullptr || *end != '\0' || !item.IsNumber()) {
        error = path + ": bad placement \"" + key + "\"";
        return false;
      }
      seed.placements[location] = item.AsInt();
    }
    seed.spoiler = root.StringOr("spoiler");
    out = std::move(seed);
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

} // namespace PortRandoGen
