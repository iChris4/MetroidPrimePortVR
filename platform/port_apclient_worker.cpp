// The Archipelago client's socket thread: the connection to the server (or the built-in
// solo server) and the loop that keeps the session in step with it. See
// port_apclient_internal.h.

#include "port_apclient_internal.h"
#include "port_json_writer.h"

namespace PortAp {
namespace ClientDetail {

// What the worker loop talks to: a WebSocket to a real server, or the
// in-process server of a built-in randomizer seed. The solo server has the
// WebSocket client's calls, so the loop below is the same for both.
struct Link {
  PortWs::Client ws;
  PortApSolo::Server solo;
  bool isSolo = false;

  bool IsOpen() const { return isSolo ? solo.IsOpen() : ws.IsOpen(); }
  bool SendText(const std::string& text) { return isSolo ? solo.SendText(text) : ws.SendText(text); }
  bool ReceiveText(std::string& text, int timeoutMs) {
    return isSolo ? solo.ReceiveText(text, timeoutMs) : ws.ReceiveText(text, timeoutMs);
  }
  const char* Error() const { return isSolo ? solo.Error() : ws.Error(); }
  void Close() {
    if (isSolo)
      solo.Close();
    else
      ws.Close();
  }
};

// Archipelago servers take each message as a JSON array of commands; a bare
// object makes MultiServer iterate its keys and drop the connection.
bool SendPacket(Link& client, const std::string& packet, std::string& error) {
  if (client.SendText(!packet.empty() && packet.front() == '[' ? packet : "[" + packet + "]"))
    return true;
  error = ErrorText(client.Error());
  return false;
}

// What the status and chat lines call the server.
std::string ServerLabel(const std::string& server) {
  std::string seedName;
  return PortApSolo::ParseServer(server, seedName) ? "solo seed " + seedName : server;
}

// The URLs to try for the configured server. A bare "host:port", as the
// Archipelago site shows it, is tried as ws:// and then wss://, the way the
// official client does, and gets Archipelago's default port when it has none.
// A solo seed ("solo:<name>") is no URL and is opened in process instead.
std::vector<std::string> ServerUrls(const std::string& server) {
  std::string seedName;
  if (server.find("://") != std::string::npos || PortApSolo::ParseServer(server, seedName))
    return {server};
  std::string address = server;
  const size_t close = address.rfind(']');
  const size_t colon = address.rfind(':');
  if (colon == std::string::npos || (close != std::string::npos && colon < close))
    address += ":38281";
  return {"ws://" + address, "wss://" + address};
}

// Records a game in its directory's game.json (for the recent-games list) and
// remembers its seed and room in archipelago.json, so the next launch picks its
// save card before the server answers.
void RecordGame(const Config& config, const std::string& seed, const std::string& room,
                const std::string& roomSuffix) {
  const std::filesystem::path dir = GameDirectory(config.slot, seed, roomSuffix);
  std::error_code ignored;
  std::filesystem::create_directories(dir, ignored);
  Protocol::Connection game;
  game.server = config.server;
  game.slot = config.slot;
  game.password = config.password;
  game.seed = seed;
  game.room = room;
  game.lastPlayed = static_cast<int64_t>(std::time(nullptr));
  std::string error;
  if (!Protocol::SaveConnectionFile((dir / "game.json").string(), game, error))
    PortLog::Write("archipelago: %s\n", error.c_str());
  std::lock_guard<std::mutex> lock(ConnectionFileMutex());
  Protocol::Connection saved = Protocol::LoadConnectionFile(ConfigPath());
  if (saved.server != config.server || saved.slot != config.slot ||
      (saved.seed == seed && saved.room == room))
    return; // changed by a Connect since this session started, or already known
  saved.seed = seed;
  saved.room = room;
  if (!Protocol::SaveConnectionFile(ConfigPath(), saved, error))
    PortLog::Write("archipelago: %s\n", error.c_str());
}

// The server named its seed. Each game keeps its progress in its own
// directory, so a seed other than the one loaded swaps that game's state in.
// The room is not known until after Connected (EnterRoom), so this goes by the
// one archipelago.json remembers for the seed. Runs on the socket thread before
// the session sees the RoomInfo.
void EnterSeed(Runtime& runtime, const std::string& seed) {
  if (seed.empty())
    return;
  Config config;
  std::string oldPath;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.session == nullptr)
      return;
    config = runtime.config;
    oldPath = runtime.statePath;
  }
  std::string suffix;
  {
    std::lock_guard<std::mutex> lock(ConnectionFileMutex());
    const Protocol::Connection saved = Protocol::LoadConnectionFile(ConfigPath());
    if (saved.seed == seed)
      suffix = RoomSuffix(config.slot, seed, saved.room);
  }
  const std::string path = GameStatePath(config.slot, seed, suffix);
  if (path == oldPath)
    return;
  runtime.FlushState(); // the old game's progress, to its own file
  Protocol::State state = Protocol::LoadStateFile(path);
  if (state.slot != config.slot || (!state.seed.empty() && state.seed != seed))
    state = Protocol::State();
  state.slot = config.slot;
  state.seed = seed;

  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (runtime.session == nullptr)
    return;
  const Protocol::State& current = runtime.session->GetState();
  if (current.seed.empty()) {
    // Checks made before the seed was known are this game's.
    for (const int64_t id : current.checkedLocations) {
      if (std::find(state.checkedLocations.begin(), state.checkedLocations.end(), id) ==
          state.checkedLocations.end())
        state.checkedLocations.push_back(id);
    }
  } else {
    // Another game's: its items and unsent checks do not carry over.
    PortLog::Write("archipelago: the server has seed \"%s\", not \"%s\"; switching games\n",
                   seed.c_str(), current.seed.c_str());
    runtime.grants.clear();
    runtime.queuedChecks.clear();
  }
  if (runtime.resetPending) {
    PortLog::Write("archipelago: MP_AP_RESET_STATE=1 discarded saved progress\n");
    state.nextItemIndex = 0;
    state.checkedLocations.clear();
    state.progressive.clear();
    runtime.resetPending = false;
  }
  runtime.session->SetState(state);
  runtime.statePath = path;
  runtime.roomSuffix = suffix;
  runtime.recordSynced = false;
  runtime.MarkStateDirtyLocked();
  // The directory's name holds the slot name, which the log leaves out.
  PortLog::Write("archipelago: game state for seed \"%s\" in archipelago_games\n", seed.c_str());
}

// The server's data storage key naming this slot's room (EnterRoom).
std::string RoomKey(const PortJson::Value& connected) {
  const PortJson::Value* team = connected.Find("team");
  const PortJson::Value* slot = connected.Find("slot");
  if (team == nullptr || slot == nullptr || !team->IsNumber() || !slot->IsNumber())
    return std::string();
  return "MetroidPrimePort_room_" + std::to_string(team->AsInt()) + "_" +
         std::to_string(slot->AsInt());
}

// Claims a name for the room: the first client in a room stores its random
// one, and every later connection reads that back in the SetReply.
std::string BuildRoomClaim(const std::string& key) {
  std::random_device device;
  std::mt19937_64 random((static_cast<uint64_t>(device()) << 32) ^ device() ^
                         static_cast<uint64_t>(std::time(nullptr)));
  char name[17];
  std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(random()));
  port::JsonWriter json;
  json.BeginArray().BeginObject();
  json.Key("cmd").String("Set").Key("key").String(key).Key("default").String(name);
  json.Key("want_reply").Bool(true).Key("operations").BeginArray().BeginObject();
  json.Key("operation").String("default").Key("value").Null();
  json.EndObject().EndArray().EndObject().EndArray();
  return json.Take();
}

// The server named the room (or could not; `room` is empty then). A new room
// generated from the same seed has the same seed name as the old one, so the
// seed alone would have this room resend the old room's checks and be given
// their items again. Each room keeps its progress in its own directory: the
// seed's own if it is the room's or unclaimed and not another room's (checks
// recorded there that this room's server has none of), else one suffixed with
// the room. Runs on the socket thread before the session sees the Connected.
void EnterRoom(Runtime& runtime, const std::string& room,
               const std::vector<int64_t>& serverChecked) {
  Config config;
  std::string seed;
  std::string oldPath;
  std::string oldSuffix;
  std::string oldRoom;
  Protocol::State base; // the seed's own directory's, as current as it is
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.session == nullptr)
      return;
    config = runtime.config;
    seed = runtime.session->GetState().seed;
    oldPath = runtime.statePath;
    oldSuffix = runtime.roomSuffix;
    oldRoom = runtime.session->GetState().room;
    if (oldSuffix.empty())
      base = runtime.session->GetState();
  }
  if (seed.empty())
    return;
  if (room.empty()) {
    // Not answered: stay in the directory the seed picked.
    RecordGame(config, seed, oldRoom, oldSuffix);
    return;
  }
  if (!oldSuffix.empty())
    base = Protocol::LoadStateFile(GameStatePath(config.slot, seed));
  bool own = base.room == room;
  if (base.room.empty()) {
    own = base.checkedLocations.empty();
    for (const int64_t id : base.checkedLocations) {
      if (std::find(serverChecked.begin(), serverChecked.end(), id) != serverChecked.end()) {
        own = true;
        break;
      }
    }
  }
  const std::string suffix = own ? std::string() : room;
  const std::string path = GameStatePath(config.slot, seed, suffix);
  if (path == oldPath) {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.session == nullptr)
      return;
    if (runtime.session->GetState().room != room) {
      runtime.session->SetRoom(room);
      runtime.MarkStateDirtyLocked();
    }
    runtime.roomSuffix = suffix;
  } else {
    runtime.FlushState(); // the old room's progress, to its own file
    Protocol::State state = Protocol::LoadStateFile(path);
    // The seed's own directory may hold a game from before rooms were named.
    if (state.slot != config.slot || state.seed != seed ||
        (!state.room.empty() && state.room != room))
      state = Protocol::State();
    state.slot = config.slot;
    state.seed = seed;
    state.room = room;
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.session == nullptr)
      return;
    // Another room's: its items and unsent checks do not carry over.
    PortLog::Write("archipelago: this is another room of seed \"%s\"; switching games\n",
                   seed.c_str());
    runtime.grants.clear();
    runtime.queuedChecks.clear();
    runtime.session->SetState(state);
    runtime.statePath = path;
    runtime.roomSuffix = suffix;
    runtime.recordSynced = false;
    runtime.MarkStateDirtyLocked();
  }
  RecordGame(config, seed, room, suffix);
}

void WorkerLoop(Runtime& runtime) {
  Config config;
  std::string statePath;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    config = runtime.config;
    statePath = runtime.statePath;
  }

  int backoff = 5;
  while (!runtime.stop.load(std::memory_order_acquire)) {
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      runtime.connected = false;
      runtime.stateLabel = "connecting";
      runtime.LogStateLocked("connecting to the server"); // the log names no server or slot
    }

    std::string host;
    std::string path;
    uint16_t port = 0;
    bool secure = false;
    std::string connectionError;
    Link client;
    client.ws.SetCancelFlag(&runtime.stop); // quitting must not wait out a timeout
    PortWs::TlsOptions tls;
    tls.caFile = config.tlsCa;
    bool transportReady = false;
    std::string soloSeed;
    if (PortApSolo::ParseServer(config.server, soloSeed)) {
      // A built-in randomizer seed: its file is read again on each attempt, so
      // one that was missing or broken can be fixed without restarting.
      client.isSolo = true;
      transportReady = client.solo.Open(soloSeed, connectionError);
      if (transportReady)
        PortLog::Write("archipelago: solo seed open\n");
    } else {
      for (const std::string& url : ServerUrls(config.server)) {
        if (!PortWs::ParseUrl(url, host, port, path, secure)) {
          connectionError = "invalid server URL: " + config.server;
        } else if (!client.ws.Connect(host, port, path, 10000, secure, tls)) {
          connectionError = ErrorText(client.Error());
        } else {
          transportReady = true;
          client.ws.SetTimeoutMs(10000);
          PortLog::Write("archipelago: WebSocket open (%s)\n",
                         client.ws.Compressed() ? "permessage-deflate" : "uncompressed");
          break;
        }
        if (runtime.stop.load(std::memory_order_acquire))
          break;
      }
    }

    bool connectionFailed = !transportReady;
    bool apConnected = false;
    // One command from the server; false once the connection has failed.
    const auto handleCommand = [&](const PortJson::Value& command) -> bool {
      const PortJson::Value* cmdValue = command.Find("cmd");
      const std::string cmd = cmdValue != nullptr && cmdValue->IsString()
                                  ? cmdValue->AsString()
                                  : std::string();
      std::vector<std::string> outgoing;
      std::vector<ItemGrant> newGrants;
      std::string connectPacket;
      std::vector<int64_t> initialChecks;
      std::vector<int64_t> sendAllChecks;
      bool connectedNow = false;
      bool refused = false;
      if (cmd == "RoomInfo") {
        const PortJson::Value* seedName = command.Find("seed_name");
        if (seedName != nullptr && seedName->IsString())
          EnterSeed(runtime, seedName->AsString());
      }
      // Every game's names can be a lot of work, and the game thread
      // takes the same lock every frame, so they are read in out here.
      const bool dataPackage = cmd == "DataPackage";
      Session::DataPackageNames packageNames;
      if (dataPackage)
        packageNames = Session::ParseDataPackage(command);
      {
        std::lock_guard<std::mutex> lock(runtime.mutex);
        if (runtime.session == nullptr)
          return true;
        const int64_t oldIndex = runtime.session->GetState().nextItemIndex;
        const std::vector<int64_t> oldChecks = runtime.session->GetState().checkedLocations;
        const std::map<int64_t, int64_t> oldProgressive =
            runtime.session->GetState().progressive;
        const std::string oldSeed = runtime.session->GetState().seed;
        const bool oldHasLogic = runtime.session->GetState().hasLogic;
        const PortApLogic::Options oldLogic = runtime.session->GetState().logic;
        const bool oldHasWorld = runtime.session->GetState().hasWorld;
        // The layout is the large part of the state, so it is not copied
        // to be compared: a new revision says it may have changed.
        const uint64_t oldWorldRevision = runtime.session->WorldRevision();
        const std::string oldError = runtime.session->LastError();
        const std::string oldResetReason = runtime.session->ResetReason();
        if (dataPackage)
          runtime.session->MergeDataPackage(std::move(packageNames));
        else
          runtime.session->HandlePacket(command, outgoing, newGrants);
        if (runtime.session->ResetReason() != oldResetReason) {
          // The recorded checks belonged to another session, so the state
          // file has just been rewritten empty. Say why, because the
          // symptom otherwise is a multiworld that sends nothing.
          PortLog::Write(
                       "archipelago: saved progress was %s; starting this slot fresh (set "
                       "MP_AP_RESET_STATE=1 to discard it deliberately)\n",
                       runtime.session->ResetReason().c_str());
        }
        const Protocol::State& state = runtime.session->GetState();
        if (state.nextItemIndex != oldIndex || state.checkedLocations != oldChecks ||
            state.progressive != oldProgressive || state.seed != oldSeed ||
            state.hasLogic != oldHasLogic || state.logic != oldLogic ||
            state.hasWorld != oldHasWorld ||
            runtime.session->WorldRevision() != oldWorldRevision)
          runtime.MarkStateDirtyLocked();
        runtime.grants.insert(runtime.grants.end(), newGrants.begin(), newGrants.end());
        runtime.lastMessage = runtime.session->LastMessage();
        for (Protocol::ChatLine line; runtime.session->TakeChatLine(line);)
          runtime.AppendChatLocked(std::move(line.type), std::move(line.text));
        runtime.DrainSessionLogLocked();
        const std::string& packetError = runtime.session->LastError();
        if (cmd != "ConnectionRefused" && !packetError.empty() && packetError != oldError &&
            packetError != runtime.lastError) {
          runtime.lastError = packetError;
          runtime.LogStateLocked(packetError);
        }
        if (cmd == "Connected")
          runtime.syncWanted = false; // the handshake replays everything anyway
        if (cmd == "RoomInfo") {
          connectPacket = runtime.session->BuildConnect();
        } else if (cmd == "Connected" && runtime.session->HandshakeComplete()) {
          connectedNow = true;
          runtime.connected = true;
          runtime.stateLabel = "connected";
          runtime.lastError.clear();
          runtime.LogStateLocked("connected");
          runtime.AppendChatLocked("port", "Connected to " + ServerLabel(config.server) + " as " + config.slot);
          initialChecks = state.checkedLocations;
          for (auto queued = runtime.queuedChecks.begin(); queued != runtime.queuedChecks.end();) {
            if (std::find(initialChecks.begin(), initialChecks.end(), *queued) !=
                initialChecks.end())
              queued = runtime.queuedChecks.erase(queued);
            else
              ++queued;
          }
          if (port::EnvFlag("MP_AP_SEND_ALL"))
            sendAllChecks = runtime.session->AllLocationIds();
          for (const std::string& warning : runtime.session->GetSlotData().warnings)
            PortLog::Write("archipelago: seed option %s; the game will not match the "
                           "seed's logic\n",
                           warning.c_str());
        } else if (cmd == "ConnectionRefused") {
          refused = true;
          runtime.connected = false;
          runtime.stateLabel = "error";
          runtime.lastError = runtime.session->LastError();
          if (runtime.lastError.empty())
            runtime.lastError = "connection refused";
          connectionError = runtime.lastError;
          runtime.LogStateLocked(runtime.lastError);
        } else if (runtime.session->HandshakeComplete() && !apConnected) {
          apConnected = true;
        }
      }

      // What the session queued goes first: RoomInfo asks for this game's
      // DataPackage, which has to arrive before Connected and its ReceivedItems
      // so the server's item ids can be read by name.
      for (const std::string& packet : outgoing) {
        if (!SendPacket(client, packet, connectionError)) {
          connectionFailed = true;
          return false;
        }
      }
      if (!connectPacket.empty() && !SendPacket(client, connectPacket, connectionError)) {
        connectionFailed = true;
        return false;
      }
      if (connectedNow) {
        apConnected = true;
        backoff = 5;
        if (!initialChecks.empty() &&
            !SendPacket(client, Session::BuildLocationChecks(initialChecks), connectionError)) {
          connectionFailed = true;
          return false;
        }
        if (!initialChecks.empty())
          CountChecks(runtime, initialChecks.size());
        if (!sendAllChecks.empty() &&
            !SendPacket(client, Session::BuildLocationChecks(sendAllChecks), connectionError)) {
          connectionFailed = true;
          return false;
        }
        if (!sendAllChecks.empty())
          CountChecks(runtime, sendAllChecks.size());
      }
      if (refused) {
        connectionFailed = true;
        return false;
      }
      return true;
    };
    // Connected and everything after it wait until the server names the room
    // (EnterRoom), so the session takes them into the room's own progress.
    bool roomAsked = false;
    bool holding = false;
    std::string roomKey;
    std::vector<PortJson::Value> held;
    std::chrono::steady_clock::time_point holdStart;
    const auto releaseHeld = [&](const std::string& room) -> bool {
      holding = false;
      std::vector<int64_t> serverChecked;
      if (const PortJson::Value* checked = held.front().Find("checked_locations");
          checked != nullptr && checked->IsArray()) {
        for (const PortJson::Value& id : checked->AsArray()) {
          if (id.IsNumber())
            serverChecked.push_back(id.AsInt());
        }
      }
      EnterRoom(runtime, room, serverChecked);
      std::vector<PortJson::Value> commands = std::move(held);
      held.clear();
      for (const PortJson::Value& command : commands) {
        if (!handleCommand(command))
          return false;
      }
      return true;
    };
    // One command from the server, held back while the room is asked for.
    const auto receiveCommand = [&](const PortJson::Value& command) -> bool {
      const PortJson::Value* cmdValue = command.Find("cmd");
      const std::string cmd =
          cmdValue != nullptr && cmdValue->IsString() ? cmdValue->AsString() : std::string();
      if (holding) {
        if (cmd == "SetReply" && command.StringOr("key") == roomKey) {
          const PortJson::Value* value = command.Find("value");
          std::string room;
          if (value != nullptr && value->IsString() && value->AsString().size() <= 64)
            room = value->AsString();
          if (room.empty())
            PortLog::Write("archipelago: the server gave no room name; keeping the seed's game\n");
          return releaseHeld(room);
        }
        if (cmd == "InvalidPacket" && command.StringOr("original_cmd") == "Set") {
          PortLog::Write("archipelago: the server refused the room name (%s); keeping the "
                         "seed's game\n",
                         command.StringOr("text").c_str());
          return releaseHeld(std::string());
        }
        held.push_back(command);
        return true;
      }
      if (cmd == "Connected" && !roomAsked) {
        roomAsked = true;
        roomKey = RoomKey(command);
        if (!roomKey.empty()) {
          if (!SendPacket(client, BuildRoomClaim(roomKey), connectionError)) {
            connectionFailed = true;
            return false;
          }
          holding = true;
          holdStart = std::chrono::steady_clock::now();
          held.push_back(command);
          return true;
        }
      }
      return handleCommand(command);
    };
    if (!transportReady) {
      if (!runtime.stop.load(std::memory_order_acquire)) // not a failure when cut short
        runtime.SetError(connectionError);
    } else {
      while (!runtime.stop.load(std::memory_order_acquire) && client.IsOpen()) {
        std::string message;
        // Checks, chat and the goal queued by the game go out after this
        // wait, so it is kept short: a check reaches the server within about
        // a tenth of a second. A frame cut by the timeout stays in the decoder.
        const bool received = client.ReceiveText(message, 100);
        if (!received && !client.IsOpen()) {
          connectionError = ErrorText(client.Error());
          connectionFailed = true;
          break;
        }
        if (!received && client.IsOpen() &&
            std::string(client.Error()) != "receive timed out") {
          connectionError = ErrorText(client.Error());
          connectionFailed = true;
          break;
        }

        if (received) {
          PortJson::Value root;
          size_t errorOffset = 0;
          const char* parseReason = nullptr;
          if (!PortJson::Parse(message, root, errorOffset, &parseReason)) {
            connectionError = "invalid server JSON at byte " + std::to_string(errorOffset) +
                              (parseReason != nullptr ? std::string(": ") + parseReason : "");
            connectionFailed = true;
            break;
          }

          std::vector<PortJson::Value> commands;
          if (root.IsArray())
            commands = root.AsArray();
          else
            commands.push_back(std::move(root));

          for (const PortJson::Value& command : commands) {
            if (!receiveCommand(command))
              break;
          }
          if (connectionFailed)
            break;
        }
        if (holding && std::chrono::steady_clock::now() - holdStart > std::chrono::seconds(10)) {
          PortLog::Write("archipelago: the server did not name the room; keeping the seed's game\n");
          if (!releaseHeld(std::string()))
            break;
        }

        std::vector<int64_t> pendingChecks;
        std::string pendingBounce;
        bool syncWanted = false;
        bool goalWanted = false;
        std::vector<std::string> pendingSay;
        std::vector<int64_t> pendingHints;
        if (apConnected && !holding) {
          std::lock_guard<std::mutex> lock(runtime.mutex);
          goalWanted = runtime.goalWanted && !runtime.goalSent;
          runtime.goalSent = runtime.goalSent || goalWanted;
          while (!runtime.queuedChecks.empty()) {
            pendingChecks.push_back(runtime.queuedChecks.front());
            runtime.queuedChecks.pop_front();
          }
          syncWanted = runtime.syncWanted;
          runtime.syncWanted = false;
          pendingSay.assign(runtime.pendingSay.begin(), runtime.pendingSay.end());
          runtime.pendingSay.clear();
          pendingHints.assign(runtime.pendingHintScouts.begin(), runtime.pendingHintScouts.end());
          runtime.pendingHintScouts.clear();
          // DeathLink: a death the game thread noticed, announced once. The
          // pending flag is *cleared* rather than moved-from, because this block
          // runs on every loop iteration and a moved-from-but-not-cleared
          // string is still non-empty, which announced the same death again on
          // every pass round the socket loop.
          if (!runtime.pendingBounce.empty()) {
            runtime.pendingBounce.clear();
            if (runtime.session != nullptr)
              pendingBounce = runtime.session->BuildBounce();
            if (pendingBounce.empty())
              runtime.deathAnnounced = false; // not in DeathLink; do not latch
          }
        }
        if (syncWanted && !SendPacket(client, Session::BuildSync(), connectionError)) {
          connectionFailed = true;
          break;
        }
        if (!pendingChecks.empty()) {
          if (!SendPacket(client, Session::BuildLocationChecks(pendingChecks), connectionError)) {
            connectionFailed = true;
            break;
          }
          CountChecks(runtime, pendingChecks.size());
        }
        if (!pendingBounce.empty()) {
          if (!SendPacket(client, pendingBounce, connectionError)) {
            connectionFailed = true;
            break;
          }
          PortLog::Write("archipelago: announced a death to the multiworld\n");
        }
        if (goalWanted) {
          if (!SendPacket(client, Session::BuildGoal(), connectionError)) {
            std::lock_guard<std::mutex> lock(runtime.mutex);
            runtime.goalSent = false; // owed again on the next connection
            connectionFailed = true;
            break;
          }
          PortLog::Write("archipelago: goal complete\n");
        }
        if (!pendingHints.empty() &&
            !SendPacket(client, Session::BuildLocationScouts(pendingHints, 2), connectionError)) {
          connectionFailed = true;
          break;
        }
        for (const std::string& text : pendingSay) {
          if (!SendPacket(client, Session::BuildSay(text), connectionError)) {
            connectionFailed = true;
            break;
          }
        }
        if (connectionFailed)
          break;
        runtime.FlushState();

        if (!received && !client.IsOpen()) {
          connectionError = ErrorText(client.Error());
          connectionFailed = true;
          break;
        }
      }
    }

    client.Close();
    runtime.FlushState();
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (runtime.connected && !runtime.stop.load(std::memory_order_acquire))
        runtime.AppendChatLocked("port", "Disconnected: " +
                                             (connectionError.empty() ? std::string("connection closed")
                                                                      : connectionError));
      runtime.connected = false;
    }
    if (runtime.stop.load(std::memory_order_acquire))
      break;
    if (connectionError.empty())
      connectionError = "connection closed";
    if (connectionFailed || !apConnected)
      runtime.SetError(connectionError);
    if (!runtime.WaitBackoff(backoff))
      break;
    backoff = std::min(backoff * 2, 60);
  }
}

// An exception escaping a std::thread terminates the game, and a broken or
// hostile server can cause one (bad_alloc on a huge message, a packet the
// session does not expect). It costs the connection instead, which retries.
void Worker(Runtime& runtime) {
  while (!runtime.stop.load(std::memory_order_acquire)) {
    try {
      WorkerLoop(runtime);
      break;
    } catch (const std::exception& error) {
      PortLog::Write("archipelago: client error: %s\n", error.what());
      runtime.SetError(std::string("internal error: ") + error.what());
    } catch (...) {
      PortLog::Write("archipelago: client error: unknown exception\n");
      runtime.SetError("internal error");
    }
    if (!runtime.WaitBackoff(30))
      break;
  }
  std::lock_guard<std::mutex> lock(runtime.mutex);
  runtime.workerDone = true;
  runtime.wake.notify_all();
}

}  // namespace ClientDetail
}  // namespace PortAp
