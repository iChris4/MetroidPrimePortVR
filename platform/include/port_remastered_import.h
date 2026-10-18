#pragma once

#include "port_remastered_effect.h"

// Builds the "remastered-models" mod inside the port, from the user's own
// copy of Metroid Prime Remastered (.nsp) and their own Switch key file.
//
// Nothing is extracted to disk on the way: the paks are read straight out of
// the encrypted image (port_remastered_nsp.h), the retail models they replace
// come off the unmodded disc that is already open, and each model in
// port_remastered_table.h goes through port_remastered_convert.h. The output
// is the user's, for their use only; the port ships none of it.
//
// The mod is written to a hidden staging folder in the import's root and only
// takes the place of <root>/remastered-models when nothing is reading the one it
// replaces: at the next start, before mods are scanned (ApplyPendingImport), or in
// the background once the mods are unloaded (StartInstall, on the import's own
// thread). The root is the mods folder on desktop and the app's own internal
// storage on Android (port_remastered_storage.h); a legacy <mods>/remastered-models
// keeps loading as the fallback until a full import has written the private one.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PortRemastered {

struct ImportState {
  bool running = false;
  bool finished = false;   // a run ended; ok, message and lines are its result
  bool ok = false;         // at least one model converted and the mod is staged
  bool cancelled = false;
  int done = 0;            // models tried so far
  int total = 0;
  int failed = 0;
  // Where a full import is: step `step` of `steps` (0 when not counted, as for a movies-only
  // import), and how far into that step, out of stepTotal (0 when it cannot say).
  int step = 0;
  int steps = 0;
  int stepDone = 0;
  int stepTotal = 0;
  bool cancelling = false; // cancel was asked for and the import has not stopped yet
  bool installing = false; // StartInstall() is moving the finished import into place; it can't be cancelled
  std::string message;     // what it is doing, or how it ended
  std::vector<std::string> lines;  // the newest kImportMaxLines of its log
};

inline constexpr size_t kImportMaxLines = 200;

// The parts of the game the panel leaves out of the next import, on top of the
// MP_REMASTERED_* switches (docs/MODS.md): a part is imported only when both want it.
struct ImportParts {
  bool hud = true;
  bool text = true;
  bool effects = true;
  bool movies = true;
  bool gallery = true;
  bool geometry = true; // the rooms' static geometry (MP_REMASTERED_GEOMETRY)
};
void SetImportParts(const ImportParts& parts);

// The mod folder an import ends up as.
inline constexpr const char* kImportModName = "remastered-models";

// An import is made in stages, and a re-import links in the previous import's output of every
// stage whose number, options and inputs are unchanged instead of making it again. Bump the
// number of the stage a change needs re-imported (all of them that it touches):
namespace ImportStage {
// port_remastered_convert/cmdl: how models and materials are written (models, effects, rooms,
// room models, HUD).
inline constexpr int kConverter = 52;
// What a converted texture holds: port_remastered_image/txtr/dds/astc, and Converter's Get, Cube
// and Baked. Each converted texture is kept across imports under its tag and this number, so a
// kConverter bump re-imports the models without converting their textures again.
inline constexpr int kTextures = 3;
inline constexpr int kModels = 8;      // the table's models, their looks and the ANCS copies
// 25: swoosh GRAD is the exact PGRD element, no longer a 101-key KEYP bake.
inline constexpr int kEffects = 29;    // port_remastered_effect_import and the particle converters
// 20: two rooms matching one area write only the closer match's files, not the later room's.
inline constexpr int kRooms = 20;      // port_remastered_room: roomenv/, .roomgeo, .roomliquid, water maps
inline constexpr int kRoomModels = 1;  // the rooms' own models and their levels of detail
inline constexpr int kText = 1;        // port_remastered_text, and the font (port_remastered_font)
inline constexpr int kHud = 7;         // port_remastered_hud, port_remastered_map
inline constexpr int kMovies = 0;      // port_remastered_movie
inline constexpr int kGallery = 0;     // the gallery pictures (port_gallery)
}  // namespace ImportStage

// The USA retail Missile WPSC's COLR collision response defaults to the
// Remastered Missile WPSM's COLR response's DEFS GENP. Keep this one pinned
// role mapping separate from the broader projectile APSM/ASW rules below.
namespace ImportDetail {
inline constexpr uint32_t kMissileWpsc = 0x466208AB;
inline constexpr uint32_t kMissileCrsc = 0xD27D6EDD;
inline constexpr uint32_t kMissileDefaultPart = 0xA594B6AE;
// Pak asset-index / EffectPairing order (printed UUID byte order).
inline constexpr std::array<uint8_t, 16> kMissileCrsm = {0xAC, 0xE0, 0xF7, 0xA8, 0x67, 0xC6, 0x4B, 0xD6,
                                                         0xB3, 0xE3, 0xC1, 0x8E, 0x1D, 0xC1, 0x10, 0x54};
inline constexpr std::array<uint8_t, 16> kMissileDefaultGenp = {0xE2, 0xCF, 0x01, 0x68, 0x90, 0xA2, 0x4E, 0xFA,
                                                                0x82, 0x3C, 0xAB, 0x99, 0xC0, 0x2D, 0x85, 0x50};
// The link records carry the same GUIDs in uuid.UUID(...).bytes_le order.
inline constexpr std::array<uint8_t, 16> kMissileCrsmLink = {0xA8, 0xF7, 0xE0, 0xAC, 0xC6, 0x67, 0xD6, 0x4B,
                                                             0xB3, 0xE3, 0xC1, 0x8E, 0x1D, 0xC1, 0x10, 0x54};
inline constexpr std::array<uint8_t, 16> kMissileDefaultGenpLink = {0x68, 0x01, 0xCF, 0xE2, 0xA2, 0x90, 0xFA, 0x4E,
                                                                    0x82, 0x3C, 0xAB, 0x99, 0xC0, 0x2D, 0x85, 0x50};

struct MissileDefaultRoleInput {
  std::string_view weaponName;  // the lower-case FrameKey, not a filename guess
  uint32_t retailWpscId = 0;
  bool retailWpscExists = false;
  bool retailCrscExists = false;
  bool retailPartExists = false;
  bool remasteredWpsmExists = false;
  bool remasteredCrsmExists = false;
  bool remasteredGenpExists = false;
  const uint8_t* retailWpsc = nullptr;
  size_t retailWpscSize = 0;
  const uint8_t* retailCrsc = nullptr;
  size_t retailCrscSize = 0;
  const uint8_t* remasteredWpsm = nullptr;
  size_t remasteredWpsmSize = 0;
  const uint8_t* remasteredCrsm = nullptr;
  size_t remasteredCrsmSize = 0;
};

struct MissileDefaultRolePair {
  std::array<uint8_t, 16> genp{};
  uint32_t retailPart = 0;
};

// The USA 1.00 PowerBall collision-response default is independent of its
// APSM projectile effect: retail WPSC COLR -> CRSC DEFS PART, and Remastered
// WPSM COLR -> CRSM DEFS GENP. These exact identities prevent cross-region or
// other-default pairing.
inline constexpr uint32_t kPowerBallWpsc = 0x87D4CA7C;
inline constexpr uint32_t kPowerBallCrsc = 0x39EB7161;
inline constexpr uint32_t kPowerBallDefaultPart = 0x09C2016D;
inline constexpr std::array<uint8_t, 16> kPowerBallCrsm = {0x76, 0x69, 0x0A, 0x3F, 0x48, 0x95, 0x4F, 0xD9,
                                                           0x8C, 0x6E, 0xB3, 0x4F, 0x15, 0x6D, 0xEC, 0x58};
inline constexpr std::array<uint8_t, 16> kPowerBallDefaultGenp = {0xDA, 0x57, 0xEF, 0xF5, 0xBB, 0xEB, 0x41, 0xDE,
                                                                 0x9C, 0x9B, 0xCA, 0x90, 0xEF, 0x19, 0x22, 0x6E};
// Reversed-guid byte order in Remastered COLR/DEFS links.
inline constexpr std::array<uint8_t, 16> kPowerBallCrsmLink = {0x3F, 0x0A, 0x69, 0x76, 0x95, 0x48, 0xD9, 0x4F,
                                                              0x8C, 0x6E, 0xB3, 0x4F, 0x15, 0x6D, 0xEC, 0x58};
inline constexpr std::array<uint8_t, 16> kPowerBallDefaultGenpLink = {0xF5, 0xEF, 0x57, 0xDA, 0xEB, 0xBB, 0xDE, 0x41,
                                                                      0x9C, 0x9B, 0xCA, 0x90, 0xEF, 0x19, 0x22, 0x6E};

struct PowerBallDefaultRoleInput {
  std::string_view weaponName;  // lower-case FrameKey for the named PowerBall WPSM
  uint32_t retailWpscId = 0;
  bool retailWpscExists = false;
  bool retailCrscExists = false;
  bool retailPartExists = false;
  bool remasteredWpsmExists = false;
  bool remasteredCrsmExists = false;
  bool remasteredGenpExists = false;
  const uint8_t* retailWpsc = nullptr;
  size_t retailWpscSize = 0;
  const uint8_t* retailCrsc = nullptr;
  size_t retailCrscSize = 0;
  const uint8_t* remasteredWpsm = nullptr;
  size_t remasteredWpsmSize = 0;
  const uint8_t* remasteredCrsm = nullptr;
  size_t remasteredCrsmSize = 0;
};

struct PowerBallDefaultRolePair {
  std::array<uint8_t, 16> genp{};
  uint32_t retailPart = 0;
};

// The USA 1.00 PlasmaBeam collision-response default is independent of its
// APSM projectile effect: retail WPSC COLR -> CRSC DEFS PART, and Remastered
// WPSM COLR -> CRSM DEFS GENP. Asset ids use printed UUID order; serialized
// role links use UUID little-endian order.
inline constexpr uint32_t kPlasmaBeamWpsc = 0x310D8B94;
inline constexpr uint32_t kPlasmaBeamCrsc = 0x48036137;
inline constexpr uint32_t kPlasmaBeamDefaultPart = 0x650D4419;
// Printed UUID order: CRSM e42c4b33-8618-44ab-ab5c-d88dd71d9ee6,
// GENP 249c566b-6276-4f00-b905-3c3a73c8c647.
inline constexpr std::array<uint8_t, 16> kPlasmaBeamCrsm = {0xE4, 0x2C, 0x4B, 0x33, 0x86, 0x18, 0x44, 0xAB,
                                                           0xAB, 0x5C, 0xD8, 0x8D, 0xD7, 0x1D, 0x9E, 0xE6};
inline constexpr std::array<uint8_t, 16> kPlasmaBeamDefaultGenp = {0x24, 0x9C, 0x56, 0x6B, 0x62, 0x76, 0x4F, 0x00,
                                                                  0xB9, 0x05, 0x3C, 0x3A, 0x73, 0xC8, 0xC6, 0x47};
// The stored links are the same UUIDs in uuid.UUID(...).bytes_le order.
inline constexpr std::array<uint8_t, 16> kPlasmaBeamCrsmLink = {0x33, 0x4B, 0x2C, 0xE4, 0x18, 0x86, 0xAB, 0x44,
                                                               0xAB, 0x5C, 0xD8, 0x8D, 0xD7, 0x1D, 0x9E, 0xE6};
inline constexpr std::array<uint8_t, 16> kPlasmaBeamDefaultGenpLink = {0x6B, 0x56, 0x9C, 0x24, 0x76, 0x62, 0x00, 0x4F,
                                                                      0xB9, 0x05, 0x3C, 0x3A, 0x73, 0xC8, 0xC6, 0x47};

struct PlasmaBeamDefaultRoleInput {
  std::string_view weaponName;  // the lower-case FrameKey for PlasmaBeam.WPSM
  uint32_t retailWpscId = 0;
  bool retailWpscExists = false;
  bool retailCrscExists = false;
  bool retailPartExists = false;
  bool remasteredWpsmExists = false;
  bool remasteredCrsmExists = false;
  bool remasteredGenpExists = false;
  const uint8_t* retailWpsc = nullptr;
  size_t retailWpscSize = 0;
  const uint8_t* retailCrsc = nullptr;
  size_t retailCrscSize = 0;
  const uint8_t* remasteredWpsm = nullptr;
  size_t remasteredWpsmSize = 0;
  const uint8_t* remasteredCrsm = nullptr;
  size_t remasteredCrsmSize = 0;
};

struct PlasmaBeamDefaultRolePair {
  std::array<uint8_t, 16> genp{};
  uint32_t retailPart = 0;
};

namespace detail {
inline bool ReadUniqueRetailAssetId(const uint8_t* data, size_t size, const char (&classId)[5],
                                    const char (&field)[5], uint32_t& id, std::string* error = nullptr) {
  return PortRemastered::ReadRetailRoleAssetId(data, size, PortRemastered::EffectFourCC(classId),
                                               PortRemastered::EffectFourCC(field), id, error);
}

inline bool ReadUniqueReversedGuidLink(const uint8_t* data, size_t size, const char (&form)[5],
                                       const char (&field)[5], uint8_t kind, std::array<uint8_t, 16>& guid,
                                       std::string* error = nullptr) {
  PortRemastered::EffectNode asset;
  std::string parseError;
  if (!PortRemastered::ParseRemasteredRoleAsset(data, size, PortRemastered::EffectFourCC(form), asset, parseError)) {
    if (error != nullptr) {
      *error = std::move(parseError);
    }
    return false;
  }
  size_t found = 0;
  std::array<uint8_t, 16> parsed{};
  const uint32_t wanted = PortRemastered::EffectFourCC(field);
  for (const PortRemastered::EffectProperty& property : asset.properties) {
    if (property.fourcc != wanted) {
      continue;
    }
    if (++found != 1) {
      if (error != nullptr) {
        *error = "duplicate top-level " + std::string(field) + " fields";
      }
      return false;
    }
    if (property.tag != kind || property.value.size() != 1 ||
        property.value.front().kind != PortRemastered::EffectValue::Kind::Guid) {
      if (error != nullptr) {
        *error = "top-level " + std::string(field) + " has the wrong kind or value type";
      }
      return false;
    }
    parsed = property.value.front().guid;
  }
  if (found != 1) {
    if (error != nullptr) {
      *error = "top-level " + std::string(field) + " field is missing";
    }
    return false;
  }
  guid = parsed;
  return true;
}
}  // namespace detail

// Returns a pair only for the exact, uniquely identified USA default-role
// chain. The RFRM and retail class ids, field tags/kinds, complete GUIDs, and
// typed resource-presence flags are all part of the gate; an incomplete or
// ambiguous chain safely leaves retail's collision response in place.
inline bool ResolveMissileDefaultRole(const MissileDefaultRoleInput& input, MissileDefaultRolePair& out,
                                      std::string* error = nullptr) {
  auto fail = [error](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };
  if (input.weaponName != "missile" || input.retailWpscId != kMissileWpsc || !input.retailWpscExists ||
      !input.retailCrscExists || !input.retailPartExists || !input.remasteredWpsmExists ||
      !input.remasteredCrsmExists || !input.remasteredGenpExists) {
    return fail("pinned identity or typed-resource presence gate failed");
  }
  uint32_t crsc = 0;
  uint32_t part = 0;
  std::array<uint8_t, 16> crsm{};
  std::array<uint8_t, 16> genp{};
  std::string why;
  if (!detail::ReadUniqueRetailAssetId(input.retailWpsc, input.retailWpscSize, "WPSM", "COLR", crsc, &why)) {
    return fail("retail WPSC: " + why);
  }
  if (crsc != kMissileCrsc) {
    return fail("retail WPSC COLR does not name the pinned CRSC");
  }
  if (!detail::ReadUniqueRetailAssetId(input.retailCrsc, input.retailCrscSize, "CRSM", "DEFS", part, &why)) {
    return fail("retail CRSC: " + why);
  }
  if (part != kMissileDefaultPart) {
    return fail("retail CRSC DEFS does not name the pinned PART");
  }
  if (!detail::ReadUniqueReversedGuidLink(input.remasteredWpsm, input.remasteredWpsmSize, "WPSM", "COLR", 0,
                                          crsm, &why)) {
    return fail("Remastered WPSM: " + why);
  }
  if (crsm != kMissileCrsmLink) {
    return fail("Remastered WPSM COLR does not name the pinned CRSM");
  }
  if (!detail::ReadUniqueReversedGuidLink(input.remasteredCrsm, input.remasteredCrsmSize, "CRSM", "DEFS", 4,
                                          genp, &why)) {
    return fail("Remastered CRSM: " + why);
  }
  if (genp != kMissileDefaultGenpLink) {
    return fail("Remastered CRSM DEFS does not name the pinned GENP");
  }
  out = {kMissileDefaultGenp, part};
  return true;
}

// Resolve only the raw, typed USA 1.00 PowerBall chain. The caller supplies
// uniqueness/type checks from the real retail and PAK indexes; every link is
// then independently parsed and compared against the pinned chain.
inline bool ResolvePowerBallDefaultRole(const PowerBallDefaultRoleInput& input, PowerBallDefaultRolePair& out,
                                        std::string* error = nullptr) {
  auto fail = [error](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };
  if (input.weaponName != "powerball" || input.retailWpscId != kPowerBallWpsc || !input.retailWpscExists ||
      !input.retailCrscExists || !input.retailPartExists || !input.remasteredWpsmExists ||
      !input.remasteredCrsmExists || !input.remasteredGenpExists) {
    return fail("pinned USA identity or unique typed-resource presence gate failed");
  }
  uint32_t crsc = 0;
  uint32_t part = 0;
  std::array<uint8_t, 16> crsm{};
  std::array<uint8_t, 16> genp{};
  std::string why;
  if (!detail::ReadUniqueRetailAssetId(input.retailWpsc, input.retailWpscSize, "WPSM", "COLR", crsc, &why)) {
    return fail("retail WPSC: " + why);
  }
  if (crsc != kPowerBallCrsc) {
    return fail("retail WPSC COLR does not name the pinned USA CRSC");
  }
  if (!detail::ReadUniqueRetailAssetId(input.retailCrsc, input.retailCrscSize, "CRSM", "DEFS", part, &why)) {
    return fail("retail CRSC: " + why);
  }
  if (part != kPowerBallDefaultPart) {
    return fail("retail CRSC DEFS does not name the pinned USA PART");
  }
  if (!detail::ReadUniqueReversedGuidLink(input.remasteredWpsm, input.remasteredWpsmSize, "WPSM", "COLR", 0,
                                          crsm, &why)) {
    return fail("Remastered WPSM: " + why);
  }
  if (crsm != kPowerBallCrsmLink) {
    return fail("Remastered WPSM COLR does not name the pinned CRSM");
  }
  if (!detail::ReadUniqueReversedGuidLink(input.remasteredCrsm, input.remasteredCrsmSize, "CRSM", "DEFS", 4,
                                          genp, &why)) {
    return fail("Remastered CRSM: " + why);
  }
  if (genp != kPowerBallDefaultGenpLink) {
    return fail("Remastered CRSM DEFS does not name the pinned GENP");
  }
  out = {kPowerBallDefaultGenp, part};
  return true;
}

// Resolve only the raw, typed USA 1.00 PlasmaBeam chain. The caller supplies
// uniqueness/type checks from the real retail and PAK indexes; every link is
// then independently parsed and compared against the pinned chain.
inline bool ResolvePlasmaBeamDefaultRole(const PlasmaBeamDefaultRoleInput& input, PlasmaBeamDefaultRolePair& out,
                                         std::string* error = nullptr) {
  auto fail = [error](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };
  if (input.weaponName != "plasmabeam" || input.retailWpscId != kPlasmaBeamWpsc || !input.retailWpscExists ||
      !input.retailCrscExists || !input.retailPartExists || !input.remasteredWpsmExists ||
      !input.remasteredCrsmExists || !input.remasteredGenpExists) {
    return fail("pinned USA identity or unique typed-resource presence gate failed");
  }
  uint32_t crsc = 0;
  uint32_t part = 0;
  std::array<uint8_t, 16> crsm{};
  std::array<uint8_t, 16> genp{};
  std::string why;
  if (!detail::ReadUniqueRetailAssetId(input.retailWpsc, input.retailWpscSize, "WPSM", "COLR", crsc, &why)) {
    return fail("retail WPSC: " + why);
  }
  if (crsc != kPlasmaBeamCrsc) {
    return fail("retail WPSC COLR does not name the pinned USA CRSC");
  }
  if (!detail::ReadUniqueRetailAssetId(input.retailCrsc, input.retailCrscSize, "CRSM", "DEFS", part, &why)) {
    return fail("retail CRSC: " + why);
  }
  if (part != kPlasmaBeamDefaultPart) {
    return fail("retail CRSC DEFS does not name the pinned USA PART");
  }
  if (!detail::ReadUniqueReversedGuidLink(input.remasteredWpsm, input.remasteredWpsmSize, "WPSM", "COLR", 0,
                                          crsm, &why)) {
    return fail("Remastered WPSM: " + why);
  }
  if (crsm != kPlasmaBeamCrsmLink) {
    return fail("Remastered WPSM COLR does not name the pinned CRSM");
  }
  if (!detail::ReadUniqueReversedGuidLink(input.remasteredCrsm, input.remasteredCrsmSize, "CRSM", "DEFS", 4,
                                          genp, &why)) {
    return fail("Remastered CRSM: " + why);
  }
  if (genp != kPlasmaBeamDefaultGenpLink) {
    return fail("Remastered CRSM DEFS does not name the pinned GENP");
  }
  out = {kPlasmaBeamDefaultGenp, part};
  return true;
}

inline bool ConflictsWithPlasmaBeamDefaultRolePair(const PlasmaBeamDefaultRolePair& candidate,
                                                   const std::array<uint8_t, 16>& existingGenp,
                                                   uint32_t existingRetail) {
  return (existingGenp == candidate.genp || existingRetail == candidate.retailPart) &&
         !(existingGenp == candidate.genp && existingRetail == candidate.retailPart);
}

// A default-role mapping must not override another established mapping that
// claims either side. An exact existing pair is handled as already resolved.
inline bool ConflictsWithPowerBallDefaultRolePair(const PowerBallDefaultRolePair& candidate,
                                                  const std::array<uint8_t, 16>& existingGenp,
                                                  uint32_t existingRetail) {
  return (existingGenp == candidate.genp || existingRetail == candidate.retailPart) &&
         !(existingGenp == candidate.genp && existingRetail == candidate.retailPart);
}
}  // namespace ImportDetail
// Any stage's bump raises it. A full import writes it to kImportStampName in the mod; the mod scan
// (port_mods.h) tells the player when an import carries a lower number, or none.
inline constexpr int kImportVersion = 38 + ImportStage::kConverter + ImportStage::kTextures + ImportStage::kModels +
                                      ImportStage::kEffects + ImportStage::kRooms + ImportStage::kRoomModels +
                                      ImportStage::kText + ImportStage::kHud + ImportStage::kMovies +
                                      ImportStage::kGallery;
inline constexpr const char* kImportStampName = ".import-version";

// ~/.switch/prod.keys if it is there, else empty.
std::string DefaultKeysPath();

// Starts an import on worker threads; `threads` 0 leaves a couple of cores to
// the game. The disc must be open. False (with the reason in the state's
// message) when one is already running or there is no import root.
bool StartImport(const std::string& nspPath, const std::string& keysPath, int threads = 0);
// Only the menu movies (port_remastered_movie.h) and the Extras gallery, into the mod an earlier
// import made: for a player who had no ffmpeg then, or imported before the gallery. Same state and cancelling;
// false when one is running or there is no such mod. It never writes into a legacy
// <mods>/remastered-models on Android, where the import belongs to the app: a player who still has
// only that one is asked for a full import instead.
bool StartMovieImport(const std::string& nspPath, const std::string& keysPath);
ImportState ImportStatus();
// Asks the running import to stop; it ends at the next model.
void CancelImport();
// Cancels and waits. Call before the disc is closed.
void StopImport();

// The root the import is made in and read from (port_remastered_storage.h): the mods folder on
// desktop, where the import is one of the mods, and <internal storage>/remastered-imports on
// Android, outside the shared mods folder. Empty when there is none, with `error` (when given)
// saying why: on Android that is the app's internal storage being unavailable, and it is never
// worked around by writing to the shared mods folder. Created when it is missing.
std::string ImportRootFolder(std::string* error = nullptr);
// Whether the root is outside the mods folder, so a legacy import there is only a fallback.
bool UsesPrivateImportRoot();
// <root>/remastered-models when that folder is installed, for the mod scan to load
// (port_mods.cpp); empty when there is none, or when the root is the mods folder the scan already
// reads. The staging folder is never answered.
std::string InstalledModFolder();

// Moves a finished import into place. Call before PortMods::Initialize().
// True if a mod was installed.
bool ApplyPendingImport();
// The same on the import's thread, reported as running with `installing` set, after which the state
// is the import's result again. Nothing may have a file of the installed mod open (unload the mods
// first); inside the root that is one rename, where the mods folder was shared storage it was a
// file-by-file walk through Android's media provider. False when an import is running or none is
// staged.
bool StartInstall();

// `--import-remastered <nsp> [keys]`: imports with every core, progress on
// stdout, and installs the mod. The disc must be open. Returns the exit code.
// `--import-remastered-movies` is StartMovieImport() the same way.
int RunImportFromCommandLine(const std::string& nspPath, const std::string& keysPath, bool moviesOnly = false);

}  // namespace PortRemastered
