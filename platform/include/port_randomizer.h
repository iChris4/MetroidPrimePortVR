#ifndef METROID_PRIME_PORT_PORT_RANDOMIZER_H
#define METROID_PRIME_PORT_PORT_RANDOMIZER_H
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace PortRandomizer {

// The model a pickup is drawn with. Pickups are built from the area file's
// static model, or from an animated model when `acs` is non-zero, so both are
// carried here. Mirrors CAnimationParameters plus the static model asset id.
struct PickupModel {
  uint32_t model = 0;
  uint32_t acs = 0;
  uint32_t character = 0;
  uint32_t animation = 0;
};

// One location of a seed file: the item and, when given, the amount and
// capacity it grants (each within -9999..9999).
struct SeedPlacement {
  int itemType = -1;
  int amount = 0;
  int capacity = 0;
  bool hasAmount = false;
  bool hasCapacity = false;
};

// A seed file (randomizer_seed.json, or MP_RANDO_SEED): a top-level object with
// an optional "seed" name, "locations" (key -> {"item", "amount"?,
// "capacity"?}, keys upper-cased) and "models" (item name -> {"model"?, "acs"?,
// "character"?, "animation"?}). Unknown keys are ignored; a repeated key keeps
// its last value.
struct SeedFile {
  std::string name;
  std::map< std::string, SeedPlacement > placements;
  std::map< int, PickupModel > models;
};

// The errorOffset of a seed that is well-formed JSON but not a valid seed.
constexpr size_t kSeedNoOffset = static_cast< size_t >(-1);

// Parses a seed file's text (PortJson, then the schema above). False on a
// malformed or invalid seed, with why: errorOffset is the byte offset for
// malformed JSON, kSeedNoOffset for a well-formed file that is not a seed.
bool ParseSeedFile(const std::string& text, SeedFile& out, size_t& errorOffset,
                   std::string& errorReason);

// Loads the configured seed once. Call on the game thread; never throws.
void EnsureLoaded();
// True when a seed contains at least one placement.
bool Enabled();
// True when MP_RANDO_DUMP is set to a non-empty value other than "0".
bool DumpEnabled();
// Name from the loaded seed, or an empty string when none is available.
const char* SeedName();
// The main log's line for a pickup a seed placed, granted by the game itself:
// what it gave and what the inventory holds after. Only with a seed loaded.
void LogPickupGrant(uint32_t worldAssetId, uint32_t areaAssetId, uint32_t entityId, int itemType,
                    int capacity, int amount, int amountAfter, int capacityAfter);
// Number of pickup checks recorded during this process.
int CheckCount();
// Short status line suitable for the F1 overlay.
const char* StatusText();
// Logs a location in dump mode, or applies its placement when enabled. `model`
// is only read in dump mode, where it is recorded so a seed's item models can
// be derived from the dump.
bool ApplyPickup(uint32_t worldAssetId, uint32_t areaAssetId, uint32_t entityId,
                 int& itemType, int& capacity, int& amount,
                 const PickupModel& model = PickupModel());
// The item and capacity the seed puts at a location, for the map's pickup
// kinds. Leaves them alone and returns false when the randomizer is off or the
// location keeps its item; a placement without a capacity keeps `capacity`.
bool Placed(uint32_t worldAssetId, uint32_t areaAssetId, uint32_t entityId, int& itemType,
            int& capacity);
// Looks up the model the seed associates with an item type. False when the seed
// carries no model for it, in which case the original model is kept.
bool ModelForItem(int itemType, PickupModel& out);
// Where the seed put an item (the first location holding it, by key order).
// False when the randomizer is off or no location holds it.
bool FindItem(int itemType, uint32_t& worldAssetId, uint32_t& areaAssetId, uint32_t& entityId);
// Records a collected pickup check when randomizer or dump mode is active.
void RecordCheck(uint32_t worldAssetId, uint32_t areaAssetId, uint32_t entityId, int itemType);
// Returns the retail item name, or "Unknown" for an out-of-range item type.
const char* ItemName(int itemType);
// Returns an item type for a case-insensitive exact name, or -1 when unknown.
int ItemFromName(const char* name);
// Formats a location as three uppercase, zero-padded eight-digit hexadecimal IDs.
void FormatLocationKey(uint32_t worldAssetId, uint32_t areaAssetId, uint32_t entityId,
                       char* out, int outSize);

} // namespace PortRandomizer

#endif
