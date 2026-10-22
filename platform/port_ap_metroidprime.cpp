#include "port_ap_metroidprime.h"

#include <cstdio>
#include <string>

namespace PortAp {
namespace MetroidPrime {
namespace {

const Location kLocations[] = {
#include "port_ap_locations.inc"
};

constexpr uint32_t kEntityMask = 0x3FFFFFFu;

// CPlayerState::EItemType values; this file stays free of game headers so the
// protocol tests can link it.
enum ItemType : int {
  kNone = -1,
  kPowerBeam = 0,
  kIceBeam = 1,
  kWaveBeam = 2,
  kPlasmaBeam = 3,
  kMissiles = 4,
  kMorphBallBombs = 6,
  kPowerBombs = 7,
  kFlamethrower = 8,
  kChargeBeam = 10,
  kSuperMissile = 11,
  kIceSpreader = 14,
  kHealthRefill = 26,
  kWavebuster = 28,
};

struct ItemInfo {
  int64_t id; // past the base
  const char* name;
  int type;
};

// Items.py. Ids 0-40 are the player-state item types themselves; the rest are
// the AP world's own. Items with no player-state item type (Spring Ball,
// Nothing, the unused slots) resolve to no item type and only count. The
// client applies the unlimited-ammo items and Spring Ball from their counts.
const ItemInfo kItems[] = {
    {0, "Power Beam", 0},
    {1, "Ice Beam", 1},
    {2, "Wave Beam", 2},
    {3, "Plasma Beam", 3},
    {4, "Missile Expansion", kMissiles},
    {5, "Scan Visor", 5},
    {6, "Morph Ball Bomb", 6},
    {7, "Power Bomb Expansion", kPowerBombs},
    {8, "Flamethrower", 8},
    {9, "Thermal Visor", 9},
    {10, "Charge Beam", 10},
    {11, "Super Missile", 11},
    {12, "Grapple Beam", 12},
    {13, "X-Ray Visor", 13},
    {14, "Ice Spreader", 14},
    {15, "Space Jump Boots", 15},
    {16, "Morph Ball", 16},
    {17, "Combat Visor", 17},
    {18, "Boost Ball", 18},
    {19, "Spider Ball", 19},
    {20, "Power Suit", 20},
    {21, "Gravity Suit", 21},
    {22, "Varia Suit", 22},
    {23, "Phazon Suit", 23},
    {24, "Energy Tank", 24},
    {25, "UnknownItem1", kNone},
    {26, "HealthRefill", kHealthRefill},
    {27, "UnknownItem2", kNone},
    {28, "Wavebuster", 28},
    {29, "Artifact of Truth", 29},
    {30, "Artifact of Strength", 30},
    {31, "Artifact of Elder", 31},
    {32, "Artifact of Wild", 32},
    {33, "Artifact of Lifegiver", 33},
    {34, "Artifact of Warrior", 34},
    {35, "Artifact of Chozo", 35},
    {36, "Artifact of Nature", 36},
    {37, "Artifact of Sun", 37},
    {38, "Artifact of World", 38},
    {39, "Artifact of Spirit", 39},
    {40, "Artifact of Newborn", 40},
    {41, "Unlimited Missiles", kNone},
    {42, "Unlimited Power Bombs", kNone},
    {43, "Missile Launcher", kMissiles},
    {44, "Power Bomb (Main)", kPowerBombs},
    {45, "Spring Ball", kNone},
    {46, "Nothing", kNone},
    {49, "Progressive Power Beam", kNone},
    {51, "Progressive Ice Beam", kNone},
    {52, "Progressive Wave Beam", kNone},
    {53, "Progressive Plasma Beam", kNone},
    {54, "Progressive Bomb", kNone},
    // The AP ISO has a charge beam per beam; the game has one, so any of them
    // grants it.
    {55, "Charge Beam (Power)", kChargeBeam},
    {56, "Charge Beam (Wave)", kChargeBeam},
    {57, "Charge Beam (Ice)", kChargeBeam},
    {58, "Charge Beam (Plasma)", kChargeBeam},
};

struct ProgressiveStep {
  const char* name;
  int type;
};

struct ProgressiveInfo {
  int64_t id;
  ProgressiveStep steps[3];
  size_t count;
};

// PROGRESSIVE_ITEM_MAPPING: the Nth copy grants step N, and later copies
// repeat the last. Spring Ball is no player-state item, so the first
// Progressive Bomb only counts (PortAp::SpringBallRule reads the count).
const ProgressiveInfo kProgressive[] = {
    {49, {{"Power Beam", kPowerBeam}, {"Charge Beam", kChargeBeam}, {"Super Missile", kSuperMissile}}, 3},
    {51, {{"Ice Beam", kIceBeam}, {"Charge Beam", kChargeBeam}, {"Ice Spreader", kIceSpreader}}, 3},
    {52, {{"Wave Beam", kWaveBeam}, {"Charge Beam", kChargeBeam}, {"Wavebuster", kWavebuster}}, 3},
    {53, {{"Plasma Beam", kPlasmaBeam}, {"Charge Beam", kChargeBeam}, {"Flamethrower", kFlamethrower}}, 3},
    {54, {{"Spring Ball", kNone}, {"Morph Ball Bomb", kMorphBallBombs}}, 2},
};

Protocol::ItemGrant MakeGrant(int64_t itemId, const char* name, int type) {
  Protocol::ItemGrant grant;
  grant.itemId = itemId;
  grant.itemType = type;
  grant.display = name;
  if (type == kHealthRefill) {
    grant.capacity = 0;
    grant.amount = 9999;
  }
  return grant;
}

} // namespace

const Location* Locations(size_t& count) {
  count = sizeof(kLocations) / sizeof(kLocations[0]);
  return kLocations;
}

const Location* FindPickup(uint32_t world, uint32_t area, uint32_t entity) {
  entity &= kEntityMask;
  for (const Location& location : kLocations) {
    if (location.world == world && location.area == area && location.pickup == entity)
      return &location;
  }
  return nullptr;
}

const Location* FindMemo(uint32_t world, uint32_t area, uint32_t entity) {
  entity &= kEntityMask;
  for (const Location& location : kLocations) {
    if (location.world == world && location.area == area && location.memo == entity)
      return &location;
  }
  return nullptr;
}

const Location* FindAudio(uint32_t world, uint32_t area, uint32_t entity) {
  entity &= kEntityMask;
  for (const Location& location : kLocations) {
    if (location.world == world && location.area == area && location.audio == entity)
      return &location;
  }
  return nullptr;
}

const Location* FindLocation(int64_t id) {
  for (const Location& location : kLocations) {
    if (location.id == id)
      return &location;
  }
  return nullptr;
}

const char* ItemName(int64_t itemId) {
  for (const ItemInfo& item : kItems) {
    if (item.id + kItemBase == itemId)
      return item.name;
  }
  return nullptr;
}

bool CanonicalItemId(const std::string& name, int64_t& itemId) {
  for (const ItemInfo& item : kItems) {
    if (name == item.name) {
      itemId = item.id + kItemBase;
      return true;
    }
  }
  return false;
}

bool ApplyDefaults(Protocol::Config& config) {
  // A config can bring its own location table (tools/make_ap_config.py joins
  // one from a pickup dump) and still take the world's item rules. Items it
  // lists itself are its own rules, so the session treats it as hand-written.
  if (config.game != "Metroid Prime" || !config.items.empty())
    return false;
  if (config.locations.empty()) {
    for (const Location& location : kLocations) {
      char key[32];
      std::snprintf(key, sizeof(key), "%08X:%08X:%08X", static_cast<unsigned int>(location.world),
                    static_cast<unsigned int>(location.area),
                    static_cast<unsigned int>(location.pickup));
      config.locations[key] = location.id;
    }
  }
  for (const ItemInfo& item : kItems) {
    Protocol::ItemEntry entry;
    static_cast<Protocol::ItemGrant&>(entry) = MakeGrant(item.id + kItemBase, item.name, item.type);
    config.items[item.id + kItemBase] = std::move(entry);
  }
  for (const ProgressiveInfo& info : kProgressive) {
    Protocol::ItemEntry& entry = config.items[info.id + kItemBase];
    for (size_t i = 0; i < info.count; ++i)
      entry.progressive.push_back(
          MakeGrant(info.id + kItemBase, info.steps[i].name, info.steps[i].type));
    static_cast<Protocol::ItemGrant&>(entry) = entry.progressive.front();
  }
  return true;
}

int AmmoCapacity(const std::map< int64_t, int64_t >& counts, bool missiles, bool requiresMain) {
  const auto count = [&counts](int64_t id) {
    const auto found = counts.find(id + kItemBase);
    return found != counts.end() && found->second > 0 ? found->second : 0;
  };
  const bool hasMain = count(missiles ? kMissileLauncher : kMainPowerBomb) > 0;
  int64_t expansions = std::min<int64_t>(count(missiles ? kMissileExpansion : kPowerBombExpansion), 9999);
  const int64_t withMain = missiles ? 5 : 4;
  const int64_t perExpansion = missiles ? 5 : 1;
  int64_t result = 0;
  if (requiresMain) {
    if (!hasMain)
      return 0;
    result = withMain + expansions * perExpansion;
  } else {
    if (hasMain) {
      result = withMain;
    } else if (expansions > 0) {
      result = withMain;
      --expansions;
    }
    result += expansions * perExpansion;
  }
  // The game clamps capacity far below this; the bound only keeps the sum an int.
  return static_cast<int>(result < 9999 ? result : 9999);
}

int PickupModelKey(int64_t itemId) {
  // The AP world's own choices (RoomData.py get_config_item_model). The
  // pickup table maps keys without a retail model (Power Beam, the visors,
  // Phazon Suit) to randomprime's custom assets; Shiny Missile and refills
  // fall back to the nearest retail pickup.
  const int64_t local = itemId - kItemBase;
  if (local >= 0 && local <= kArtifactNewborn)
    return static_cast<int>(local);
  switch (local) {
  case kSpringBall:
    return kModelOtherProgression; // the Cog

  case kUnlimitedMissiles:
  case kMissileLauncher:
    return kMissileExpansion;
  case kUnlimitedPowerBombs:
    return kPowerBombExpansion;
  case kMainPowerBomb:
    return kModelMainPowerBomb;
  case 49: // Progressive Power Beam, shown as a Super Missile
    return 11;
  case 51: // Progressive Ice, Wave and Plasma Beam
    return 1;
  case 52:
    return 2;
  case 53:
    return 3;
  case kProgressiveBomb:
    return 6;
  case 55: // the four Charge Beams
  case 56:
  case 57:
  case 58:
    return 10;
  default:
    return kModelOtherGame;
  }
}

int OtherGameModelKey(int64_t flags) {
  // Traps get an Ice Trap model in the AP world, a custom asset we don't have.
  if ((flags & 1) != 0)
    return kModelOtherProgression;
  if ((flags & 2) != 0)
    return kModelOtherUseful;
  return kModelOtherGame;
}

const char* JingleFile(int64_t itemId, bool sameGame) {
  constexpr const char* kShort = "/audio/itm_x_short_02.dsp";
  constexpr const char* kItem = "audio/jin_itemattain.dsp";
  constexpr const char* kArtifact = "audio/jin_artifact.dsp";
  // Another game's item is the AP world's "Nothing" pickup: the short jingle.
  if (!sameGame)
    return kShort;
  const int64_t local = itemId - kItemBase;
  if (local >= kArtifactTruth && local <= kArtifactNewborn)
    return kArtifact;
  switch (local) {
  case 0:  // Power Beam
  case kMissileExpansion:
  case kPowerBombExpansion:
  case 20: // Power Suit
  case kEnergyTank:
  case 25: // UnknownItem1
  case 26: // HealthRefill
  case 27: // UnknownItem2
  case kUnlimitedMissiles:
  case kUnlimitedPowerBombs:
  case 46: // Nothing
    return kShort;
  default:
    // The launchers, Spring Ball, the progressive items and every other
    // upgrade.
    return kItem;
  }
}

const char* JingleForItemType(int itemType) {
  if (itemType < 0 || itemType > kArtifactNewborn)
    return nullptr;
  return JingleFile(kItemBase + itemType, true);
}

bool IsAmmoItem(int64_t itemId, bool& missiles) {
  const int64_t local = itemId - kItemBase;
  if (local == kMissileExpansion || local == kMissileLauncher) {
    missiles = true;
    return true;
  }
  if (local == kPowerBombExpansion || local == kMainPowerBomb) {
    missiles = false;
    return true;
  }
  return false;
}

} // namespace MetroidPrime
} // namespace PortAp
