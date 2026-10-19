#pragma once

// Morph ball looks retail lacks, made from the player's disc:
//
// - The Power Suit spider ball. Retail's is the Gravity Suit's
//   (SamusSpiderBallANCS material set 0, indigo). This makes Varia's (set 1)
//   with Varia's own colours moved to the Power Suit's: the shift is fitted
//   from the two suits' normal balls, and applied only where Varia's spider
//   ball texture differs from Gravity's, so the parts all suits share (the red
//   bands) stay as they are.
// - The Gravity and Phazon Suit balls without the Spider Ball. Retail gives
//   both the Power Suit's (SamusBallANCS set 0). These make Varia's (set 1),
//   recoloured where Varia's ball texture differs from Power's. Gravity's
//   shift is fitted texel by texel from the spider ball's Varia and Gravity
//   sets (same UVs); Phazon's spider ball is another model, so its shift is
//   the difference of the two shells' colour statistics instead.
//
// Each goes under the PortModelVariant ids for its suit, which the game
// already loads when they exist (CMorphBall::PortSuitVariant). PortMods serves
// them below every mod, mods on or off, from a cache in the user folder
// (generated/<look>/), and leaves a look out when a mod brings any of its
// resources (the Remastered import).

#include "port_mods.h"
#include "port_remastered_image.h"

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace PortBallLooks {

// Bump when the output changes: the cache is remade when its stamp differs.
inline constexpr int kVersion = 3;

enum class Look { PowerSpider, Gravity, Phazon };
inline constexpr Look kLooks[] = {Look::PowerSpider, Look::Gravity, Look::Phazon};

// The retail resources the looks are made from.
inline constexpr uint32_t kSpiderAncs = 0xF37654A2;      // SamusSpiderBallANCS
inline constexpr uint32_t kSpiderCmdl = 0x223353FE;      // its model
inline constexpr uint32_t kLowPolyCmdl = 0x0B57F9E8;     // SamusSpiderBallLowPolyCMDL
inline constexpr uint32_t kGlassCmdl = 0xAE78BEBE;       // SamusSpiderBallGlassCMDL
inline constexpr uint32_t kSpiderVaria = 0x2EE6F56F;     // set 1 (Varia) shell
inline constexpr uint32_t kSpiderGravity = 0x50A70472;   // set 0 (Gravity) shell
inline constexpr uint32_t kLowPolyVaria = 0x07675658;
inline constexpr uint32_t kLowPolyGravity = 0xBA7DF5D6;
inline constexpr uint32_t kBallAncs = 0x9FE58F7A;        // SamusBallANCS
inline constexpr uint32_t kBallCmdl = 0x4E57CE08;        // its model
inline constexpr uint32_t kBallLowPolyCmdl = 0x180418E0; // SamusBallLowPolyCMDL
inline constexpr uint32_t kBallPower = 0xC01FFF01;       // the normal ball's Power shell (set 0)
inline constexpr uint32_t kBallVaria = 0x49A1D81D;       // and its Varia shell (set 1)
inline constexpr uint32_t kPhazonShell = 0x8B105F2E;     // SamusPhazonBallANCS's shell

// Two texels count as different when a channel differs by more than this.
inline constexpr int kDifferThreshold = 16;

// What the look is called in the log, and the suit (model table index) whose
// variant ids it takes.
const char* Name(Look look);
int Suit(Look look);

// A move in HSV: hue in degrees, saturation and value in 0..1.
struct Shift {
  double hue = 0.0;
  double saturation = 0.0;
  double value = 0.0;
  size_t samples = 0;  // texels the fit used
};

// The colour of a texture's region: the median hue of its coloured texels
// (taken round the circle, -180..180 from their mean, then put back in
// 0..360), and the median saturation and value of its opaque, not dark ones.
struct Stats {
  double hue = 0.0;
  double saturation = 0.0;
  double value = 0.0;
  size_t samples = 0;     // texels in the saturation and value medians
  size_t hueSamples = 0;  // coloured texels in the hue median
};

// The top level of a CMPR TXTR (format 10) as RGBA, decoded as GX does (the
// three-colour mode's fourth colour is transparent). False for another format.
bool DecodeCmprTxtr(const std::vector<uint8_t>& txtr, PortRemastered::Image& out);

// The median HSV move from `from` to `to` over the texels both have opaque and
// coloured that differ: the two looks of one texture.
Shift FitShift(const PortRemastered::Image& from, const PortRemastered::Image& to,
               int threshold = kDifferThreshold);

// The statistics of `image`; with `other`, only of the texels that differ
// from it (the same texture in another look). Zero samples when the sizes
// differ.
Stats ColourStats(const PortRemastered::Image& image, const PortRemastered::Image* other = nullptr,
                  int threshold = kDifferThreshold);

// The move from one region's statistics to another's, hue the short way round.
// Zero samples when either has none.
Shift StatsShift(const Stats& from, const Stats& to);

// Applies `shift` to the opaque texels of `image` that differ from `other`
// (the same texture in the look it should keep apart from). Returns how many
// changed; zero when the sizes differ.
size_t Recolour(PortRemastered::Image& image, const PortRemastered::Image& other, const Shift& shift,
                int threshold = kDifferThreshold);

// Makes material set `to` of a CMDL a copy of set `from`, with the texture ids
// in `swap` replaced in its texture list. False, and the model unchanged, when
// it is not a CMDL with both sets.
bool CopyMaterialSet(std::vector<uint8_t>& cmdl, uint32_t from, uint32_t to,
                     const std::map<uint32_t, uint32_t>& swap = {});

// The (type, id) keys that, when a mod has one, mean it brings its own look of
// that ball: the retail resources the look replaces and the generated ids.
std::vector<std::pair<uint32_t, uint32_t>> ModelKeys(Look look);

// The look's generated resources, made or taken from the cache. Empty when the
// disc lacks a source (logged). Needs the disc open; for PortMods::Initialize.
std::vector<PortMods::LooseResource> Collect(Look look);

// What the last PortMods::Initialize did: whether the setting was on, and
// which looks are being served (bit 1 << Look).
void SetState(bool requested, unsigned activeLooks);
bool Requested();
bool Active(Look look);

}  // namespace PortBallLooks
