#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Resources that are on no disc: randomprime's custom pickup models
// (custom_assets.rs), built from disc models and its MIT textures the first
// time something asks for them. CResLoader answers these ids from here.
namespace PortCustomRes {

// randomprime's ids, so its seeds' model choices mean the same thing here.
enum : uint32_t {
  kPhazonSuitTxtr1 = 0xDEAF0000,
  kPhazonSuitTxtr2 = 0xDEAF0001,
  kPhazonSuitCmdl = 0xDEAF0002,
  kPhazonSuitAncs = 0xDEAF0003,
  kNothingTxtr = 0xDEAF0004,
  kNothingCmdl = 0xDEAF0005,
  kNothingAncs = 0xDEAF0006,
  kZoomerCmdl = 0xDEAF0007,
  kZoomerAncs = 0xDEAF0008,
  kCogCmdl = 0xDEAF0009,
  kCogAncs = 0xDEAF000A,
  kThermalCmdl = 0xDEAF000B,
  kThermalAncs = 0xDEAF000C,
  kXrayCmdl = 0xDEAF000D,
  kXrayAncs = 0xDEAF000E,
  kCombatCmdl = 0xDEAF000F,
  kCombatAncs = 0xDEAF0010,
  // Door shields and their textures for Archipelago's door types (ids of
  // the port's own: no seed names them).
  kDoorPowerHolorimTxtr = 0xDEAF0100,
  kDoorBombHolorimTxtr = 0xDEAF0101,
  kDoorBombPatternTxtr = 0xDEAF0102,
  kDoorBombColorTxtr = 0xDEAF0103,
  kDoorPowerCmdl = 0xDEAF0110, // each followed by its vertical twin
  kDoorBombCmdl = 0xDEAF0112,
  kDoorMissileCmdl = 0xDEAF0114,
  kDoorDisabledCmdl = 0xDEAF0116,
  kDoorPlasmaVerticalCmdl = 0xDEAF0118,
  // Blast shields, eight ids per kind (ShieldKind): the missile shield's
  // glow border, glow trim, body and moving glow recoloured, and at + 7 its
  // model wearing them.
  // Map legend swatches (16x16 RGBA8 drawn at run time): a filled dot, a ring,
  // a grey cross and a cyan square.
  kLegendDotTxtr = 0xDEAF0200,
  kLegendRingTxtr = 0xDEAF0201,
  kLegendCrossTxtr = 0xDEAF0202,
  kLegendMarkTxtr = 0xDEAF0203,
  kShieldBase = 0xDEAF0120,
  kShieldEnd = kShieldBase + 8 * 7,
};
// The blast shields the disc has no model for, in kShieldBase order.
enum ShieldKind {
  kShieldBomb,
  kShieldCharge,
  kShieldFlamethrower,
  kShieldIceSpreader,
  kShieldWavebuster,
  kShieldPowerBomb,
  kShieldSuperMissile,
  kShieldKinds
};
constexpr uint32_t ShieldCmdl(int kind) { return kShieldBase + 8 * uint32_t(kind) + 7; }

inline bool IsCustomId(uint32_t id) { return (id & 0xFFFF0000u) == 0xDEAF0000u; }

// Scan text made at run time (a randomized pickup's item name) is a SCAN and
// STRG pair from here up, far past randomprime's ids, which count up from
// 0xDEAF0000 on a patched disc.
constexpr uint32_t kTextBase = 0xDEAF8000;

struct Resource {
  uint32_t type = 0; // FourCC
  std::vector<uint8_t> data; // uncompressed
};

// Reads a disc resource, decompressed. False when the disc lacks it.
using DiscReader = std::function<bool(uint32_t id, std::vector<uint8_t>& out)>;

// The resource for a custom id, built on first use; null for an unknown id or
// when its disc source is missing or not the expected layout. Pointers stay
// valid for the whole run.
const Resource* Find(uint32_t id, const DiscReader& read);

// The SCAN id of a scan showing `text` (UTF-8, in the game's text markup),
// made on first use of `key`; the same key always gets the same id (its STRG
// is id + 1), and a new text replaces the old one for the next build. 0 once
// the id range is used up.
uint32_t TextScan(uint64_t key, const std::string& text);

// A one-string STRG and a text-only SCAN pointing at `strg` (exposed for the
// unit test).
std::vector<uint8_t> MakeStrg(const std::u16string& text);
std::vector<uint8_t> MakeScan(uint32_t strg);
// UTF-8 to UTF-16; a malformed byte becomes U+FFFD.
std::u16string Utf16(const std::string& text);

// In-place patches (exposed for the unit test). False, and data unchanged,
// when the layout isn't what randomprime's recipe expects.
bool SetCmdlTexture(std::vector<uint8_t>& cmdl, uint32_t index, uint32_t texture);
bool SetAncsModel(std::vector<uint8_t>& ancs, uint32_t expectedModel, uint32_t model);
// Recolours a CMPR texture, every mip: each block's two colours go through
// `matrix` (out = matrix * rgb, row per output channel), and the block keeps
// its mode when that changes their order. False for another format.
bool TintTxtr(std::vector<uint8_t>& txtr, const float matrix[3][3]);

} // namespace PortCustomRes
