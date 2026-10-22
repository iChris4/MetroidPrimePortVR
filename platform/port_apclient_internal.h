// Internal to the Archipelago client (port_apclient.h): the client's state (Runtime) and
// the helpers both halves use, in PortAp::ClientDetail. port_apclient.cpp holds the
// game side and the public calls; port_apclient_worker.cpp holds the socket thread that
// talks to the server. Nothing outside those files should include this.

#pragma once

#include "port_env.h"
#include "port_hash.h"
#include "port_apclient.h"
#include "port_log.h"
#include "port_paths.h"

#include "port_ap_metroidprime.h"
#include "port_ap_protocol.h"
#include "port_ap_solo.h"
#include "port_custom_res.h"
#include "port_debug.h"
#include "port_rando_gen.h"
#include "port_randomizer.h"
#include "port_savestate.h"
#include "port_skip_cutscenes.h"
#include "port_strings.h"
#include "port_ws.h"

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <functional>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace PortAp {
namespace ClientDetail {

using Protocol::Config;
using Protocol::ItemGrant;
using Protocol::Session;

inline std::string UserDirectory() {
  const std::string& dir = PortPaths::UserFolder();
  return dir.empty() ? std::string(".") : dir;
}

inline std::string ConfigPath() {
  if (const char* env = port::EnvString("MP_AP_CONFIG")) {
    if (env[0] != '\0')
      return env;
  }
  return (std::filesystem::path(UserDirectory()) / "archipelago.json").string();
}

// archipelago.json and the per-game directories live here.
inline std::filesystem::path ConfigDirectory() {
  std::filesystem::path parent = std::filesystem::path(ConfigPath()).parent_path();
  return parent.empty() ? std::filesystem::path(".") : parent;
}

// One slot's game in one seed: its save card, state file and game.json. A room
// generated from a seed another room already plays has the same seed name, so
// it gets its own directory, the seed's with the room's `suffix` (EnterRoom).
inline std::filesystem::path GameDirectory(const std::string& slot, const std::string& seed,
                                    const std::string& suffix = std::string()) {
  return ConfigDirectory() / "archipelago_games" /
         Protocol::GameDirectoryName(slot, suffix.empty() ? seed : seed + "-" + suffix);
}

inline std::string GameStatePath(const std::string& slot, const std::string& seed,
                          const std::string& suffix = std::string()) {
  return (GameDirectory(slot, seed, suffix) / "archipelago_state.json").string();
}

// The directory suffix of `room` in `seed`: none if the seed's own directory
// belongs to it, or if the room is not known yet.
inline std::string RoomSuffix(const std::string& slot, const std::string& seed, const std::string& room) {
  if (room.empty() || Protocol::LoadStateFile(GameStatePath(slot, seed)).room == room)
    return std::string();
  return room;
}

// archipelago.json is written from the overlay (Connect, Disconnect) and by the
// socket thread (the seed), so the read-modify-writes take turns.
inline std::mutex& ConnectionFileMutex() {
  static std::mutex mutex;
  return mutex;
}

inline std::string ErrorText(const char* text) {
  return text != nullptr && text[0] != '\0' ? text : "connection failed";
}

struct Runtime {
  // Called once at exit. A worker blocked in a DNS lookup or a connect cannot
  // be cut short, so it gets a moment to notice `stop` and is otherwise left
  // behind (the Runtime is never freed) rather than holding up the exit.
  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      exiting = true; // a restart still in flight must not start a new worker
      // Set under the lock: WaitBackoff checks it under the lock, so a notify
      // sent between its check and its wait would otherwise be lost.
      stop.store(true, std::memory_order_release);
    }
    wake.notify_all();
    // A restart in flight holds the old worker itself and joins it, so it is
    // waited for too; with `exiting` set it starts nothing new.
    std::thread current;
    std::vector<std::thread> restarts;
    bool done;
    {
      std::unique_lock<std::mutex> lock(mutex);
      done = wake.wait_for(lock, std::chrono::seconds(2), [this] {
        return restartsRunning == 0 && (!worker.joinable() || workerDone);
      });
      current = std::move(worker);
      restarts = std::move(restarters);
    }
    for (std::thread& thread : restarts) {
      if (done)
        thread.join(); // finished, or returning from the lambda
      else
        thread.detach();
    }
    if (current.joinable()) {
      if (done)
        current.join();
      else
        current.detach();
    }
    FlushState(); // whatever the worker had not written yet
  }

  void LogStateLocked(const std::string& message) {
    if (lastLogged != message) {
      PortLog::Write( "archipelago: %s\n", message.c_str());
      lastLogged = message;
    }
  }

  // The session's account of what it did (Session::TakeLogLine), into the log.
  void DrainSessionLogLocked() {
    if (session == nullptr)
      return;
    for (std::string line; session->TakeLogLine(line);)
      PortLog::Write("archipelago: %s\n", line.c_str());
  }

  void SetError(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex);
    connected = false;
    stateLabel = "error";
    lastError = error;
    LogStateLocked(error);
  }

  bool WaitBackoff(int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::unique_lock<std::mutex> lock(mutex);
    while (!stop.load(std::memory_order_acquire)) {
      if (!wake.wait_until(lock, deadline, [this] {
            return stop.load(std::memory_order_acquire) || stateDirty;
          }))
        break; // the backoff ran out
      if (stateDirty) {
        lock.unlock();
        FlushState(); // checks collected while offline are still recorded
        lock.lock();
      }
    }
    return !stop.load(std::memory_order_acquire);
  }

  // The state file is written by the socket thread (or at shutdown, once it
  // has stopped), never by the game thread: a pickup must not wait on disk.
  void MarkStateDirtyLocked() {
    stateDirty = true;
    wake.notify_all();
  }

  void AppendChatLocked(std::string type, std::string text) {
    constexpr size_t kChatLines = 500;
    if (chat.size() >= kChatLines)
      chat.pop_front();
    chat.push_back(ChatLine{std::move(type), std::move(text)});
    ++chatSerial;
  }

  // Writes the state file if it has changed. Only one thread calls this at a
  // time, so the snapshot taken under the lock is written in order.
  void FlushState() {
    std::lock_guard<std::mutex> flushing(flushMutex); // a restart and the exit may overlap
    Protocol::State snapshot;
    std::string path;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!stateDirty || session == nullptr)
        return;
      stateDirty = false;
      // No seed yet, so no game to write it for. The save records the checks,
      // and the state is written once the server names the seed.
      if (statePath.empty())
        return;
      snapshot = session->GetState();
      path = statePath;
    }
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    const bool written = Protocol::SaveStateFile(path, snapshot);
    if (!written && !stateWriteFailed) // said once, not on every pickup
      PortLog::Write("archipelago: could not write %s\n", path.c_str());
    stateWriteFailed = !written;
  }

  std::mutex mutex;
  std::condition_variable wake;
  std::atomic<bool> stop{false};
  std::thread worker;
  bool workerDone = false; // under mutex; set as the worker returns
  bool attempted = false;
  bool exiting = false; // under mutex; Shutdown() has begun
  std::mutex flushMutex;
  // Held for the whole of a restart (Connect/Disconnect), so two in a row run
  // one after the other.
  std::mutex restartMutex;
  // The restart threads (under mutex), kept so Shutdown() can join them, and
  // how many have not returned yet. Finished ones are joined by the next
  // StartRestart.
  std::vector<std::thread> restarters;
  int restartsRunning = 0;
  // The last logic evaluation and what it was made from; it only changes with
  // an item or the seed's options, and the map asks every frame.
  bool logicValid = false;
  PortApLogic::Options logicOptions;
  bool logicHasWorld = false;
  uint64_t logicWorldRevision = 0; // Session::WorldRevision
  // The seed's layout as last copied out of the session, handed out shared
  // (SessionWorldLocked) and copied again only when the session's world
  // revision moves on.
  std::shared_ptr<const PortApWorld::Layout> worldCache;
  uint64_t worldCacheRevision = 0;
  PortApLogic::Items logicItems;
  std::vector<PortApLogic::Level> logicLevels;
  bool enabled = false;
  bool connected = false;
  std::string stateLabel = "off";
  std::string lastError;
  std::string lastLogged;
  std::string lastMessage;
  // The seed's own state file, in its game directory; empty until the seed is
  // known (remembered in archipelago.json, or named by the server).
  std::string statePath;
  // The game directory's room suffix (GameDirectory); empty for the seed's own.
  std::string roomSuffix;
  // MP_AP_RESET_STATE=1 at launch, waiting for the seed's state to be loaded.
  bool resetPending = false;
  Config config;
  std::unique_ptr<Session> session;
  std::deque<ItemGrant> grants;
  std::deque<int64_t> queuedChecks;
  // DeathLink: a death noticed on the game thread, waiting for the socket
  // thread to announce it. Non-empty means one is owed.
  std::string pendingBounce;
  // A loaded game rewound the session, so the socket thread owes the server a
  // Sync to get the full inventory replayed.
  bool syncWanted = false;
  // The session state has changed since the state file was last written.
  bool stateDirty = false;
  bool stateWriteFailed = false; // only touched by the thread that flushes
  std::deque<std::string> notifications;
  // The overlay's chat log, kept across reconnects, and Say texts waiting for
  // the socket thread.
  std::deque<ChatLine> chat;
  uint64_t chatSerial = 0;
  std::deque<std::string> pendingSay;
  // Own-world artifact locations the player scanned a totem for, to be
  // scouted as server hints by the socket thread.
  std::deque<int64_t> pendingHintScouts;
  int itemCount = 0;
  int checkCount = 0;
  // Locations collected in game that the table has no id for. Counted and
  // reported once, because dropping them is otherwise invisible.
  int unmappedCount = 0;
  // DeathLink: whether this client's death has already been announced, so a
  // death is sent once rather than on every tick the flag stays clear for.
  bool deathAnnounced = false;
  // The game reached the end-of-game world: the goal is owed to the server
  // (`goalWanted`) until the socket thread has sent it once (`goalSent`).
  bool goalWanted = false;
  bool goalSent = false;
  // The checks the running game records have been handed to this session.
  bool recordSynced = false;
  // SwitchNotice(): set when a game of another seed or room was sent back to
  // the title.
  std::string switchNotice;
};

void CountChecks(Runtime& runtime, size_t count);

// The socket thread (port_apclient_worker.cpp).
std::vector<std::string> ServerUrls(const std::string& server);
void Worker(Runtime& runtime);

}  // namespace ClientDetail
}  // namespace PortAp
