#include "port_env.h"
#include "port_strings.h"
#include "port_json.h"
#include "port_randomizer.h"
#include "port_log.h"
#include "port_paths.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace PortRandomizer {
namespace {

struct ItemEntry {
  const char* name;
  int value;
};

// Frozen retail data. The enum source is include/MetroidPrime/Player/CPlayerState.hpp;
// tests/port_randomizer.cpp drift-checks these values against the real enum.
constexpr ItemEntry kItems[] = {
    {"PowerBeam", 0},      {"IceBeam", 1},       {"WaveBeam", 2},
    {"PlasmaBeam", 3},     {"Missiles", 4},      {"ScanVisor", 5},
    {"MorphBallBombs", 6}, {"PowerBombs", 7},    {"Flamethrower", 8},
    {"ThermalVisor", 9},   {"ChargeBeam", 10},   {"SuperMissile", 11},
    {"GrappleBeam", 12},   {"XRayVisor", 13},    {"IceSpreader", 14},
    {"SpaceJumpBoots", 15},{"MorphBall", 16},    {"CombatVisor", 17},
    {"BoostBall", 18},     {"SpiderBall", 19},   {"PowerSuit", 20},
    {"GravitySuit", 21},   {"VariaSuit", 22},    {"PhazonSuit", 23},
    {"EnergyTanks", 24},   {"UnknownItem1", 25}, {"HealthRefill", 26},
    {"UnknownItem2", 27},  {"Wavebuster", 28},   {"Truth", 29},
    {"Strength", 30},      {"Elder", 31},        {"Wild", 32},
    {"Lifegiver", 33},     {"Warrior", 34},      {"Chozo", 35},
    {"Nature", 36},        {"Sun", 37},          {"World", 38},
    {"Spirit", 39},        {"Newborn", 40},
};

using Placement = SeedPlacement;

struct State {
  std::map<std::string, Placement> placements;
  std::set<std::string> loggedPlacements;
  std::map<int, PickupModel> itemModels;
  std::string seedName;
  int checkCount = 0;
  bool enabled = false;
  bool dump = false;
};

State& GetState() {
  static State state;
  return state;
}

std::string UserDirectory() {
  const std::string& dir = PortPaths::UserFolder();
  return dir.empty() ? std::string("./") : dir;
}

std::string SeedPath() {
  if (const char* env = port::EnvString("MP_RANDO_SEED")) {
    if (env[0] != '\0')
      return env;
  }
  return UserDirectory() + "randomizer_seed.json";
}

std::string LogPath(const char* name) { return UserDirectory() + name; }

// Whole numbers that fit an int: the seed grammar has no fractions.
bool SeedInteger(const PortJson::Value& value, int& out) {
  if (!value.IsNumber())
    return false;
  const double number = value.AsNumber();
  if (!(number >= std::numeric_limits< int >::min() && number <= std::numeric_limits< int >::max()) ||
      number != std::floor(number))
    return false;
  out = static_cast< int >(number);
  return true;
}

// Asset ids are written as 1-8 hexadecimal digits, optionally prefixed with
// "0x", so a seed reads like the location keys and the LOC dump lines.
bool SeedHexAsset(const PortJson::Value& value, uint32_t& out) {
  if (!value.IsString())
    return false;
  const std::string& text = value.AsString();
  size_t i = 0;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    i = 2;
  if (i >= text.size() || text.size() - i > 8)
    return false;
  uint32_t result = 0;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    result <<= 4;
    if (c >= '0' && c <= '9')
      result |= static_cast< uint32_t >(c - '0');
    else if (c >= 'a' && c <= 'f')
      result |= static_cast< uint32_t >(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      result |= static_cast< uint32_t >(c - 'A' + 10);
    else
      return false;
  }
  out = result;
  return true;
}

// Bounded well inside int so the game's own sums (amount plus what the
// player already has) cannot overflow.
bool SeedGrantValue(const PortJson::Value& value, int& out, std::string& reason, const std::string& where) {
  if (!SeedInteger(value, out)) {
    reason = where + ": amount and capacity must be whole numbers";
    return false;
  }
  if (out < -9999 || out > 9999) {
    reason = where + ": amount and capacity must be between -9999 and 9999";
    return false;
  }
  return true;
}

bool ReadPlacement(const PortJson::Value& value, Placement& out, std::string& reason, const std::string& where) {
  if (!value.IsObject()) {
    reason = where + ": a placement is an object";
    return false;
  }
  Placement placement;
  bool hasItem = false;
  // Members in file order, so a repeated field keeps its last value.
  for (const auto& [key, field] : value.AsObject()) {
    if (key == "item") {
      if (!field.IsString()) {
        reason = where + ": item must be a string";
        return false;
      }
      placement.itemType = PortRandomizer::ItemFromName(field.AsString().c_str());
      if (placement.itemType < 0) {
        reason = where + ": unknown item name \"" + field.AsString() + "\"";
        return false;
      }
      hasItem = true;
    } else if (key == "amount") {
      if (!SeedGrantValue(field, placement.amount, reason, where))
        return false;
      placement.hasAmount = true;
    } else if (key == "capacity") {
      if (!SeedGrantValue(field, placement.capacity, reason, where))
        return false;
      placement.hasCapacity = true;
    }
  }
  if (!hasItem) {
    reason = where + ": placement is missing required item";
    return false;
  }
  out = placement;
  return true;
}

bool ReadModel(const PortJson::Value& value, PickupModel& out, std::string& reason, const std::string& where) {
  if (!value.IsObject()) {
    reason = where + ": a model entry is an object";
    return false;
  }
  PickupModel model;
  for (const auto& [key, field] : value.AsObject()) {
    if (key == "model" || key == "acs") {
      if (!SeedHexAsset(field, key == "model" ? model.model : model.acs)) {
        reason = where + ": " + key + " must be 1 to 8 hexadecimal digits";
        return false;
      }
    } else if (key == "character" || key == "animation") {
      int number = 0;
      if (!SeedInteger(field, number) || number < 0) {
        reason = where + ": character and animation must be non-negative whole numbers";
        return false;
      }
      (key == "character" ? model.character : model.animation) = static_cast< uint32_t >(number);
    }
  }
  if (model.model == 0 && model.acs == 0) {
    reason = where + ": model entry needs a model or acs asset";
    return false;
  }
  out = model;
  return true;
}

bool LoadSeed(State& state) {
  const std::string path = SeedPath();
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return false;
  const std::string text = port::ReadAllText(input);
  if (input.bad()) {
    PortLog::Write( "randomizer: seed read error at byte offset 0: could not read file\n");
    return false;
  }

  SeedFile seed;
  size_t offset = 0;
  std::string reason;
  if (!ParseSeedFile(text, seed, offset, reason)) {
    if (offset == kSeedNoOffset)
      PortLog::Write("randomizer: seed error: %s\n", reason.c_str());
    else
      PortLog::Write("randomizer: seed parse error at byte offset %zu: %s\n", offset, reason.c_str());
    return false;
  }

  state.seedName = std::move(seed.name);
  state.placements = std::move(seed.placements);
  state.itemModels = std::move(seed.models);
  state.enabled = !state.placements.empty();
  return state.enabled;
}

// The logs are appended to across runs, so one past this size is moved to
// "<name>.old" (replacing the last one) and started afresh.
constexpr std::uintmax_t kLogLimit = 4u * 1024u * 1024u;

void AppendLog(const char* fileName, const char* line) noexcept {
  try {
    const std::string path = LogPath(fileName);
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (!error && size > kLogLimit)
      std::filesystem::rename(path, path + ".old", error);
    std::ofstream output(path, std::ios::app);
    if (output)
      output << line;
  } catch (...) {
  }
}

} // namespace

bool ParseSeedFile(const std::string& text, SeedFile& out, size_t& errorOffset,
                   std::string& errorReason) {
  PortJson::Value root;
  const char* syntax = nullptr;
  if (!PortJson::Parse(text, root, errorOffset, &syntax)) {
    errorReason = syntax != nullptr ? syntax : "invalid JSON";
    return false;
  }
  // From here on the text is well-formed JSON; a problem is in what it says.
  errorOffset = kSeedNoOffset;
  if (!root.IsObject()) {
    errorReason = "a seed file is a JSON object";
    return false;
  }
  SeedFile seed;
  for (const auto& [key, value] : root.AsObject()) {
    if (key == "seed") {
      if (!value.IsString()) {
        errorReason = "\"seed\" must be a string";
        return false;
      }
      seed.name = value.AsString();
    } else if (key == "locations") {
      if (!value.IsObject()) {
        errorReason = "\"locations\" must be an object";
        return false;
      }
      for (const auto& [rawKey, entry] : value.AsObject()) {
        // The game formats its keys in upper case (FormatLocationKey).
        std::string location = rawKey;
        std::transform(location.begin(), location.end(), location.begin(),
                       [](unsigned char c) { return static_cast< char >(std::toupper(c)); });
        Placement placement;
        if (!ReadPlacement(entry, placement, errorReason, "location \"" + rawKey + "\""))
          return false;
        seed.placements[std::move(location)] = placement;
      }
    } else if (key == "models") {
      if (!value.IsObject()) {
        errorReason = "\"models\" must be an object";
        return false;
      }
      for (const auto& [name, entry] : value.AsObject()) {
        const int item = PortRandomizer::ItemFromName(name.c_str());
        if (item < 0) {
          errorReason = "models: unknown item name \"" + name + "\"";
          return false;
        }
        PickupModel model;
        if (!ReadModel(entry, model, errorReason, "model for " + name))
          return false;
        seed.models[item] = model;
      }
    }
    // Other keys are ignored, whatever they hold.
  }
  out = std::move(seed);
  return true;
}

void EnsureLoaded() {
  try {
    static const bool loaded = []() noexcept {
      try {
        State& state = GetState();
        state.dump = port::EnvFlag("MP_RANDO_DUMP");
        (void)LoadSeed(state);
      } catch (...) {
        try {
          State& state = GetState();
          state.enabled = false;
          state.placements.clear();
          state.seedName.clear();
        } catch (...) {
        }
      }
      return true;
    }();
    (void)loaded;
    // Announce the configuration once so a run tells the user whether a seed
    // loaded. Silent when the randomizer is inactive, so normal runs are clean.
    static const bool announced = [] {
      const State& state = GetState();
      if (state.dump) {
        PortLog::Write( "randomizer: dump mode (MP_RANDO_DUMP)\n");
      } else if (state.enabled) {
        PortLog::Write( "randomizer: seed '%s', %zu placements, %zu item models\n",
                     state.seedName.c_str(), state.placements.size(), state.itemModels.size());
      }
      return true;
    }();
    (void)announced;
  } catch (...) {
    // Includes function-local static initialization failures: the game hook must not unwind.
  }
}

bool Enabled() {
  EnsureLoaded();
  try {
    return GetState().enabled;
  } catch (...) {
    return false;
  }
}

bool DumpEnabled() {
  EnsureLoaded();
  try {
    return GetState().dump;
  } catch (...) {
    return false;
  }
}

const char* SeedName() {
  EnsureLoaded();
  try {
    return GetState().seedName.c_str();
  } catch (...) {
    return "";
  }
}

int CheckCount() {
  EnsureLoaded();
  try {
    return GetState().checkCount;
  } catch (...) {
    return 0;
  }
}

const char* StatusText() {
  EnsureLoaded();
  static char status[48];
  try {
    const State& state = GetState();
    if (state.dump) {
      std::snprintf(status, sizeof(status), "rando: dump mode");
    } else if (state.enabled) {
      std::snprintf(status, sizeof(status), "rando: %.20s, %d checks", state.seedName.c_str(),
                    state.checkCount);
    } else {
      std::snprintf(status, sizeof(status), "rando: off");
    }
  } catch (...) {
    std::snprintf(status, sizeof(status), "rando: off");
  }
  return status;
}

bool ApplyPickup(uint32_t worldAssetId, uint32_t areaAssetId, uint32_t entityId, int& itemType,
                 int& capacity, int& amount, const PickupModel& model) {
  EnsureLoaded();
  try {
    State& state = GetState();
    char key[32];
    FormatLocationKey(worldAssetId, areaAssetId, entityId, key, sizeof(key));
    if (state.dump) {
      char line[200];
      std::snprintf(line, sizeof(line),
                    "LOC %s %s amount=%d capacity=%d model=%08X acs=%08X character=%u animation=%u\n",
                    key, ItemName(itemType), amount, capacity, model.model, model.acs,
                    model.character, model.animation);
      AppendLog("randomizer_locations.log", line);
      return false;
    }
    if (!state.enabled)
      return false;
    const auto it = state.placements.find(key);
    if (it == state.placements.end())
      return false;
    const int previous = itemType;
    itemType = it->second.itemType;
    if (it->second.hasAmount)
      amount = it->second.amount;
    if (it->second.hasCapacity)
      capacity = it->second.capacity;
    // Record the rewrite so a seed can be checked without playing the whole
    // game; areas reconstruct their pickups every time they load, so log each
    // location once per session instead of once per load. The model is the one
    // the seed associates with the new item (zero when it keeps the original).
    if (state.loggedPlacements.insert(key).second) {
      const auto modelIt = state.itemModels.find(itemType);
      const uint32_t modelId = modelIt != state.itemModels.end() ? modelIt->second.model : 0;
      const uint32_t acsId = modelIt != state.itemModels.end() ? modelIt->second.acs : 0;
      char line[200];
      std::snprintf(line, sizeof(line),
                    "PLACE %s %s -> %s amount=%d capacity=%d model=%08X acs=%08X\n", key,
                    ItemName(previous), ItemName(itemType), amount, capacity, modelId, acsId);
      AppendLog("randomizer_placements.log", line);
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool Placed(uint32_t worldAssetId, uint32_t areaAssetId, uint32_t entityId, int& itemType,
            int& capacity) {
  EnsureLoaded();
  try {
    const State& state = GetState();
    if (!state.enabled)
      return false;
    // Keys hold the editor id with its layer bits; match the area's keys on
    // the id alone.
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "%08X:%08X:", static_cast<unsigned int>(worldAssetId),
                  static_cast<unsigned int>(areaAssetId));
    for (auto it = state.placements.lower_bound(prefix);
         it != state.placements.end() && it->first.compare(0, 18, prefix) == 0; ++it) {
      const unsigned long id = std::strtoul(it->first.c_str() + 18, nullptr, 16);
      if ((id & 0x3FFFFFFul) != (entityId & 0x3FFFFFFu))
        continue;
      itemType = it->second.itemType;
      if (it->second.hasCapacity)
        capacity = it->second.capacity;
      return true;
    }
    return false;
  } catch (...) {
    return false;
  }
}

bool ModelForItem(int itemType, PickupModel& out) {
  EnsureLoaded();
  try {
    const State& state = GetState();
    const auto it = state.itemModels.find(itemType);
    if (it == state.itemModels.end())
      return false;
    out = it->second;
    return true;
  } catch (...) {
    return false;
  }
}

void RecordCheck(uint32_t worldAssetId, uint32_t areaAssetId, uint32_t entityId, int itemType) {
  EnsureLoaded();
  try {
    State& state = GetState();
    if (!state.enabled && !state.dump)
      return;
    if (state.checkCount < std::numeric_limits<int>::max())
      ++state.checkCount;
    char key[32];
    FormatLocationKey(worldAssetId, areaAssetId, entityId, key, sizeof(key));
    char line[128];
    std::snprintf(line, sizeof(line), "CHECK %s %s\n", key, ItemName(itemType));
    AppendLog("randomizer_checks.log", line);
  } catch (...) {
  }
}

void LogPickupGrant(uint32_t worldAssetId, uint32_t areaAssetId, uint32_t entityId, int itemType,
                    int capacity, int amount, int amountAfter, int capacityAfter) {
  try {
    if (!GetState().enabled)
      return;
    char key[32];
    FormatLocationKey(worldAssetId, areaAssetId, entityId, key, sizeof(key));
    PortLog::Write("randomizer: picked up %s: %s, capacity +%d, amount +%d; now %d/%d (amount/capacity)\n",
                   key, ItemName(itemType), capacity, amount, amountAfter, capacityAfter);
  } catch (...) {
  }
}

const char* ItemName(int itemType) {
  for (const ItemEntry& item : kItems) {
    if (item.value == itemType)
      return item.name;
  }
  return "Unknown";
}

int ItemFromName(const char* name) {
  if (name == nullptr)
    return -1;
  for (const ItemEntry& item : kItems) {
    const unsigned char* left = reinterpret_cast<const unsigned char*>(name);
    const unsigned char* right = reinterpret_cast<const unsigned char*>(item.name);
    while (*left != '\0' && *right != '\0' &&
           std::tolower(*left) == std::tolower(*right)) {
      ++left;
      ++right;
    }
    if (*left == '\0' && *right == '\0')
      return item.value;
  }
  return -1;
}

bool FindItem(int itemType, uint32_t& worldAssetId, uint32_t& areaAssetId, uint32_t& entityId) {
  EnsureLoaded();
  try {
    const State& state = GetState();
    if (!state.enabled)
      return false;
    for (const auto& [key, placement] : state.placements) {
      unsigned int world = 0, area = 0, entity = 0;
      if (placement.itemType == itemType &&
          std::sscanf(key.c_str(), "%8X:%8X:%8X", &world, &area, &entity) == 3) {
        worldAssetId = world;
        areaAssetId = area;
        entityId = entity;
        return true;
      }
    }
  } catch (...) {
  }
  return false;
}

void FormatLocationKey(uint32_t worldAssetId, uint32_t areaAssetId, uint32_t entityId, char* out,
                       int outSize) {
  if (out == nullptr || outSize <= 0)
    return;
  std::snprintf(out, static_cast<size_t>(outSize), "%08X:%08X:%08X",
                static_cast<unsigned int>(worldAssetId), static_cast<unsigned int>(areaAssetId),
                static_cast<unsigned int>(entityId));
}

} // namespace PortRandomizer
