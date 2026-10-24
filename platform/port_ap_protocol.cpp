#include "port_ap_protocol.h"
#include "port_hash.h"
#include "port_json_writer.h"
#include "port_strings.h"

#include "port_ap_metroidprime.h"
#include "port_randomizer.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace PortAp {
namespace Protocol {
namespace {

const PortJson::Value* Member(const PortJson::Value& object, const char* name) {
  return object.Find(name);
}

bool Integer(const PortJson::Value* value, int64_t& result) {
  if (value == nullptr || !value->IsNumber())
    return false;
  const double number = value->AsNumber();
  constexpr double kInt64Limit = 9223372036854775808.0;
  if (!std::isfinite(number) || std::trunc(number) != number || number < -kInt64Limit ||
      number >= kInt64Limit)
    return false;
  result = value->AsInt();
  return true;
}

bool IntegerMember(const PortJson::Value& object, const char* name, int64_t& result) {
  return Integer(Member(object, name), result);
}

// Writes through a temporary file and a rename, so a crash mid-write leaves
// the old file rather than half of the new one.
bool WriteFileAtomically(const std::string& path, const std::string& contents) {
  const std::filesystem::path target(path);
  if (!target.parent_path().empty())
    std::filesystem::create_directories(target.parent_path());
  const std::string temporary = path + ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
      return false;
    file << contents;
    file.flush();
    if (!file)
      return false;
    file.close();
    if (!file)
      return false;
  }
  if (std::rename(temporary.c_str(), path.c_str()) == 0)
    return true;
#ifdef _WIN32
  // The C rename operation replaces an existing destination on POSIX, but
  // not on Windows. MoveFileExW replaces it in one step, so there is never a
  // moment without the old file or the new one (removing the old file first
  // lost the state if the game died in between). The paths go through
  // std::filesystem::path, as the narrow ones above are read by the CRT.
  return MoveFileExW(std::filesystem::path(temporary).c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return false;
#endif
}

bool ValidLocationKey(const std::string& key) {
  if (key.size() != 26 || key[8] != ':' || key[17] != ':')
    return false;
  for (size_t i = 0; i < key.size(); ++i) {
    if (i == 8 || i == 17)
      continue;
    const char c = key[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return false;
  }
  return true;
}

bool ParseItemKey(const std::string& key, int64_t& value) {
  if (key.empty())
    return false;
  for (char c : key) {
    if (c < '0' || c > '9')
      return false;
  }
  const auto parsed = std::from_chars(key.data(), key.data() + key.size(), value, 10);
  return parsed.ec == std::errc() && parsed.ptr == key.data() + key.size();
}

constexpr int64_t kMaxGrantValue = 9999;

bool OptionalInt(const PortJson::Value& object, const char* field, int fallback, int& result,
                 std::string& error) {
  const PortJson::Value* value = Member(object, field);
  if (value == nullptr) {
    result = fallback;
    return true;
  }
  // Bounded well inside int so the game's own sums (amount plus what the
  // player already has) cannot overflow.
  int64_t number = 0;
  if (!Integer(value, number) || number < -kMaxGrantValue || number > kMaxGrantValue) {
    error = std::string("items.") + field + " must be an integer between -9999 and 9999";
    return false;
  }
  result = static_cast<int>(number);
  return true;
}

// Parses one grant object ({"item", "amount", "capacity", "display"}).
// `label` names it in errors: "item 12" or "item 12 progressive step 2".
bool ParseGrant(const PortJson::Value& object, const std::string& label, int64_t itemId,
                ItemGrant& grant, std::string& error) {
  if (!object.IsObject()) {
    error = label + " must be an object";
    return false;
  }
  const PortJson::Value* nameValue = Member(object, "item");
  if (nameValue == nullptr || !nameValue->IsString()) {
    error = label + " requires an item name string";
    return false;
  }
  const std::string& itemName = nameValue->AsString();
  const int itemType = PortRandomizer::ItemFromName(itemName.c_str());
  if (itemType < 0) {
    error = "unknown item name: " + itemName;
    return false;
  }
  grant.itemId = itemId;
  grant.itemType = itemType;
  // `display` is the player-facing name; it is independent of the
  // randomizer key in `item` and defaults to that resolved item's name.
  grant.display = itemName;
  const PortJson::Value* displayValue = Member(object, "display");
  if (displayValue != nullptr) {
    if (!displayValue->IsString()) {
      error = label + " display must be a string";
      return false;
    }
    grant.display = displayValue->AsString();
  }
  return OptionalInt(object, "amount", 1, grant.amount, error) &&
         OptionalInt(object, "capacity", 1, grant.capacity, error);
}

// Parses one "items" entry: a flat grant, or {"progressive": [grant, ...]}.
bool ParseItemEntry(const PortJson::Value& object, const std::string& key, int64_t itemId,
                    ItemEntry& entry, std::string& error) {
  const std::string label = "item " + key;
  if (!object.IsObject()) {
    error = label + " must be an object";
    return false;
  }
  const PortJson::Value* steps = Member(object, "progressive");
  if (steps == nullptr) {
    if (Member(object, "item") == nullptr) {
      error = label + " requires an item name string or a progressive list";
      return false;
    }
    return ParseGrant(object, label, itemId, entry, error);
  }
  if (Member(object, "item") != nullptr) {
    error = label + " cannot have both item and progressive";
    return false;
  }
  if (!steps->IsArray() || steps->AsArray().empty()) {
    error = label + " progressive must be a non-empty array of grants";
    return false;
  }
  const PortJson::Value::Elements& list = steps->AsArray();
  entry.progressive.reserve(list.size());
  for (size_t i = 0; i < list.size(); ++i) {
    ItemGrant step;
    if (!ParseGrant(list[i], label + " progressive step " + std::to_string(i + 1), itemId, step,
                    error))
      return false;
    entry.progressive.push_back(std::move(step));
  }
  static_cast<ItemGrant&>(entry) = entry.progressive.front();
  return true;
}

std::string ProcessUuid() {
  static const std::string uuid = [] {
    std::array<uint8_t, 16> bytes{};
    try {
      std::random_device random;
      for (uint8_t& byte : bytes)
        byte = static_cast<uint8_t>(random());
    } catch (...) {
      const uint64_t seed = static_cast<uint64_t>(
          std::chrono::high_resolution_clock::now().time_since_epoch().count());
      std::mt19937_64 random;
      random.seed(seed ^ reinterpret_cast<uintptr_t>(&bytes));
      for (uint8_t& byte : bytes)
        byte = static_cast<uint8_t>(random());
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (size_t i = 0; i < bytes.size(); ++i) {
      if (i == 4 || i == 6 || i == 8 || i == 10)
        result.push_back('-');
      result.push_back(kHex[bytes[i] >> 4]);
      result.push_back(kHex[bytes[i] & 0x0f]);
    }
    return result;
  }();
  return uuid;
}

void AppendNotification(std::vector<std::string>& notifications, std::string text) {
  constexpr size_t kNotificationLimit = 32;
  if (notifications.size() >= kNotificationLimit)
    notifications.erase(notifications.begin());
  notifications.push_back(std::move(text));
}

// How much of a session's receipts the tracker keeps. Comfortably more than a
// play session sends, and bounded so a long one cannot grow without limit.
constexpr size_t kMaxTrackedItems = 256;

// The name to show for an item: the display name of the step that copy grants,
// falling back to the item's own name when the config gave none.
std::string ItemDisplay(const ItemEntry& entry, int64_t count) {
  const ItemGrant& step = entry.Step(count);
  if (!step.display.empty())
    return step.display;
  if (!entry.display.empty())
    return entry.display;
  if (step.itemType >= 0) {
    const char* name = PortRandomizer::ItemName(step.itemType);
    if (name != nullptr)
      return name;
  }
  return "item";
}

// Reads the options that decide logic from a slot_data object, or from the
// copy of them the state file keeps under the same names.
void ParseLogicOptions(const PortJson::Value& data, PortApLogic::Options& logic) {
  logic = PortApLogic::Options();
  const auto number = [&data](const char* name, int64_t fallback) {
    int64_t value = fallback;
    const PortJson::Value* member = Member(data, name);
    if (member != nullptr && member->IsBool())
      value = member->AsBool() ? 1 : 0;
    else
      IntegerMember(data, name, value);
    return static_cast< int >(std::clamp< int64_t >(value, -1, 3));
  };
  const auto names = [&data](const char* name, std::vector< std::string >& out) {
    const PortJson::Value* member = Member(data, name);
    if (member == nullptr || !member->IsArray())
      return;
    for (const PortJson::Value& entry : member->AsArray()) {
      if (entry.IsString())
        out.push_back(entry.AsString());
    }
  };
  logic.trickDifficulty = number("trick_difficulty", -1);
  logic.combatLogic = number("combat_logic_difficulty", 0);
  logic.removeXray = number("remove_xray_requirements", 0);
  logic.removeThermal = number("remove_thermal_requirements", 0);
  logic.flaahgraPowerBombs = number("flaahgra_power_bombs", 0) > 0;
  logic.progressiveBeams = number("progressive_beam_upgrades", 0) > 0;
  logic.mainMissile = number("missile_launcher", 0) > 0;
  logic.mainPowerBomb = number("main_power_bomb", 0) > 0;
  logic.variaOnlyHeat = number("non_varia_heat_damage", 0) > 0;
  logic.preScanElevators = number("pre_scan_elevators", 0) > 0;
  names("trick_allow_list", logic.trickAllow);
  names("trick_deny_list", logic.trickDeny);
}

// The "logic" object of the state file, under the seed's slot_data names.
void WriteLogicOptions(port::JsonWriter& json, const PortApLogic::Options& logic) {
  const auto names = [&json](const char* name, const std::vector< std::string >& list) {
    json.Key(name).BeginArray();
    for (const std::string& entry : list)
      json.String(entry);
    json.EndArray();
  };
  json.BeginObject();
  json.Key("trick_difficulty").Number(logic.trickDifficulty);
  json.Key("combat_logic_difficulty").Number(logic.combatLogic);
  json.Key("remove_xray_requirements").Number(logic.removeXray);
  json.Key("remove_thermal_requirements").Number(logic.removeThermal);
  json.Key("flaahgra_power_bombs").Number(logic.flaahgraPowerBombs ? 1 : 0);
  json.Key("progressive_beam_upgrades").Number(logic.progressiveBeams ? 1 : 0);
  json.Key("missile_launcher").Number(logic.mainMissile ? 1 : 0);
  json.Key("main_power_bomb").Number(logic.mainPowerBomb ? 1 : 0);
  json.Key("non_varia_heat_damage").Number(logic.variaOnlyHeat ? 1 : 0);
  json.Key("pre_scan_elevators").Number(logic.preScanElevators ? 1 : 0);
  names("trick_allow_list", logic.trickAllow);
  names("trick_deny_list", logic.trickDeny);
  json.EndObject();
}

// Reads a Metroid Prime slot_data object. What it names as unsupported is
// something the AP ISO patches into the game and the port does not; a seed
// generated with it can expect a start the port does not have, so the player
// is told up front rather than stuck later.
void ParseSlotData(const PortJson::Value& data, SlotData& slot) {
  slot = SlotData();
  slot.received = true;
  const auto number = [&data](const char* name, int64_t fallback) {
    int64_t value = fallback;
    const PortJson::Value* member = Member(data, name);
    if (member != nullptr && member->IsBool())
      value = member->AsBool() ? 1 : 0;
    else
      IntegerMember(data, name, value);
    return value;
  };
  slot.requireMissileLauncher = number("missile_launcher", 0) > 0;
  slot.requireMainPowerBomb = number("main_power_bomb", 0) > 0;
  slot.requiredArtifacts = static_cast<int>(std::clamp<int64_t>(number("required_artifacts", 12), 0, 12));
  slot.progressiveBeamUpgrades = number("progressive_beam_upgrades", 0) > 0;
  slot.variaOnlyHeat = number("non_varia_heat_damage", 0) > 0;
  slot.preScanElevators = number("pre_scan_elevators", 0) > 0;

  slot.springBall = static_cast< int >(std::clamp< int64_t >(number("spring_ball", 0), 0, 3));
  const PortJson::Value* room = Member(data, "starting_room_name");
  if (room != nullptr && room->IsString()) {
    PortApWorld::Layout layout;
    PortApWorld::Place place;
    layout.startRoom = room->AsString();
    if (!PortApWorld::StartRoom(layout, place))
      slot.warnings.push_back("not supported: starting in " + room->AsString());
  }
}

} // namespace

const ItemGrant& ItemEntry::Step(int64_t count) const {
  if (progressive.empty())
    return *this;
  if (count <= 0)
    return progressive.front();
  if (static_cast<uint64_t>(count) >= progressive.size())
    return progressive.back();
  return progressive[static_cast<size_t>(count)];
}

Config ParseConfig(const std::string& text) {
  Config config;
  try {
    PortJson::Value root;
    size_t errorOffset = 0;
    const char* errorReason = nullptr;
    if (!PortJson::Parse(text, root, errorOffset, &errorReason)) {
      config.error = "invalid JSON at byte " + std::to_string(errorOffset) + ": " +
                     (errorReason != nullptr ? errorReason : "parse error");
      return config;
    }
    if (!root.IsObject()) {
      config.error = "configuration must be a JSON object";
      return config;
    }

    const PortJson::Value* server = Member(root, "server");
    if (server != nullptr && server->IsString())
      config.server = server->AsString();
    const PortJson::Value* slot = Member(root, "slot");
    if (slot != nullptr && slot->IsString())
      config.slot = slot->AsString();
    if (config.server.empty()) {
      config.error = "configuration requires a non-empty server string";
      return config;
    }
    if (config.slot.empty()) {
      config.error = "configuration requires a non-empty slot string";
      return config;
    }

    const PortJson::Value* value = Member(root, "enabled");
    if (value != nullptr && value->IsBool() && !value->AsBool()) {
      config.error = "disconnected (\"enabled\": false in the configuration)";
      return config;
    }
    value = Member(root, "password");
    if (value != nullptr) {
      if (!value->IsString()) {
        config.error = "password must be a string";
        return config;
      }
      config.password = value->AsString();
    }
    value = Member(root, "game");
    if (value != nullptr) {
      if (!value->IsString()) {
        config.error = "game must be a string";
        return config;
      }
      config.game = value->AsString();
    }
    value = Member(root, "tls_ca");
    if (value != nullptr) {
      if (!value->IsString()) {
        config.error = "tls_ca must be a string";
        return config;
      }
      config.tlsCa = value->AsString();
    }
    int64_t integer = 0;
    value = Member(root, "items_handling");
    if (value != nullptr) {
      if (!Integer(value, integer) || integer < std::numeric_limits<int>::min() ||
          integer > std::numeric_limits<int>::max()) {
        config.error = "items_handling must be an integer";
        return config;
      }
      config.itemsHandling = static_cast<int>(integer);
    }
    value = Member(root, "death_link");
    if (value != nullptr) {
      if (!value->IsBool()) {
        config.error = "death_link must be a boolean";
        return config;
      }
      config.deathLink = value->AsBool();
      config.deathLinkSet = true;
    }
    value = Member(root, "version");
    if (value != nullptr) {
      if (!value->IsObject()) {
        config.error = "version must be an object";
        return config;
      }
      const char* names[] = {"major", "minor", "build"};
      int* destinations[] = {&config.versionMajor, &config.versionMinor, &config.versionBuild};
      for (size_t i = 0; i < 3; ++i) {
        const PortJson::Value* component = Member(*value, names[i]);
        if (component == nullptr)
          continue;
        if (!Integer(component, integer) || integer < std::numeric_limits<int>::min() ||
            integer > std::numeric_limits<int>::max()) {
          config.error = std::string("version.") + names[i] + " must be an integer";
          return config;
        }
        *destinations[i] = static_cast<int>(integer);
      }
    }
    value = Member(root, "tags");
    if (value != nullptr) {
      if (!value->IsArray()) {
        config.error = "tags must be an array of strings";
        return config;
      }
      for (const PortJson::Value& tag : value->AsArray()) {
        if (!tag.IsString()) {
          config.error = "tags must be an array of strings";
          return config;
        }
        config.tags.push_back(tag.AsString());
      }
    }
    value = Member(root, "locations");
    if (value != nullptr) {
      if (!value->IsObject()) {
        config.error = "locations must be an object";
        return config;
      }
      for (const auto& entry : value->AsObject()) {
        if (!ValidLocationKey(entry.first)) {
          config.error = "invalid location key: " + entry.first;
          return config;
        }
        if (!Integer(&entry.second, integer)) {
          config.error = "location id for " + entry.first + " must be an integer";
          return config;
        }
        // The game formats its keys in upper case (FormatLocationKey).
        std::string key = entry.first;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        config.locations[key] = integer;
      }
    }
    value = Member(root, "items");
    if (value != nullptr) {
      if (!value->IsObject()) {
        config.error = "items must be an object";
        return config;
      }
      for (const auto& entry : value->AsObject()) {
        int64_t itemId = 0;
        if (!ParseItemKey(entry.first, itemId)) {
          config.error = "invalid decimal item id: " + entry.first;
          return config;
        }
        ItemEntry item;
        if (!ParseItemEntry(entry.second, entry.first, itemId, item, config.error))
          return config;
        config.items[itemId] = std::move(item);
      }
    }
    // No items given: a Metroid Prime slot uses the world's own.
    config.builtin = MetroidPrime::ApplyDefaults(config);
    config.valid = true;
    config.error.clear();
  } catch (const std::exception& error) {
    config.valid = false;
    config.error = std::string("configuration error: ") + error.what();
  } catch (...) {
    config.valid = false;
    config.error = "configuration error: unexpected failure";
  }
  return config;
}

Config LoadConfigFile(const std::string& path) {
  try {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
      Config config;
      config.error = "could not open configuration file: " + path;
      return config;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    if (file.bad()) {
      Config config;
      config.error = "could not read configuration file: " + path;
      return config;
    }
    return ParseConfig(contents.str());
  } catch (const std::exception& error) {
    Config config;
    config.error = "could not read configuration file " + path + ": " + error.what();
    return config;
  } catch (...) {
    Config config;
    config.error = "could not read configuration file: " + path;
    return config;
  }
}

State LoadStateFile(const std::string& path) {
  State state;
  try {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
      return state;
    std::ostringstream contents;
    contents << file.rdbuf();
    if (file.bad())
      return state;
    PortJson::Value root;
    size_t errorOffset = 0;
    const char* errorReason = nullptr;
    if (!PortJson::Parse(contents.str(), root, errorOffset, &errorReason) || !root.IsObject())
      return state;
    const PortJson::Value* slot = Member(root, "slot");
    if (slot != nullptr && slot->IsString())
      state.slot = slot->AsString();
    // Absent in files written before the seed was recorded: an unknown seed,
    // which the first RoomInfo adopts rather than treating as a mismatch.
    const PortJson::Value* seed = Member(root, "seed");
    if (seed != nullptr && seed->IsString())
      state.seed = seed->AsString();
    const PortJson::Value* room = Member(root, "room");
    if (room != nullptr && room->IsString())
      state.room = room->AsString();
    const PortJson::Value* itemIds = Member(root, "item_ids");
    state.canonicalIds = itemIds != nullptr && itemIds->IsString() && itemIds->AsString() == "canonical";
    int64_t number = 0;
    if (IntegerMember(root, "next_item_index", number) && number >= 0)
      state.nextItemIndex = number;
    const PortJson::Value* checks = Member(root, "checked_locations");
    if (checks != nullptr && checks->IsArray()) {
      for (const PortJson::Value& check : checks->AsArray()) {
        if (Integer(&check, number) &&
            std::find(state.checkedLocations.begin(), state.checkedLocations.end(), number) ==
                state.checkedLocations.end())
          state.checkedLocations.push_back(number);
      }
    }
    const PortJson::Value* progressive = Member(root, "progressive");
    if (progressive != nullptr && progressive->IsObject()) {
      for (const auto& entry : progressive->AsObject()) {
        int64_t itemId = 0;
        if (ParseItemKey(entry.first, itemId) && Integer(&entry.second, number) && number > 0)
          state.progressive[itemId] = number;
      }
    }
    const PortJson::Value* logic = Member(root, "logic");
    if (logic != nullptr && logic->IsObject()) {
      ParseLogicOptions(*logic, state.logic);
      state.hasLogic = true;
    }
    const PortJson::Value* world = Member(root, "world");
    if (world != nullptr && world->IsObject()) {
      PortApWorld::Parse(*world, state.world);
      state.hasWorld = true;
    }
  } catch (...) {
    return State();
  }
  return state;
}

bool SaveStateFile(const std::string& path, const State& state) {
  try {
    port::JsonWriter json;
    json.BeginObject();
    json.Key("slot").String(state.slot);
    json.Key("seed").String(state.seed);
    if (!state.room.empty())
      json.Key("room").String(state.room);
    json.Key("next_item_index").Number(state.nextItemIndex);
    if (state.canonicalIds)
      json.Key("item_ids").String("canonical");
    json.Key("checked_locations").BeginArray();
    for (const int64_t location : state.checkedLocations)
      json.Number(location);
    json.EndArray();
    json.Key("progressive").BeginObject();
    for (const auto& [item, count] : state.progressive)
      json.Key(std::to_string(item)).Number(count);
    json.EndObject();
    if (state.hasLogic) {
      json.Key("logic");
      WriteLogicOptions(json, state.logic);
    }
    if (state.hasWorld)
      json.Key("world").Raw(PortApWorld::Text(state.world));
    json.EndObject();
    return WriteFileAtomically(path, json.Take());
  } catch (...) {
    return false;
  }
}

Connection LoadConnectionFile(const std::string& path) {
  Connection connection;
  try {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
      return connection;
    std::ostringstream contents;
    contents << file.rdbuf();
    PortJson::Value root;
    size_t errorOffset = 0;
    const char* errorReason = nullptr;
    if (file.bad() || !PortJson::Parse(contents.str(), root, errorOffset, &errorReason) ||
        !root.IsObject())
      return connection;
    connection.server = root.StringOr("server");
    connection.slot = root.StringOr("slot");
    connection.password = root.StringOr("password");
    const PortJson::Value* enabled = Member(root, "enabled");
    connection.enabled = enabled == nullptr || !enabled->IsBool() || enabled->AsBool();
    connection.seed = root.StringOr("seed");
    connection.room = root.StringOr("room");
    const PortJson::Value* lastPlayed = Member(root, "last_played");
    if (lastPlayed != nullptr)
      connection.lastPlayed = lastPlayed->AsInt(0);
  } catch (...) {
    return Connection();
  }
  return connection;
}

bool SaveConnectionFile(const std::string& path, const Connection& connection,
                        std::string& error) {
  try {
    PortJson::Value::Members members;
    std::ifstream file(path, std::ios::binary);
    if (file.is_open()) {
      std::ostringstream contents;
      contents << file.rdbuf();
      file.close();
      if (contents.str().find_first_not_of(" \t\r\n") != std::string::npos) {
        PortJson::Value root;
        size_t errorOffset = 0;
        const char* errorReason = nullptr;
        // Anything else in the file (tables, DeathLink, a CA bundle) is the
        // player's; a file that cannot be read back is not overwritten blind.
        if (!PortJson::Parse(contents.str(), root, errorOffset, &errorReason) ||
            !root.IsObject()) {
          error = path + " is not a JSON object; fix or delete it first";
          return false;
        }
        members = root.AsObject();
      }
    }
    const auto set = [&members](const char* key, PortJson::Value value, bool keep) {
      auto found = std::find_if(members.begin(), members.end(),
                                [key](const auto& member) { return member.first == key; });
      if (!keep) {
        if (found != members.end())
          members.erase(found);
      } else if (found != members.end()) {
        found->second = std::move(value);
      } else {
        members.emplace_back(key, std::move(value));
      }
    };
    set("server", PortJson::Value::MakeString(connection.server), true);
    set("slot", PortJson::Value::MakeString(connection.slot), true);
    set("password", PortJson::Value::MakeString(connection.password),
        !connection.password.empty());
    set("enabled", PortJson::Value::MakeBool(false), !connection.enabled);
    set("seed", PortJson::Value::MakeString(connection.seed), !connection.seed.empty());
    set("room", PortJson::Value::MakeString(connection.room), !connection.room.empty());
    set("last_played", PortJson::Value::MakeNumber(static_cast<double>(connection.lastPlayed)),
        connection.lastPlayed != 0);
    port::JsonWriter json;
    json.Value(PortJson::Value::MakeObject(std::move(members)));
    const std::string text = json.Take() + '\n';
    if (!WriteFileAtomically(path, text)) {
      error = "could not write " + path;
      return false;
    }
    return true;
  } catch (const std::exception& exception) {
    error = std::string("could not write the configuration: ") + exception.what();
    return false;
  } catch (...) {
    error = "could not write the configuration";
    return false;
  }
}

std::string GameDirectoryName(const std::string& slot, const std::string& seed) {
  bool changed = false;
  const auto clean = [&changed](const std::string& text) {
    std::string out;
    for (const char c : text) {
      const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-';
      out.push_back(safe ? c : '_');
      changed = changed || !safe;
    }
    if (out.size() > 40) {
      out.resize(40);
      changed = true;
    }
    // "." and ".." are not names, and a leading dot hides the directory.
    if (out.empty() || out[0] == '.') {
      out.insert(out.begin(), '_');
      changed = true;
    }
    return out;
  };
  std::string name = clean(slot) + "-" + clean(seed);
  if (changed) {
    const uint32_t hash = port::Fnv1a32(slot + std::string(1, '\0') + seed); // over both, as they were
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "-%08x", hash);
    name += suffix;
  }
  return name;
}

Session::Session(const Config& config, const State& state) : mConfig(config), mState(state) {}

uint64_t Session::NextWorldRevision() {
  static std::atomic< uint64_t > next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

Session::DataPackageNames Session::ParseDataPackage(const PortJson::Value& packet) {
  DataPackageNames result;
  try {
    if (!packet.IsObject())
      return result;
    const PortJson::Value* data = Member(packet, "data");
    const PortJson::Value* games =
        data != nullptr && data->IsObject() ? Member(*data, "games") : nullptr;
    if (games == nullptr || !games->IsObject())
      return result;
    for (const auto& [game, package] : games->AsObject()) {
      if (!package.IsObject())
        continue;
      // Made even when the package has no tables: the game's names have been
      // asked for, and Connected does not ask again.
      GameNames& names = result[game];
      const auto invert = [&package](const char* field, std::map< int64_t, std::string >& out) {
        const PortJson::Value* table = Member(package, field);
        if (table == nullptr || !table->IsObject())
          return;
        for (const auto& [name, idValue] : table->AsObject()) {
          int64_t id = 0;
          if (Integer(&idValue, id))
            out[id] = name;
        }
      };
      invert("item_name_to_id", names.items);
      invert("location_name_to_id", names.locations);
    }
  } catch (...) {
    // A malformed package or an allocation failure keeps what was read so far.
  }
  return result;
}

void Session::MergeDataPackage(DataPackageNames&& names) {
  try {
    for (auto& [game, tables] : names) {
      const auto found = mGameNames.find(game);
      if (found == mGameNames.end()) {
        mGameNames.emplace(game, std::move(tables));
        continue;
      }
      // A later package's names win, as they did when read straight in.
      for (auto& [id, name] : tables.items)
        found->second.items[id] = std::move(name);
      for (auto& [id, name] : tables.locations)
        found->second.locations[id] = std::move(name);
    }
    // The official AP world numbers its items from 41 differently than the
    // built-in table, but the names agree, so the server's ids are translated
    // by name. Ids the table has no name for stay as they are.
    const auto own = mGameNames.find(mConfig.game);
    if (mConfig.builtin && own != mGameNames.end() && !own->second.items.empty()) {
      mItemIds.clear();
      for (const auto& [serverId, name] : own->second.items) {
        int64_t canonical = 0;
        if (MetroidPrime::CanonicalItemId(name, canonical) && canonical != serverId)
          mItemIds[serverId] = canonical;
      }
      if (!mItemIds.empty())
        Log("the server's Metroid Prime item ids differ from the built-in table (official AP world): " +
            std::to_string(mItemIds.size()) + " remapped by name");
    }
  } catch (...) {
    // An allocation failure keeps the names merged so far.
  }
}

int64_t Session::CanonicalItem(int64_t serverId) const {
  const auto found = mItemIds.find(serverId);
  return found != mItemIds.end() ? found->second : serverId;
}

bool Session::PlaysOwnGame(int64_t slot) const {
  if (mOwnSlot != 0 && slot == mOwnSlot)
    return true;
  const auto game = mSlotGames.find(slot);
  return game != mSlotGames.end() && game->second == mConfig.game;
}

void Session::MigrateItemIds() {
  // Without this game's names the server's numbering is unknown: the state
  // stays unmarked so a later connection can still recount it.
  if (mState.canonicalIds || !mConfig.builtin || mGameNames.find(mConfig.game) == mGameNames.end())
    return;
  mState.canonicalIds = true;
  // A state from before the ids were translated counted the server's own ids.
  // When those differ from the built-in ones the counts mean other items (the
  // official world's Missile Launcher, 41, would read as Unlimited Missiles),
  // so the replay recounts everything from the next full inventory. The loaded
  // game already holds what the state said was received, so nothing is granted
  // twice. A numbering that matches needs no recount.
  if (!mItemIds.empty() && (mState.nextItemIndex > 0 || !mState.progressive.empty())) {
    Log("the recorded item counts use the server's item ids, which differ from the built-in ones: recounting");
    RewindTo(mState.nextItemIndex);
  }
}

void Session::HandlePacket(const PortJson::Value& packet, std::vector<std::string>& outgoing,
                           std::vector<ItemGrant>& granted) try {
  if (!packet.IsObject())
    return;
  const PortJson::Value* commandValue = Member(packet, "cmd");
  if (commandValue == nullptr || !commandValue->IsString())
    return;
  const std::string& command = commandValue->AsString();
  if (command == "RoomInfo") {
    mHandshakeComplete = false;
    mSlotDescription.clear();
    mOwnSlot = 0;
    mSeedName.clear();
    const PortJson::Value* seedName = Member(packet, "seed_name");
    if (seedName != nullptr && seedName->IsString())
      mSeedName = seedName->AsString();
    // The recorded checks and item index are only true for the multiworld that
    // granted them. A different seed is a different session, so keeping them
    // would claim locations this slot never collected and skip the items the
    // server owes it. Discard them and say so; an empty recorded seed (a file
    // from before this was tracked) is adopted instead of treated as a mismatch.
    if (!mSeedName.empty() && mState.seed != mSeedName) {
      if (!mState.seed.empty()) {
        mState.nextItemIndex = 0;
        mState.checkedLocations.clear();
        mState.progressive.clear();
        mState.canonicalIds = true;
        mState.room.clear();
        mGrantFrom = 0;
        mResetReason = "recorded for seed \"" + mState.seed + "\", server has \"" + mSeedName + "\"";
      }
      mState.seed = mSeedName;
    }
    // The server sends ReceivedItems right behind Connected, ahead of any reply
    // to a request made from Connected, so the names that translate the ids
    // are asked for here. The client sends this ahead of the Connect packet.
    mInventoryReceived = false;
    mPackageAsked.clear();
    if (mConfig.builtin && mGameNames.find(mConfig.game) == mGameNames.end()) {
      port::JsonWriter request;
      request.BeginObject().Key("cmd").String("GetDataPackage").Key("games").BeginArray();
      request.String(mConfig.game).EndArray().EndObject();
      outgoing.push_back(request.Take());
      mPackageAsked.insert(mConfig.game);
    }
  } else if (command == "ConnectionRefused") {
    mHandshakeComplete = false;
    mSlotDescription.clear();
    mOwnSlot = 0;
    mLastError.clear();
    const PortJson::Value* errors = Member(packet, "errors");
    if (errors != nullptr && errors->IsArray()) {
      bool firstError = true;
      for (const PortJson::Value& error : errors->AsArray()) {
        if (!error.IsString())
          continue;
        if (!firstError)
          mLastError += ", ";
        mLastError += error.AsString();
        firstError = false;
      }
    }
  } else if (command == "Connected") {
    mHandshakeComplete = true;
    int64_t slot = 0;
    int64_t team = 0;
    IntegerMember(packet, "slot", slot);
    IntegerMember(packet, "team", team);
    mSlotDescription = "slot " + std::to_string(slot) + ", team " + std::to_string(team);
    mOwnSlot = slot;
    mTeam = team;
    mArtifactHints.clear();
    mPlayers.clear();
    const PortJson::Value* players = Member(packet, "players");
    if (players != nullptr && players->IsArray()) {
      for (const PortJson::Value& player : players->AsArray()) {
        if (!player.IsObject())
          continue;
        int64_t playerSlot = 0;
        if (!IntegerMember(player, "slot", playerSlot))
          continue;
        std::string name;
        const PortJson::Value* alias = Member(player, "alias");
        if (alias != nullptr && alias->IsString())
          name = alias->AsString();
        if (name.empty()) {
          const PortJson::Value* playerName = Member(player, "name");
          if (playerName != nullptr && playerName->IsString())
            name = playerName->AsString();
        }
        if (!name.empty())
          mPlayers[playerSlot] = std::move(name);
      }
    }
    // Item and location ids only mean something within a game, and the server
    // announces other players' finds by id, so fetch every game's names.
    mSlotGames.clear();
    mScouts.clear();
    const PortJson::Value* slotInfo = Member(packet, "slot_info");
    if (slotInfo != nullptr && slotInfo->IsObject()) {
      std::vector< std::string > games;
      for (const auto& [key, info] : slotInfo->AsObject()) {
        int64_t infoSlot = 0;
        const PortJson::Value* game = info.IsObject() ? Member(info, "game") : nullptr;
        if (!ParseItemKey(key, infoSlot) || game == nullptr || !game->IsString())
          continue;
        mSlotGames[infoSlot] = game->AsString();
        if (std::find(games.begin(), games.end(), game->AsString()) == games.end() &&
            mGameNames.find(game->AsString()) == mGameNames.end() &&
            mPackageAsked.find(game->AsString()) == mPackageAsked.end())
          games.push_back(game->AsString());
      }
      if (!games.empty()) {
        port::JsonWriter request;
        request.BeginObject().Key("cmd").String("GetDataPackage").Key("games").BeginArray();
        for (const std::string& name : games) {
          request.String(name);
          mPackageAsked.insert(name);
        }
        request.EndArray().EndObject();
        outgoing.push_back(request.Take());
      }
    }
    const PortJson::Value* checked = Member(packet, "checked_locations");
    if (checked != nullptr && checked->IsArray()) {
      for (const PortJson::Value& location : checked->AsArray()) {
        int64_t id = 0;
        if (Integer(&location, id) &&
            std::find(mState.checkedLocations.begin(), mState.checkedLocations.end(), id) ==
                mState.checkedLocations.end())
          mState.checkedLocations.push_back(id);
      }
    }
    // Only the built-in tables know what Metroid Prime's options mean; a
    // hand-written config says everything itself.
    const PortJson::Value* slotData = Member(packet, "slot_data");
    if (mConfig.builtin && slotData != nullptr && slotData->IsObject()) {
      ParseSlotData(*slotData, mSlotData);
      ParseLogicOptions(*slotData, mState.logic);
      mState.hasLogic = true;
      mWorldRevision = NextWorldRevision(); // first, in case Parse stops part way
      PortApWorld::Parse(*slotData, mState.world);
      mState.hasWorld = true;
      for (const std::string& warning : mSlotData.warnings)
        AppendNotification(mNotifications, "Seed option " + warning);
      // The seed's DeathLink option decides unless archipelago.json said
      // otherwise. The Connect already went out without the tag, so add it.
      const PortJson::Value* deathLink = Member(*slotData, "death_link");
      int64_t deathLinkValue = 0;
      const bool seedDeathLink = deathLink != nullptr &&
                                 ((deathLink->IsBool() && deathLink->AsBool()) ||
                                  (Integer(deathLink, deathLinkValue) && deathLinkValue != 0));
      if (seedDeathLink && !mConfig.deathLinkSet && !mConfig.deathLink) {
        mConfig.deathLink = true;
        port::JsonWriter update;
        update.BeginObject().Key("cmd").String("ConnectUpdate").Key("tags").BeginArray();
        for (const std::string& tag : mConfig.tags) {
          if (tag != "DeathLink")
            update.String(tag);
        }
        update.String("DeathLink").EndArray().EndObject();
        outgoing.push_back(update.Take());
      }
    }
    {
      std::string line = "connected as " + mSlotDescription + "; the server has " +
                         std::to_string(mState.checkedLocations.size()) + " location(s) checked";
      if (mConfig.builtin) {
        line += mSlotData.received ? "; seed options: missile_launcher=" +
                                         std::to_string(mSlotData.requireMissileLauncher ? 1 : 0) +
                                         " main_power_bomb=" +
                                         std::to_string(mSlotData.requireMainPowerBomb ? 1 : 0) +
                                         " required_artifacts=" + std::to_string(mSlotData.requiredArtifacts) +
                                         " spring_ball=" + std::to_string(mSlotData.springBall) +
                                         " death_link=" + std::to_string(mConfig.deathLink ? 1 : 0)
                                   : "; no slot_data, so the seed's options are the defaults";
      }
      Log(std::move(line));
      for (const std::string& warning : mSlotData.warnings)
        Log("seed option " + warning);
    }
    // The built-in tables take the retail item out of each pickup, so what the
    // pickup holds instead is asked for up front and named when collected.
    if (mConfig.builtin) {
      const std::vector< int64_t > ids = AllLocationIds();
      if (!ids.empty())
        outgoing.push_back(BuildLocationScouts(ids));
      // The totems name where each artifact is. A newer AP world puts that in
      // slot_data; the released one hints every artifact at the start, and
      // the server keeps this slot's hints in data storage.
      const PortJson::Value* artifacts =
          slotData != nullptr && slotData->IsObject() ? Member(*slotData, "artifact_locations") : nullptr;
      if (artifacts != nullptr && artifacts->IsObject()) {
        for (const auto& [name, where] : artifacts->AsObject()) {
          int64_t location = 0;
          int64_t player = 0;
          if (!where.IsArray() || where.Size() != 2 || !Integer(&where.AsArray()[0], location) ||
              !Integer(&where.AsArray()[1], player))
            continue;
          for (int64_t item = MetroidPrime::kArtifactTruth; item <= MetroidPrime::kArtifactNewborn; ++item) {
            const char* itemName = MetroidPrime::ItemName(MetroidPrime::kItemBase + item);
            if (itemName != nullptr && name == itemName)
              mArtifactHints[MetroidPrime::kItemBase + item] = HintedLocation{location, player};
          }
        }
      }
      const std::string hintsKey = "_read_hints_" + std::to_string(mTeam) + "_" + std::to_string(mOwnSlot);
      for (const char* command : {"Get", "SetNotify"}) {
        port::JsonWriter request;
        request.BeginObject().Key("cmd").String(command).Key("keys").BeginArray().String(hintsKey).EndArray();
        outgoing.push_back(request.EndObject().Take());
      }
    }
  } else if (command == "Retrieved" || command == "SetReply") {
    const std::string hintsKey = "_read_hints_" + std::to_string(mTeam) + "_" + std::to_string(mOwnSlot);
    if (command == "SetReply") {
      const PortJson::Value* key = Member(packet, "key");
      const PortJson::Value* value = Member(packet, "value");
      if (key != nullptr && key->IsString() && key->AsString() == hintsKey && value != nullptr)
        ReadHints(*value);
    } else {
      const PortJson::Value* keys = Member(packet, "keys");
      const PortJson::Value* value =
          keys != nullptr && keys->IsObject() ? Member(*keys, hintsKey.c_str()) : nullptr;
      if (value != nullptr)
        ReadHints(*value);
    }
  } else if (command == "DataPackage") {
    MergeDataPackage(ParseDataPackage(packet));
  } else if (command == "LocationInfo") {
    const PortJson::Value* locations = Member(packet, "locations");
    if (locations == nullptr || !locations->IsArray())
      return;
    for (const PortJson::Value& entry : locations->AsArray()) {
      int64_t item = 0;
      int64_t location = 0;
      int64_t player = 0;
      int64_t flags = 0;
      bool ok = false;
      if (entry.IsObject()) {
        ok = IntegerMember(entry, "item", item) && IntegerMember(entry, "location", location) &&
             IntegerMember(entry, "player", player);
        if (ok && !IntegerMember(entry, "flags", flags))
          flags = 0;
      } else if (entry.IsArray() && entry.Size() == 4) {
        ok = Integer(&entry.AsArray()[0], item) && Integer(&entry.AsArray()[1], location) &&
             Integer(&entry.AsArray()[2], player);
        if (ok && !Integer(&entry.AsArray()[3], flags))
          flags = 0;
      }
      if (ok) {
        if (PlaysOwnGame(player))
          item = CanonicalItem(item);
        mScouts[location] = ScoutedItem{item, player, flags};
      }
    }
  } else if (command == "ReceivedItems") {
    int64_t index = 0;
    if (!IntegerMember(packet, "index", index) || index < 0)
      return; // malformed; a negative index would re-grant received items
    const PortJson::Value* items = Member(packet, "items");
    const PortJson::Value::Elements empty;
    const auto& itemList = items != nullptr && items->IsArray() ? items->AsArray() : empty;
    MigrateItemIds();
    const int64_t expectedIndex = mState.nextItemIndex;
    Log("received " + std::to_string(itemList.size()) + " item(s) from #" + std::to_string(index) +
        " (next expected #" + std::to_string(expectedIndex) + ", the loaded game holds the first " +
        std::to_string(mGrantFrom) + ")");
    // Nothing processed yet: the server is starting a fresh inventory, so the
    // progressive counts start over and the replay rebuilds them.
    if (index == 0 && expectedIndex == 0) {
      mState.progressive.clear();
      mState.canonicalIds = true;
    }
    // Index 0 is the whole inventory (the reply to Connect or Sync), which
    // closes any gap; the loop below skips what was already processed.
    if (index == 0)
      mDesynced = false;
    if (index > 0 && index != expectedIndex) {
      Log("item #" + std::to_string(index) + " is not the next one (#" + std::to_string(expectedIndex) +
          "): asking the server to resend everything" +
          (index > expectedIndex ? "; these wait for its reply" : "; the ones already processed are skipped"));
      mDesynced = true;
      outgoing.push_back(BuildSync());
      // Items past a gap are left for the Sync reply. Taking them now would
      // move nextItemIndex past the missing ones, and the replay would then
      // skip those as already received.
      if (index > expectedIndex)
        return;
    }
    for (size_t position = 0; position < itemList.size(); ++position) {
      int64_t itemId = 0;
      const PortJson::Value& item = itemList[position];
      const PortJson::Value* itemValue = nullptr;
      if (item.IsObject()) {
        itemValue = Member(item, "item");
      } else if (item.IsArray() && item.Size() == 4) {
        itemValue = &item.AsArray()[0];
      }
      if (itemValue == nullptr || !Integer(itemValue, itemId))
        continue;
      itemId = CanonicalItem(itemId); // received items are always this game's
      if (position > static_cast<size_t>(std::numeric_limits<int64_t>::max()) ||
          index > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(position))
        continue;
      const int64_t receivedIndex = index + static_cast<int64_t>(position);
      if (receivedIndex < mState.nextItemIndex)
        continue; // processed already (a resend's overlap)
      // Below mGrantFrom the loaded save already holds the item: the replay
      // only rebuilds the progressive counts and the tracker.
      const bool alreadyHeld = receivedIndex < mGrantFrom;
      const auto found = mConfig.items.find(itemId);
      std::string notification;
      // Copies of this item already processed, which is what makes a
      // progressive grant a step rather than a repeat of the first one. Needed
      // by the tracker below, which is outside the entry branch.
      int64_t seenBefore = 0;
      if (found != mConfig.items.end()) {
        const ItemEntry& entry = found->second;
        int64_t count = 0;
        // The built-in tables count every item: ammo capacity depends on how
        // many expansions and main items arrived, not on this one alone.
        if (entry.IsProgressive() || mConfig.builtin) {
          int64_t& stored = mState.progressive[itemId];
          count = stored;
          if (stored < std::numeric_limits<int64_t>::max())
            ++stored;
        }
        ItemGrant grant = entry.Step(count);
        bool missiles = false;
        if (mConfig.builtin && MetroidPrime::IsAmmoItem(itemId, missiles)) {
          const bool requiresMain =
              missiles ? mSlotData.requireMissileLauncher : mSlotData.requireMainPowerBomb;
          const int after = MetroidPrime::AmmoCapacity(mState.progressive, missiles, requiresMain);
          int64_t& stored = mState.progressive[itemId];
          --stored;
          const int before = MetroidPrime::AmmoCapacity(mState.progressive, missiles, requiresMain);
          ++stored;
          grant.capacity = grant.amount = std::max(0, after - before);
        }
        grant.itemId = itemId;
        grant.index = receivedIndex;
        seenBefore = count;
        notification = grant.display;
        {
          std::string line = "item #" + std::to_string(receivedIndex) + " " + std::to_string(itemId) + " " +
                             grant.display;
          if (grant.itemType < 0)
            line += ": counted, nothing to grant";
          else
            line += ": game item " + std::to_string(grant.itemType) + ", capacity +" +
                    std::to_string(grant.capacity) + ", amount +" + std::to_string(grant.amount);
          if (alreadyHeld)
            line += " (the loaded game holds it already: counted, not granted)";
          Log(std::move(line));
        }
        if (!alreadyHeld)
          granted.push_back(std::move(grant));
      } else {
        ItemGrant grant;
        grant.itemId = itemId;
        grant.itemType = -1;
        grant.index = receivedIndex;
        if (!alreadyHeld)
          granted.push_back(grant);
        mLastError = "unknown item id " + std::to_string(itemId);
        Log("item #" + std::to_string(receivedIndex) + " " + std::to_string(itemId) +
            ": not an item of this game's table, nothing granted");
        notification = "unknown item " + std::to_string(itemId);
      }

      int64_t itemPlayer = 0;
      bool hasPlayer = false;
      if (item.IsObject()) {
        hasPlayer = IntegerMember(item, "player", itemPlayer);
      } else if (item.IsArray() && item.Size() == 4) {
        hasPlayer = Integer(&item.AsArray()[2], itemPlayer);
      }
      int64_t itemLocation = 0;
      if (item.IsObject())
        IntegerMember(item, "location", itemLocation);
      else if (item.IsArray() && item.Size() == 4)
        Integer(&item.AsArray()[1], itemLocation);
      if (hasPlayer && mOwnSlot != 0 && itemPlayer != mOwnSlot)
        notification += " from " + PlayerName(itemPlayer);
      if (!alreadyHeld && !(hasPlayer && AnnouncedLocally(itemLocation, itemPlayer)))
        AppendNotification(mNotifications, std::move(notification));
      // The tracker keeps the same receipt with its parts separated, so a
      // session's worth of items stays readable after the HUD line is gone.
      {
        TrackedItem tracked;
        tracked.itemId = itemId;
        const auto entryIt = mConfig.items.find(itemId);
        if (entryIt != mConfig.items.end()) {
          // The step's own display name, which is what the grant resolves to.
          // The entry's flat fields mirror step 0, so a progressive item needs
          // the step rather than the entry to be named correctly.
          tracked.name = ItemDisplay(entryIt->second, seenBefore);
          if (entryIt->second.IsProgressive()) {
            tracked.total = static_cast< int64_t >(entryIt->second.progressive.size());
            tracked.step = seenBefore + 1;
            if (tracked.step > tracked.total)
              tracked.step = tracked.total;
          }
        } else {
          tracked.name = "item " + std::to_string(itemId);
        }
        if (hasPlayer && mOwnSlot != 0 && itemPlayer != mOwnSlot)
          tracked.from = PlayerName(itemPlayer);
        // A long session can send a lot of items; keep the tail, which is what
        // a player is still looking at.
        mTracked.push_back(std::move(tracked));
        if (mTracked.size() > kMaxTrackedItems)
          mTracked.erase(mTracked.begin(), mTracked.end() - kMaxTrackedItems);
      }
    }
    int64_t endIndex = index;
    if (itemList.size() <= static_cast<size_t>(std::numeric_limits<int64_t>::max()) &&
        index <= std::numeric_limits<int64_t>::max() - static_cast<int64_t>(itemList.size()))
      endIndex = index + static_cast<int64_t>(itemList.size());
    mState.nextItemIndex = std::max(mState.nextItemIndex, endIndex);
    if (index == 0)
      mInventoryReceived = true;
  } else if (command == "PrintJSON") {
    std::string message;
    const PortJson::Value* data = Member(packet, "data");
    if (data != nullptr && data->IsArray()) {
      for (const PortJson::Value& part : data->AsArray()) {
        const PortJson::Value* text = Member(part, "text");
        if (text == nullptr || !text->IsString())
          continue;
        // Players, items and locations arrive as ids in `text`; the part's
        // type says which, and `player` whose game an item or location is.
        const std::string type = part.StringOr("type");
        int64_t id = 0;
        int64_t owner = 0;
        const bool isId = ParseItemKey(text->AsString(), id);
        const bool hasOwner = IntegerMember(part, "player", owner);
        if (isId && type == "player_id")
          message += PlayerName(id);
        else if (isId && hasOwner && type == "item_id")
          message += ItemName(PlaysOwnGame(owner) ? CanonicalItem(id) : id, owner);
        else if (isId && hasOwner && type == "location_id")
          message += LocationName(id, owner);
        else
          message += text->AsString();
      }
    }
    // An ItemSend for a find of this slot's that the HUD already named.
    const PortJson::Value* sent = Member(packet, "item");
    int64_t sentLocation = 0;
    int64_t sentFinder = 0;
    const bool repeat = Member(packet, "type") != nullptr && packet.StringOr("type") == "ItemSend" &&
                        sent != nullptr && sent->IsObject() &&
                        IntegerMember(*sent, "location", sentLocation) &&
                        IntegerMember(*sent, "player", sentFinder) &&
                        AnnouncedLocally(sentLocation, sentFinder);
    // The log keeps line breaks (command replies such as !help span lines) and
    // far more text than the HUD shows.
    constexpr size_t kChatLimit = 4000;
    constexpr size_t kChatLines = 256;
    if (mChat.size() >= kChatLines)
      mChat.erase(mChat.begin());
    std::string chatText = message.size() > kChatLimit ? message.substr(0, kChatLimit) + "..." : message;
    while (!chatText.empty() && (chatText.back() == '\n' || chatText.back() == '\r'))
      chatText.pop_back();
    mChat.push_back(ChatLine{packet.StringOr("type"), std::move(chatText)});
    std::string normalized;
    normalized.reserve(message.size());
    bool previousWasNewline = false;
    for (char c : message) {
      if (c == '\r' || c == '\n') {
        if (!previousWasNewline)
          normalized.push_back(' ');
        previousWasNewline = true;
      } else {
        normalized.push_back(c);
        previousWasNewline = false;
      }
    }
    constexpr size_t kMessageLimit = 200;
    if (normalized.size() > kMessageLimit) {
      normalized.resize(kMessageLimit);
      normalized += "...";
    }
    mLastMessage = std::move(normalized);
    if (!repeat)
      AppendNotification(mNotifications, mLastMessage);
  } else if (command == "InvalidPacket") {
    const PortJson::Value* text = Member(packet, "text");
    mLastError = text != nullptr && text->IsString() ? text->AsString() : std::string();
  } else if (command == "Bounced") {
    // DeathLink: someone else in the multiworld died. The server relays a
    // client's Bounce as Bounced to every client whose tags match, so a
    // DeathLink is a Bounced carrying the "DeathLink" tag; other bounces
    // (trackers, game-specific links) are none of the game's business. The
    // data has `time`, an optional `cause`, and `source`, the dead player's
    // name as that client sent it.
    //
    // The server echoes a bounce to its sender too, so one whose source is
    // this client's own slot name is a death it already announced, and acting
    // on it would kill the player a second time. A missing source is still
    // somebody else's death and still counts.
    if (!mConfig.deathLink)
      return;
    const PortJson::Value* tags = Member(packet, "tags");
    bool deathLink = false;
    if (tags != nullptr && tags->IsArray()) {
      for (const PortJson::Value& tag : tags->AsArray())
        deathLink = deathLink || (tag.IsString() && tag.AsString() == "DeathLink");
    }
    if (!deathLink)
      return;
    const PortJson::Value* data = Member(packet, "data");
    const PortJson::Value* source =
        data != nullptr && data->IsObject() ? Member(*data, "source") : nullptr;
    const PortJson::Value* cause =
        data != nullptr && data->IsObject() ? Member(*data, "cause") : nullptr;
    const std::string sourceName =
        source != nullptr && source->IsString() ? source->AsString() : std::string();
    if (!sourceName.empty() && sourceName == mConfig.slot)
      return; // our own death, coming back to us
    mLastDeathSource = sourceName;
    ++mDeathsReceived;
    // Counted even when the game is not running: a bounce that arrives at the
    // title screen must not be lost, or the next run would neither die nor show
    // it. Poll applies it and clears the counter.
    // The cause is another client's free text, so it is capped like PrintJSON.
    if (cause != nullptr && cause->IsString() && !cause->AsString().empty())
      AppendNotification(mNotifications, cause->AsString().substr(0, 200));
    else
      AppendNotification(mNotifications,
                         sourceName.empty() ? "Someone died" : sourceName + " died");
  }
  } catch (...) {
    // Malformed packets and allocation failures must not escape into the client.
  }

void Session::Log(std::string line) {
  // Far more than one packet writes; a session that nobody drains stays bounded.
  constexpr size_t kLogLimit = 512;
  if (mLog.size() >= kLogLimit)
    mLog.erase(mLog.begin());
  mLog.push_back(std::move(line));
}

bool Session::TakeLogLine(std::string& line) {
  try {
    if (mLog.empty())
      return false;
    line = std::move(mLog.front());
    mLog.erase(mLog.begin());
    return true;
  } catch (...) {
    return false;
  }
}

bool Session::TakeChatLine(ChatLine& line) {
  try {
    if (mChat.empty())
      return false;
    line = std::move(mChat.front());
    mChat.erase(mChat.begin());
    return true;
  } catch (...) {
    return false;
  }
}

bool Session::TakeNotification(std::string& text) {
  try {
    if (mNotifications.empty()) {
      text.clear();
      return false;
    }
    text = std::move(mNotifications.front());
    mNotifications.erase(mNotifications.begin());
    return true;
  } catch (...) {
    return false;
  }
}

std::string Session::PlayerName(int64_t slot) const {
  try {
    const auto player = mPlayers.find(slot);
    if (player != mPlayers.end() && !player->second.empty())
      return player->second;
    return "player " + std::to_string(slot);
  } catch (...) {
    return std::string();
  }
}

std::string Session::BuildConnect() const {
  port::JsonWriter json;
  json.BeginObject();
  json.Key("cmd").String("Connect");
  json.Key("password").String(mConfig.password);
  json.Key("game").String(mConfig.game);
  json.Key("name").String(mConfig.slot);
  json.Key("uuid").String(ProcessUuid());
  json.Key("version").BeginObject();
  json.Key("class").String("Version");
  json.Key("major").Number(mConfig.versionMajor);
  json.Key("minor").Number(mConfig.versionMinor);
  json.Key("build").Number(mConfig.versionBuild);
  json.EndObject();
  json.Key("items_handling").Number(mConfig.itemsHandling);
  json.Key("tags").BeginArray();
  for (const std::string& tag : mConfig.tags)
    json.String(tag);
  // The server routes DeathLink bounces by tag, so a client without it would
  // neither receive other players' deaths nor be expected to send its own.
  if (mConfig.deathLink &&
      std::find(mConfig.tags.begin(), mConfig.tags.end(), "DeathLink") == mConfig.tags.end())
    json.String("DeathLink");
  json.EndArray();
  // The built-in tables read the seed's options; a hand-written config has no
  // use for them.
  json.Key("slot_data").Bool(mConfig.builtin);
  json.EndObject();
  return json.Take();
}

namespace {

// {"cmd":<command>,"locations":[ids...]}, still open for more members.
void BeginLocationsPacket(port::JsonWriter& json, const char* command, const std::vector< int64_t >& ids) {
  json.BeginObject().Key("cmd").String(command).Key("locations").BeginArray();
  for (const int64_t id : ids)
    json.Number(id);
  json.EndArray();
}

} // namespace

std::string Session::BuildLocationChecks(const std::vector<int64_t>& ids) {
  port::JsonWriter json;
  BeginLocationsPacket(json, "LocationChecks", ids);
  return json.EndObject().Take();
}

std::string Session::BuildSync() { return "{\"cmd\":\"Sync\"}"; }

std::string Session::BuildSay(const std::string& text) {
  port::JsonWriter json;
  json.BeginObject().Key("cmd").String("Say").Key("text").String(text).EndObject();
  return json.Take();
}

std::string Session::BuildGoal() { return "{\"cmd\":\"StatusUpdate\",\"status\":30}"; }

std::string Session::BuildLocationScouts(const std::vector< int64_t >& ids, int createAsHint) {
  port::JsonWriter json;
  BeginLocationsPacket(json, "LocationScouts", ids);
  return json.Key("create_as_hint").Number(createAsHint).EndObject().Take();
}

std::string Session::ItemName(int64_t itemId, int64_t slot) const {
  try {
    const auto game = mSlotGames.find(slot);
    const std::string& gameName =
        game != mSlotGames.end() ? game->second : (slot == mOwnSlot ? mConfig.game : std::string());
    // This game's ids reach here already translated to the built-in numbering.
    if (gameName == mConfig.game && mConfig.builtin) {
      const char* name = MetroidPrime::ItemName(itemId);
      if (name != nullptr)
        return name;
    }
    const auto names = mGameNames.find(gameName);
    if (names != mGameNames.end()) {
      const auto name = names->second.items.find(itemId);
      if (name != names->second.items.end())
        return name->second;
    }
    if (gameName == mConfig.game) {
      const auto entry = mConfig.items.find(itemId);
      if (entry != mConfig.items.end())
        return ItemDisplay(entry->second, 0);
    }
    return "item " + std::to_string(itemId);
  } catch (...) {
    return std::string();
  }
}

std::string Session::LocationName(int64_t locationId, int64_t slot) const {
  try {
    const auto game = mSlotGames.find(slot);
    const std::string& gameName =
        game != mSlotGames.end() ? game->second : (slot == mOwnSlot ? mConfig.game : std::string());
    const auto names = mGameNames.find(gameName);
    if (names != mGameNames.end()) {
      const auto name = names->second.locations.find(locationId);
      if (name != names->second.locations.end())
        return name->second;
    }
    if (gameName == mConfig.game && mConfig.builtin) {
      const MetroidPrime::Location* location = MetroidPrime::FindLocation(locationId);
      if (location != nullptr)
        return location->name;
    }
    return "location " + std::to_string(locationId);
  } catch (...) {
    return std::string();
  }
}

std::string Session::LocationText(int64_t locationId) const {
  try {
    const auto scout = mScouts.find(locationId);
    if (scout == mScouts.end())
      return std::string();
    std::string text = "Found " + ItemName(scout->second.item, scout->second.player);
    if (mOwnSlot != 0 && scout->second.player != mOwnSlot)
      text += " for " + PlayerName(scout->second.player);
    return text;
  } catch (...) {
    return std::string();
  }
}

bool Session::AnnouncedLocally(int64_t locationId, int64_t finder) const {
  return mOwnSlot != 0 && finder == mOwnSlot && mAnnounced.count(locationId) != 0;
}

bool Session::ScoutedAt(int64_t locationId, int64_t& item, bool& sameGame,
                        int64_t* flags) const {
  const auto scout = mScouts.find(locationId);
  if (scout == mScouts.end())
    return false;
  item = scout->second.item;
  if (flags != nullptr)
    *flags = scout->second.flags;
  if (mOwnSlot != 0 && scout->second.player == mOwnSlot) {
    sameGame = true;
  } else {
    const auto game = mSlotGames.find(scout->second.player);
    sameGame = game != mSlotGames.end() && game->second == mConfig.game;
  }
  return true;
}

namespace {
// '&' starts the game's text markup; CTextParser reads "&&" as a plain '&'.
std::string GameText(std::string text) {
  for (size_t at = text.find('&'); at != std::string::npos; at = text.find('&', at + 2))
    text.insert(at, 1, '&');
  return text;
}
} // namespace

std::string Session::ScanText(int64_t locationId) const {
  try {
    const auto scout = mScouts.find(locationId);
    if (scout == mScouts.end())
      return std::string();
    const int64_t player = scout->second.player;
    std::string text = ItemName(scout->second.item, player) + "\nfor ";
    if (mOwnSlot == 0 || player == mOwnSlot) {
      text += "you";
    } else {
      text += PlayerName(player);
      const auto game = mSlotGames.find(player);
      if (game != mSlotGames.end() && !game->second.empty())
        text += " (" + game->second + ")";
    }
    return GameText(text);
  } catch (...) {
    return std::string();
  }
}

void Session::ReadHints(const PortJson::Value& hints) {
  if (!hints.IsArray())
    return;
  for (const PortJson::Value& hint : hints.AsArray()) {
    int64_t receiver = 0;
    int64_t finder = 0;
    int64_t location = 0;
    int64_t item = 0;
    if (!hint.IsObject() || !IntegerMember(hint, "receiving_player", receiver) ||
        !IntegerMember(hint, "finding_player", finder) || !IntegerMember(hint, "location", location) ||
        !IntegerMember(hint, "item", item))
      continue;
    if (PlaysOwnGame(receiver))
      item = CanonicalItem(item);
    if (receiver == mOwnSlot && item >= MetroidPrime::kItemBase + MetroidPrime::kArtifactTruth &&
        item <= MetroidPrime::kItemBase + MetroidPrime::kArtifactNewborn)
      mArtifactHints[item] = HintedLocation{location, finder};
  }
}

std::string Session::ArtifactHint(int64_t itemId) const {
  try {
    const char* name = MetroidPrime::ItemName(itemId);
    if (name == nullptr)
      return std::string();
    const std::string artifact = std::string("&push;&main-color=#c300ff;") + name + "&pop;";
    HintedLocation where;
    const auto hinted = mArtifactHints.find(itemId);
    if (hinted != mArtifactHints.end()) {
      where = hinted->second;
    } else {
      // This slot's own locations were all scouted, so an artifact in its own
      // world is known without a hint.
      for (const auto& [location, scout] : mScouts) {
        if (scout.item == itemId && scout.player == mOwnSlot && mOwnSlot != 0) {
          where = HintedLocation{location, mOwnSlot};
          break;
        }
      }
    }
    if (where.player == 0)
      return "The " + artifact + " has not been collected.";
    const std::string owner = where.player == mOwnSlot ? "your" : GameText(PlayerName(where.player)) + "'s";
    return "The " + artifact + " can be found in &push;&main-color=#d4cc33;" + owner +
           "&pop; &push;&main-color=#89a1ff;" + GameText(LocationName(where.location, where.player)) +
           "&pop;.";
  } catch (...) {
    return std::string();
  }
}

bool Session::TakeArtifactScanHint(int64_t itemId, int64_t& location) {
  if (mOwnSlot == 0)
    return false;
  int64_t found = 0;
  const auto hinted = mArtifactHints.find(itemId);
  if (hinted != mArtifactHints.end()) {
    if (hinted->second.player != mOwnSlot)
      return false;
    found = hinted->second.location;
  } else {
    for (const auto& [id, scout] : mScouts) {
      if (scout.item == itemId && scout.player == mOwnSlot) {
        found = id;
        break;
      }
    }
  }
  if (found == 0 || !mScanHinted.insert(found).second)
    return false;
  location = found;
  return true;
}

std::string Session::AnnounceLocation(int64_t locationId) {
  std::string text = LocationText(locationId);
  if (!text.empty())
    mAnnounced.insert(locationId);
  return text;
}

int64_t Session::ReceivedCount(int64_t itemId) const {
  const auto found = mState.progressive.find(itemId);
  return found != mState.progressive.end() ? found->second : 0;
}

std::string Session::BuildBounce(const std::string& cause) const {
  // A client that dies in DeathLink announces it with a Bounce aimed at the
  // "DeathLink" tag; the server relays it as Bounced to every client carrying
  // that tag, this one included. `time` is required (clients use it to spot
  // duplicates) and `source` is this slot's name, which is also how the echo
  // is recognised in HandlePacket. Sent only when the configuration asked for
  // DeathLink, so a normal session never emits one.
  if (!mConfig.deathLink)
    return std::string();
  // Unix seconds with millisecond precision, formatted by hand so the locale
  // cannot turn the decimal point into a comma.
  const int64_t millis = std::chrono::duration_cast< std::chrono::milliseconds >(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  char time[32];
  std::snprintf(time, sizeof(time), "%lld.%03lld", static_cast< long long >(millis / 1000),
                static_cast< long long >(millis % 1000));
  port::JsonWriter json;
  json.BeginObject().Key("cmd").String("Bounce").Key("tags").BeginArray().String("DeathLink").EndArray();
  json.Key("data").BeginObject();
  json.Key("time").Raw(time);
  json.Key("source").String(mConfig.slot);
  json.Key("cause").String(cause.empty() ? mConfig.slot + " died" : cause);
  json.EndObject().EndObject();
  return json.Take();
}

void Session::RewindTo(int64_t heldCount) {
  Log("rewinding: the loaded game holds the first " + std::to_string(std::max<int64_t>(0, heldCount)) +
      " item(s); the server's next resend grants the rest");
  mState.nextItemIndex = 0;
  mState.progressive.clear();
  mState.canonicalIds = true; // the replay counts in the built-in numbering
  mInventoryReceived = false;
  mTracked.clear();
  mGrantFrom = std::max<int64_t>(0, heldCount);
}

bool Session::MarkLocationChecked(const std::string& locationKey, int64_t& id) {
  const auto found = mConfig.locations.find(locationKey);
  if (found == mConfig.locations.end() ||
      std::find(mState.checkedLocations.begin(), mState.checkedLocations.end(), found->second) !=
          mState.checkedLocations.end())
    return false;
  id = found->second;
  mState.checkedLocations.push_back(id);
  return true;
}

bool Session::KnowsLocation(const std::string& locationKey) const {
  return mConfig.locations.find(locationKey) != mConfig.locations.end();
}

bool Session::LocationIdFor(const std::string& locationKey, int64_t& id) const {
  const auto found = mConfig.locations.find(locationKey);
  if (found == mConfig.locations.end())
    return false;
  id = found->second;
  return true;
}

bool Session::LocationChecked(int64_t id) const {
  return std::find(mState.checkedLocations.begin(), mState.checkedLocations.end(), id) !=
         mState.checkedLocations.end();
}

std::vector<int64_t> Session::AllLocationIds() const {
  std::vector<int64_t> ids;
  ids.reserve(mConfig.locations.size());
  for (const auto& location : mConfig.locations)
    ids.push_back(location.second);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

} // namespace Protocol
} // namespace PortAp
