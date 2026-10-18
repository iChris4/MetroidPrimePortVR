#include "port_hash.h"
#include "port_rando_gen.h"

#include "port_ap_logic.h"
#include "port_ap_metroidprime.h"
#include "port_ap_world.h"
#include "port_json_writer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <sstream>

namespace PortRandoGen {
namespace {

namespace MP = PortAp::MetroidPrime;

// AP item ids past MP::kItemBase, as the port's own table (port_ap_metroidprime.cpp)
// has them. They differ from the released apworld's numbers, so never take an id
// from the Python reference.
enum Item : int {
  kPower = 0,
  kIce = 1,
  kWave = 2,
  kPlasma = 3,
  kMissileExp = 4,
  kScan = 5,
  kBomb = 6,
  kPbExp = 7,
  kFlamethrower = 8,
  kThermal = 9,
  kCharge = 10,
  kSuper = 11,
  kGrapple = 12,
  kXray = 13,
  kIceSpreader = 14,
  kSpace = 15,
  kMorph = 16,
  kBoost = 18,
  kSpider = 19,
  kGravity = 21,
  kVaria = 22,
  kPhazon = 23,
  kEtank = 24,
  kWavebuster = 28,
  kArtifactFirst = 29,
  kArtifactLast = 40,
  kUnlimitedMissiles = 41,
  kUnlimitedPowerBombs = 42,
  kLauncher = 43,
  kMainPb = 44,
  kProgPower = 49,
  kProgIce = 51,
  kProgWave = 52,
  kProgPlasma = 53,
  kItemSlots = 64,
};

constexpr size_t kLocationCount = 100;
// Most failed attempts are shield layouts that close the start, caught before the fill.
constexpr int kMaxAttempts = 500;

// ---------------------------------------------------------------------------
// Determinism: our own PRNG and hash, since the standard distributions and
// shuffle differ between standard libraries.

uint64_t SplitMix(uint64_t& state) {
  uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

class Rng {
public:
  explicit Rng(uint64_t seed) {
    for (uint64_t& word : mState)
      word = SplitMix(seed);
  }

  // xoshiro256**
  uint64_t Next() {
    const uint64_t result = Rotl(mState[1] * 5, 7) * 9;
    const uint64_t t = mState[1] << 17;
    mState[2] ^= mState[0];
    mState[3] ^= mState[1];
    mState[1] ^= mState[2];
    mState[0] ^= mState[3];
    mState[2] ^= t;
    mState[3] = Rotl(mState[3], 45);
    return result;
  }

  // Uniform in [0, n), without modulo bias.
  size_t Below(size_t n) {
    const uint64_t bound = n;
    const uint64_t threshold = (0 - bound) % bound;
    for (;;) {
      const uint64_t value = Next();
      if (value >= threshold)
        return static_cast< size_t >(value % bound);
    }
  }

  template < typename T >
  void Shuffle(std::vector< T >& values) {
    for (size_t i = values.size(); i > 1; --i)
      std::swap(values[i - 1], values[Below(i)]);
  }

  template < typename T >
  const T& Pick(const std::vector< T >& values) {
    return values[Below(values.size())];
  }

private:
  static uint64_t Rotl(uint64_t value, int shift) { return (value << shift) | (value >> (64 - shift)); }
  std::array< uint64_t, 4 > mState{};
};

class Hasher {
public:
  void Add(int64_t value) {
    for (int i = 0; i < 8; ++i)
      Byte(static_cast< uint8_t >(static_cast< uint64_t >(value) >> (8 * i)));
  }
  void Add(const std::string& text) {
    Add(static_cast< int64_t >(text.size()));
    for (const char c : text)
      Byte(static_cast< uint8_t >(c));
  }
  uint64_t Value() const { return mHash; }

private:
  void Byte(uint8_t byte) { mHash = port::Fnv1a64Step(mHash, byte); }
  uint64_t mHash = port::kFnv64Basis;
};

uint64_t HashInput(const Settings& s, const std::string& seedText) {
  Hasher h;
  h.Add(seedText);
  h.Add(s.requiredArtifacts);
  h.Add(s.finalBosses);
  h.Add(s.artifactHints);
  h.Add(s.missileLauncher);
  h.Add(s.mainPowerBomb);
  h.Add(s.shuffleScanVisor);
  h.Add(s.preScanElevators);
  h.Add(s.elevatorRandomization);
  h.Add(s.doorColorRandomization);
  h.Add(s.progressiveBeams);
  h.Add(s.nonVariaHeatDamage);
  h.Add(s.staggeredSuitDamage);
  h.Add(s.combatLogic);
  h.Add(s.trickDifficulty);
  h.Add(static_cast< int64_t >(s.trickAllow.size()));
  for (const std::string& name : s.trickAllow)
    h.Add(name);
  h.Add(static_cast< int64_t >(s.trickDeny.size()));
  for (const std::string& name : s.trickDeny)
    h.Add(name);
  h.Add(s.flaahgraPowerBombs);
  // Only a changed capacity enters the hash, so older seeds keep their text.
  if (s.etankCapacity != 100)
    h.Add(s.etankCapacity);
  h.Add(s.backwardsLowerMines);
  h.Add(s.removeXray);
  h.Add(s.removeThermal);
  h.Add(s.removeHiveMecha);
  h.Add(s.springBall);
  // Off leaves the hash as it was, so older seeds keep their text.
  if (s.shuffleUnlimitedMissiles)
    h.Add(std::string("shuffle_unlimited_missiles"));
  if (s.shuffleUnlimitedPowerBombs)
    h.Add(std::string("shuffle_unlimited_power_bombs"));
  h.Add(s.startingRoom);
  h.Add(s.randomizeStartingBeam);
  h.Add(s.blastShieldRandomization);
  h.Add(s.blastShieldFrequency);
  h.Add(s.blastShieldAvailableTypes);
  h.Add(s.lockedDoorCount);
  h.Add(s.includePowerBeamDoors);
  h.Add(s.includeMorphBallBombDoors);
  return h.Value();
}

// ---------------------------------------------------------------------------
// Layout: elevators and door colours (Transports.py, DoorRando.py).

const char* const kAreas[] = {"Tallon Overworld", "Chozo Ruins", "Magmoor Caverns", "Phendrana Drifts",
                              "Phazon Mines"};
constexpr int kAreaCount = 5;

struct ElevatorInfo {
  int area;
  const char* name;
  const char* destination; // where it leads on the disc
};

// default_elevator_mappings, in the apworld's dictionary order (the random
// walk picks the first of equally large areas).
const ElevatorInfo kElevators[] = {
    {0, "Transport to Chozo Ruins West", "Transport to Tallon Overworld North"},
    {0, "Transport to Magmoor Caverns East", "Transport to Tallon Overworld West"},
    {0, "Transport to Chozo Ruins East", "Transport to Tallon Overworld East"},
    {0, "Transport to Chozo Ruins South", "Chozo Ruins: Transport to Tallon Overworld South"},
    {0, "Transport to Phazon Mines East", "Phazon Mines: Transport to Tallon Overworld South"},
    {1, "Transport to Tallon Overworld North", "Transport to Chozo Ruins West"},
    {1, "Transport to Magmoor Caverns North", "Transport to Chozo Ruins North"},
    {1, "Transport to Tallon Overworld East", "Transport to Chozo Ruins East"},
    {1, "Chozo Ruins: Transport to Tallon Overworld South", "Transport to Chozo Ruins South"},
    {2, "Transport to Chozo Ruins North", "Transport to Magmoor Caverns North"},
    {2, "Transport to Phendrana Drifts North", "Transport to Magmoor Caverns West"},
    {2, "Transport to Tallon Overworld West", "Transport to Magmoor Caverns East"},
    {2, "Transport to Phendrana Drifts South", "Phendrana Drifts: Transport to Magmoor Caverns South"},
    {2, "Transport to Phazon Mines West", "Phazon Mines: Transport to Magmoor Caverns South"},
    {3, "Transport to Magmoor Caverns West", "Transport to Phendrana Drifts North"},
    {3, "Phendrana Drifts: Transport to Magmoor Caverns South", "Transport to Phendrana Drifts South"},
    {4, "Phazon Mines: Transport to Tallon Overworld South", "Transport to Phazon Mines East"},
    {4, "Phazon Mines: Transport to Magmoor Caverns South", "Transport to Phazon Mines West"},
};
constexpr int kElevatorCount = 18;

using ElevatorMap = std::map< std::string, std::map< std::string, std::string > >;

ElevatorMap DefaultElevators() {
  ElevatorMap out;
  for (const ElevatorInfo& e : kElevators)
    out[kAreas[e.area]][e.name] = e.destination;
  return out;
}

// ---------------------------------------------------------------------------
// Start rooms (StartRoomData.py), in its dictionary order.

enum Difficulty : int { kNormalStart = 0, kSafeStart = 1, kBuckleUpStart = 2 };

// A location the start room's loadout fills; one of `choices` at random.
// kLauncher and kMainPb stand for whatever the options make of them, and a
// beam for its progressive form under progressive beams.
struct ItemRule {
  const char* location;
  std::vector< int > choices;
};
using RuleSet = std::vector< ItemRule >;

struct Loadout {
  int beam;
  std::vector< int > items;     // given at the start
  std::vector< RuleSet > rules; // one set is prefilled, picked at random
};

// Under elevator randomization, the start area's elevator `source` may only
// lead to one of `targets`.
struct AllowedElevator {
  const char* source;
  std::vector< const char* > targets;
};

struct StartRoom {
  const char* name;
  int area;
  int difficulty;
  std::vector< Loadout > loadouts;
  std::vector< int > localEarly; // placed in a location open from the start
  std::vector< AllowedElevator > elevators;
  bool needsScanVisor; // only when the Scan Visor isn't shuffled
  bool forceBeam;      // the loadout's beam stays whatever the door colours
  bool noPowerDoorAtStart;
};

const std::vector< StartRoom >& StartRooms() {
  static const char* const kHive = "Chozo Ruins: Hive Totem";
  static const char* const kBeetle = "Chozo Ruins: Ruined Shrine - Plated Beetle";
  static const char* const kGallery = "Chozo Ruins: Ruined Gallery - Missile Wall";
  static const std::vector< RuleSet > kLandingRules = {
      {{kHive, {kLauncher}}, {kBeetle, {kMorph}}, {kGallery, {kBomb}}},
      {{kHive, {kLauncher}}, {kBeetle, {kBomb}}, {kGallery, {kMorph}}},
  };
  static const std::vector< AllowedElevator > kTallonElevators = {
      {"Transport to Chozo Ruins West", {"Transport to Magmoor Caverns North", "Transport to Magmoor Caverns West"}},
      {"Transport to Magmoor Caverns East",
       {"Transport to Tallon Overworld North", "Transport to Magmoor Caverns West",
        "Transport to Magmoor Caverns North"}},
  };
  static const std::vector< StartRoom > kRooms = {
      {"Landing Site", 0, kNormalStart,
       {{kPower, {}, kLandingRules}, {kPower, {}, {{{kHive, {kLauncher}}, {kGallery, {kMorph}}}}}},
       {}, {}, false, false, false},
      {"Arboretum", 1, kSafeStart, {{kPower, {kLauncher}, {}}}, {kMorph, kScan}, {}, true, false, false},
      {"Burn Dome", 1, kSafeStart,
       {{kPower,
         {kMorph},
         {{{"Chozo Ruins: Burn Dome - Incinerator Drone", {kBomb}}, {"Chozo Ruins: Burn Dome - Missile", {kLauncher}}}}}},
       {}, {}, false, false, false},
      {"Ruined Fountain", 1, kSafeStart, {{kPower, {kLauncher}, {}}}, {kMorph}, {}, false, false, false},
      {"Save Station 1", 1, kSafeStart, {{kPower, {}, kLandingRules}}, {},
       {{"Transport to Tallon Overworld North",
         {"Transport to Chozo Ruins East", "Transport to Magmoor Caverns West", "Transport to Chozo Ruins West"}}},
       false, false, false},
      {"Save Station 2", 1, kSafeStart, {{kPower, {kLauncher}, {}}}, {}, {}, false, false, false},
      {"Tower Chamber", 1, kSafeStart,
       {{kWave,
         {},
         {{{"Chozo Ruins: Tower Chamber", {kMorph}},
           {kBeetle, {kBomb, kMainPb}},
           {"Chozo Ruins: Ruined Shrine - Lower Tunnel", {kLauncher}}}}}},
       {}, {}, false, false, false},
      {"Warrior Shrine", 2, kSafeStart,
       {{kPower, {kVaria, kMorph}, {{{"Magmoor Caverns: Storage Cavern", {kBomb}}}}}}, {}, {}, false, false, false},
      {"East Tower", 3, kBuckleUpStart,
       {{kWave,
         {kLauncher},
         {{{"Phendrana Drifts: Phendrana Canyon", {kSpace}},
           {"Phendrana Drifts: Research Lab Aether - Tank", {kPlasma}}}}}},
       {}, {}, true, false, true},
      {"Save Station B", 3, kSafeStart,
       {{kPlasma, {kLauncher}, {{{"Phendrana Drifts: Phendrana Shorelines - Behind Ice", {kSpace}}}}}}, {}, {}, true,
       true, false},
      {"Arbor Chamber", 0, kSafeStart, {{kPower, {kLauncher}, {}}}, {}, kTallonElevators, false, false, false},
      {"Transport to Chozo Ruins East", 0, kSafeStart,
       {{kIce, {kMorph}, {{{"Tallon Overworld: Overgrown Cavern", {kLauncher}}}}}}, {}, kTallonElevators, false, false,
       false},
      {"Quarantine Monitor", 3, kBuckleUpStart,
       {{kWave,
         {kThermal},
         {{{"Phendrana Drifts: Quarantine Monitor", {kMorph}},
           {"Phendrana Drifts: Quarantine Cave", {kSpider}},
           {"Phendrana Drifts: Ice Ruins East - Spider Track", {kSpace}},
           {"Phendrana Drifts: Ruined Courtyard", {kPlasma}}}}}},
       {}, {}, true, false, false},
      {"Sunchamber Lobby", 1, kBuckleUpStart, {{kPower, {kMorph, kLauncher, kBomb}, {}}}, {}, {}, false, false, false},
  };
  return kRooms;
}

const StartRoom& FindStartRoom(const char* name) {
  for (const StartRoom& room : StartRooms()) {
    if (std::string(room.name) == name)
      return room;
  }
  return StartRooms()[0];
}

// get_random_elevator_mapping: pair elevators two ways across areas, always
// starting from the area with the most unpaired ones. The start room's
// allowed elevators (in `area`) are paired first. False when the walk strands
// the last area (the apworld would raise); the caller rolls again.
bool RandomElevators(Rng& rng, int area, const std::vector< AllowedElevator >& allowed, ElevatorMap& out) {
  out.clear();
  std::vector< std::vector< int > > available(kAreaCount);
  for (int i = 0; i < kElevatorCount; ++i)
    available[kElevators[i].area].push_back(i);
  std::vector< int > alive = {0, 1, 2, 3, 4};

  const auto drop = [&available, &alive](int area, int elevator) {
    std::vector< int >& list = available[area];
    list.erase(std::find(list.begin(), list.end(), elevator));
    if (list.empty())
      alive.erase(std::find(alive.begin(), alive.end(), area));
  };
  const auto pair = [&](int source, int target) {
    out[kAreas[kElevators[source].area]][kElevators[source].name] = kElevators[target].name;
    out[kAreas[kElevators[target].area]][kElevators[target].name] = kElevators[source].name;
    drop(kElevators[source].area, source);
    drop(kElevators[target].area, target);
  };

  for (const AllowedElevator& rule : allowed) {
    int source = -1;
    for (const int i : available[area]) {
      if (std::string(rule.source) == kElevators[i].name)
        source = i;
    }
    if (source < 0)
      return false;
    std::vector< int > options;
    for (int i = 0; i < kElevatorCount; ++i) {
      const std::vector< int >& left = available[kElevators[i].area];
      if (kElevators[i].area == area || std::find(left.begin(), left.end(), i) == left.end())
        continue;
      for (const char* name : rule.targets) {
        if (std::string(name) == kElevators[i].name)
          options.push_back(i);
      }
    }
    if (options.empty())
      return false;
    pair(source, rng.Pick(options));
  }

  while (!alive.empty()) {
    int sourceArea = alive[0];
    for (const int area : alive) {
      if (available[area].size() > available[sourceArea].size())
        sourceArea = area;
    }
    const int source = rng.Pick(available[sourceArea]);
    std::vector< int > targets;
    for (const int area : alive) {
      if (area != sourceArea)
        targets.push_back(area);
    }
    if (targets.empty())
      return false;
    const int targetArea = rng.Pick(targets);
    pair(source, rng.Pick(available[targetArea]));
  }
  return true;
}

// The lock a beam opens, as DoorLockType names it.
const char* LockOf(int beam) {
  switch (beam) {
  case kWave: return "Wave Beam";
  case kIce: return "Ice Beam";
  case kPlasma: return "Plasma Beam";
  default: return "Power Beam Only";
  }
}

const char* const kColorLocks[] = {"Wave Beam", "Ice Beam", "Plasma Beam"};

// generate_random_door_color_mapping: the coloured locks (and Power Beam Only
// doors when asked) shuffled until none of the three keeps its own colour.
// Quarantine Monitor's start also needs its wave doors not to turn ice.
std::map< std::string, std::string > RandomLocks(const Settings& s, bool quarantineMonitor, Rng& rng) {
  std::vector< std::string > shuffled(std::begin(kColorLocks), std::end(kColorLocks));
  if (s.includePowerBeamDoors && !s.randomizeStartingBeam)
    shuffled.push_back("Power Beam Only");
  std::map< std::string, std::string > out;
  for (;;) {
    rng.Shuffle(shuffled);
    out.clear();
    bool valid = true;
    for (size_t i = 0; i < std::size(kColorLocks); ++i) {
      out[kColorLocks[i]] = shuffled[i];
      valid = valid && shuffled[i] != kColorLocks[i];
    }
    if (quarantineMonitor)
      valid = valid && out["Wave Beam"] != "Ice Beam";
    if (valid)
      return out;
  }
}

// get_world_door_mapping, with the bomb doors of one area other than the start's.
ElevatorMap RandomDoorColors(const Settings& s, const StartRoom& room, Rng& rng) {
  ElevatorMap colors;
  const bool quarantineMonitor = std::string(room.name) == "Quarantine Monitor";
  if (s.doorColorRandomization == 1) {
    const auto mapping = RandomLocks(s, quarantineMonitor, rng);
    for (const char* area : kAreas)
      colors[area] = mapping;
  } else if (s.doorColorRandomization == 2) {
    for (const char* area : kAreas)
      colors[area] = RandomLocks(s, quarantineMonitor, rng);
  } else {
    return colors;
  }
  if (s.includeMorphBallBombDoors) {
    std::vector< int > areas;
    for (int area = 0; area < kAreaCount; ++area) {
      if (area != room.area)
        areas.push_back(area);
    }
    const int area = rng.Pick(areas);
    colors[kAreas[area]][kColorLocks[rng.Below(std::size(kColorLocks))]] = "Bomb";
  }
  return colors;
}

// The start room and loadout (init_starting_room_data, init_starting_beam).
struct Start {
  const StartRoom* room = nullptr;
  Loadout loadout;
  // The loadout's items and prefill are given (the apworld's bk prevention,
  // off for a Normal start that has more checks to reach).
  bool bkPrevention = true;
  int beam = kPower;
  bool beamReplaced = false;
};

Start PickStart(const Settings& s, Rng& rng) {
  Start out;
  if (s.startingRoom == kNormalStart) {
    // Without pre-scanned elevators and a shuffled Scan Visor, Landing Site has
    // no way out, tricks or not, so Save Station 1 is used then as well (the
    // apworld skips that case). Its prefill is then kept: without it, shields
    // often close every way out of Save Station 1.
    const bool noScanElevators = !s.preScanElevators && s.shuffleScanVisor;
    const bool saveStation = s.elevatorRandomization || noScanElevators;
    out.room = &FindStartRoom(saveStation ? "Save Station 1" : "Landing Site");
    const bool moreChecks =
        (s.blastShieldRandomization != 0 || s.trickDifficulty != -1) && !s.elevatorRandomization;
    out.bkPrevention = !moreChecks || saveStation;
  } else {
    std::vector< const StartRoom* > candidates;
    for (const StartRoom& room : StartRooms()) {
      if (room.difficulty != s.startingRoom || (room.needsScanVisor && s.shuffleScanVisor))
        continue;
      // A random beam without door colours needs a Power Beam room.
      if (s.doorColorRandomization == 0 && s.randomizeStartingBeam && room.loadouts[0].beam != kPower)
        continue;
      candidates.push_back(&room);
    }
    out.room = rng.Pick(candidates);
  }
  out.loadout = rng.Pick(out.room->loadouts);
  if (!out.bkPrevention)
    out.loadout.items.clear();
  out.beam = out.loadout.beam;
  return out;
}

// init_starting_beam: a coloured start beam follows its doors' new colour,
// otherwise randomize_starting_beam picks one.
void PickBeam(const Settings& s, const ElevatorMap& colors, Start& start, Rng& rng) {
  if (start.room->forceBeam)
    return;
  if (!colors.empty() && start.beam != kPower) {
    const std::string& lock = colors.at(kAreas[start.room->area]).at(LockOf(start.beam));
    for (const int beam : {kPower, kWave, kIce, kPlasma}) {
      if (lock == LockOf(beam)) {
        start.beam = beam;
        start.beamReplaced = true;
      }
    }
  } else if (s.randomizeStartingBeam) {
    start.beam = rng.Pick(std::vector< int >{kWave, kIce, kPlasma});
    start.beamReplaced = true;
  }
}

// remap_doors_to_power_beam_if_necessary: doors of the start beam's colour
// open to the Power Beam instead.
void RemapPowerDoors(const Settings& s, const Start& start, ElevatorMap& colors) {
  if (!s.includePowerBeamDoors || colors.empty() || start.beam == kPower)
    return;
  for (auto& area : colors) {
    if (area.first == kAreas[start.room->area] && start.room->noPowerDoorAtStart)
      continue;
    for (auto& entry : area.second) {
      if (entry.second == LockOf(start.beam))
        entry.second = "Power Beam Only";
    }
  }
}

// ---------------------------------------------------------------------------
// Blast shields (BlastShieldRando.py, data/BlastShieldRegions.py).

struct ShieldRegion {
  int area;
  const char* name;
  bool lockable; // may hold a locked door
  std::vector< std::pair< const char*, const char* > > doors; // (room, the room it leads to)
  std::vector< const char* > invalidStarts;                   // start rooms it can't take a shield with
};

const std::vector< ShieldRegion >& ShieldRegions() {
  static const std::vector< ShieldRegion > kRegions = {
      {0, "Alcove", false, {{"Landing Site", "Alcove"}}, {"Arbor Chamber"}},
      {0, "Canyon Cavern", true, {{"Landing Site", "Canyon Cavern"}}, {"Arbor Chamber", "Transport to Chozo Ruins East", "Landing Site"}},
      {0, "Temple Hall", false, {{"Landing Site", "Temple Hall"}}, {"Arbor Chamber", "Transport to Chozo Ruins East", "Landing Site"}},
      {0, "Waterfall Cavern", true, {{"Landing Site", "Waterfall Cavern"}}, {"Transport to Chozo Ruins East", "Landing Site"}},
      {0, "Transport Tunnel B", true, {{"Tallon Canyon", "Root Tunnel"}, {"Root Cave", "Transport Tunnel B"}}, {"Warrior Shrine", "Arbor Chamber", "Transport to Chozo Ruins East", "Landing Site"}},
      {0, "Transport Tunnel D", true, {{"Great Tree Hall", "Transport Tunnel D"}}, {"Warrior Shrine"}},
      {0, "Transport Tunnel E", true, {{"Great Tree Hall", "Transport Tunnel E"}}, {"Warrior Shrine"}},
      {0, "Great Tree Chamber", false, {{"Great Tree Hall", "Great Tree Chamber"}}, {}},
      {1, "Ruins Entrance", false, {{"Transport to Tallon Overworld North", "Ruins Entrance"}, {"Ruins Entrance", "Main Plaza"}}, {"Warrior Shrine", "Transport to Chozo Ruins East", "Arbor Chamber", "Landing Site"}},
      {1, "Ruined Shrine", false, {{"Main Plaza", "Ruined Shrine Access"}}, {"Arboretum", "Sunchamber Lobby", "Save Station 1", "Save Station 2", "Burn Dome", "Ruined Fountain", "Tower Chamber"}},
      {1, "Tower of Light", false, {{"Ruined Shrine", "Tower of Light Access"}}, {"Tower Chamber"}},
      {1, "Ruined Nursery", true, {{"Main Plaza", "Nursery Access"}}, {"Arboretum", "Sunchamber Lobby", "Save Station 1", "Save Station 2", "Burn Dome", "Ruined Fountain", "Tower Chamber"}},
      {1, "Hive Totem", true, {{"North Atrium", "Ruined Gallery"}, {"Totem Access", "Hive Totem"}, {"Transport Access North", "Transport to Magmoor Caverns North"}}, {"Save Station 1", "Tower Chamber"}},
      {1, "Vault", false, {{"Transport to Magmoor Caverns North", "Vault Access"}, {"Vault", "Plaza Access"}}, {}},
      {1, "Training Chamber", false, {{"Main Plaza", "Ruined Fountain Access"}, {"Ruined Fountain", "Meditation Fountain"}, {"Meditation Fountain", "Magma Pool"}, {"Magma Pool", "Training Chamber Access"}, {"Training Chamber Access", "Training Chamber"}}, {}},
      {1, "Ruined Fountain", false, {{"Ruined Fountain", "Arboretum Access"}, {"Arboretum Access", "Arboretum"}}, {"Arboretum", "Sunchamber Lobby", "Save Station 1", "Save Station 2", "Burn Dome", "Ruined Fountain"}},
      {1, "Arboretum", false, {{"Sunchamber Lobby", "Arboretum"}, {"Gathering Hall Access", "Arboretum"}}, {"Arboretum", "Sunchamber Lobby", "Save Station 1", "Save Station 2", "Burn Dome", "Ruined Fountain"}},
      {1, "Watery Hall", false, {{"Gathering Hall", "Watery Hall Access"}, {"Watery Hall", "Dynamo Access"}}, {"Arboretum", "Sunchamber Lobby", "Save Station 1", "Save Station 2", "Burn Dome", "Ruined Fountain"}},
      {1, "Energy Core", true, {{"Gathering Hall", "East Atrium"}, {"Energy Core Access", "Energy Core"}}, {"Arboretum", "Sunchamber Lobby", "Save Station 1", "Save Station 2", "Burn Dome", "Ruined Fountain"}},
      {1, "Burn Dome", false, {{"Burn Dome Access", "Burn Dome"}}, {"Burn Dome", "Arboretum"}},
      {1, "Furnace", true, {{"Furnace", "East Furnace Access"}}, {}},
      {1, "Crossway", false, {{"Crossway Access West", "Crossway"}}, {}},
      {1, "Elder Hall Access", false, {{"Crossway", "Elder Hall Access"}}, {}},
      {1, "Hall of the Elders", false, {{"Hall of the Elders", "Reflecting Pool Access"}}, {}},
      {1, "Reflecting Pool", false, {{"Antechamber", "Reflecting Pool"}, {"Save Station 3", "Reflecting Pool"}, {"Transport Access South", "Reflecting Pool"}}, {}},
      {2, "Lava Lake", false, {{"Transport to Chozo Ruins North", "Burning Trail"}, {"Lake Tunnel", "Lava Lake"}}, {"Warrior Shrine"}},
      {2, "Pit Tunnel", false, {{"Lava Lake", "Pit Tunnel"}}, {"Warrior Shrine"}},
      {2, "Storage Cavern", false, {{"Triclops Pit", "Storage Cavern"}}, {"Warrior Shrine"}},
      {2, "Transport to Phendrana Drifts North", false, {{"Monitor Station", "Transport Tunnel A"}, {"Transport to Phendrana Drifts North", "Transport Tunnel A"}}, {}},
      {2, "Warrior Shrine", false, {{"Monitor Station", "Warrior Shrine"}}, {"Warrior Shrine"}},
      {2, "Transport Tunnel B", false, {{"Fiery Shores", "Transport Tunnel B"}}, {"Warrior Shrine"}},
      {2, "Geothermal Core", false, {{"Transport to Tallon Overworld West", "Twin Fires Tunnel"}, {"Twin Fires Tunnel", "Twin Fires"}, {"North Core Tunnel", "Geothermal Core"}}, {}},
      {2, "Magmoor Workstation", false, {{"South Core Tunnel", "Magmoor Workstation"}}, {}},
      {2, "Transport Tunnel C", false, {{"Magmoor Workstation", "Transport Tunnel C"}}, {}},
      {2, "Workstation Tunnel", false, {{"Magmoor Workstation", "Workstation Tunnel"}}, {}},
      {3, "Shoreline Entrance", false, {{"Phendrana Shorelines", "Shoreline Entrance"}}, {}},
      {3, "Temple Entryway", false, {{"Phendrana Shorelines", "Temple Entryway"}}, {"Save Station B", "East Tower", "Quarantine Monitor"}},
      {3, "Ice Ruins East", true, {{"Ice Ruins Access", "Phendrana Shorelines"}, {"Plaza Walkway", "Phendrana Shorelines"}}, {"Save Station B", "East Tower", "Quarantine Monitor"}},
      {3, "Ice Ruins West", false, {{"Phendrana Shorelines", "Ruins Entryway"}, {"Ruins Entryway", "Ice Ruins West"}}, {"Save Station B", "East Tower", "Quarantine Monitor"}},
      {3, "Phendrana Canyon", false, {{"Ice Ruins West", "Canyon Entryway"}}, {"Save Station B", "East Tower", "Quarantine Monitor"}},
      {3, "Ruined Courtyard", false, {{"Ice Ruins West", "Courtyard Entryway"}}, {"Save Station B", "East Tower", "Quarantine Monitor"}},
      {3, "Quarantine Access", true, {{"Ruined Courtyard", "Quarantine Access"}}, {"Save Station B", "East Tower", "Quarantine Monitor"}},
      {3, "Specimen Storage", true, {{"Ruined Courtyard", "Specimen Storage"}}, {"Save Station B", "East Tower", "Quarantine Monitor"}},
      {3, "Quarantine Cave", true, {{"Quarantine Cave", "South Quarantine Tunnel"}}, {"Save Station B", "East Tower", "Quarantine Monitor"}},
      {3, "Research Lab Hydra", true, {{"Research Lab Hydra", "Observatory Access"}, {"Observatory", "West Tower Entrance"}, {"Control Tower", "East Tower"}, {"Research Lab Aether", "Research Core Access"}, {"Research Core", "Pike Access"}}, {"Save Station B", "East Tower", "Quarantine Monitor"}},
      {3, "Transport to Magmoor Caverns South", false, {{"Transport to Magmoor Caverns South", "Transport Access"}}, {"Quarantine Monitor"}},
      {3, "Frost Cave", true, {{"Frozen Pike", "Frost Cave Access"}, {"Frost Cave", "Upper Edge Tunnel"}, {"Hunter Cave", "Lower Edge Tunnel"}}, {"Quarantine Monitor"}},
      {4, "Quarry Access", false, {{"Main Quarry", "Quarry Access"}}, {}},
      {4, "Waste Disposal", false, {{"Main Quarry", "Waste Disposal"}}, {}},
      {4, "Elite Research", false, {{"Main Quarry", "Security Access A"}, {"Security Access B", "Elite Research"}}, {}},
      {4, "Ore Processing", true, {{"Research Access", "Ore Processing"}}, {}},
      {4, "Elevator A", true, {{"Ore Processing", "Elevator Access A"}, {"Elevator Access A", "Elevator A"}}, {}},
      {4, "Storage Depot B", false, {{"Ore Processing", "Storage Depot B"}}, {}},
      {4, "Elite Control", true, {{"Elite Control Access", "Elite Control"}}, {}},
      {4, "Maintenance Tunnel", true, {{"Elite Control", "Maintenance Tunnel"}}, {}},
      {4, "Ventilation Shaft", true, {{"Elite Control", "Ventilation Shaft"}}, {}},
      {4, "Metroid Quarantine A", true, {{"Central Dynamo", "Quarantine Access A"}, {"Quarantine Access A", "Metroid Quarantine A"}, {"Elevator Access B", "Metroid Quarantine A"}}, {}},
      {4, "Metroid Quarantine B", true, {{"Fungal Hall Access", "Fungal Hall A"}, {"Fungal Hall A", "Phazon Mining Tunnel"}, {"Fungal Hall B", "Quarantine Access B"}, {"Metroid Quarantine B", "Elite Quarters Access"}}, {}},
      {4, "Elite Quarters", true, {{"Elite Quarters", "Processing Center Access"}}, {}},
      {4, "Phazon Processing Center", true, {{"Phazon Processing Center", "Maintenance Tunnel"}}, {}},
  };
  return kRegions;
}

using ShieldMap = std::map< std::string, std::map< std::string, std::map< int, std::string > > >;

bool IsBeamCombo(const std::string& type) {
  return type == "Flamethrower" || type == "Ice Spreader" || type == "Wavebuster";
}

// get_world_blast_shield_mapping: shields per area, and locked_door_count
// areas with one door locked for good. Every area that takes part has a
// mapping, empty or not.
bool RandomShields(const Settings& s, const StartRoom& start, Rng& rng, ShieldMap& out, std::string& why) {
  static const char* const kTypes[] = {"Bomb",       "Charge Beam", "Flamethrower",  "Ice Spreader",
                                       "Wavebuster", "Power Bomb",  "Super Missile", "Missile"};
  std::vector< int > locked;
  if (s.lockedDoorCount > 0) {
    std::vector< int > areas = {0, 1, 3, 4};
    rng.Shuffle(areas);
    locked.assign(areas.begin(), areas.begin() + std::min< std::ptrdiff_t >(s.lockedDoorCount, 4));
  }
  std::vector< int > areas = locked;
  if (s.blastShieldRandomization != 0)
    areas = {0, 1, 2, 3, 4};

  for (const int area : areas) {
    auto& mapping = out[kAreas[area]];
    std::vector< const ShieldRegion* > regions;
    std::vector< const ShieldRegion* > startRegions;
    for (const ShieldRegion& region : ShieldRegions()) {
      if (region.area != area)
        continue;
      const bool invalid =
          area == start.area && std::any_of(region.invalidStarts.begin(), region.invalidStarts.end(),
                                            [&start](const char* name) { return std::string(name) == start.name; });
      (invalid ? startRegions : regions).push_back(&region);
    }
    int combos = 0;
    const auto pickType = [&]() {
      std::vector< std::string > types;
      for (const char* type : kTypes) {
        if (s.blastShieldRandomization == 1 && std::string(type) == "Missile")
          continue;
        if (IsBeamCombo(type) && (s.blastShieldAvailableTypes != 1 || combos >= 1))
          continue;
        types.push_back(type);
      }
      const std::string type = rng.Pick(types);
      combos += IsBeamCombo(type) ? 1 : 0;
      return type;
    };
    // A random door of the region; `type` null for a random shield.
    const auto place = [&](const ShieldRegion& region, const char* type) {
      const auto& door = rng.Pick(region.doors);
      const int dock = PortApWorld::DockTo(kAreas[area], door.first, door.second);
      const std::string chosen = type != nullptr ? std::string(type) : pickType();
      if (dock < 0) {
        why = std::string("no door from ") + door.first + " to " + door.second;
        return false;
      }
      mapping[door.first][dock] = chosen;
      return true;
    };
    if (s.blastShieldRandomization == 2) {
      std::vector< const ShieldRegion* > shuffled = regions;
      rng.Shuffle(shuffled);
      const size_t count =
          std::min(shuffled.size(), (static_cast< size_t >(s.blastShieldFrequency) * shuffled.size() + 9) / 10);
      for (size_t i = 0; i < count; ++i) {
        if (!place(*shuffled[i], nullptr))
          return false;
      }
    } else if (s.blastShieldRandomization == 1) {
      for (const auto& door : PortApWorld::DiscShields(kAreas[area])) {
        // The apworld's door data has no shield between Dynamo Access and
        // Dynamo, so its replace_existing leaves that pair as on the disc.
        if (door.first == "Dynamo" || (door.first == "Dynamo Access" && door.second == 1))
          continue;
        // The apworld replaces the start's own exits too, which closes Save
        // Station 1 for good (its prefilled Morph Ball or Bombs sit behind
        // Main Plaza's shields). The doors of the regions mix_it_up leaves
        // alone for this start keep their Missile shield.
        const bool kept = std::any_of(startRegions.begin(), startRegions.end(), [&](const ShieldRegion* region) {
          return std::any_of(region->doors.begin(), region->doors.end(), [&](const auto& pair) {
            return (pair.first == door.first &&
                    PortApWorld::DockTo(kAreas[area], pair.first, pair.second) == door.second) ||
                   (pair.second == door.first &&
                    PortApWorld::DockTo(kAreas[area], pair.second, pair.first) == door.second);
          });
        });
        if (!kept)
          mapping[door.first][door.second] = pickType();
      }
    }
    if (std::find(locked.begin(), locked.end(), area) != locked.end()) {
      for (const ShieldRegion* region : regions) {
        if (region->lockable) {
          if (!place(*region, "Disabled"))
            return false;
          break;
        }
      }
    }
  }
  return true;
}

PortApWorld::Layout MakeLayout(const Settings& s, const Start& start, const ElevatorMap& elevators,
                               const ElevatorMap& colors, const ShieldMap& shields) {
  PortApWorld::Layout layout;
  layout.startRoom = start.room->name;
  layout.finalBosses = s.finalBosses;
  layout.requiredArtifacts = s.requiredArtifacts;
  layout.elevators = elevators;
  layout.doorColorRandomization = s.doorColorRandomization != 0;
  layout.hasDoorColors = !colors.empty();
  layout.doorColors = colors;
  layout.hasShields = s.blastShieldRandomization != 0 || s.lockedDoorCount > 0;
  layout.shields = shields;
  // Without the Power Beam the Hive Mecha can't be fought from these starts.
  const std::string room = start.room->name;
  layout.removeHiveMecha =
      s.removeHiveMecha || ((room == "Landing Site" || room == "Save Station 1") && start.beam != kPower);
  layout.backwardsLowerMines = s.backwardsLowerMines;
  layout.flaahgraPowerBombs = s.flaahgraPowerBombs;
  layout.etankCapacity = s.etankCapacity;
  return layout;
}

PortApLogic::Options LogicFor(const Settings& s, const PortApWorld::Layout& layout) {
  PortApLogic::Options options;
  options.trickDifficulty = s.trickDifficulty;
  options.combatLogic = s.combatLogic;
  options.removeXray = s.removeXray;
  options.removeThermal = s.removeThermal;
  options.flaahgraPowerBombs = s.flaahgraPowerBombs;
  options.progressiveBeams = s.progressiveBeams;
  options.mainMissile = s.missileLauncher;
  options.mainPowerBomb = s.mainPowerBomb;
  options.variaOnlyHeat = s.nonVariaHeatDamage;
  options.preScanElevators = s.preScanElevators;
  options.trickAllow = s.trickAllow;
  options.trickDeny = s.trickDeny;
  PortApWorld::FillLogic(layout, options);
  return options;
}

// ---------------------------------------------------------------------------
// Items (ItemPool.py).

struct PoolItem {
  int id;
  bool progression;
};

int BeamItem(const Settings& s, int beam) {
  if (!s.progressiveBeams)
    return beam;
  switch (beam) {
  case kPower: return kProgPower;
  case kIce: return kProgIce;
  case kWave: return kProgWave;
  default: return kProgPlasma;
  }
}

int LauncherItem(const Settings& s) { return s.missileLauncher ? kLauncher : kMissileExp; }
int MainPbItem(const Settings& s) { return s.mainPowerBomb ? kMainPb : kPbExp; }

// get_item_for_options for a loadout item.
int MapItem(const Settings& s, int item) {
  switch (item) {
  case kLauncher: return LauncherItem(s);
  case kMainPb: return MainPbItem(s);
  case kPower:
  case kWave:
  case kIce:
  case kPlasma: return BeamItem(s, item);
  default: return item;
  }
}

// The start loadout's prefill: location name -> item, from one of its rule
// sets picked at random (StartRoomData.py). Only with bk prevention.
std::vector< std::pair< const char*, int > > PickPrefill(const Settings& s, const Start& start, Rng& rng) {
  std::vector< std::pair< const char*, int > > out;
  if (!start.bkPrevention || start.loadout.rules.empty())
    return out;
  for (const ItemRule& rule : rng.Pick(start.loadout.rules))
    out.emplace_back(rule.location, MapItem(s, rng.Pick(rule.choices)));
  return out;
}

// generate_base_start_inventory.
std::vector< int > StartInventory(const Settings& s, const Start& start) {
  std::vector< int > items = {BeamItem(s, start.beam)};
  for (const int item : start.loadout.items)
    items.push_back(MapItem(s, item));
  if (!s.shuffleScanVisor)
    items.push_back(kScan);
  return items;
}

std::vector< PoolItem > BuildPool(const Settings& s, const std::vector< int >& prefilled,
                                  const std::vector< int >& start) {
  std::vector< PoolItem > items;
  for (int artifact = kArtifactFirst; artifact <= kArtifactLast; ++artifact)
    items.push_back({artifact, true});
  for (const int id : {kMorph, kBomb, kThermal, kXray, kScan, kGrapple, kSpace, kSpider, kBoost, kVaria,
                       kGravity, kPhazon})
    items.push_back({id, true});
  for (int i = 0; i < 8; ++i)
    items.push_back({kMissileExp, true});
  items.push_back({LauncherItem(s), true});
  for (int i = 0; i < 4; ++i)
    items.push_back({kPbExp, false});
  items.push_back({MainPbItem(s), true});
  for (int i = 0; i < 14; ++i)
    items.push_back({kEtank, i < 8});
  if (s.progressiveBeams) {
    for (const int id : {kProgPower, kProgIce, kProgWave, kProgPlasma}) {
      for (int i = 0; i < 3; ++i)
        items.push_back({id, true});
    }
  } else {
    for (const int id : {kPower, kWave, kIce, kPlasma, kCharge, kSuper, kWavebuster, kIceSpreader, kFlamethrower})
      items.push_back({id, true});
  }
  if (s.shuffleUnlimitedMissiles)
    items.push_back({kUnlimitedMissiles, false});
  if (s.shuffleUnlimitedPowerBombs)
    items.push_back({kUnlimitedPowerBombs, false});
  const auto remove = [&items](int id) {
    for (size_t i = 0; i < items.size(); ++i) {
      if (items[i].id == id) {
        items.erase(items.begin() + static_cast< std::ptrdiff_t >(i));
        return;
      }
    }
  };
  for (const int id : prefilled)
    remove(id);
  for (const int id : start)
    remove(id);
  while (items.size() + prefilled.size() < kLocationCount)
    items.push_back({kMissileExp, false});
  return items;
}

// ---------------------------------------------------------------------------
// Reachability.

using Counts = std::array< int, kItemSlots >;

PortApLogic::Items ToItems(const Counts& counts) {
  PortApLogic::Items items;
  for (size_t i = 0; i < counts.size(); ++i) {
    if (counts[i] > 0)
      items[MP::kItemBase + static_cast< int64_t >(i)] = counts[i];
  }
  return items;
}

// Collects what the player can reach from `base`, picking up the items in
// `placed` (-1 for an empty location) as their locations come in logic.
// `reached` marks every location in logic; `spheres` (optional) lists the
// locations of each sweep.
void Closure(const PortApLogic::Options& options, Counts have, const std::vector< int >& placed,
             std::vector< char >& reached, Counts* finalItems = nullptr,
             std::vector< std::vector< int > >* spheres = nullptr) {
  reached.assign(kLocationCount, 0);
  for (;;) {
    const std::vector< PortApLogic::Level > levels = PortApLogic::Evaluate(options, ToItems(have));
    std::vector< int > sphere;
    for (size_t i = 0; i < kLocationCount; ++i) {
      if (reached[i] || levels[i] != PortApLogic::Level::Normal)
        continue;
      reached[i] = 1;
      sphere.push_back(static_cast< int >(i));
    }
    if (sphere.empty())
      break;
    for (const int location : sphere) {
      if (placed[static_cast< size_t >(location)] >= 0)
        ++have[static_cast< size_t >(placed[static_cast< size_t >(location)])];
    }
    if (spheres != nullptr)
      spheres->push_back(sphere);
  }
  if (finalItems != nullptr)
    *finalItems = have;
}

// Regions.py's Mission Complete rule, from the items alone (the Artifact
// Temple being in logic is checked by the caller).
bool CanComplete(const Settings& s, const Counts& have) {
  const auto has = [&have](int id) { return have[static_cast< size_t >(id)] > 0; };
  const auto count = [&have](int id) { return have[static_cast< size_t >(id)]; };
  int artifacts = 0;
  for (int id = kArtifactFirst; id <= kArtifactLast; ++id)
    artifacts += has(id) ? 1 : 0;
  if (artifacts < s.requiredArtifacts)
    return false;
  const bool missile = s.missileLauncher ? has(kLauncher) : has(kMissileExp);
  if (!missile)
    return false;
  if (s.finalBosses == 3)
    return true;
  const bool power = has(kPower) || has(kProgPower);
  const bool ice = has(kIce) || has(kProgIce);
  const bool wave = has(kWave) || has(kProgWave);
  const bool plasma = has(kPlasma) || has(kProgPlasma);
  const bool charge = has(kCharge) || count(kProgPower) >= 2 || count(kProgIce) >= 2 || count(kProgWave) >= 2 ||
                      count(kProgPlasma) >= 2;
  const auto combat = [&](int normalTanks, int minimalTanks) {
    if (s.combatLogic < 0)
      return true;
    return count(kEtank) >= (s.combatLogic == 0 ? normalTanks : minimalTanks) && charge;
  };
  const bool ridley = combat(8, 8);
  if (s.finalBosses == 1) {
    const bool superMissile = power && missile && ((has(kCharge) && has(kSuper)) || count(kProgPower) >= 3);
    return (plasma || superMissile) && ridley;
  }
  const bool prime = combat(8, 5);
  const bool xray = s.removeXray == 2 || has(kXray);
  const bool thermal = s.removeThermal == 2 || has(kThermal);
  return prime && ridley && has(kPhazon) && plasma && wave && ice && power && xray && thermal;
}

// ---------------------------------------------------------------------------
// Output.

void AppendNames(port::JsonWriter& json, const char* key, const std::vector< std::string >& names) {
  json.Key(key).BeginArray();
  for (const std::string& name : names)
    json.String(name);
  json.EndArray();
}

// area -> name -> name, with the area repeated inside each area's object as
// the apworld writes door_color_mapping.
void AppendMapping(port::JsonWriter& json, const ElevatorMap& mapping, bool withArea) {
  for (const auto& [area, entries] : mapping) {
    json.Key(area).BeginObject();
    if (withArea)
      json.Key("area").String(area).Key("type_mapping").BeginObject();
    for (const auto& [from, to] : entries)
      json.Key(from).String(to);
    if (withArea)
      json.EndObject();
    json.EndObject();
  }
}

const char* ItemLabel(int64_t id) {
  const char* name = MP::ItemName(id);
  return name != nullptr ? name : "?";
}

struct Attempt {
  PortApWorld::Layout layout;
  std::vector< int > placed; // location index -> item offset
  std::vector< int > start;
  int beam = kPower;
  bool beamReplaced = false;
  std::vector< std::vector< int > > spheres;
  Counts finalItems{};
};

std::string SlotData(const Settings& s, const Attempt& a, const std::vector< int64_t >& locationIds) {
  port::JsonWriter json;
  const auto flag = [](bool value) { return value ? 1 : 0; };
  json.BeginObject();
  json.Key("missile_launcher").Number(flag(s.missileLauncher));
  json.Key("main_power_bomb").Number(flag(s.mainPowerBomb));
  json.Key("progressive_beam_upgrades").Number(flag(s.progressiveBeams));
  json.Key("shuffle_scan_visor").Number(flag(s.shuffleScanVisor));
  json.Key("pre_scan_elevators").Number(flag(s.preScanElevators));
  json.Key("elevator_randomization").Number(flag(s.elevatorRandomization));
  json.Key("door_color_randomization").Number(s.doorColorRandomization);
  json.Key("trick_difficulty").Number(s.trickDifficulty);
  json.Key("combat_logic_difficulty").Number(s.combatLogic);
  json.Key("remove_xray_requirements").Number(s.removeXray);
  json.Key("remove_thermal_requirements").Number(s.removeThermal);
  json.Key("flaahgra_power_bombs").Number(flag(s.flaahgraPowerBombs));
  json.Key("etank_capacity").Number(s.etankCapacity);
  json.Key("non_varia_heat_damage").Number(flag(s.nonVariaHeatDamage));
  json.Key("staggered_suit_damage").Number(s.staggeredSuitDamage);
  json.Key("backwards_lower_mines").Number(flag(s.backwardsLowerMines));
  json.Key("remove_hive_mecha").Number(flag(a.layout.removeHiveMecha));
  json.Key("spring_ball").Number(flag(s.springBall));
  json.Key("shuffle_unlimited_missiles").Number(flag(s.shuffleUnlimitedMissiles));
  json.Key("shuffle_unlimited_power_bombs").Number(flag(s.shuffleUnlimitedPowerBombs));
  json.Key("required_artifacts").Number(s.requiredArtifacts);
  json.Key("final_bosses").Number(s.finalBosses);
  json.Key("artifact_hints").Number(flag(s.artifactHints));
  json.Key("starting_room").Number(s.startingRoom);
  json.Key("randomize_starting_beam").Number(flag(s.randomizeStartingBeam));
  json.Key("blast_shield_randomization").Number(s.blastShieldRandomization);
  json.Key("blast_shield_frequency").Number(s.blastShieldFrequency);
  json.Key("blast_shield_available_types").Number(s.blastShieldAvailableTypes);
  json.Key("locked_door_count").Number(s.lockedDoorCount);
  json.Key("include_power_beam_doors").Number(flag(s.includePowerBeamDoors));
  json.Key("include_morph_ball_bomb_doors").Number(flag(s.includeMorphBallBombDoors));
  if (a.beamReplaced)
    json.Key("starting_beam").String(ItemLabel(MP::kItemBase + a.beam)); // a SuitUpgrade, not a lock
  AppendNames(json, "trick_allow_list", s.trickAllow);
  AppendNames(json, "trick_deny_list", s.trickDeny);
  json.Key("starting_room_name").String(a.layout.startRoom);
  json.Key("elevator_mapping").BeginObject();
  AppendMapping(json, a.layout.elevators, false);
  json.EndObject();
  if (a.layout.hasDoorColors) {
    json.Key("door_color_mapping").BeginObject();
    AppendMapping(json, a.layout.doorColors, true);
    json.EndObject();
  }
  if (a.layout.hasShields) {
    // area -> {area, type_mapping: room -> dock -> blast shield}
    json.Key("blast_shield_mapping").BeginObject();
    for (const auto& [area, rooms] : a.layout.shields) {
      json.Key(area).BeginObject().Key("area").String(area).Key("type_mapping").BeginObject();
      for (const auto& [room, docks] : rooms) {
        json.Key(room).BeginObject();
        for (const auto& [dock, shield] : docks)
          json.Key(std::to_string(dock)).String(shield);
        json.EndObject();
      }
      json.EndObject().EndObject();
    }
    json.EndObject();
  }
  if (s.artifactHints) {
    // artifact -> [location id, player]
    json.Key("artifact_locations").BeginObject();
    for (int artifact = kArtifactFirst; artifact <= kArtifactLast; ++artifact) {
      for (size_t i = 0; i < kLocationCount; ++i) {
        if (a.placed[i] != artifact)
          continue;
        json.Key(ItemLabel(MP::kItemBase + artifact)).BeginArray().Number(locationIds[i]).Number(1).EndArray();
      }
    }
    json.EndObject();
  }
  json.EndObject();
  return json.Take();
}

std::string Spoiler(const std::string& name, const Settings& s, const Attempt& a, const MP::Location* locations) {
  std::ostringstream text;
  text << "Metroid Prime seed " << name << "\n\nStart: " << a.layout.startRoom << "\nStarting items:";
  for (const int id : a.start)
    text << ' ' << ItemLabel(MP::kItemBase + id) << ';';
  text << '\n';
  if (a.beamReplaced)
    text << "Starting beam: " << ItemLabel(MP::kItemBase + a.beam) << '\n';
  if (s.elevatorRandomization) {
    text << "\nElevators:\n";
    for (const auto& area : a.layout.elevators) {
      for (const auto& entry : area.second)
        text << "  " << area.first << ": " << entry.first << " -> " << entry.second << '\n';
    }
  }
  if (a.layout.hasDoorColors) {
    text << "\nDoor colours:\n";
    for (const auto& area : a.layout.doorColors) {
      text << "  " << area.first << ':';
      for (const auto& entry : area.second)
        text << ' ' << entry.first << " -> " << entry.second << ';';
      text << '\n';
    }
  }
  if (a.layout.hasShields) {
    text << "\nBlast shields:\n";
    for (const auto& area : a.layout.shields) {
      for (const auto& room : area.second) {
        for (const auto& entry : room.second)
          text << "  " << area.first << ": " << room.first << " door " << entry.first << ": " << entry.second << '\n';
      }
    }
  }
  text << "\nPlaythrough:\n";
  for (size_t sphere = 0; sphere < a.spheres.size(); ++sphere) {
    bool header = false;
    for (const int location : a.spheres[sphere]) {
      const int item = a.placed[static_cast< size_t >(location)];
      // Expansions and tanks never gate anything the spoiler needs to show.
      if (item == kMissileExp || item == kPbExp || item == kEtank)
        continue;
      if (!header)
        text << "  Sphere " << sphere << ":\n";
      header = true;
      text << "    " << locations[location].name << ": " << ItemLabel(MP::kItemBase + item) << '\n';
    }
  }
  text << "\nLocations:\n";
  for (size_t i = 0; i < kLocationCount; ++i)
    text << "  " << locations[i].name << ": " << ItemLabel(MP::kItemBase + a.placed[i]) << '\n';
  return text.str();
}

// ---------------------------------------------------------------------------
// One attempt: layout, assumed fill, independent check.

bool TryOnce(const Settings& s, Rng& rng, Attempt& out, std::string& why) {
  size_t locationCount = 0;
  const MP::Location* locations = MP::Locations(locationCount);
  if (locationCount != kLocationCount) {
    why = "unexpected location table";
    return false;
  }

  // In the apworld's order: start room, door colours, start beam, shields,
  // elevators.
  Start start = PickStart(s, rng);
  ElevatorMap colors = RandomDoorColors(s, *start.room, rng);
  PickBeam(s, colors, start, rng);
  RemapPowerDoors(s, start, colors);
  const auto prefill = PickPrefill(s, start, rng);
  ShieldMap shields;
  if ((s.blastShieldRandomization != 0 || s.lockedDoorCount > 0) &&
      !RandomShields(s, *start.room, rng, shields, why))
    return false;
  ElevatorMap elevators;
  if (s.elevatorRandomization) {
    if (!RandomElevators(rng, start.room->area, start.room->elevators, elevators)) {
      why = "elevator layout stranded an area";
      return false;
    }
  } else {
    elevators = DefaultElevators();
  }
  out.layout = MakeLayout(s, start, elevators, colors, shields);
  out.beam = start.beam;
  out.beamReplaced = start.beamReplaced;
  const PortApLogic::Options options = LogicFor(s, out.layout);

  // The start inventory and the prefilled locations; a prefilled item the
  // start inventory already has is left to the fill.
  out.start = StartInventory(s, start);
  out.placed.assign(kLocationCount, -1);
  std::vector< int > prefilledItems;
  for (const auto& entry : prefill) {
    if (std::find(out.start.begin(), out.start.end(), entry.second) != out.start.end())
      continue;
    size_t i = 0;
    while (i < kLocationCount && std::string(locations[i].name) != entry.first)
      ++i;
    if (i == kLocationCount) {
      why = std::string("unknown prefill location ") + entry.first;
      return false;
    }
    out.placed[i] = entry.second;
    prefilledItems.push_back(entry.second);
  }
  std::vector< PoolItem > pool = BuildPool(s, prefilledItems, out.start);

  Counts base{};
  for (const int id : out.start)
    ++base[static_cast< size_t >(id)];
  std::vector< char > reached;

  // Every location must be reachable with every item in hand; a layout that
  // fails this can't be saved by any placement.
  {
    Counts everything = base;
    for (const PoolItem& item : pool)
      ++everything[static_cast< size_t >(item.id)];
    for (const int id : prefilledItems)
      ++everything[static_cast< size_t >(id)];
    const std::vector< int > nothing(kLocationCount, -1);
    Closure(options, everything, nothing, reached);
    if (std::count(reached.begin(), reached.end(), 1) != static_cast< std::ptrdiff_t >(kLocationCount)) {
      why = "layout leaves a location out of logic";
      return false;
    }
  }

  std::vector< int > progression;
  std::vector< int > rest;
  for (const PoolItem& item : pool)
    (item.progression ? progression : rest).push_back(item.id);
  rng.Shuffle(progression);

  // Items placed early, in a location open from the start: the start room's
  // local_early items (with bk prevention), and the Scan Visor when the
  // elevators need it. Prefilled locations count as open, they're placed already.
  std::vector< int > early;
  if (start.bkPrevention) {
    for (const int item : start.room->localEarly)
      early.push_back(MapItem(s, item));
  }
  if (s.shuffleScanVisor && !s.preScanElevators)
    early.push_back(kScan);
  for (size_t e = 0; e < early.size(); ++e) {
    const int item = early[e];
    if (std::find(early.begin(), early.begin() + static_cast< std::ptrdiff_t >(e), item) !=
        early.begin() + static_cast< std::ptrdiff_t >(e))
      continue;
    const auto it = std::find(progression.begin(), progression.end(), item);
    if (it == progression.end())
      continue;
    progression.erase(it);
    Closure(options, base, out.placed, reached);
    std::vector< int > candidates;
    for (size_t i = 0; i < kLocationCount; ++i) {
      if (reached[i] && out.placed[i] < 0)
        candidates.push_back(static_cast< int >(i));
    }
    if (candidates.empty()) {
      // Nothing free opens before some other item (the prefill holds all of
      // sphere 0, or shields close the start): the item is placed like the
      // rest, as AP's early fill does.
      progression.push_back(item);
    } else {
      out.placed[static_cast< size_t >(rng.Pick(candidates))] = item;
    }
  }

  // Assumed fill: each progression item goes where the player could stand
  // holding every progression item still unplaced. Every such set holds
  // sphere 0, so one free sphere-0 location is kept back until it's the only
  // choice: a small sphere 0 (blast shields, buckle-up starts) otherwise
  // fills up before the last items.
  Closure(options, base, out.placed, reached);
  std::vector< int > sphere0;
  for (size_t i = 0; i < kLocationCount; ++i) {
    if (reached[i] && out.placed[i] < 0)
      sphere0.push_back(static_cast< int >(i));
  }
  if (sphere0.empty() && !progression.empty()) {
    // Shields or locked doors can close every way out of a start: no
    // placement saves it, so fail before the fill.
    why = "the start reaches no open location";
    return false;
  }
  const int reserve = sphere0.empty() ? -1 : rng.Pick(sphere0);
  Counts assumed = base;
  for (const int id : progression)
    ++assumed[static_cast< size_t >(id)];
  for (const int item : progression) {
    --assumed[static_cast< size_t >(item)];
    Closure(options, assumed, out.placed, reached);
    std::vector< int > candidates;
    for (size_t i = 0; i < kLocationCount; ++i) {
      if (reached[i] && out.placed[i] < 0 && static_cast< int >(i) != reserve)
        candidates.push_back(static_cast< int >(i));
    }
    if (candidates.empty() && reserve >= 0)
      candidates.push_back(reserve);
    if (candidates.empty()) {
      why = "no reachable location for a progression item";
      return false;
    }
    out.placed[static_cast< size_t >(rng.Pick(candidates))] = item;
  }

  // The rest anywhere.
  std::vector< int > open;
  for (size_t i = 0; i < kLocationCount; ++i) {
    if (out.placed[i] < 0)
      open.push_back(static_cast< int >(i));
  }
  rng.Shuffle(open);
  rng.Shuffle(rest);
  if (open.size() != rest.size()) {
    why = "pool does not fill the locations";
    return false;
  }
  for (size_t i = 0; i < open.size(); ++i)
    out.placed[static_cast< size_t >(open[i])] = rest[i];

  // The check proper: a walk from the start inventory over the final placements.
  out.spheres.clear();
  Closure(options, base, out.placed, reached, &out.finalItems, &out.spheres);
  if (std::count(reached.begin(), reached.end(), 1) != static_cast< std::ptrdiff_t >(kLocationCount)) {
    why = "a location is out of reach in the final walk";
    return false;
  }
  if (!CanComplete(s, out.finalItems)) {
    why = "the final inventory can't finish the game";
    return false;
  }
  return true;
}

std::string RandomName() {
  std::random_device device;
  char text[16];
  std::snprintf(text, sizeof(text), "%08x", static_cast< unsigned >(device()));
  return text;
}

} // namespace

bool Generate(const Settings& settings, const std::string& seedText, Seed& out, std::string& error) {
  Settings s = settings;
  s.requiredArtifacts = std::clamp(s.requiredArtifacts, 1, 12);
  s.finalBosses = std::clamp(s.finalBosses, 0, 3);
  s.doorColorRandomization = std::clamp(s.doorColorRandomization, 0, 2);
  s.combatLogic = std::clamp(s.combatLogic, -1, 1);
  s.etankCapacity = std::clamp(s.etankCapacity, 10, 200);
  s.trickDifficulty = std::clamp(s.trickDifficulty, -1, 2);
  s.removeXray = std::clamp(s.removeXray, 0, 2);
  s.removeThermal = std::clamp(s.removeThermal, 0, 2);
  s.startingRoom = std::clamp(s.startingRoom, 0, 2);
  s.blastShieldRandomization = std::clamp(s.blastShieldRandomization, 0, 2);
  s.blastShieldFrequency = std::clamp(s.blastShieldFrequency, 1, 6);
  s.blastShieldAvailableTypes = std::clamp(s.blastShieldAvailableTypes, 0, 1);
  s.lockedDoorCount = std::clamp(s.lockedDoorCount, 0, 2);

  const std::string name = seedText.empty() ? RandomName() : seedText;
  const uint64_t hash = HashInput(s, name);
  size_t locationCount = 0;
  const MP::Location* locations = MP::Locations(locationCount);
  std::vector< int64_t > locationIds;
  for (size_t i = 0; i < locationCount; ++i)
    locationIds.push_back(locations[i].id);

  std::string why;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    Rng rng(hash + static_cast< uint64_t >(attempt) * 0x9E3779B97F4A7C15ull);
    Attempt result;
    if (!TryOnce(s, rng, result, why))
      continue;
    out = Seed();
    out.name = name;
    out.settings = s;
    for (const int id : result.start)
      out.startItems.push_back(MP::kItemBase + id);
    for (size_t i = 0; i < locationCount; ++i)
      out.placements[locationIds[i]] = MP::kItemBase + result.placed[i];
    out.slotData = SlotData(s, result, locationIds);
    out.spoiler = Spoiler(name, s, result, locations);
    error.clear();
    return true;
  }
  error = "no beatable seed found in " + std::to_string(kMaxAttempts) + " attempts (" + why + ")";
  return false;
}

} // namespace PortRandoGen
