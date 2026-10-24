#ifndef METROID_PRIME_PORT_PORT_AP_METROIDPRIME_H
#define METROID_PRIME_PORT_PORT_AP_METROIDPRIME_H
#include "port_ap_protocol.h"

#include <cstdint>
#include <map>

// The Metroid Prime AP world's own tables, built in so a connection needs only
// a server, a slot and a password. The locations come from the AP world's
// Locations.py joined with randomprime's pickup table (tools/gen_ap_locations.py
// writes port_ap_locations.inc); the item ids and names are the world's
// Items.py. A configuration file that carries its own "locations" or "items"
// replaces these (docs/ARCHIPELAGO.md).
namespace PortAp {
namespace MetroidPrime {

constexpr int64_t kItemBase = 5031000;

// Item ids past the base that the grant logic treats specially.
enum : int64_t {
  kMissileExpansion = 4,
  kPowerBombExpansion = 7,
  kEnergyTank = 24,
  kArtifactTruth = 29,
  kArtifactNewborn = 40,
  kUnlimitedMissiles = 41,
  kUnlimitedPowerBombs = 42,
  kMissileLauncher = 43,
  kMainPowerBomb = 44,
  kSpringBall = 45,
  // Each beam, then its Charge Beam, then its combo.
  kProgressivePowerBeam = 49,
  kProgressiveIceBeam = 51,
  kProgressiveWaveBeam = 52,
  kProgressivePlasmaBeam = 53,
  kProgressiveBomb = 54, // Spring Ball first, then the Morph Ball Bombs
};

// World asset ids the client cares about.
constexpr uint32_t kFrigateWorld = 0x158EFE17u;
constexpr uint32_t kTallonWorld = 0x39F2DE28u;
constexpr uint32_t kEndOfGameWorld = 0x13D79165u;
// Artifact Temple (Tallon), whose layers hold the artifact totems.
constexpr uint32_t kArtifactTempleArea = 0x2398E906u;
// Its index in Tallon's area list, for when Tallon is not the running world.
constexpr int kArtifactTempleIndex = 16;
// Landing Site (Tallon), where an AP game starts.
constexpr uint32_t kLandingSiteArea = 0xB2701146u;

struct Location {
  int64_t id;      // AP location id
  uint32_t world;  // MLVL
  uint32_t area;   // MREA
  uint32_t pickup; // pickup editor id, layer bits cleared
  uint32_t memo;   // the pickup's "acquired" HUD memo
  uint32_t relay;  // the memory relay the AP world reads the check from
  uint32_t audio;  // the StreamedAudio that plays the pickup's jingle
  const char* name;
};

// Every location, in AP id order.
const Location* Locations(size_t& count);
// The location whose pickup is `entity` in that world and area, or null.
const Location* FindPickup(uint32_t world, uint32_t area, uint32_t entity);
// The location whose pickup memo is `entity`, or null.
const Location* FindMemo(uint32_t world, uint32_t area, uint32_t entity);
// The location whose attainment-jingle StreamedAudio is `entity`, or null.
const Location* FindAudio(uint32_t world, uint32_t area, uint32_t entity);
const Location* FindLocation(int64_t id);

// The AP name of an item id (with the base), or null when the world has none.
const char* ItemName(int64_t itemId);

// The built-in id (with the base) of the item the AP world calls `name`. The
// official world numbers its items from 41 differently than this table does,
// so the session translates the server's ids by name; see Session::MergeDataPackage.
bool CanonicalItemId(const std::string& name, int64_t& itemId);

// Fills in the built-in item table, and the location table too when the
// configuration left it out. A configuration with its own items keeps them and
// gets neither. True when it filled the items, which `Config::builtin` records.
bool ApplyDefaults(Protocol::Config& config);

// Ammo capacity the received items add up to, following the AP world's
// count_ammo: missiles are 5 for the launcher and 5 per expansion, power bombs
// 4 for the main bomb and 1 per expansion. Without the main item the first
// expansion stands in for it, unless the seed requires the main item, in which
// case there is no capacity at all until it arrives. `counts` maps item ids
// (with the base) to copies received.
int AmmoCapacity(const std::map< int64_t, int64_t >& counts, bool missiles, bool requiresMain);
// Whether this item id feeds one of the ammo pools, and which.
bool IsAmmoItem(int64_t itemId, bool& missiles);

// Pickup model keys past the item types (0-40), matching the table
// tools/gen_ap_pickup_patches.py writes.
// Another game's items look like the AP world draws them: progression = Cog,
// useful = Zoomer, anything else = Nothing (randomprime's custom models).
enum : int {
  kModelMainPowerBomb = 41,
  kModelOtherGame = 42,
  kModelOtherProgression = 43,
  kModelOtherUseful = 44,
};
// The model key a pickup holding this item id (with the base) is drawn with.
// kModelOtherGame for ids this world doesn't have.
int PickupModelKey(int64_t itemId);
// The model key for another game's item with these AP classification flags.
int OtherGameModelKey(int64_t flags);

// The jingle a pickup plays, as randomprime's PickupType::attainment_audio_file_name
// picks it: the artifact jingle for artifacts, the item jingle for upgrades,
// the short one for expansions, Energy Tanks, refills and anything that isn't
// an item of this game; `sameGame` is false for another game's item.
const char* JingleFile(int64_t itemId, bool sameGame);
// The same for a player-state item type an offline seed placed (0-40, the AP
// item ids' own range); null for anything else.
const char* JingleForItemType(int itemType);

} // namespace MetroidPrime
} // namespace PortAp

#endif // METROID_PRIME_PORT_PORT_AP_METROIDPRIME_H
