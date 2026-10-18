#ifndef METROID_PRIME_PORT_PORT_RANDO_GEN_H
#define METROID_PRIME_PORT_PORT_RANDO_GEN_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The built-in randomizer: makes a Metroid Prime seed from settings, with the
// MetroidAPrime apworld's options and item pool and the tracker's logic
// (PortApLogic), and plays it as a one-player Archipelago game through an
// in-process server (PortApSolo), so every AP patch applies unchanged.
namespace PortRandoGen {

// The apworld's options (PrimeOptions.py), with its values. Cosmetic options
// are not offered.
struct Settings {
  int requiredArtifacts = 12;      // 1..12
  int finalBosses = 0;             // 0 both, 1 Ridley, 2 Prime, 3 none
  bool artifactHints = true;       // totems name where each artifact is
  bool missileLauncher = false;    // missiles need the Missile Launcher
  bool mainPowerBomb = false;      // power bombs need the main Power Bomb
  bool shuffleScanVisor = false;
  bool preScanElevators = true;
  bool elevatorRandomization = false;
  int doorColorRandomization = 0;  // 0 none, 1 global, 2 regional
  bool progressiveBeams = false;
  bool nonVariaHeatDamage = true;
  int staggeredSuitDamage = 1;     // 0 default, 1 progressive, 2 additive
  int combatLogic = 0;             // -1 none, 0 normal, 1 minimal (PortApLogic::Options)
  int trickDifficulty = -1;        // -1 none, 0 easy, 1 medium, 2 hard
  std::vector< std::string > trickAllow;
  std::vector< std::string > trickDeny;
  bool flaahgraPowerBombs = false;
  int etankCapacity = 100;         // 10..200, energy per tank; the suit holds one less
  bool backwardsLowerMines = false;
  int removeXray = 0;              // 0 none, 1 most, 2 all but Omega Pirate
  int removeThermal = 0;           // 0 none, 1 most, 2 all
  bool removeHiveMecha = false;
  bool springBall = true;
  bool shuffleUnlimitedMissiles = false;    // Unlimited Missiles is an item in the pool
  bool shuffleUnlimitedPowerBombs = false;  // Unlimited Power Bombs is an item in the pool
  int startingRoom = 0;            // 0 normal, 1 safe, 2 buckle up (a random room and loadout)
  bool randomizeStartingBeam = false;
  int blastShieldRandomization = 0; // 0 none, 1 replace existing, 2 mix it up
  int blastShieldFrequency = 4;    // 1 low, 4 medium, 6 high (tenths of each area's regions)
  int blastShieldAvailableTypes = 0; // 0 no beam combos, 1 all
  int lockedDoorCount = 0;         // 0..2 areas with one door locked for good
  bool includePowerBeamDoors = false;
  bool includeMorphBallBombDoors = false;

  bool operator==(const Settings& other) const;
  bool operator!=(const Settings& other) const { return !(*this == other); }
};

// The settings as one JSON object (apworld option names), and back. Parse
// keeps the defaults for anything missing and false only for malformed text.
std::string SettingsText(const Settings& settings);
bool ParseSettings(const std::string& text, Settings& out);

struct Seed {
  std::string name;     // what the player types to make it again, e.g. "a1b2c3d4"
  Settings settings;
  // A slot_data object (JSON text) as the apworld's fill_slot_data writes it,
  // plus "artifact_locations" when artifactHints is on.
  std::string slotData;
  std::vector< int64_t > startItems;           // AP item ids given at the start
  std::map< int64_t, int64_t > placements;     // AP location id -> AP item id, every location
  std::string spoiler;                         // human-readable playthrough and layout
};

// Makes the seed for `seedText` (any string; empty picks one at random).
// The same settings and text give the same seed on every platform. False with
// `error` when no beatable seed was found.
bool Generate(const Settings& settings, const std::string& seedText, Seed& out, std::string& error);

// Seed files: <user dir>/randomizer_seeds/<name>.json.
std::string SeedDirectory();
std::string SeedPath(const std::string& name);
// A different seed already saved under the name gets a new one (<name>-2, ...),
// written back to seed.name.
bool Save(Seed& seed, std::string& error);
bool Load(const std::string& path, Seed& out, std::string& error);

} // namespace PortRandoGen

#endif // METROID_PRIME_PORT_PORT_RANDO_GEN_H
