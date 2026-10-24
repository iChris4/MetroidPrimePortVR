#ifndef METROID_PRIME_PORT_PORT_AP_PROTOCOL_H
#define METROID_PRIME_PORT_PORT_AP_PROTOCOL_H
#include "port_ap_logic.h"
#include "port_ap_world.h"
#include "port_json.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// Protocol core of the Archipelago client: the configuration and its item and
// location maps, the packet state machine, and the queues of outgoing packets
// and incoming items. It deliberately knows nothing about sockets or game
// state, so the tests drive it directly. port_apclient_worker.cpp owns the socket
// thread, and port_apclient.cpp the step that turns ItemGrants into player-state
// changes.
namespace PortAp {
namespace Protocol {

// One item the server told us we received, resolved to the port's item model.
struct ItemGrant {
  int64_t itemId = 0;
  int itemType = -1; // CPlayerState::EItemType value; -1 when the id is unknown
  int amount = 1;
  int capacity = 1;
  std::string display; // name shown to the player; the item name when unset
  int64_t index = 0;   // position in the slot's received-items list
};

// One item as the tracker shows it: the name a player reads, where it came
// from, and which step of a progressive sequence it was. A flat item is step 1.
struct TrackedItem {
  int64_t itemId = 0;
  std::string name;
  std::string from;  // player alias, empty when the item is the player's own
  int64_t step = 1;  // 1-based; the last step repeats for later copies
  int64_t total = 1; // steps in the sequence, 1 for a flat item
};

// One PrintJSON message for the chat log. `type` is the packet's type (Chat,
// ServerChat, ItemSend, Hint, CommandResult, Join, ...), "" when it had none.
struct ChatLine {
  std::string type;
  std::string text;
};

// A configured item: one grant, or a progressive sequence where the Nth copy
// received grants step N and later copies repeat the last step. The inherited
// fields are the flat grant, and mirror step 0 for a progressive item.
struct ItemEntry : ItemGrant {
  std::vector< ItemGrant > progressive; // empty for a flat item

  bool IsProgressive() const { return !progressive.empty(); }
  // The grant for a copy received after `count` earlier copies of this id.
  const ItemGrant& Step(int64_t count) const;
};

// archipelago.json, as documented in docs/ARCHIPELAGO.md.
struct Config {
  std::string server; // ws:// or wss://host[:port][/path]
  std::string slot;
  std::string password;
  std::string game = "Metroid Prime";
  // PEM CA bundle for wss:// servers the system trust store does not cover.
  // Empty means the system store. Stored as written; a relative path is
  // resolved against the config file's directory by the client.
  std::string tlsCa;
  int itemsHandling = 7;
  // DeathLink: when another player dies, this one dies too. Off unless the
  // configuration asks for it, so a session that never opted in is unaffected.
  bool deathLink = false;
  // The file said either way, which wins over the slot's own death_link option.
  bool deathLinkSet = false;
  std::vector< std::string > tags;
  int versionMajor = 0;
  int versionMinor = 6;
  int versionBuild = 8;
  std::map< std::string, int64_t > locations; // randomizer key -> AP location id
  std::map< int64_t, ItemEntry > items;       // AP item id -> grant
  // The tables are the built-in Metroid Prime ones (port_ap_metroidprime.h),
  // so the session applies that world's rules: every item is counted, ammo is
  // granted as capacity deltas, and slot_data is read.
  bool builtin = false;
  bool valid = false;
  std::string error; // why the configuration was rejected, for the log
};

// Parses configuration text. Never throws; `valid` reports whether a server and
// a slot were found. Unknown keys are ignored so the file can grow.
Config ParseConfig(const std::string& text);
// Reads and parses a file; a missing or unreadable file yields an invalid
// configuration whose `error` says so.
Config LoadConfigFile(const std::string& path);

// The part of archipelago.json the overlay's Connect screen edits.
struct Connection {
  std::string server;
  std::string slot;
  std::string password;
  // False once the player disconnects: the file keeps the details for next
  // time, but the client does not start from it ("enabled": false).
  bool enabled = true;
  // The seed the server had when this slot last connected ("seed"), so the
  // game's own save card is known before the server answers. Empty if unknown.
  std::string seed;
  // The room this slot last played that seed in ("room"; see State::room), so
  // two rooms generated from one seed keep their own save cards. Empty if
  // unknown.
  std::string room;
  // Unix time this game was last connected ("last_played"), in the per-game
  // copies of the file; 0 when absent.
  int64_t lastPlayed = 0;
};
// Reads those fields; absent ones stay empty. Never throws.
Connection LoadConnectionFile(const std::string& path);
// Writes them into the file and keeps every other key (tables, DeathLink, a
// CA bundle). A file that exists but is not a JSON object is left alone and
// the reason is put in `error`. Never throws.
bool SaveConnectionFile(const std::string& path, const Connection& connection,
                        std::string& error);
// The directory name of one slot's game in one seed: "<slot>-<seed>", with
// anything but letters, digits, '.', '_' and '-' replaced, each part cut to 40
// bytes, and a hash of the originals appended when that changed them, so two
// games never share a name.
std::string GameDirectoryName(const std::string& slot, const std::string& seed);

// archipelago_state.json: what the client must remember between sessions so a
// reconnect does not hand the player the same items twice.
struct State {
  std::string slot;
  // Seed name this progress belongs to, learned from the server's RoomInfo. A
  // different seed means a different session, so the progress is discarded
  // rather than replayed into it. Empty in files written before this existed.
  std::string seed;
  // The room this progress belongs to. Rooms generated from one seed share its
  // seed name, so the client names each room in the server's data storage the
  // first time it connects to it (port_apclient_worker.cpp, EnterRoom). Empty until
  // then, and in files written before this existed.
  std::string room;
  int64_t nextItemIndex = 0;
  std::vector< int64_t > checkedLocations;
  // Item id -> copies processed so far: progressive ids, or every id with the
  // built-in tables (ammo capacity follows the counts). Persisted because items
  // below nextItemIndex are skipped on reconnect, so the counts cannot be
  // rebuilt from what the server resends. Absent in older files: all zero.
  std::map< int64_t, int64_t > progressive;
  // Whether the item ids in `progressive` are the built-in table's own numbering
  // ("item_ids":"canonical" in the file). Files from before the session
  // translated the server's ids hold the server's raw ids, which differ from
  // the built-in ones on the official AP world; see Session::MigrateItemIds.
  bool canonicalIds = false;
  // The seed's options that decide which checks are in logic ("logic", under
  // their slot_data names), kept so the tracker also works before the server
  // answers. hasLogic is false until a Metroid Prime slot has sent them.
  bool hasLogic = false;
  PortApLogic::Options logic;
  // The seed's layout ("world", under its slot_data names): start room and
  // elevators. Kept for the same reason, and because a new game is set up
  // before the server has said anything.
  bool hasWorld = false;
  PortApWorld::Layout world;
};
State LoadStateFile(const std::string& path);
// Writes the state through a temporary file and renames it into place.
bool SaveStateFile(const std::string& path, const State& state);

// What the port reads from the slot_data of a Metroid Prime slot.
struct SlotData {
  bool received = false;
  // The seed makes missiles or power bombs unusable until their main item.
  bool requireMissileLauncher = false;
  bool requireMainPowerBomb = false;
  int requiredArtifacts = 12;
  bool progressiveBeamUpgrades = false;
  // Only the Varia Suit keeps out heat (retail: any suit past the Power Suit).
  bool variaOnlyHeat = false;
  // spring_ball: 0 none, 1 with the Morph Ball Bombs, 2 its own item (Spring
  // Ball), 3 the first Progressive Bomb.
  int springBall = 0;
  // pre_scan_elevators: elevator holograms start scanned, so every elevator
  // works on arrival (randomprime's autoEnabledElevators).
  bool preScanElevators = false;
  // Options the port does not implement, one line each, for the log and HUD.
  std::vector< std::string > warnings;
};

// One connection's protocol state.
class Session {
public:
  Session(const Config& config, const State& state);

  // Handles one server packet (a JSON object carrying a "cmd"). Outgoing
  // packets are appended to `outgoing`, and items to grant to `granted`.
  void HandlePacket(const PortJson::Value& packet, std::vector< std::string >& outgoing,
                    std::vector< ItemGrant >& granted);

  // A DataPackage's id -> name tables, per game. Building them is the slow
  // part of a DataPackage (every name of every game) and needs nothing from
  // the session, so the client builds them outside its lock and then merges
  // them in; HandlePacket does both for a DataPackage. Neither throws.
  struct GameNames {
    std::map< int64_t, std::string > items;
    std::map< int64_t, std::string > locations;
  };
  using DataPackageNames = std::map< std::string, GameNames >;
  static DataPackageNames ParseDataPackage(const PortJson::Value& packet);
  void MergeDataPackage(DataPackageNames&& names);
  // A server item id of this game's as the built-in table numbers it.
  int64_t CanonicalItem(int64_t serverId) const;
  // Whether `slot` plays this session's game, whose item ids CanonicalItem maps.
  bool PlaysOwnGame(int64_t slot) const;
  // Takes a loaded state without the canonical-ids marker over to the built-in
  // numbering once the server's numbering is known; see the definition.
  void MigrateItemIds();

  // Human-readable notifications for the HUD and overlay, oldest first: item
  // receipts ("Energy Tank from Bob") and PrintJSON text. Drained by whoever
  // displays them, so nothing is shown twice. The queue is capped.
  bool TakeNotification(std::string& text);
  // Every PrintJSON message in full, repeats included, for the chat log. Drained
  // like the notifications; the queue is capped.
  bool TakeChatLine(ChatLine& line);
  // What the session did with each packet that a bug report needs: the handshake
  // and the seed's options, every item received (its index, what it grants, or
  // why it grants nothing), gaps and resends, and rewinds. Plain sentences, no
  // prefix, oldest first; drained by the client into the log. The queue is capped.
  bool TakeLogLine(std::string& line);
  // Seed name from RoomInfo, or "" before it arrives.
  const std::string& SeedName() const { return mSeedName; }
  // Alias of a player slot from Connected, or "player <slot>" when unknown.
  std::string PlayerName(int64_t slot) const;

  // The Connect packet to send after RoomInfo.
  std::string BuildConnect() const;
  static std::string BuildLocationChecks(const std::vector< int64_t >& ids);
  static std::string BuildSync();
  // StatusUpdate 30: this slot has reached its goal.
  static std::string BuildGoal();
  // Say: chat text, or a server command such as "!hint Missile Launcher".
  static std::string BuildSay(const std::string& text);
  // LocationScouts for `ids`. createAsHint is the AP value: 0 creates no hints,
  // 2 announces only the hints that are new.
  static std::string BuildLocationScouts(const std::vector< int64_t >& ids, int createAsHint = 0);

  // Name of an item id in the game of the slot that receives it: the server's
  // DataPackage, else the configured or built-in names for this game, else
  // "item <id>". Location names work the same way.
  std::string ItemName(int64_t itemId, int64_t slot) const;
  std::string LocationName(int64_t locationId, int64_t slot) const;
  // HUD text for collecting one of this slot's locations, from the
  // LocationScouts reply: "Found X" or "Found X for Bob". Empty before the
  // reply, or for a location it did not cover.
  std::string LocationText(int64_t locationId) const;
  // LocationText for the game to show as the player collects the location.
  // The server's own announcement of that find is then left off the HUD.
  std::string AnnounceLocation(int64_t locationId);
  // The item at one of this slot's locations, from the LocationScouts reply,
  // and whether a slot playing this game receives it (this one or another
  // Metroid Prime slot), so the id is one of this game's. False before the
  // reply, or for a location it did not cover. flags, when given, gets the
  // item's classification bits (1 progression, 2 useful, 4 trap).
  bool ScoutedAt(int64_t locationId, int64_t& item, bool& sameGame,
                 int64_t* flags = nullptr) const;
  // Scan text for the pickup at one of this slot's locations: the item and
  // whose world it is for ("Hookshot\nfor Bob (A Link to the Past)", or
  // "Energy Tank\nfor you"). Empty before the LocationScouts reply.
  std::string ScanText(int64_t locationId) const;
  // Artifact Temple totem text for an artifact this slot receives (item id
  // with the base), in the AP world's words: where it is when a hint or a
  // scout says, else that it has not been collected.
  std::string ArtifactHint(int64_t itemId) const;
  // The player scanned this artifact's totem. Gives true once per location,
  // with the location to scout as a server hint, when the artifact is in this
  // slot's own world (LocationScouts only covers a slot's own locations; a
  // hint for another world's location is the AP world's to make).
  bool TakeArtifactScanHint(int64_t itemId, int64_t& location);

  // slot_data from Connected, for built-in tables; defaults before it arrives.
  const SlotData& GetSlotData() const { return mSlotData; }
  // Copies of an item id processed so far. Built-in tables count every id;
  // otherwise only progressive ids are counted.
  int64_t ReceivedCount(int64_t itemId) const;

  // Everything received this session, oldest first, for the item tracker. The
  // notification queue is the same information but capped and drained by
  // whoever displays it, so a player who misses a HUD line loses it for good;
  // this is the whole session and is bounded by its own cap.
  const std::vector< TrackedItem >& Tracked() const { return mTracked; }

  // DeathLink. `DeathsPending` is a count of bounces the server has sent that
  // the game has not applied yet, so one that arrives at the title screen is
  // not lost. `TakeDeathPending` returns how many are owed and clears them,
  // which the client calls once it has killed the player. `LastDeathSource` is
  // who to name (the sender's slot name), empty when the packet did not say.
  int DeathsPending() const { return mDeathsReceived; }
  int TakeDeathPending() {
    const int owed = mDeathsReceived;
    mDeathsReceived = 0;
    return owed;
  }
  const std::string& LastDeathSource() const { return mLastDeathSource; }
  // The DeathLink Bounce packet for a death of this client's own, or "" when
  // the configuration does not enable DeathLink. `cause` is the text other
  // players see; empty means "<slot> died".
  std::string BuildBounce(const std::string& cause = std::string()) const;

  // Records a collected location. False when the key has no id, or was already
  // checked; otherwise `id` is the AP location id to send.
  bool MarkLocationChecked(const std::string& locationKey, int64_t& id);
  // Whether the location table has an id for this key at all, as opposed to it
  // being a key that was already checked. Both cases are a false return from
  // MarkLocationChecked, but only one is a problem, and it is a silent one: a
  // key with no id is dropped on the floor, so the session plays normally and
  // the server simply never records the check.
  bool KnowsLocation(const std::string& locationKey) const;
  // The location id the table gives the key, for naming a check in the log.
  bool LocationIdFor(const std::string& locationKey, int64_t& id) const;
  bool LocationChecked(int64_t id) const;
  // Every configured location id, for the MP_AP_SEND_ALL debug path.
  std::vector< int64_t > AllLocationIds() const;

  const Config& GetConfig() const { return mConfig; }
  const State& GetState() const { return mState; }
  // Lines the session up with a loaded game that holds the first `heldCount`
  // received items: the next full inventory (the reply to Connect or Sync) is
  // replayed from the start, granting only the items from `heldCount` on and
  // rebuilding the progressive counts from the ones before it.
  void RewindTo(int64_t heldCount);
  // True once the server's full inventory (ReceivedItems from index 0) has been
  // processed, so ReceivedCount is complete rather than a partial replay.
  bool InventoryReceived() const { return mInventoryReceived; }
  // Swaps in another game's progress (a seed's own state file); no item it
  // replays counts as already held.
  void SetState(const State& state) {
    mState = state;
    mGrantFrom = 0;
    mWorldRevision = NextWorldRevision();
  }
  // Claims the current progress for the server room named `room`.
  void SetRoom(const std::string& room) { mState.room = room; }
  // Changes whenever State::world may have (slot_data parsed, a state swapped
  // in), so the client can tell the layout is unchanged without comparing or
  // copying it. Unique across sessions; never 0.
  uint64_t WorldRevision() const { return mWorldRevision; }
  bool HandshakeComplete() const { return mHandshakeComplete; }
  // "slot 3, team 0" after Connected, empty before.
  const std::string& SlotDescription() const { return mSlotDescription; }
  // Most recent PrintJSON text, truncated for the overlay.
  const std::string& LastMessage() const { return mLastMessage; }
  // Most recent refusal or protocol complaint.
  const std::string& LastError() const { return mLastError; }
  // Set when ReceivedItems arrived with an index other than the expected one.
  bool Desynced() const { return mDesynced; }
  // Why the loaded progress was thrown away on the last RoomInfo, empty when
  // nothing was. The state itself is already reset when this is set.
  const std::string& ResetReason() const { return mResetReason; }

private:
  Config mConfig;
  State mState;
  bool mHandshakeComplete = false;
  bool mDesynced = false;
  bool mInventoryReceived = false;
  // Server item id -> the built-in table's id, for the ids of this game's items
  // that the server numbers differently (empty when the numbering matches or
  // the names have not arrived). Built from Metroid Prime's DataPackage names.
  std::map< int64_t, int64_t > mItemIds;
  // Games a GetDataPackage was sent for on this connection, so the one sent
  // before Connect is not asked for again from Connected.
  std::set< std::string > mPackageAsked;
  // Received items below this index are in the loaded game already; see RewindTo.
  int64_t mGrantFrom = 0;
  std::string mSlotDescription;
  std::string mLastMessage;
  std::string mLastError;
  std::string mSeedName;
  std::string mResetReason;
  int64_t mOwnSlot = 0;
  std::map< int64_t, std::string > mPlayers;
  // Slot -> game, from Connected's slot_info.
  std::map< int64_t, std::string > mSlotGames;
  // Id -> name per game, from DataPackage.
  std::map< std::string, GameNames > mGameNames;
  // See WorldRevision.
  static uint64_t NextWorldRevision();
  uint64_t mWorldRevision = NextWorldRevision();
  // What sits at this slot's locations, from LocationInfo.
  struct ScoutedItem {
    int64_t item = 0;
    int64_t player = 0;
    int64_t flags = 0;
  };
  std::map< int64_t, ScoutedItem > mScouts;
  // Where this slot's artifacts are (item id -> location and the slot whose
  // world has it), from the server's hints (the AP world hints every artifact
  // at the start when artifact_hints is on) or slot_data artifact_locations.
  struct HintedLocation {
    int64_t location = 0;
    int64_t player = 0;
  };
  std::map< int64_t, HintedLocation > mArtifactHints;
  // Locations already scouted as hints after a totem scan.
  std::set< int64_t > mScanHinted;
  int64_t mTeam = 0;
  void ReadHints(const PortJson::Value& hints);
  // Locations whose text the HUD showed when they were collected, so the
  // server's own announcement of those finds would repeat it.
  std::set< int64_t > mAnnounced;
  bool AnnouncedLocally(int64_t locationId, int64_t finder) const;
  std::vector< std::string > mNotifications;
  std::vector< ChatLine > mChat;
  std::vector< std::string > mLog;
  void Log(std::string line);
  std::vector< TrackedItem > mTracked;
  SlotData mSlotData;
  // DeathLink bookkeeping: bounces owed to the game, and who to blame.
  int mDeathsReceived = 0;
  std::string mLastDeathSource;
};

} // namespace Protocol
} // namespace PortAp

#endif // METROID_PRIME_PORT_PORT_AP_PROTOCOL_H
