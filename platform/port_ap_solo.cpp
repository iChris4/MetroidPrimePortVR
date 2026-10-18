#include "port_strings.h"
#include "port_ap_solo.h"

#include "port_ap_metroidprime.h"
#include "port_json.h"
#include "port_json_writer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace PortApSolo {
namespace {

constexpr int64_t kSlot = 1;
constexpr int64_t kGoalComplete = 30; // ClientStatus.CLIENT_GOAL
constexpr int64_t kStartLocation = -2; // what Archipelago gives a starting item

void IdList(port::JsonWriter& json, const char* key, const std::vector< int64_t >& ids) {
  json.Key(key).BeginArray();
  for (const int64_t id : ids)
    json.Number(id);
  json.EndArray();
}

// One line of server text, the way MultiServer words its PrintJSON. A line
// about the player names its team and slot, and a chat line also carries
// what was said.
std::string PrintJson(const char* type, const std::string& text, bool aboutPlayer = false,
                      const std::string* message = nullptr) {
  port::JsonWriter json;
  json.BeginObject().Key("cmd").String("PrintJSON").Key("type").String(type);
  if (aboutPlayer)
    json.Key("team").Number(0).Key("slot").Number(kSlot);
  if (message != nullptr)
    json.Key("message").String(*message);
  json.Key("data").BeginArray().BeginObject().Key("type").String("text").Key("text").String(text).EndObject();
  json.EndArray().EndObject();
  return json.Take();
}

// A version object as the server sends it.
void Version(port::JsonWriter& json, const char* key) {
  json.Key(key).BeginObject();
  json.Key("major").Number(0).Key("minor").Number(6).Key("build").Number(0).Key("class").String("Version");
  json.EndObject();
}

std::vector< int64_t > Ids(const PortJson::Value* list) {
  std::vector< int64_t > ids;
  if (list == nullptr || !list->IsArray())
    return ids;
  for (const PortJson::Value& id : list->AsArray()) {
    if (id.IsNumber())
      ids.push_back(id.AsInt());
  }
  return ids;
}

bool WriteAtomic(const std::string& path, const std::string& text) {
  std::error_code ec;
  const std::filesystem::path target(path);
  std::filesystem::create_directories(target.parent_path(), ec);
  const std::string temp = path + ".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast< std::streamsize >(text.size()));
    file.flush();
    if (!file) {
      std::filesystem::remove(temp, ec);
      return false;
    }
  }
  std::filesystem::rename(temp, target, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    return false;
  }
  return true;
}

} // namespace

bool ParseServer(const std::string& server, std::string& seedName) {
  const std::string prefix = kServerPrefix;
  if (server.compare(0, prefix.size(), prefix) != 0 || server.size() == prefix.size())
    return false;
  seedName = server.substr(prefix.size());
  return true;
}

std::string StatePath(const std::string& seedName) {
  // SeedPath adds ".json" after making the name safe for a file name.
  std::string path = PortRandoGen::SeedPath(seedName);
  path.resize(path.size() - 5);
  return path + ".state.json";
}

bool Server::Open(const std::string& seedName, std::string& error) {
  PortRandoGen::Seed seed;
  if (!PortRandoGen::Load(PortRandoGen::SeedPath(seedName), seed, error))
    return false;
  return OpenSeed(seed, StatePath(seedName), error);
}

bool Server::OpenSeed(const PortRandoGen::Seed& seed, const std::string& statePath, std::string& error) {
  PortJson::Value slotData;
  size_t offset = 0;
  const char* reason = nullptr;
  if (!PortJson::Parse(seed.slotData, slotData, offset, &reason) || !slotData.IsObject()) {
    error = "seed " + seed.name + ": its slot data is not a JSON object";
    return false;
  }
  std::lock_guard< std::mutex > lock(mMutex);
  mSeed = seed;
  mStatePath = statePath;
  mChecked.clear();
  mReceived.clear();
  mOutgoing.clear();
  mError.clear();
  for (const int64_t item : mSeed.startItems)
    mReceived.push_back({item, kStartLocation});
  // The checks made before, in the order they were made. A location the seed
  // does not have (the seed was regenerated under the same name) is dropped.
  std::string text;
  if (!mStatePath.empty()) {
    std::ifstream file(mStatePath, std::ios::binary);
    text = port::ReadAllText(file);
  }
  PortJson::Value state;
  if (!text.empty() && PortJson::Parse(text, state, offset, &reason) && state.IsObject()) {
    for (const int64_t location : Ids(state.Find("checked")))
      Check(location);
  }
  mOpen = true;
  // The first thing a server says; the client's Connect answers it.
  port::JsonWriter json;
  json.BeginObject().Key("cmd").String("RoomInfo");
  Version(json, "version");
  Version(json, "generator_version");
  json.Key("tags").BeginArray().EndArray();
  json.Key("password").Bool(false);
  json.Key("permissions").BeginObject();
  json.Key("release").Number(0).Key("collect").Number(0).Key("remaining").Number(0);
  json.EndObject();
  json.Key("hint_cost").Number(0);
  json.Key("location_check_points").Number(1);
  json.Key("games").BeginArray().String("Metroid Prime").EndArray();
  json.Key("datapackage_checksums").BeginObject().EndObject();
  json.Key("seed_name").String("solo-" + mSeed.name);
  json.Key("time").Number(0);
  Queue(json.EndObject().Take());
  return true;
}

bool Server::IsOpen() const {
  std::lock_guard< std::mutex > lock(mMutex);
  return mOpen;
}

void Server::Close() {
  {
    std::lock_guard< std::mutex > lock(mMutex);
    mOpen = false;
  }
  mWake.notify_all();
}

bool Server::ReceiveText(std::string& message, int timeoutMs) {
  std::unique_lock< std::mutex > lock(mMutex);
  mWake.wait_for(lock, std::chrono::milliseconds(std::max(0, timeoutMs)),
                 [this] { return !mOutgoing.empty() || !mOpen; });
  if (!mOutgoing.empty()) {
    message = std::move(mOutgoing.front());
    mOutgoing.pop_front();
    return true;
  }
  mError = mOpen ? "receive timed out" : "connection closed";
  return false;
}

std::vector< int64_t > Server::CheckedLocations() const {
  std::lock_guard< std::mutex > lock(mMutex);
  return mChecked;
}

bool Server::SendText(const std::string& message) {
  PortJson::Value root;
  size_t offset = 0;
  const char* reason = nullptr;
  std::unique_lock< std::mutex > lock(mMutex);
  if (!mOpen) {
    mError = "connection closed";
    return false;
  }
  if (!PortJson::Parse(message, root, offset, &reason)) {
    Queue(R"({"cmd":"InvalidPacket","type":"cmd","original_cmd":null,"text":"not JSON"})");
  } else {
    std::vector< PortJson::Value > commands;
    if (root.IsArray())
      commands = root.AsArray();
    else
      commands.push_back(std::move(root));
    for (const PortJson::Value& command : commands)
      Handle(command);
  }
  lock.unlock();
  mWake.notify_all();
  return true;
}

void Server::Queue(const std::string& packet) {
  mOutgoing.push_back("[" + packet + "]");
}

// Archipelago flags: 1 progression, 2 useful, 4 trap. Expansions and tanks are
// the only filler a seed holds; everything else is something a seed's logic
// can need.
int64_t Server::Flags(int64_t item) const {
  namespace Mp = PortAp::MetroidPrime;
  const int64_t offset = item - Mp::kItemBase;
  if (offset == Mp::kMissileExpansion || offset == Mp::kPowerBombExpansion || offset == Mp::kEnergyTank)
    return 0;
  return 1;
}

void Server::WriteItem(port::JsonWriter& json, const Received& received) const {
  json.BeginObject();
  json.Key("item").Number(received.item);
  json.Key("location").Number(received.location);
  json.Key("player").Number(kSlot);
  json.Key("flags").Number(Flags(received.item));
  json.EndObject();
}

std::string Server::ReceivedItemsJson(size_t from) const {
  port::JsonWriter json;
  json.BeginObject().Key("cmd").String("ReceivedItems").Key("index").Number(from).Key("items").BeginArray();
  for (size_t i = from; i < mReceived.size(); ++i)
    WriteItem(json, mReceived[i]);
  return json.EndArray().EndObject().Take();
}

bool Server::Check(int64_t location) {
  const auto placed = mSeed.placements.find(location);
  if (placed == mSeed.placements.end() ||
      std::find(mChecked.begin(), mChecked.end(), location) != mChecked.end())
    return false;
  mChecked.push_back(location);
  mReceived.push_back({placed->second, location});
  return true;
}

void Server::SaveState() {
  if (mStatePath.empty())
    return;
  // Items are numbered in check order, so a lost state file renumbers them
  // on the next launch: say so rather than lose it quietly.
  port::JsonWriter json;
  json.BeginObject();
  IdList(json, "checked", mChecked);
  if (!WriteAtomic(mStatePath, json.EndObject().Take() + "\n"))
    Queue(PrintJson("Tutorial", "Could not save the seed's progress to " + mStatePath));
}

void Server::Handle(const PortJson::Value& packet) {
  const std::string command = packet.StringOr("cmd");
  if (command == "Connect") {
    if (packet.StringOr("name") != kSlotName) {
      Queue(R"({"cmd":"ConnectionRefused","errors":["InvalidSlot"]})");
      return;
    }
    if (packet.StringOr("game") != "Metroid Prime") {
      Queue(R"({"cmd":"ConnectionRefused","errors":["InvalidGame"]})");
      return;
    }
    std::vector< int64_t > missing;
    for (const auto& [location, item] : mSeed.placements) {
      if (std::find(mChecked.begin(), mChecked.end(), location) == mChecked.end())
        missing.push_back(location);
    }
    // No slot_info: it only exists to have the client fetch other games' names.
    port::JsonWriter json;
    json.BeginObject().Key("cmd").String("Connected").Key("team").Number(0).Key("slot").Number(kSlot);
    json.Key("players").BeginArray().BeginObject();
    json.Key("team").Number(0).Key("slot").Number(kSlot).Key("alias").String(kSlotName).Key("name").String(kSlotName);
    json.EndObject().EndArray();
    IdList(json, "missing_locations", missing);
    IdList(json, "checked_locations", mChecked);
    json.Key("slot_data").Raw(mSeed.slotData); // JSON text already, as generated or loaded
    Queue(json.EndObject().Take());
    Queue(ReceivedItemsJson(0));
  } else if (command == "LocationChecks") {
    const size_t before = mReceived.size();
    for (const int64_t location : Ids(packet.Find("locations")))
      Check(location);
    // The client repeats its checks on every connection; those add nothing.
    if (mReceived.size() != before) {
      SaveState();
      Queue(ReceivedItemsJson(before));
    }
  } else if (command == "Sync") {
    Queue(ReceivedItemsJson(0));
  } else if (command == "LocationScouts") {
    port::JsonWriter json;
    json.BeginObject().Key("cmd").String("LocationInfo").Key("locations").BeginArray();
    for (const int64_t location : Ids(packet.Find("locations"))) {
      const auto placed = mSeed.placements.find(location);
      if (placed != mSeed.placements.end())
        WriteItem(json, {placed->second, location});
    }
    Queue(json.EndArray().EndObject().Take());
  } else if (command == "StatusUpdate") {
    const PortJson::Value* status = packet.Find("status");
    if (status != nullptr && status->AsInt() == kGoalComplete) {
      Queue(PrintJson("Goal", std::string(kSlotName) + " has completed their goal.", true));
    }
  } else if (command == "Say") {
    const std::string text = packet.StringOr("text");
    if (!text.empty() && text[0] == '!') {
      Queue(PrintJson("CommandResult", "Commands are not available in a solo game."));
    } else {
      Queue(PrintJson("Chat", std::string(kSlotName) + ": " + text, true, &text));
    }
  } else if (command == "Get" || command == "SetNotify") {
    // Nothing is stored in a solo game; a key asked for is just empty.
    port::JsonWriter json;
    json.BeginObject().Key("cmd").String("Retrieved").Key("keys").BeginObject();
    const PortJson::Value* keys = packet.Find("keys");
    if (keys != nullptr && keys->IsArray()) {
      for (const PortJson::Value& key : keys->AsArray()) {
        if (key.IsString())
          json.Key(key.AsString()).Null();
      }
    }
    Queue(json.EndObject().EndObject().Take());
  } else if (command == "Set") {
    // Only the client's room name is ever set (PortAp's RoomKey). A seed file
    // is one room for good, so the name is fixed rather than stored.
    const PortJson::Value* wantReply = packet.Find("want_reply");
    if (wantReply != nullptr && wantReply->IsBool() && wantReply->AsBool()) {
      port::JsonWriter json;
      json.BeginObject().Key("cmd").String("SetReply").Key("key").String(packet.StringOr("key"));
      json.Key("value").String("solo").Key("original_value").String("solo").Key("slot").Number(kSlot);
      Queue(json.EndObject().Take());
    }
  } else if (command == "GetDataPackage") {
    // The client has the built-in names for Metroid Prime already.
    Queue(R"({"cmd":"DataPackage","data":{"games":{}}})");
  }
  // Bounce, ConnectUpdate and the rest have nobody to be relayed to.
}

} // namespace PortApSolo
