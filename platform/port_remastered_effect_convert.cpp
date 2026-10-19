#include "port_remastered_effect_convert.h"
#include "port_bytes.h"
#include "Kyoto/Particles/CPortVfxConstants.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

namespace PortRemastered {
namespace {

constexpr uint32_t F(const char (&text)[5]) { return EffectFourCC(text); }

// The types CParticleDataFactory reads a value as.
enum class Type : uint8_t { Int, Real, Vector, ModVector, Color, Emitter, Texture, Bool, Asset };

// Retail's properties (CParticleDataFactory::CreateGPSM) and the type each is
// read as. An Asset property also names the type of asset it refers to.
struct PropertyType {
  Type type;
  uint32_t asset = 0;
};

using PropertyTable = std::unordered_map<uint32_t, PropertyType>;

void AddProperties(PropertyTable& t, const char* names, Type type, uint32_t asset = 0) {
  for (const char* c = names; *c != 0;) {
    const uint32_t fourcc = port::FourCC(c);
    t[fourcc] = {type, asset};
    c += 4;
    while (*c == ' ') {
      ++c;
    }
  }
}

// Retail's swoosh properties (CParticleSwooshDataFactory::CreateWPSM).
const PropertyTable& SwooshProperties() {
  static const PropertyTable table = [] {
    PropertyTable t;
    AddProperties(t, "PSLT LENG SIDE SPLN TSPN", Type::Int);
    AddProperties(t, "TIME LRAD RRAD IROT ROTM", Type::Real);
    AddProperties(t, "POFS IVEL NPOS", Type::Vector);
    AddProperties(t, "VELM VLM2", Type::ModVector);
    AddProperties(t, "COLR", Type::Color);
    AddProperties(t, "TEXR", Type::Texture);
    AddProperties(t, "LLRD CROS SROT VLS1 VLS2 WIRE AALP ZBUF ORNT TEXW CRND", Type::Bool);
    return t;
  }();
  return table;
}

// Retail's electric properties (CParticleElectricDataFactory::CreateELSM).
// GPSM and EPSM name child PARTs, read as ICTS is.
const PropertyTable& ElectricProperties() {
  static const PropertyTable table = [] {
    PropertyTable t;
    AddProperties(t, "LIFE SLIF SCNT SSEG", Type::Int);
    AddProperties(t, "GRAT AMPL AMPD LWD1 LWD2 LWD3", Type::Real);
    AddProperties(t, "COLR LCL1 LCL2 LCL3", Type::Color);
    AddProperties(t, "IEMT FEMT", Type::Emitter);
    AddProperties(t, "ZERY", Type::Bool);
    AddProperties(t, "GPSM EPSM", Type::Asset, F("PART"));
    AddProperties(t, "SSWH", Type::Asset, F("SWHC"));
    return t;
  }();
  return table;
}

// Port-only: the ELC2 (CParticleElectric2) fields retail's ELSM has no property for. An ELC2 is written
// as an ELSC stream that starts with the PEL2 marker and carries these (ELC2_RE.md).
const PropertyTable& Elc2Properties() {
  static const PropertyTable table = [] {
    PropertyTable t;
    AddProperties(t, "SIDE LWQD NFOM TSPN BDEP BNDI", Type::Int);
    AddProperties(t, "RADS LRAD RRAD SLVF BLNC BCFF BLEN BANG BIDF BLDF AMDF", Type::Real);
    AddProperties(t, "NCLR", Type::Color);
    AddProperties(t, "LLRD FPLB", Type::Bool);
    return t;
  }();
  return table;
}

const PropertyTable& RetailProperties() {
  static const PropertyTable table = [] {
    PropertyTable t;
    auto add = [&t](const char* names, Type type, uint32_t asset = 0) { AddProperties(t, names, type, asset); };
    add("PSLT PSWT MBSP MAXP LTME SEED NCSY CSSD NDSY PISY SISY SSSD SESD LTYP LFOT", Type::Int);
    add("PSTS GRTE SIZE ROTA LENG WIDT LINT LFOR LSLA ADV1 ADV2 ADV3 ADV4 ADV5 ADV6 ADV7 ADV8", Type::Real);
    add("PSIV PSOV ILOC IVEC POFS PMOP PMRT PMSC SSPO SEPO LOFF LDIR", Type::Vector);
    add("PSVM VEL1 VEL2 VEL3 VEL4", Type::ModVector);
    add("COLR PMCL LCLR", Type::Color);
    add("EMTR", Type::Emitter);
    add("TEXR TIND", Type::Texture);
    add("LIT_ ORNT RSOP AAPH ZBUF SORT MBLR VMD1 VMD2 VMD3 VMD4 CIND PMAB PMUS PMOO LINE FXLL OPTS", Type::Bool);
    add("PMDL", Type::Asset, F("CMDL"));
    add("ICTS IDTS IITS", Type::Asset, F("PART"));
    add("SSWH", Type::Asset, F("SWHC"));
    add("SELC", Type::Asset, F("ELSC"));
    return t;
  }();
  return table;
}

// The properties of a retail asset type, and the FourCC its file starts with.
const PropertyTable& PropertiesOf(uint32_t type) {
  if (type == F("SWHC")) {
    return SwooshProperties();
  }
  if (type == F("ELSC")) {
    return ElectricProperties();
  }
  return RetailProperties();
}

uint32_t HeaderOf(uint32_t type) {
  if (type == F("SWHC")) {
    return F("SWSH");
  }
  if (type == F("ELSC")) {
    return F("ELSM");
  }
  return F("GPSM");
}

// Each retail element per type, and its arguments: i int, r real, v vector,
// m mod vector, c colour, B bool (CNST and a byte), k a keyframe block, # a
// literal of the type (CNST's int or float). Empty is no arguments.
using Signatures = std::unordered_map<uint32_t, const char*>;

const Signatures& ElementsOf(Type type) {
  static const Signatures intElements = {
      {F("CNST"), "#"},  {F("KEYE"), "k"},  {F("KEYP"), "k"},   {F("TSCL"), "r"},  {F("DETH"), "ii"},
      {F("CHAN"), "iii"}, {F("ADD_"), "ii"}, {F("MULT"), "ii"},  {F("MODU"), "ii"}, {F("RAND"), "ii"},
      {F("IMPL"), "i"},  {F("ILPT"), "i"},  {F("SPAH"), "iii"}, {F("IRND"), "ii"}, {F("CLMP"), "iii"},
      {F("PULS"), "iiii"}, {F("NONE"), ""}, {F("RTOI"), "rr"},  {F("SUB_"), "ii"}, {F("GTCP"), ""},
      {F("GAPC"), ""},   {F("GEMT"), ""},
      // Port-only (CParticleDataFactory under TARGET_PC): FIAT start, count, divisor, loop.
      {F("FIAT"), "iiiB"},
      // Port-only PRAN: Remastered's int RAND (CIEPortRandom), min and max.
      {F("PRAN"), "ii"},
  };
  static const Signatures realElements = {
      {F("CNST"), "#"},   {F("NONE"), ""},    {F("KEYE"), "k"},    {F("KEYP"), "k"},   {F("SCAL"), "r"},
      {F("SINE"), "rrr"}, {F("ADD_"), "rr"},  {F("MULT"), "rr"},   {F("DOTP"), "vv"},  {F("RAND"), "rr"},
      {F("IRND"), "rr"},  {F("CHAN"), "rri"}, {F("CLMP"), "rrr"},  {F("PULS"), "iirr"}, {F("RLPT"), "r"},
      {F("LFTW"), "rr"},  {F("PRLW"), ""},    {F("PSLL"), ""},     {F("PAP1"), ""},    {F("PAP2"), ""},
      {F("PAP3"), ""},    {F("PAP4"), ""},    {F("PAP5"), ""},     {F("PAP6"), ""},    {F("PAP7"), ""},
      {F("PAP8"), ""},    {F("VXTR"), "v"},   {F("VYTR"), "v"},    {F("VZTR"), "v"},   {F("VMAG"), "v"},
      {F("ISWT"), "rr"},  {F("CLTN"), "rrrr"}, {F("CEQL"), "rrrr"}, {F("CRNG"), "rrrrr"}, {F("CEXT"), "i"},
      {F("ITRL"), "ir"},  {F("PSSZ"), ""},    {F("SUB_"), "rr"},  {F("GTCR"), "c"},    {F("GTCG"), "c"},   {F("GTCB"), "c"},
      {F("GTCA"), "c"},
      {F("DFCS"), "rr"},  {F("DFCP"), "rr"},
      // Port-only: a converted KEWS, its KEYP read at the swoosh node key.
      {F("KEWS"), "r"},
      // Port-only (CParticleDataFactory under TARGET_PC).
      {F("DIVD"), "rr"},
  };
  static const Signatures vectorElements = {
      {F("NONE"), ""},      {F("CNST"), "rrr"}, {F("KEYE"), "k"},  {F("KEYP"), "k"},   {F("ANGC"), "rrrrr"},
      {F("CONE"), "vr"},    {F("CIRC"), "vvrrr"}, {F("CCLU"), "vvir"}, {F("ADD_"), "vv"}, {F("MULT"), "vv"},
      {F("CHAN"), "vvi"},   {F("PULS"), "iivv"}, {F("RTOV"), "r"},  {F("PLOC"), ""},    {F("PLCO"), ""},
      {F("PVEL"), ""},      {F("PSOF"), ""},    {F("PSOU"), ""},   {F("PSOR"), ""},    {F("PSTR"), ""},
      {F("SUB_"), "vv"},    {F("CTVC"), "c"},     {F("PQAD"), "vv"},
      // Port-only (CParticleDataFactory under TARGET_PC): NORM, and the generator's system
      // vectors (camera in world and in system space, external translation, scale and rotation as
      // Euler degrees), and PRZY.
      {F("NORM"), "v"},     {F("CMPS"), ""},      {F("CPSS"), ""},      {F("EXTT"), ""},
      {F("EXTS"), ""},      {F("EXTR"), ""},      {F("PRZY"), "vvv"},
      // Port-only CVEPortBasis: Remastered's particle space to retail's and back.
      {F("BSIS"), "v"},     {F("BSSC"), "v"},
      // Port-only CVEPortAngleCone: Remastered's ANCR with its REUL as three angles.
      {F("ANCR"), "rrrrrr"},
  };
  static const Signatures modVectorElements = {
      {F("NONE"), ""},     {F("CNST"), "rrr"},  {F("GRAV"), "v"},    {F("WIND"), "vr"},  {F("EXPL"), "rr"},
      {F("CHAN"), "mmi"},  {F("PULS"), "iimm"}, {F("IMPL"), "vrrrB"}, {F("LMPL"), "vrrrB"}, {F("EMPL"), "vrrrB"},
      {F("SWRL"), "vvrr"}, {F("BNCE"), "vvrrB"}, {F("SPOS"), "v"},
  };
  static const Signatures colorElements = {
      {F("CNST"), "rrrr"}, {F("KEYE"), "k"},   {F("KEYP"), "k"},    {F("FADE"), "ccr"}, {F("CFDE"), "ccrr"},
      {F("CHAN"), "cci"},  {F("PULS"), "iicc"}, {F("PCOL"), ""},    {F("NONE"), ""},
      // Port-only (CParticleDataFactory under TARGET_PC).
      {F("MULT"), "cc"},   {F("MDAO"), "cr"},
  };
  // SETR is left out: Remastered writes SEMR, which retail reads too.
  static const Signatures emitterElements = {
      {F("NONE"), ""}, {F("SEMR"), "vv"}, {F("SPHE"), "vrr"}, {F("ASPH"), "vrrrrrr"},
      // Port-only CEEPortAngleSphere: Remastered's ASPR with its REUL as three angles.
      {F("ASPR"), "vrrrrrrr"},
  };
  static const Signatures none;
  switch (type) {
  case Type::Int:
    return intElements;
  case Type::Real:
    return realElements;
  case Type::Vector:
    return vectorElements;
  case Type::ModVector:
    return modVectorElements;
  case Type::Color:
    return colorElements;
  case Type::Emitter:
    return emitterElements;
  default:
    return none;
  }
}

Type TypeOfLetter(char letter) {
  switch (letter) {
  case 'i':
    return Type::Int;
  case 'r':
    return Type::Real;
  case 'v':
    return Type::Vector;
  case 'm':
    return Type::ModVector;
  case 'c':
    return Type::Color;
  default:
    return Type::Bool;
  }
}

using port::AppendBE32;
using port::ReadLE16;
using port::ReadLE32;

float FloatFromBits(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

uint32_t FloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
}

float HalfToFloat(uint16_t half) {
  const uint32_t sign = uint32_t(half >> 15) << 31;
  const int exponent = (half >> 10) & 0x1f;
  const uint32_t mantissa = half & 0x3ff;
  uint32_t bits;
  if (exponent == 0) {
    // Zero or subnormal: value = mantissa * 2^-24.
    float value = float(mantissa) * (1.0f / 16777216.0f);
    std::memcpy(&bits, &value, 4);
    bits |= sign;
  } else if (exponent == 31) {
    bits = sign | 0x7f800000u | mantissa << 13;
  } else {
    bits = sign | uint32_t(exponent - 15 + 127) << 23 | mantissa << 13;
  }
  float out;
  std::memcpy(&out, &bits, 4);
  return out;
}

bool IsElement(const EffectValue& value, uint32_t fourcc) {
  return value.kind == EffectValue::Kind::Element && value.fourcc == fourcc;
}

// Whether an element or any element under it is `fourcc`.
bool ContainsElement(const EffectValue& value, uint32_t fourcc) {
  if (IsElement(value, fourcc)) {
    return true;
  }
  for (const EffectValue& arg : value.args) {
    if (ContainsElement(arg, fourcc)) {
      return true;
    }
  }
  return false;
}

// Whether every leaf of an element is literal data, so Remastered might fold it to a constant
// (over-reports: a RAND of constants counts too). A keyframe never is: CIEKeyframeEmitter and
// CIEKeyframeInput::GetConstOverride (0x2aa748, 0x2aa764) answer false.
bool ConstantLeaves(const EffectValue& value) {
  if (value.kind != EffectValue::Kind::Element) {
    return true;
  }
  if (value.args.empty() || (value.fourcc >> 8) == (F("KEYE") >> 8)) {
    return false;
  }
  for (const EffectValue& arg : value.args) {
    if (!ConstantLeaves(arg)) {
      return false;
    }
  }
  return true;
}

// SPAF/SPAI/SPAC/SPAV(#n, fallback): the parent system's additional variable n
// (GetRealElement 0x260de4: CElementParentAddlVar over SAddVarAddress(ctx 0, n),
// AddParentAddlVarRef). DPV*/TPV*(guid, fallback): the direct or topmost parent's
// PVAR variable (AddDirectParentExtVarRef 0x25584c, AddTopmostParentExtVarRef
// 0x255bf4). Each of the type it is read as.
bool IsParentRead(uint32_t fourcc, Type type) {
  switch (type) {
  case Type::Real:
    return fourcc == F("SPAF") || fourcc == F("DPVF") || fourcc == F("TPVF");
  case Type::Int:
    return fourcc == F("SPAI") || fourcc == F("DPVI") || fourcc == F("TPVI");
  case Type::Color:
    return fourcc == F("SPAC") || fourcc == F("DPVC") || fourcc == F("TPVC");
  case Type::Vector:
    return fourcc == F("SPAV") || fourcc == F("DPVV") || fourcc == F("TPVV");
  default:
    return false;
  }
}

// PSA0..PSA9: the system's own additional real n, or -1.
int SystemRealRead(uint32_t fourcc) {
  return fourcc >= F("PSA0") && fourcc <= F("PSA9") ? int(fourcc - F("PSA0")) : -1;
}

// SVR0..9, SVI0..9, SVC0..9, SVV0..9: the system's additional variables, as
// (EPortVarType, n), or type ~0u.
std::pair<uint32_t, uint32_t> SystemVarWrite(uint32_t fourcc) {
  const uint32_t prefix = fourcc & 0xffffff00u;
  const uint32_t digit = fourcc & 0xffu;
  if (digit < '0' || digit > '9') {
    return {~0u, 0};
  }
  const uint32_t type = prefix == (F("SVR0") & 0xffffff00u)   ? 0
                        : prefix == (F("SVI0") & 0xffffff00u) ? 1
                        : prefix == (F("SVC0") & 0xffffff00u) ? 2
                        : prefix == (F("SVV0") & 0xffffff00u) ? 3
                                                               : ~0u;
  return {type, digit - '0'};
}

// The PSAn the value reads anywhere, as a bit mask.
uint32_t SystemReadsOf(const EffectValue& value) {
  uint32_t mask = 0;
  if (value.kind == EffectValue::Kind::Element) {
    const int n = SystemRealRead(value.fourcc);
    if (n >= 0 && value.args.empty()) {
      mask |= 1u << n;
    }
  }
  for (const EffectValue& arg : value.args) {
    mask |= SystemReadsOf(arg);
  }
  return mask;
}

// Whether IElement::IsConstant holds: a CNST of words or of constants.
bool IsConstantValue(const EffectValue& value) {
  if (value.kind == EffectValue::Kind::Word || value.kind == EffectValue::Kind::Byte) {
    return true;
  }
  if (!IsElement(value, F("CNST")) || value.args.empty()) {
    return false;
  }
  return std::all_of(value.args.begin(), value.args.end(), IsConstantValue);
}

// CNST holding one word (an int, or a real's bits).
bool ConstWord(const EffectValue& value, uint32_t& word) {
  if (!IsElement(value, F("CNST")) || value.args.size() != 1 || value.args[0].kind != EffectValue::Kind::Word) {
    return false;
  }
  word = value.args[0].word;
  return true;
}

bool ConstIs(const EffectValue& value, uint32_t word) {
  uint32_t have;
  return ConstWord(value, have) && have == word;
}

// RAND or IRND of CNST 0 and CNST `high` (a real's bits or an int).
bool RangeFromZero(const EffectValue& value, uint32_t fourcc, uint32_t& high) {
  return IsElement(value, fourcc) && value.args.size() == 2 && ConstIs(value.args[0], 0) &&
         ConstWord(value.args[1], high);
}

// TRST's x scale of a particle that is mirrored half the time: KPIN(CREL(LTHN(RAND(0, 1)), 0.5, 1, -1)).
bool RandomMirror(const EffectValue& value) {
  uint32_t high;
  if (!IsElement(value, F("KPIN")) || value.args.size() != 1 || !IsElement(value.args[0], F("CREL"))) {
    return false;
  }
  const EffectValue& rel = value.args[0];
  return rel.args.size() == 4 && IsElement(rel.args[0], F("LTHN")) && rel.args[0].args.size() == 1 &&
         RangeFromZero(rel.args[0].args[0], F("RAND"), high) && high == FloatBits(1.0f) &&
         ConstIs(rel.args[1], FloatBits(0.5f)) && ConstIs(rel.args[2], FloatBits(1.0f)) &&
         ConstIs(rel.args[3], FloatBits(-1.0f));
}

// TXFB's transform: TRST(0, 0, x scale, 1, 0, raw) with the scale 1 or a random mirror.
bool FlipbookTransform(const EffectValue& value, bool& flip) {
  if (!IsElement(value, F("TRST")) || value.args.size() != 6 || !ConstIs(value.args[0], 0) ||
      !ConstIs(value.args[1], 0) || !ConstIs(value.args[3], FloatBits(1.0f)) || !ConstIs(value.args[4], 0)) {
    return false;
  }
  flip = RandomMirror(value.args[2]);
  return flip || ConstIs(value.args[2], FloatBits(1.0f));
}

// ---- The port-only material (VMAT), from a MATI and its shader ----

// aurora::gfx::vfx::Feature.
constexpr uint32_t kColorTex = 1, kOpacityTex = 2, kErosion = 4, kRamp = 8, kIndirect = 16, kDepthSoften = 32,
                   kThresholding = 64, kDualMod = 128, kOpacityFresnel = 256, kColorIndexing = 512, kAddColor = 1024,
                   kOpacityFade = 2048, kColorRgbOnly = 4096;
// aurora::gfx::vfx::Blend::Multiply, the port's stand-in for the FrameBuffer_* shaders.
constexpr uint32_t kBlendMultiply = 4;
constexpr uint32_t kBlendAdditiveOne = 5;  // aurora::gfx::vfx::Blend::AdditiveOne (RGB One/One)
// aurora::gfx::vfx::AlreadyExposed: an exposure policy carried in the features word, not a shader feature.
constexpr uint32_t kAlreadyExposed = 1u << 31;

// MTIN 756533fc-e832-48f2-b1ef-ebb41fcea14f, the wave orb's material, as the table's little-endian GUID.
constexpr EffectGuid kWaveOrbMati = {0xfc, 0x33, 0x65, 0x75, 0x32, 0xe8, 0xf2, 0x48,
                                     0xb1, 0xef, 0xeb, 0xb4, 0x1f, 0xce, 0xa1, 0x4f};

// PowerBall projectile/APSM child, MATI and constant PMDL.
// These are little-endian UUID bytes, as read from the Remastered GENP/MATI.
constexpr EffectGuid kPowerBallChild = {0x82, 0x07, 0x87, 0xae, 0xf4, 0x41, 0x26, 0x4c,
                                        0x88, 0x76, 0xc2, 0xf0, 0x70, 0xcf, 0x1e, 0x06};
constexpr EffectGuid kPowerBallMati = {0x46, 0x2a, 0x75, 0x86, 0x33, 0xe7, 0x23, 0x4d,
                                       0xad, 0xc8, 0xc7, 0x4f, 0x28, 0x15, 0x80, 0xec};
constexpr EffectGuid kPowerBallModel = {0x60, 0xae, 0xf6, 0x4b, 0xe5, 0x3b, 0x51, 0x4d,
                                        0x88, 0xbd, 0x50, 0xbb, 0xd1, 0xbd, 0xf6, 0x71};
constexpr uint32_t kPowerBallShader = 0xe899b1df;
// Power held-charge sphere: a different child/MATI, despite sharing the model and shader.
constexpr EffectGuid kPowerChargeChild = {0xdf, 0x39, 0xe6, 0xa9, 0xfe, 0x9d, 0x41, 0x42,
                                          0x84, 0x2e, 0x0b, 0x8f, 0x8a, 0x65, 0x48, 0xe2};
constexpr EffectGuid kPowerChargeMati = {0x3c, 0x1c, 0x7d, 0x90, 0xc8, 0xcc, 0x9b, 0x4b,
                                         0x87, 0x8f, 0xee, 0x8a, 0x41, 0xfc, 0x1e, 0x7f};

enum class Role { Color, Opacity, Ramp, Ramp2, Threshold, Indirect, Palette };

// A MATI texture parameter (`tag`) in the role it plays, and which CCH0
// components scale the indirect warp of its u and v (-1: not warped).
struct TexSpec {
  const char* tag;
  Role role;
  int warpU;
  int warpV;
};

// Order of the VMAT blob's Src records.
enum SrcIndex {
  kSrcErosion,
  kSrcThrX,
  kSrcThrY,
  kSrcThrW,
  kSrcFresnelX,
  kSrcFresnelY,
  kSrcFadeX,
  kSrcFadeY,
  kSrcIndexScale,
  kSrcIndexOffset,
  kSrcIndexRow,
  kSrcCount
};

// Where a uniform comes from: row >= 0 is component `comp` of PMTR row `row`;
// row < 0 with comp >= 0 is the MATI's CCH0[comp], a constant unless an SMTR
// overrides it.
struct SrcSpec {
  SrcIndex index;
  int row;
  int comp;
};

struct Recipe {
  uint32_t shader;
  uint32_t features;
  std::vector<TexSpec> textures;
  std::vector<SrcSpec> sources;
  int addRow = -1;         // PMTR row of the AddColor (rgb, scale); -1: none
  bool unitCch0 = false;   // shaders without v3 gain/bias support require identity CCH0.z/.w
  bool frameBuffer = false; // a FrameBuffer_* shader (scene x vc.rgb, warped by CCH0.xy): drawn as a multiply
};

// RECIPES.md "(b) Per shader" and RECIPES-all.md. Derived from each shader's
// own code; a Lit shader uses its perm-0 (000_0) maths, which is the unlit
// maths.
const Recipe* RecipeOf(uint32_t shader) {
  static const std::vector<Recipe> table = {
      // FrameBuffer_Indirect(_Opacity)_Unlit: the scene behind at the fragment, offset by TCH0's
      // (resp. TCH1's) warp x CCH0.xy, times vc.rgb; alpha vc.a (resp. x TCH0.x). The warp is dropped.
      {0x3db95827, 0, {}, {}, -1, false, true},
      {0xd955396d, kOpacityTex,
       {{"TCH0", Role::Opacity, -1, -1}},
       {}, -1, false, true},
      // VFX_IceChargeBeam (Lit): the PMTR colour pair blended by BCLR.x x NMAP.x, plus BCLR.rgb lit
      // through NMAP, x vc.rgb; alpha is vc.a alone. Approximated as BCLR.rgb x vc.rgb.
      {0x9e0605cd, kColorTex | kColorRgbOnly,
       {{"BCLR", Role::Color, -1, -1}},
       {}},
      {0x84e0fe6c, kOpacityTex,
       {{"TCH0", Role::Opacity, -1, -1}},
       {}},
      {0x642dd72b, kColorTex,
       {{"BCLR", Role::Color, -1, -1}},
       {}},
      {0x73118d89, kColorTex | kErosion,
       {{"BCLR", Role::Color, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0x5bbdb68a, kColorTex | kOpacityTex,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {}},
      {0x461071b8, kRamp,
       {{"TCH0", Role::Ramp, -1, -1}},
       {}},
      {0x75cdfb9a, kColorTex | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}},
       {}},
      {0x3c4deeae, kOpacityTex | kErosion,
       {{"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0xe1a93093, kColorTex | kIndirect,
       {{"BCLR", Role::Color, 0, 1}, {"TCH0", Role::Indirect, -1, -1}},
       {}},
      {0x12c96e89, kColorTex | kOpacityTex | kErosion,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0x8e6bfa33, kRamp | kIndirect | kThresholding,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Threshold, -1, -1}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x6c165efd, kOpacityTex | kRamp,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x034cb0d3, kOpacityTex | kDepthSoften,
       {{"TCH0", Role::Opacity, -1, -1}},
       {}},
      {0x2eaed6b8, kColorTex | kOpacityTex | kErosion,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0x17713aeb, kColorTex | kIndirect | kDepthSoften,
       {{"BCLR", Role::Color, 0, 1}, {"TCH0", Role::Indirect, -1, -1}},
       {}},
      {0x493fe0ff, kColorTex | kOpacityTex | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {}},
      {0x3fc4b661, kErosion | kRamp,
       {{"TCH0", Role::Ramp, -1, -1}},
       {{kSrcErosion, 2, 0}}},
      {0xe27969fe, kRamp | kDepthSoften,
       {{"TCH0", Role::Ramp, -1, -1}},
       {}},
      {0xccff18d9, kRamp | kIndirect | kThresholding | kOpacityFresnel,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Threshold, -1, -1}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcFresnelX, 2, 2}, {kSrcFresnelY, 2, 3}, {kSrcThrW, -1, 0}}},
      {0x7a29ff64, kOpacityTex | kAddColor,
       {{"TCH0", Role::Opacity, -1, -1}},
       {}, 0},  // [I] perm 001_0 (no 000_0)
      {0x08757dae, kColorTex | kErosion,
       {{"BCLR", Role::Color, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0x42d8bf93, kOpacityTex | kRamp | kIndirect,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x20e149b2, kColorTex | kOpacityTex | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {}},
      {0xfef02d58, kColorTex,
       {{"BCLR", Role::Color, -1, -1}},
       {}},
      {0x58d7fb7b, kColorTex | kErosion | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0xfa49ff3c, kRamp | kIndirect,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {}},
      {0x8177c4e0, kOpacityTex | kRamp | kDepthSoften,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0xb658b55f, kErosion | kRamp | kIndirect,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcErosion, 2, 0}}},
      {0x158f4374, kOpacityTex | kRamp | kIndirect | kThresholding,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Indirect, -1, -1}, {"TCH2", Role::Opacity, -1, -1}, {"TCH3", Role::Threshold, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0xccdf1984, kColorTex | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}},
       {}},
      {0xeba92100, kErosion | kRamp | kIndirect | kDepthSoften,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcErosion, 2, 0}}},
      {0x63c470ce, kColorTex | kIndirect,
       {{"BCLR", Role::Color, 0, 1}, {"TCH0", Role::Indirect, -1, -1}},
       {}},
      {0x4dfde16d, kOpacityTex | kErosion,
       {{"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0x55584bd3, kColorTex | kOpacityTex,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {}},
      {0x8e97872f, kRamp | kIndirect | kDepthSoften | kThresholding,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Threshold, -1, -1}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x28bf8533, kOpacityTex | kColorIndexing,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Palette, -1, -1}, {"TCH1", Role::Opacity, -1, -1}},
       {{kSrcIndexScale, 0, 0}, {kSrcIndexOffset, 0, 1}, {kSrcIndexRow, 0, 2}}},
      {0x9adb322c, kOpacityTex | kErosion | kDepthSoften,
       {{"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0xb8eed42e, kOpacityTex | kRamp | kIndirect,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x38fbea2d, kOpacityTex | kRamp | kIndirect | kDepthSoften,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x7116d51a, kColorTex | kErosion | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0x7d52bd6b, kRamp | kIndirect | kOpacityFade,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcFadeX, 2, 0}, {kSrcFadeY, 2, 1}}, -1, true},
      {0xb1e9a0e4, kRamp | kIndirect | kDepthSoften | kThresholding | kOpacityFresnel,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Threshold, -1, -1}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcFresnelX, 2, 2}, {kSrcFresnelY, 2, 3}, {kSrcThrW, -1, 0}}},
      {0x72cc98d8, kColorTex | kOpacityTex | kErosion | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0x2d3ee3e3, kColorTex | kThresholding,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Threshold, -1, -1}},
       {{kSrcThrX, 0, 0}, {kSrcThrY, 0, 1}, {kSrcThrW, -1, 0}}},
      {0xa327502c, kOpacityTex | kRamp | kIndirect | kDepthSoften,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x0cc45d52, kColorTex | kIndirect | kDepthSoften,
       {{"BCLR", Role::Color, 0, 1}, {"TCH0", Role::Indirect, -1, -1}},
       {}},
      {0xa617da1d, kColorTex | kOpacityTex | kErosion | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0xbdf7eb7f, kRamp | kIndirect | kThresholding | kOpacityFresnel,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Threshold, -1, -1}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcFresnelX, 2, 2}, {kSrcFresnelY, 2, 3}, {kSrcThrW, -1, 0}}},
      {0x54844806, kOpacityTex | kDepthSoften,
       {{"TCH0", Role::Opacity, -1, -1}},
       {}},
      {0x1a9ef0d8, kOpacityTex | kAddColor,
       {{"TCH0", Role::Opacity, -1, -1}},
       {}, 0},
      {0x2da497da, kErosion | kDepthSoften | kColorIndexing,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Palette, -1, -1}},
       {{kSrcIndexScale, 0, 0}, {kSrcIndexOffset, 0, 1}, {kSrcIndexRow, 0, 2}, {kSrcErosion, 0, 3}}},
      {0xe4d4c263, kColorTex | kOpacityTex | kErosion | kDepthSoften | kOpacityFade,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {{kSrcFadeX, 0, 0}, {kSrcFadeY, 0, 1}, {kSrcErosion, 0, 2}}},
      {0x605484a4, kOpacityTex | kRamp | kDualMod,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Ramp2, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0xb9ccfc57, kColorTex | kIndirect | kDepthSoften,
       {{"BCLR", Role::Color, 0, 1}, {"TCH0", Role::Indirect, -1, -1}},
       {}},
      {0xe899b1df, kRamp | kIndirect | kDepthSoften | kOpacityFade,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcFadeX, 2, 0}, {kSrcFadeY, 2, 1}}, -1, false},
      {0x3a867837, kRamp | kIndirect,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {}},
      {0xf1c52f0d, kColorTex | kErosion | kOpacityFade,
       {{"BCLR", Role::Color, -1, -1}},
       {{kSrcFadeX, 0, 0}, {kSrcFadeY, 0, 1}, {kSrcErosion, 0, 2}}},
      {0xde9a121b, kOpacityTex | kErosion | kDepthSoften,
       {{"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0xdbe138ec, kOpacityTex | kErosion | kColorIndexing,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Palette, -1, -1}, {"TCH1", Role::Opacity, -1, -1}},
       {{kSrcIndexScale, 0, 0}, {kSrcIndexOffset, 0, 1}, {kSrcIndexRow, 0, 2}, {kSrcErosion, 0, 3}}},
      {0x49a43a10, kRamp,
       {{"TCH0", Role::Ramp, -1, -1}},
       {}},
      {0x9abe8368, kColorTex | kIndirect | kThresholding,
       {{"BCLR", Role::Color, 1, 2}, {"TCH0", Role::Threshold, -1, -1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcThrX, 0, 0}, {kSrcThrY, 0, 1}, {kSrcThrW, -1, 0}}},
      {0xfbfffd6f, kRamp | kThresholding,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Threshold, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0xd471baa6, kOpacityTex | kRamp | kIndirect | kThresholding,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Indirect, -1, -1}, {"TCH2", Role::Opacity, -1, -1}, {"TCH3", Role::Threshold, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x0fb73e1b, kColorTex | kIndirect,
       {{"BCLR", Role::Color, 0, 1}, {"TCH0", Role::Indirect, -1, -1}},
       {}},
      {0x96e205db, kRamp | kIndirect | kDualMod | kOpacityFade,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Ramp2, 2, 3}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcFadeX, 2, 0}, {kSrcFadeY, 2, 1}}},
      {0xb995317d, kRamp | kIndirect | kDualMod | kOpacityFade,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Ramp2, 2, 3}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcFadeX, 2, 0}, {kSrcFadeY, 2, 1}}},
      {0x93df8f48, kRamp | kIndirect | kThresholding,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Threshold, -1, -1}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x641fc76e, kColorTex | kDepthSoften | kThresholding,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Threshold, -1, -1}},
       {{kSrcThrX, 0, 0}, {kSrcThrY, 0, 1}, {kSrcThrW, -1, 0}}},
      {0x5f2a0715, kErosion | kRamp | kIndirect,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcErosion, 2, 0}}},
      {0x0cbeb3de, kOpacityTex | kRamp | kDualMod,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Ramp2, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x5283e927, kErosion | kRamp,
       {{"TCH0", Role::Ramp, -1, -1}},
       {{kSrcErosion, 2, 0}}},
      {0x10dbd66e, kOpacityTex | kRamp | kDepthSoften | kThresholding,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Opacity, -1, -1}, {"TCH2", Role::Threshold, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x71dc2465, kRamp | kIndirect | kDepthSoften | kDualMod,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Ramp2, 2, 3}, {"TCH2", Role::Indirect, -1, -1}},
       {}},
      {0xec6b1bb2, kOpacityTex | kRamp | kThresholding,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Opacity, -1, -1}, {"TCH2", Role::Threshold, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x83549ed0, kOpacityTex | kDepthSoften | kColorIndexing,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Palette, -1, -1}, {"TCH1", Role::Opacity, -1, -1}},
       {{kSrcIndexScale, 0, 0}, {kSrcIndexOffset, 0, 1}, {kSrcIndexRow, 0, 2}}},
      {0xcc9405ea, kRamp | kDualMod,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Ramp2, -1, -1}},
       {}},
      {0x9ea86831, kRamp | kDualMod,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Ramp2, -1, -1}},
       {}},
      {0x33ba3497, kDepthSoften | kColorIndexing,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Palette, -1, -1}},
       {{kSrcIndexScale, 0, 0}, {kSrcIndexOffset, 0, 1}, {kSrcIndexRow, 0, 2}}},
      {0xeeb36096, kOpacityTex | kRamp | kDepthSoften | kDualMod,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Ramp2, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x1ad75997, kRamp | kIndirect | kOpacityFade,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcFadeX, 2, 0}, {kSrcFadeY, 2, 1}}, -1, true},
      {0x574c1724, kColorTex | kIndirect | kDepthSoften | kThresholding,
       {{"BCLR", Role::Color, 1, 2}, {"TCH0", Role::Threshold, -1, -1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcThrX, 0, 0}, {kSrcThrY, 0, 1}, {kSrcThrW, -1, 0}}},
      {0x4e386476, kRamp | kIndirect | kDepthSoften,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {}},
      {0x68bf7bc6, kRamp | kDepthSoften | kDualMod,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Ramp2, -1, -1}},
       {}},
      {0x3fd46bb7, kOpacityTex | kRamp | kDualMod | kOpacityFade,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Ramp2, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {{kSrcFadeX, 2, 0}, {kSrcFadeY, 2, 1}}},
      {0x207abf3f, kRamp | kThresholding,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Threshold, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x33b751f2, kRamp | kIndirect | kDualMod,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Ramp2, 2, 3}, {"TCH2", Role::Indirect, -1, -1}},
       {}},
      {0x8d9c7aa7, kOpacityTex | kRamp | kDepthSoften,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x440fbd96, kColorTex | kOpacityTex | kErosion | kOpacityFade,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {{kSrcFadeX, 0, 0}, {kSrcFadeY, 0, 1}, {kSrcErosion, 0, 2}}},
      {0xc5cd782b, kOpacityTex | kRamp,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0xfb91aaad, kErosion | kRamp | kDepthSoften,
       {{"TCH0", Role::Ramp, -1, -1}},
       {{kSrcErosion, 2, 0}}},
      {0x555e40fa, kRamp | kDepthSoften | kThresholding,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Threshold, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x80a47126, kColorTex | kIndirect | kThresholding,
       {{"BCLR", Role::Color, 1, 2}, {"TCH0", Role::Threshold, -1, -1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcThrX, 0, 0}, {kSrcThrY, 0, 1}, {kSrcThrW, -1, 0}}},
      {0xb32b6148, kRamp | kIndirect | kDepthSoften | kDualMod,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Ramp2, 2, 3}, {"TCH2", Role::Indirect, -1, -1}},
       {}},
  };
  for (const Recipe& recipe : table) {
    if (recipe.shader == shader) {
      return &recipe;
    }
  }
  return nullptr;
}

struct MatiTexture {
  EffectGuid guid{};
  uint32_t texCoord = 0;
  int32_t filter = -1;
  int32_t wrapX = -1;
  int32_t wrapY = -1;
};

constexpr uint32_t kNoHeaderBlend = 0xffffffffu;

struct Mati {
  uint32_t shader = 0;  // the four bytes at 0x48, big-endian
  uint32_t headerBlend = kNoHeaderBlend;  // the header's EBlendMode, the LE u32 at 0x61
  std::map<std::string, MatiTexture> textures;
  std::map<std::string, std::array<float, 4>> vectors;
};

using port::ReadLEFloat;

// The header's shader and the parameter entries (u8 type + four tag bytes +
// payload): 6 texture (guid, texCoord, filter, wrap x/y/z; 36 bytes), 3 vec4,
// 1 float, 0 int. An unknown type ends the list.
bool ParseMati(const std::vector<uint8_t>& d, Mati& out) {
  if (d.size() < 0x6d) {
    return false;
  }
  out.shader = port::GetBE32(d, 0x48);
  out.headerBlend = ReadLE32(d.data() + 0x61);
  const uint32_t count = ReadLE32(d.data() + 0x69);
  size_t at = 0x6d;
  for (uint32_t i = 0; i < count && at + 5 <= d.size(); ++i) {
    const uint8_t type = d[at];
    const std::string tag(reinterpret_cast<const char*>(d.data() + at + 1), 4);
    at += 5;
    const size_t size = type == 6 ? 36 : type == 3 ? 16 : type == 1 || type == 0 ? 4 : 0;
    if (size == 0 || at + size > d.size()) {
      break;
    }
    if (type == 6) {
      MatiTexture texture;
      std::memcpy(texture.guid.data(), d.data() + at, 16);
      texture.texCoord = ReadLE32(d.data() + at + 16);
      texture.filter = int32_t(ReadLE32(d.data() + at + 20));
      texture.wrapX = int32_t(ReadLE32(d.data() + at + 24));
      texture.wrapY = int32_t(ReadLE32(d.data() + at + 28));
      out.textures[tag] = texture;
    } else if (type == 3) {
      std::array<float, 4> value;
      for (int c = 0; c < 4; ++c) {
        value[size_t(c)] = ReadLEFloat(d.data() + at + 4 * size_t(c));
      }
      out.vectors[tag] = value;
    }
    at += size;
  }
  return true;
}

// Remastered's wrap enum (-1 default, 0 clamp, 1 repeat, 2 mirror) as GXTexWrapMode.
// [I] the default taken as repeat.
uint32_t WrapOf(int32_t wrap) { return wrap == 0 ? 0 : wrap == 2 ? 2 : 1; }

// PVAR variable kinds, in the table's own order (reals, ints, colours, vectors, rotations).
// The numbers are what PVRT stores (EPortVarType).
struct PvarEntry {
  EffectGuid guid;
  uint32_t type;
  float def[4];
};

// One item of the port-only PSYS: the variable's EPortVarType and slot, whether
// only CAddVarSystem::Init evaluates it, and its element.
struct SystemItem {
  uint32_t type;
  uint32_t slot;
  uint32_t initOnly;
  std::vector<uint8_t> element;
};

// A zero of `type` as a retail CNST.
void Zero(Type type, std::vector<uint8_t>& out) {
  AppendBE32(out, F("CNST"));
  const int reals = type == Type::Vector ? 3 : type == Type::Color ? 4 : 0;
  if (reals == 0) {
    AppendBE32(out, 0);  // an int's 0 and a real's bits
  }
  for (int i = 0; i < reals; ++i) {
    AppendBE32(out, F("CNST"));
    AppendBE32(out, 0);
  }
}

uint32_t VarTypeOf(uint32_t fourcc) {
  return fourcc == F("VARF") ? 0 : fourcc == F("VARI") ? 1 : fourcc == F("VARC") ? 2 : 3;
}

class Writer {
public:
  Writer(const uint8_t* data, const EffectConvertIO& io) : m_data(data), m_io(io) {}

  uint32_t AssetId(const EffectGuid& id, uint32_t type) const {
    if (m_io.assetId) {
      return m_io.assetId(id, type);
    }
    return EffectRetailId(id).value_or(0);
  }

  // A written vector element taken through the port-only BSIS or BSSC (CVEPortBasis): (x, y, z) to
  // (-x, z, y) or (x, z, y). Each is its own inverse, so BSIS(BSIS(v)) is written as v, and a
  // constant is turned here.
  static void Basis(uint32_t op, const std::vector<uint8_t>& inner, std::vector<uint8_t>& out) {
    const auto word = [&inner](size_t at) { return port::ReadBE32(inner.data() + at); };
    if (inner.size() >= 4 && word(0) == op) {
      out.insert(out.end(), inner.begin() + 4, inner.end());
      return;
    }
    const uint32_t cnst = F("CNST");
    if (inner.size() == 28 && word(0) == cnst && word(4) == cnst && word(12) == cnst && word(20) == cnst) {
      AppendBE32(out, cnst);
      AppendBE32(out, cnst);
      AppendBE32(out, op == F("BSIS") ? word(8) ^ 0x80000000u : word(8));
      AppendBE32(out, cnst);
      AppendBE32(out, word(24));
      AppendBE32(out, cnst);
      AppendBE32(out, word(16));
      return;
    }
    AppendBE32(out, op);
    out.insert(out.end(), inner.begin(), inner.end());
  }

  // A vector element in Remastered's numbers, written as the port's (BSIS around it).
  bool PortVector(const EffectValue& value, std::vector<uint8_t>& out, std::string& why) const {
    std::vector<uint8_t> inner;
    if (!Element(value, Type::Vector, inner, why)) {
      return false;
    }
    Basis(F("BSIS"), inner, out);
    return true;
  }

  // -x for a real element: a constant's sign flipped, anything else MULT(x, CNST -1).
  bool NegatedReal(const EffectValue& value, std::vector<uint8_t>& out, std::string& why) const {
    std::vector<uint8_t> inner;
    if (!Element(value, Type::Real, inner, why)) {
      return false;
    }
    if (inner.size() == 8 && port::ReadBE32(inner.data()) == F("CNST")) {
      AppendBE32(out, F("CNST"));
      AppendBE32(out, port::ReadBE32(inner.data() + 4) ^ 0x80000000u);
      return true;
    }
    AppendBE32(out, F("MULT"));
    out.insert(out.end(), inner.begin(), inner.end());
    AppendBE32(out, F("CNST"));
    AppendBE32(out, FloatBits(-1.0f));
    return true;
  }

  // One element of `type` into `out`, or false with `why` set.
  //
  // Remastered's particle space is X left, Y up, Z forward; retail's is Z up. A vector tree is
  // written to give Remastered's numbers: reads of the port's state are taken into its space
  // (BSIS, BSSC for EXTS), MPCB(v) is BSIS(v), and the port turns the result back where it is
  // used (a vector property, a mod vector's or emitter's vector argument).
  bool Element(const EffectValue& value, Type type, std::vector<uint8_t>& out, std::string& why) const {
    if (value.kind != EffectValue::Kind::Element) {
      why = "a bare value where retail reads an element";
      return false;
    }
    // In a system variable, KPIN(x) and IRND(a, b) are evaluated once, by
    // CAddVarSystem::Init (CComplexKeepInitial::RebaseComplexOp; IRND is
    // CRERandomConstant): each becomes a hidden init-only real the tree reads.
    if (m_sysItems != nullptr && type == Type::Real &&
        ((value.fourcc == F("KPIN") && value.args.size() == 1) ||
         (value.fourcc == F("IRND") && value.args.size() == 2))) {
      SystemItem hidden{0, m_sysNextHidden++, 1, {}};
      if (value.fourcc == F("IRND")) {
        AppendBE32(hidden.element, F("RAND"));
        if (!Element(value.args[0], type, hidden.element, why) || !Element(value.args[1], type, hidden.element, why)) {
          return false;
        }
      } else if (!Element(value.args[0], type, hidden.element, why)) {
        return false;
      }
      AppendBE32(out, F("PSYR"));
      AppendBE32(out, hidden.slot);
      m_sysItems->push_back(std::move(hidden));
      return true;
    }
    // PSA0..PSA9: the system's own additional real n (GetRealElement 0x260de4:
    // AddRefVar(ctx 0, real, n)), which its SVRn sets. The port-only PSYR and n.
    if (type == Type::Real && value.args.empty() && SystemRealRead(value.fourcc) >= 0) {
      AppendBE32(out, F("PSYR"));
      AppendBE32(out, uint32_t(SystemRealRead(value.fourcc)));
      return true;
    }
    // Reads of the port's state, in retail's space, taken into Remastered's.
    if (type == Type::Vector && value.args.empty() &&
        (value.fourcc == F("PLOC") || value.fourcc == F("PLCO") || value.fourcc == F("PVEL") ||
         value.fourcc == F("PSOF") || value.fourcc == F("PSOU") || value.fourcc == F("PSOR") ||
         value.fourcc == F("PSTR") || value.fourcc == F("CMPS") || value.fourcc == F("CPSS") ||
         value.fourcc == F("EXTT") ||
         value.fourcc == F("EXTS"))) {
      AppendBE32(out, value.fourcc == F("EXTS") ? F("BSSC") : F("BSIS"));
      AppendBE32(out, value.fourcc);
      return true;
    }
    // MPCB(v) is MPT_CVEConvertToXLeftYUpZForward 0x2a3020, (-x, z, y) of a vector in
    // retail's space: BSIS(v). MCBS(v) is its scale form (0x2a30f8, (x, z, y)): BSSC(v).
    // MPCB(MPAC(x bias, y bias, x range, y range), magnitude) is retail's ANGC with the same five
    // arguments (checked against the disc's pairs), a direction in retail's space.
    if ((value.fourcc == F("MPCB") || value.fourcc == F("MCBS")) && type == Type::Vector) {
      std::vector<uint8_t> inner;
      if (value.args.size() == 1) {
        if (!Element(value.args[0], type, inner, why)) {
          return false;
        }
        Basis(value.fourcc == F("MCBS") ? F("BSSC") : F("BSIS"), inner, out);
        return true;
      }
      if (value.fourcc == F("MPCB") && value.args.size() == 2 && IsElement(value.args[0], F("MPAC")) &&
          value.args[0].args.size() == 4) {
        AppendBE32(inner, F("ANGC"));
        for (const EffectValue& arg : value.args[0].args) {
          if (!Element(arg, Type::Real, inner, why)) {
            return false;
          }
        }
        if (!Element(value.args[1], Type::Real, inner, why)) {
          return false;
        }
        Basis(F("BSIS"), inner, out);
        return true;
      }
      why = EffectFourCCString(value.fourcc) + " in a form retail has no element for";
      return false;
    }
    // MPCB(m) on a mod vector runs m in retail's space on the particle's position and velocity,
    // so m's vectors are not turned.
    if (value.fourcc == F("MPCB") && type == Type::ModVector) {
      if (value.args.size() != 1) {
        why = "MPCB in its angle form where retail reads a mod vector";
        return false;
      }
      const bool outer = m_mp1Space;
      m_mp1Space = true;
      const bool ok = Element(value.args[0], type, out, why);
      m_mp1Space = outer;
      return ok;
    }
    // A mod vector's CNST sets the velocity, (-x, z, y) of Remastered's.
    if (value.fourcc == F("CNST") && type == Type::ModVector && value.args.size() == 3 && !m_mp1Space) {
      AppendBE32(out, F("CNST"));
      return NegatedReal(value.args[0], out, why) && Element(value.args[2], Type::Real, out, why) &&
             Element(value.args[1], Type::Real, out, why);
    }
    // ANCR(REUL(x, y, z, #00), x range, y range, magnitude): the port-only ANCR
    // (x, y, z, x range, y range, magnitude) runs CVEAngleCone 0x29eebc as is, in
    // Remastered's space like the tree around it.
    if (value.fourcc == F("ANCR") && type == Type::Vector && value.args.size() == 4) {
      AppendBE32(out, F("ANCR"));
      if (!ConeRotation(value.args[0], out, why)) {
        return false;
      }
      for (size_t i = 1; i < 4; ++i) {
        if (!Element(value.args[i], Type::Real, out, why)) {
          return false;
        }
      }
      return true;
    }
    // ASPR(origin, REUL, range 1, range 2, radius, speed): the port-only ASPR
    // (origin, x, y, z, range 1, range 2, radius, speed) runs CVEAngleSphere
    // 0x2cb100 as is and hands the port its position and velocity.
    if (value.fourcc == F("ASPR") && type == Type::Emitter && value.args.size() == 6) {
      AppendBE32(out, F("ASPR"));
      if (!PortVector(value.args[0], out, why) || !ConeRotation(value.args[1], out, why)) {
        return false;
      }
      for (size_t i = 2; i < 6; ++i) {
        if (!Element(value.args[i], Type::Real, out, why)) {
          return false;
        }
      }
      return true;
    }
    // RNDV(magnitude): the port-only RNDV runs CVERandomVector 0x29e954 as is
    // (a cube point normalised by the ARM rsqrt estimate, so not uniform).
    if (value.fourcc == F("RNDV") && type == Type::Vector && value.args.size() == 1) {
      AppendBE32(out, F("RNDV"));
      return Element(value.args[0], Type::Real, out, why);
    }
    if (value.fourcc == F("GRAD") && type == Type::Color) {
      return Gradient(value, out, why);
    }
    // ILPT(n) rounds n% of the lifetime as Remastered does: the port-only PLPT, where the
    // property's lifetime context is known (LifetimeOffset); elsewhere retail's ILPT.
    if (value.fourcc == F("ILPT") && type == Type::Int && value.args.size() == 1 && m_lifetimeOffset >= 0) {
      AppendBE32(out, F("PLPT"));
      AppendBE32(out, uint32_t(m_lifetimeOffset));
      return Element(value.args[0], Type::Int, out, why);
    }
    // KEWS: an LE u32 key count, then that many LE floats over a swoosh's
    // points (newest 0, oldest 1). Written as the port-only KEWS around percent
    // keyframes: CREPortSwooshNodeTime reads them at the point's node key
    // (Remastered 0x24f00c), while true KEYPs keep the point's age.
    if (value.fourcc == F("KEWS") && type == Type::Real && value.args.size() == 1 &&
        value.args[0].kind == EffectValue::Kind::Raw) {
      const uint8_t* p = m_data + value.args[0].offset;
      const size_t size = value.args[0].size;
      const uint32_t count = size >= 4 ? ReadLE32(p) : 0;
      if (count < 2 || 4 + size_t(count) * 4 > size) {
        why = "KEWS without its keys";
        return false;
      }
      AppendBE32(out, F("KEWS"));
      AppendBE32(out, F("KEYP"));
      AppendBE32(out, 1);
      AppendBE32(out, 0);
      out.push_back(0);
      out.push_back(0);
      AppendBE32(out, 101);
      AppendBE32(out, 0);
      AppendBE32(out, 101);
      for (int percent = 0; percent <= 100; ++percent) {
        const float at = float(percent) / 100.0f * float(count - 1);
        const uint32_t i = std::min(uint32_t(at), count - 2);
        float a, b;
        std::memcpy(&a, p + 4 + 4 * i, 4);
        std::memcpy(&b, p + 8 + 4 * i, 4);
        AppendBE32(out, FloatBits(a + (b - a) * (at - float(i))));
      }
      return true;
    }
    // SLCT(index, ARRY(entries)): Remastered's CElementArray<T>::GetValue (float
    // 0x27121c, CVector3f 0x27186c, CColor4f16 0x271ee4) evaluates the index,
    // takes the entry at index % count (C remainder) and evaluates it, each time
    // the value is read (per particle per frame for COLR). Written as the
    // port-only SLCT: the index element, the count, then the entries.
    if (value.fourcc == F("SLCT") && (type == Type::Real || type == Type::Vector || type == Type::Color) &&
        value.args.size() == 2 && IsElement(value.args[1], F("ARRY"))) {
      const std::vector<EffectValue>& array = value.args[1].args;
      if (array.size() < 2) {
        why = "SLCT over an ARRY whose entries do not parse";
        return false;
      }
      AppendBE32(out, F("SLCT"));
      if (!Element(value.args[0], Type::Int, out, why)) {
        return false;
      }
      AppendBE32(out, uint32_t(array.size() - 1));
      for (size_t i = 1; i < array.size(); ++i) {
        if (!Element(array[i], type, out, why)) {
          return false;
        }
      }
      return true;
    }
    // MPRD(a, b): a random int between two, as RAND.
    if (value.fourcc == F("MPRD") && value.args.size() == 2 && (type == Type::Int || type == Type::Real)) {
      AppendBE32(out, F("RAND"));
      return Element(value.args[0], type, out, why) && Element(value.args[1], type, out, why);
    }
    // Int RAND(min, max) (FUN_002709a4): min + Next() % (max - min + 1) (CIERandom 0x2a88a8,
    // CIERandomConstant 0x2a895c), the port-only PRAN; but with both constant and min < 0, raw
    // Next() (CIERandom2 0x2a89a0), which retail's RAND gives for a constant min <= 0.
    if (value.fourcc == F("RAND") && type == Type::Int && value.args.size() == 2) {
      auto literal = [](const EffectValue& c) {
        return IsElement(c, F("CNST")) && c.args.size() == 1 &&
               (c.args[0].kind == EffectValue::Kind::Word || c.args[0].kind == EffectValue::Kind::Byte);
      };
      const EffectValue& min = value.args[0];
      const EffectValue& max = value.args[1];
      const bool negative =
          literal(min) && min.args[0].kind == EffectValue::Kind::Word && int32_t(min.args[0].word) < 0;
      if ((!literal(min) && ConstantLeaves(min)) || (negative && !literal(max) && ConstantLeaves(max))) {
        why = "int RAND whose arguments may fold to constants";
        return false;
      }
      const bool rawNext = negative && literal(max);
      AppendBE32(out, rawNext ? F("RAND") : F("PRAN"));
      return Element(value.args[0], type, out, why) && Element(value.args[1], type, out, why);
    }
    // DFCS/DFCP(a, b): a ramp of the depth from the camera, a port-only real element.
    if ((value.fourcc == F("DFCP") || value.fourcc == F("DFCS")) && value.args.size() == 2 &&
        type == Type::Real) {
      AppendBE32(out, value.fourcc);
      return Element(value.args[0], Type::Real, out, why) &&
             Element(value.args[1], Type::Real, out, why);
    }
    // GPUA is CREGPUAvailabilty, how much GPU time is free (to thin effects
    // out under load): taken as 1, all of it.
    if (value.fourcc == F("GPUA") && value.args.empty() && type == Type::Real) {
      m_approximated.push_back("GPUA taken as 1");
      AppendBE32(out, F("CNST"));
      AppendBE32(out, FloatBits(1.0f));
      return true;
    }
    // A parent's variable (IsParentRead), as the port-only element of the same
    // fourcc: n (SPA*) or the guid (DPV*/TPV*), then the fallback. The fallback
    // is read only when the parent has no such variable, and only a constant
    // one (CREParentAdditionalValue::GetValue 0x296824); any other reads 0. An
    // n above 9 is never a variable (0xff).
    if (value.args.size() == 2 && IsParentRead(value.fourcc, type)) {
      const bool slot = (value.fourcc & 0xffffff00u) == (F("SPAF") & 0xffffff00u);
      if (slot ? (value.args[0].kind != EffectValue::Kind::Byte && value.args[0].kind != EffectValue::Kind::Word)
               : value.args[0].kind != EffectValue::Kind::Guid) {
        why = EffectFourCCString(value.fourcc) + " without its variable";
        return false;
      }
      AppendBE32(out, value.fourcc);
      if (slot) {
        AppendBE32(out, value.args[0].word > 9 ? 0xffu : value.args[0].word);
      } else {
        out.insert(out.end(), value.args[0].guid.begin(), value.args[0].guid.end());
      }
      if (IsConstantValue(value.args[1])) {
        return Element(value.args[1], type, out, why);
      }
      Zero(type, out);
      return true;
    }
    // VARF/VARI/VARV/VARC(guid): the generator's own variable. Port-only elements, written as
    // the fourcc and the variable's index in the PVRT table (PVAR guid order).
    if (value.args.size() == 1 && value.args[0].kind == EffectValue::Kind::Guid) {
      const bool ours = (value.fourcc == F("VARF") && type == Type::Real) ||
                        (value.fourcc == F("VARI") && type == Type::Int) ||
                        (value.fourcc == F("VARV") && type == Type::Vector) ||
                        (value.fourcc == F("VARC") && type == Type::Color);
      if (ours) {
        for (size_t i = 0; i < m_vars.size(); ++i) {
          if (m_vars[i].guid == value.args[0].guid && m_vars[i].type == VarTypeOf(value.fourcc)) {
            AppendBE32(out, value.fourcc);
            AppendBE32(out, uint32_t(i));
            return true;
          }
        }
        why = EffectFourCCString(value.fourcc) + " " + EffectGuidString(value.args[0].guid) +
              " is not in the effect's PVAR";
        return false;
      }
    }
    const Signatures& elements = ElementsOf(type);
    const auto found = elements.find(value.fourcc);
    if (found == elements.end()) {
      why = "retail has no element " + EffectFourCCString(value.fourcc) + " there";
      return false;
    }
    const std::string sig = found->second;
    AppendBE32(out, value.fourcc);
    if (sig == "#") {
      return Literal(value, type, out, why);
    }
    if (sig == "k") {
      return value.args.size() == 1 && Keyframes(value.args[0], type, out, why);
    }
    if (value.args.size() != sig.size()) {
      why = EffectFourCCString(value.fourcc) + " with " + std::to_string(value.args.size()) + " arguments, retail reads " +
            std::to_string(sig.size());
      return false;
    }
    // A mod vector's or emitter's vectors are used in the port's space.
    const bool portVectors = (type == Type::ModVector && !m_mp1Space) || type == Type::Emitter;
    for (size_t i = 0; i < sig.size(); ++i) {
      const bool ok = sig[i] == 'B'                   ? Bool(value.args[i], out, why)
                      : sig[i] == 'v' && portVectors ? PortVector(value.args[i], out, why)
                                                      : Element(value.args[i], TypeOfLetter(sig[i]), out, why);
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  // A cone's REUL(x, y, z, #00) as its three angle reals, which the port's ANCR and ASPR turn
  // into Remastered's order-0 quaternion themselves.
  bool ConeRotation(const EffectValue& reul, std::vector<uint8_t>& out, std::string& why) const {
    if (!IsElement(reul, F("REUL")) || reul.args.size() != 4) {
      why = "a cone rotation that is not REUL";
      return false;
    }
    const EffectValue& order = reul.args[3];
    if ((order.kind != EffectValue::Kind::Byte && order.kind != EffectValue::Kind::Word) || order.word != 0) {
      why = "REUL in an unseen axis order";
      return false;
    }
    for (size_t i = 0; i < 3; ++i) {
      if (!Element(reul.args[i], Type::Real, out, why)) {
        return false;
      }
    }
    return true;
  }

  // The Remastered particle basis B = (90, 0, 180) in PMRT's Euler convention: (-x, z, y), the
  // model converter's default basis. B is self-inverse.
  static void AppendRotationBasis(std::vector<uint8_t>& out) {
    AppendBE32(out, F("CNST"));
    for (const float angle : {90.f, 0.f, 180.f}) {
      AppendBE32(out, F("CNST"));
      AppendBE32(out, FloatBits(angle));
    }
  }

  // PMDL meshes are converted from Remastered coordinates with the model
  // converter's basis B. Conjugate the source rotation by B so it remains in the
  // converted model's frame: PQAD(B, PQAD(R, B)) is B * R * B^-1. R is
  // written as an Euler vector by SourceRotation; Element() preserves each real
  // expression, including nested IRND, and the PART's PIRN marker keeps those
  // samples stable per particle. A top-level MPCB(R) is B * R * B already
  // (0x29d69c), so it conjugates back to R.
  bool ModelRotation(const std::vector<EffectValue>& value, std::vector<uint8_t>& out, std::string& why) const {
    if (value.size() != 1) {
      why = "a rotation that is not REUL/RADD/RAZY/MPCB/EXTR";
      return false;
    }
    if (IsElement(value[0], F("MPCB")) && value[0].args.size() == 1) {
      return SourceRotation(value[0].args[0], out, why);
    }
    AppendBE32(out, F("PQAD"));
    AppendRotationBasis(out);
    AppendBE32(out, F("PQAD"));
    if (!SourceRotation(value[0], out, why)) {
      return false;
    }
    AppendRotationBasis(out);
    return true;
  }

  // A Remastered rotation element in Remastered's particle space, as a port Euler vector:
  // REUL(x, y, z, #00) -> CNST(x, y, z); RADD(a, b) -> PQAD(a, b) (the ordered product);
  // RAZY(first, second, fallback) -> the port-only PRZY (CERotationFromAxis 0x29d1bc as
  // GetRotationElement 0x25e418 builds it: Z = first, X = second x Z, Y = Z x X, or the
  // fallback when either is degenerate). PRZY's vectors stay in Remastered's space.
  // MPCB(r) -> PQAD(B, PQAD(r, B)) (qB * r * qB, 0x29d69c); EXTR (AddRefVar(0, 4, 10),
  // GetRotationElement 0x25d59c) -> the port-only EXTR vector, the system orientation.
  bool SourceRotation(const EffectValue& rot, std::vector<uint8_t>& out, std::string& why) const {
    if (IsElement(rot, F("REUL")) && rot.args.size() == 4) {
      const EffectValue& order = rot.args[3];
      if ((order.kind != EffectValue::Kind::Byte && order.kind != EffectValue::Kind::Word) || order.word != 0) {
        why = "a rotation order other than 0 (XYZ)";
        return false;
      }
      AppendBE32(out, F("CNST"));
      for (size_t i = 0; i < 3; ++i) {
        if (!Element(rot.args[i], Type::Real, out, why)) {
          return false;
        }
      }
      return true;
    }
    if (IsElement(rot, F("RADD")) && rot.args.size() == 2) {
      AppendBE32(out, F("PQAD"));
      return SourceRotation(rot.args[0], out, why) && SourceRotation(rot.args[1], out, why);
    }
    if (IsElement(rot, F("RAZY")) && rot.args.size() == 3) {
      AppendBE32(out, F("PRZY"));
      return Element(rot.args[0], Type::Vector, out, why) && Element(rot.args[1], Type::Vector, out, why) &&
             SourceRotation(rot.args[2], out, why);
    }
    if (IsElement(rot, F("MPCB")) && rot.args.size() == 1) {
      AppendBE32(out, F("PQAD"));
      AppendRotationBasis(out);
      AppendBE32(out, F("PQAD"));
      if (!SourceRotation(rot.args[0], out, why)) {
        return false;
      }
      AppendRotationBasis(out);
      return true;
    }
    if (IsElement(rot, F("EXTR")) && rot.args.empty()) {
      AppendBE32(out, F("EXTR"));
      return true;
    }
    why = "a rotation that is not REUL/RADD/RAZY/MPCB/EXTR";
    return false;
  }

  // GRAD: u8 stop count, per stop four halves (RGBA) and an f32 position,
  // then what the position runs over: ILPT(CNST(n)) (n% of the particle's
  // life) or CNST(n) (n frames), then a byte (1: the gradient repeats).
  // Written as the port-only PGRD element, which evaluates Remastered's CCEGradient (0x2c8d48)
  // exactly on the age in frames Remastered feeds it: a particle's age (UpdateExistingParticles
  // 0x24a26c) or a swoosh segment's (UpdateExisting 0x27d590). The period stays the source's own
  // int element: CNST n, or ILPT(CNST n) as the port-only PLPT (Remastered's rounding).
  bool Gradient(const EffectValue& value, std::vector<uint8_t>& out, std::string& why) const {
    if (value.args.size() != 1 || value.args[0].kind != EffectValue::Kind::Raw) {
      why = "GRAD without its stops";
      return false;
    }
    const uint8_t* p = m_data + value.args[0].offset;
    const size_t size = value.args[0].size;
    const size_t stops = size == 0 ? 0 : p[0];
    size_t at = 1 + 12 * stops;
    if (stops == 0 || at > size) {
      why = "GRAD without its stops";
      return false;
    }
    AppendBE32(out, F("PGRD"));
    AppendBE32(out, uint32_t(stops));
    for (size_t i = 0; i < stops; ++i) {
      for (size_t c = 0; c < 4; ++c) {
        AppendBE32(out, FloatBits(HalfToFloat(uint16_t(p[1 + 12 * i + 2 * c] | p[2 + 12 * i + 2 * c] << 8))));
      }
      AppendBE32(out, ReadLE32(p + 1 + 12 * i + 8));  // the f32 position, bits unchanged
    }
    if (at + 12 <= size && ReadLE32(p + at) == F("ILPT") && ReadLE32(p + at + 4) == F("CNST")) {
      if (m_lifetimeOffset < 0) {
        m_approximated.push_back("GRAD over the life taken over the port's lifetime (context unverified)");
      }
      AppendBE32(out, F("PLPT"));
      AppendBE32(out, uint32_t(std::max(m_lifetimeOffset, 0)));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, ReadLE32(p + at + 8));
      at += 12;
    } else if (at + 8 <= size && ReadLE32(p + at) == F("CNST")) {
      AppendBE32(out, F("CNST"));
      AppendBE32(out, ReadLE32(p + at + 4));
      at += 8;
    } else {
      why = "GRAD over something other than the life or frames";
      return false;
    }
    out.push_back(at < size && p[at] != 0 ? 1 : 0);
    return true;
  }

  // What the port's particle lifetime needs added to be the one an ILPT in this property reads,
  // or -1 where the port's evaluation context is unverified. Remastered's ILPT (CREParticleLifetime
  // 0x29694c) reads the CVarContext slot the property's tag picks: tag -> EVarContextTime by the
  // table at 0x1d129b4 (1 -> 0, 3 -> 2, 5 -> 4), then GetDefaultVarContextTime 0x2513dc (2 -> slot
  // 1, 4 -> slot 2, else 0). Slot 0 is the system's PSLT (CElementGen ctor 0x244cfc), as the port's
  // light update has it; slot 1 a particle's LTM2 (0x24c0dc), one above the port's LTME (Property),
  // or a swoosh's life, which the port's swoosh lifetime equals.
  static int LifetimeOffset(bool part, bool swoosh, const EffectProperty& property) {
    if (part && property.tag == 3) {
      return 1;
    }
    if (part && property.tag == 1 && property.fourcc == F("LCLR")) {
      return 0;
    }
    if (swoosh && property.tag == 3 && property.fourcc == F("COLR")) {
      return 0;
    }
    return -1;
  }

  // CNST's literal: retail reads an s32 for an int and a float for a real.
  bool Literal(const EffectValue& value, Type type, std::vector<uint8_t>& out, std::string& why) const {
    if (value.args.size() != 1) {
      why = "CNST with " + std::to_string(value.args.size()) + " arguments";
      return false;
    }
    const EffectValue& arg = value.args[0];
    if (arg.kind == EffectValue::Kind::Word) {
      AppendBE32(out, arg.word);
      return true;
    }
    if (arg.kind == EffectValue::Kind::Byte) {
      AppendBE32(out, type == Type::Int ? arg.word : FloatBits(float(arg.word)));
      return true;
    }
    why = "CNST holding neither a number nor a byte";
    return false;
  }

  // GetBool: a class id (CNST) and a byte.
  bool Bool(const EffectValue& value, std::vector<uint8_t>& out, std::string& why) const {
    const EffectValue* byte = &value;
    if (IsElement(value, F("CNST")) && value.args.size() == 1) {
      byte = &value.args[0];
    }
    if (byte->kind != EffectValue::Kind::Byte && byte->kind != EffectValue::Kind::Word) {
      why = "not a flag";
      return false;
    }
    AppendBE32(out, F("CNST"));
    out.push_back(byte->word != 0 ? 1 : 0);
    return true;
  }

  // A keyframe block: percent, unknown, loop, unknown (u32 u32 u8 u8), loop
  // end and start (u32 u32), a count and that many keys of the type's size:
  // 4 bytes for an int or real, 12 for a vector, 16 for a colour. Remastered
  // may store a colour's key as four halves; those are widened.
  bool Keyframes(const EffectValue& value, Type type, std::vector<uint8_t>& out, std::string& why) const {
    if (value.kind != EffectValue::Kind::Keys || value.size < 22) {
      why = "not a keyframe block";
      return false;
    }
    const uint8_t* p = m_data + value.offset;
    const uint32_t count = ReadLE32(p + 18);
    const size_t keys = value.size - 22;
    const size_t want = type == Type::Color ? 16 : type == Type::Vector ? 12 : 4;
    const size_t keySize = count == 0 ? want : keys / count;
    const bool halves = type == Type::Color && keySize == 8;
    if ((count == 0 && keys != 0) || (count != 0 && keys % count != 0) || (keySize != want && !halves)) {
      why = "keyframes of " + std::to_string(keySize) + " bytes where retail reads " + std::to_string(want);
      return false;
    }
    AppendBE32(out, ReadLE32(p));
    AppendBE32(out, ReadLE32(p + 4));
    out.push_back(p[8]);
    out.push_back(p[9]);
    AppendBE32(out, ReadLE32(p + 10));
    AppendBE32(out, ReadLE32(p + 14));
    AppendBE32(out, count);
    if (halves) {
      for (size_t at = 22; at < value.size; at += 2) {
        AppendBE32(out, FloatBits(HalfToFloat(uint16_t(p[at] | p[at + 1] << 8))));
      }
      return true;
    }
    for (size_t at = 22; at < value.size; at += 4) {
      AppendBE32(out, ReadLE32(p + at));
    }
    return true;
  }

  // An id, bare or as CNST(id); NONE is no asset.
  bool Asset(const std::vector<EffectValue>& value, uint32_t type, std::vector<uint8_t>& out, std::string& why) const {
    if (value.size() == 1 && IsElement(value[0], F("NONE"))) {
      AppendBE32(out, F("NONE"));
      return true;
    }
    // A zero id names nothing, as NONE does.
    if (value.size() == 1 && value[0].kind == EffectValue::Kind::Guid && value[0].guid == EffectGuid{}) {
      AppendBE32(out, F("NONE"));
      return true;
    }
    const EffectValue* guid = nullptr;
    if (value.size() == 1 && value[0].kind == EffectValue::Kind::Guid) {
      guid = &value[0];
    } else if (value.size() == 1 && IsElement(value[0], F("CNST")) && value[0].args.size() == 1 &&
               value[0].args[0].kind == EffectValue::Kind::Guid) {
      guid = &value[0].args[0];
    } else if (value.size() == 2 && value[0].kind == EffectValue::Kind::Byte && value[1].kind == EffectValue::Kind::Guid) {
      guid = &value[1];
    }
    if (guid == nullptr) {
      why = "not an asset id";
      return false;
    }
    const uint32_t id = AssetId(guid->guid, type);
    if (id == 0) {
      why = EffectFourCCString(type) + " " + EffectGuidString(guid->guid) + " has no retail id";
      return false;
    }
    AppendBE32(out, F("CNST"));
    AppendBE32(out, id);
    return true;
  }

  // PATL: TXP2 (an atlas, a random tile), TXFB (an array texture over the
  // particle's life, with TRST's optional random mirror) or ATX2 (an atlas over
  // the particle's life). Only those patterns.
  bool Atlas(const std::vector<EffectValue>& value, std::vector<uint8_t>& out, std::string& why) const {
    const EffectValue& head = value[0];
    const std::string name = EffectFourCCString(head.fourcc);
    if (head.args.empty() || head.args[0].kind != EffectValue::Kind::Guid) {
      why = name + " without a texture";
      return false;
    }
    uint32_t id = 0;
    int32_t cols = 0, rows = 0, count = 0, mode = 0, flip = 0;
    if (head.fourcc == F("TXP2")) {
      uint32_t c, r, high;
      if (value.size() != 1 || head.args.size() != 4 || !ConstWord(head.args[1], c) || !ConstWord(head.args[2], r) ||
          c == 0 || r == 0 || c > 64 || r > 64 || !RangeFromZero(head.args[3], F("IRND"), high)) {
        why = "TXP2 that is not an atlas with a random tile";
        return false;
      }
      cols = int32_t(c);
      rows = int32_t(r);
      // The range is a real over the whole atlas, or an int tile index.
      if (high == FloatBits(1.0f)) {
        count = cols * rows;
      } else if (high < uint32_t(cols * rows)) {
        count = int32_t(high) + 1;
      } else {
        why = "TXP2 tile range " + std::to_string(high) + " is not the atlas";
        return false;
      }
      id = AssetId(head.args[0].guid, F("TXTR"));
      if (id == 0) {
        why = "TXTR " + EffectGuidString(head.args[0].guid) + " has no retail id";
        return false;
      }
    } else if (head.fourcc == F("ATX2")) {
      // ATX2(id, cols, rows, cycle frames, loop): with the cycle ILPT(100),
      // retail's whole lifetime in frames, the atlas plays once over the life.
      uint32_t c, r;
      const EffectValue& cycle = head.args.size() == 5 ? head.args[3] : head;
      if (value.size() != 1 || head.args.size() != 5 || !ConstWord(head.args[1], c) ||
          !ConstWord(head.args[2], r) || c == 0 || r == 0 || c > 64 || r > 64 || !IsElement(cycle, F("ILPT")) ||
          cycle.args.size() != 1 || !ConstIs(cycle.args[0], 100)) {
        why = "ATX2 that is not an atlas over the particle's life";
        return false;
      }
      id = AssetId(head.args[0].guid, F("TXTR"));
      if (id == 0) {
        why = "TXTR " + EffectGuidString(head.args[0].guid) + " has no retail id";
        return false;
      }
      cols = int32_t(c);
      rows = int32_t(r);
      count = cols * rows;
      mode = 1;
    } else {
      uint32_t high;
      bool mirror = false;
      if (value.size() > 2 || head.args.size() != 2 || !IsElement(head.args[1], F("LFTW")) ||
          !RangeFromZero(head.args[1], F("LFTW"), high) || high != FloatBits(1.0f) ||
          (value.size() == 2 && !FlipbookTransform(value[1], mirror))) {
        why = "TXFB that is not a flipbook over the particle's life";
        return false;
      }
      const FlipbookAtlas atlas = m_io.flipbook ? m_io.flipbook(head.args[0].guid) : FlipbookAtlas{};
      if (atlas.id == 0) {
        why = "TXTR " + EffectGuidString(head.args[0].guid) + " has no flipbook atlas";
        return false;
      }
      id = atlas.id;
      cols = atlas.cols;
      rows = atlas.rows;
      count = atlas.frames;
      mode = 1;
      flip = mirror ? 1 : 0;
    }
    AppendBE32(out, F("PATL"));
    AppendBE32(out, F("CNST"));
    AppendBE32(out, id);
    for (const int32_t v : {cols, rows, count, mode, flip}) {
      AppendBE32(out, F("CNST"));
      AppendBE32(out, uint32_t(v));
    }
    return true;
  }

  // PMDL's SLCT(index, ARRY of n CNST ids), as the model ids a particle picks from by its seed.
  // Remastered takes entry index % n; IRND(0, h) is latched per particle, so the list is
  // expanded to h + 1 entries (entry i = model i % n) to keep its odds, and CNST(k) is one model.
  bool Variants(const std::vector<EffectValue>& value, std::vector<uint32_t>& ids, std::string& why) const {
    uint32_t high = 0, constant = 0;
    bool fixed = false;
    if (value.size() != 1 || !IsElement(value[0], F("SLCT")) || value[0].args.size() != 2 ||
        !IsElement(value[0].args[1], F("ARRY")) || value[0].args[1].args.empty() ||
        value[0].args[1].args[0].kind != EffectValue::Kind::Raw) {
      why = "a model choice that is not SLCT(index, ARRY)";
      return false;
    }
    if (ConstWord(value[0].args[0], constant)) {
      fixed = true;
    } else if (!RangeFromZero(value[0].args[0], F("IRND"), high) || int32_t(high) < 0 || high >= 64) {
      why = "a model choice indexed by neither CNST nor IRND(0, h < 64)";
      return false;
    }
    const EffectValue& array = value[0].args[1].args[0];
    const uint8_t* p = m_data + array.offset;
    const uint32_t count = array.size >= 4 ? ReadLE32(p) : 0;
    if (count == 0 || count > 256 || array.size != 4 + size_t(count) * 20) {
      why = "SLCT ARRY of " + std::to_string(count) + " models in " + std::to_string(array.size) + " bytes";
      return false;
    }
    std::vector<uint32_t> models;
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* entry = p + 4 + size_t(i) * 20;
      EffectGuid guid;
      std::memcpy(guid.data(), entry + 4, 16);
      if (ReadLE32(entry) != F("CNST")) {
        why = "SLCT model that is not a constant";
        return false;
      }
      const uint32_t id = AssetId(guid, F("CMDL"));
      if (id == 0) {
        why = "CMDL " + EffectGuidString(guid) + " has no retail id";
        return false;
      }
      models.push_back(id);
    }
    if (fixed) {
      // C remainder, as CElementArray::GetValue: a negative index stays negative.
      const int32_t index = int32_t(constant) % int32_t(count);
      if (index < 0) {
        why = "SLCT model index " + std::to_string(int32_t(constant)) + " is negative";
        return false;
      }
      ids.push_back(models[size_t(index)]);
      return true;
    }
    for (uint32_t i = 0; i <= high; ++i) {
      ids.push_back(models[i % count]);
    }
    return true;
  }

  // TEXR/TIND: Remastered's `CNST(id), NONE` or `ATEX(id, ...)`, retail's
  // `CNST CNST id` and `ATEX CNST id ...`.
  bool Texture(const std::vector<EffectValue>& value, std::vector<uint8_t>& out, std::string& why) const {
    if (value.empty() || value[0].kind != EffectValue::Kind::Element) {
      why = "not a texture";
      return false;
    }
    const EffectValue& head = value[0];
    if (head.fourcc == F("NONE")) {
      AppendBE32(out, F("NONE"));
      return true;
    }
    if (head.fourcc == F("TXP2") || head.fourcc == F("TXFB") || head.fourcc == F("ATX2")) {
      return Atlas(value, out, why);
    }
    const bool animated = head.fourcc == F("ATEX");
    if ((head.fourcc != F("CNST") && !animated) || head.args.empty() || head.args[0].kind != EffectValue::Kind::Guid) {
      why = "texture element " + EffectFourCCString(head.fourcc) + " retail does not have";
      return false;
    }
    const uint32_t id = AssetId(head.args[0].guid, F("TXTR"));
    if (id == 0) {
      why = "TXTR " + EffectGuidString(head.args[0].guid) + " has no retail id";
      return false;
    }
    AppendBE32(out, head.fourcc);
    AppendBE32(out, F("CNST"));
    AppendBE32(out, id);
    if (!animated) {
      return true;
    }
    // ATEX: tile width and height, stride width and height, cycle frames, loop.
    const char* sig = "iiiiiB";
    if (head.args.size() != 7) {
      why = "ATEX with " + std::to_string(head.args.size()) + " arguments";
      return false;
    }
    for (size_t i = 0; i < 6; ++i) {
      const bool ok = sig[i] == 'B' ? Bool(head.args[i + 1], out, why) : Element(head.args[i + 1], Type::Int, out, why);
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  // One property's value, as the type retail reads it.
  bool Property(uint32_t fourcc, const PropertyType& type, const std::vector<EffectValue>& value,
                std::vector<uint8_t>& out, std::string& why) const {
    switch (type.type) {
    case Type::Bool:
      if (value.size() != 1) {
        why = "not a flag";
        return false;
      }
      return Bool(value[0], out, why);
    case Type::Texture:
      return Texture(value, out, why);
    case Type::Asset:
      return Asset(value, type.asset, out, why);
    default:
      break;
    }
    // LFOT and LTYP: Remastered writes the enum as a byte. The exe maps it to retail's
    // int as LFOT 0-3 unchanged and LTYP 0, 2, 1, 3 (its point and spot are swapped).
    if (fourcc == F("LFOT") || fourcc == F("LTYP")) {
      const EffectValue* byte = value.size() == 1 ? &value[0] : nullptr;
      if (byte != nullptr && IsElement(*byte, F("CNST")) && byte->args.size() == 1) {
        byte = &byte->args[0];
      }
      if (byte != nullptr && byte->kind == EffectValue::Kind::Byte) {
        if (byte->word > 3) {
          why = "enum byte " + std::to_string(byte->word);
          return false;
        }
        static constexpr uint32_t kLightType[4] = {0, 2, 1, 3};
        AppendBE32(out, F("CNST"));
        AppendBE32(out, fourcc == F("LTYP") ? kLightType[byte->word] : byte->word);
        return true;
      }
    }
    if (value.size() != 1) {
      why = "a value of " + std::to_string(value.size()) + " parts where retail reads one element";
      return false;
    }
    // ROTA: Remastered turns the other way. MULT(x, -1) is x in retail; anything
    // else is wrapped in MULT(..., CNST -1).
    // LTM2 is one frame longer than retail's LTME: constants, and the bounds
    // of a random lifetime, come down by one.
    if (fourcc == F("LTME")) {
      const EffectValue& v = value[0];
      auto constant = [](const EffectValue& c) {
        return IsElement(c, F("CNST")) && c.args.size() == 1 &&
               (c.args[0].kind == EffectValue::Kind::Word || c.args[0].kind == EffectValue::Kind::Byte);
      };
      auto putLess = [&out](const EffectValue& c) {
        AppendBE32(out, F("CNST"));
        AppendBE32(out, c.args[0].word == 0 ? 0 : c.args[0].word - 1);
      };
      if (constant(v)) {
        putLess(v);
        return true;
      }
      if ((IsElement(v, F("IRND")) || IsElement(v, F("RAND")) || IsElement(v, F("MPRD"))) && v.args.size() == 2 &&
          constant(v.args[0]) && constant(v.args[1])) {
        // Int RAND of constants is PRAN for min >= 0, retail's RAND (raw Next()) below, as in Element.
        const bool pran = v.fourcc == F("RAND") && int32_t(v.args[0].args[0].word) >= 0;
        AppendBE32(out, v.fourcc == F("MPRD") ? F("RAND") : pran ? F("PRAN") : v.fourcc);
        putLess(v.args[0]);
        putLess(v.args[1]);
        return true;
      }
      AppendBE32(out, F("SUB_"));
      if (!Element(v, Type::Int, out, why)) {
        return false;
      }
      AppendBE32(out, F("CNST"));
      AppendBE32(out, 1);
      return true;
    }
    if (fourcc == F("ROTA")) {
      const EffectValue& v = value[0];
      if (IsElement(v, F("CNST")) && v.args.size() == 1 && v.args[0].kind == EffectValue::Kind::Word) {
        AppendBE32(out, F("CNST"));
        AppendBE32(out, v.args[0].word ^ 0x80000000u);
        return true;
      }
      if (IsElement(v, F("MULT")) && v.args.size() == 2 && IsElement(v.args[1], F("CNST")) && v.args[1].args.size() == 1 &&
          v.args[1].args[0].kind == EffectValue::Kind::Word && v.args[1].args[0].word == FloatBits(-1.0f)) {
        return Element(v.args[0], Type::Real, out, why);
      }
      AppendBE32(out, F("MULT"));
      if (!Element(v, Type::Real, out, why)) {
        return false;
      }
      AppendBE32(out, F("CNST"));
      AppendBE32(out, FloatBits(-1.0f));
      return true;
    }
    // A vector property is Remastered's numbers, used in the port's space: BSIS, or BSSC for
    // PMSC's scale. PMRT is turned as a rotation (ModelRotation), and PSOV is never written.
    if (type.type == Type::Vector && fourcc != F("PMRT") && fourcc != F("PSOV")) {
      std::vector<uint8_t> inner;
      if (!Element(value[0], type.type, inner, why)) {
        return false;
      }
      Basis(fourcc == F("PMSC") ? F("BSSC") : F("BSIS"), inner, out);
      return true;
    }
    return Element(value[0], type.type, out, why);
  }

  // A swoosh or electric child a spawn table starts: retail has one of each
  // per generator (SSWH at frame SSSD, SELC at frame SESD), not in KSSM.
  struct Started {
    uint32_t type;  // SWHC or ELSC
    uint32_t id;
    uint32_t frame;
  };

  // Retail's KSSM after its FourCC, from Remastered's: CNST, four ints (the
  // third is the end frame) and per frame its PART ids. Empty when nothing
  // but swooshes and electric children are spawned (they go to `started`).
  // Remastered's tables are merged and its SEVT events (moves) dropped; what
  // a table's selector chooses is not known.
  bool SpawnTable(const EffectProperty& property, std::vector<uint8_t>& out, std::vector<Started>& started,
                  std::string& why) const {
    EffectSpawnTable table;
    if (!ParseSpawnTable(m_data, property.offset + property.size, property, table)) {
      why = "spawn table does not parse";
      return false;
    }
    if (table.tables.empty()) {
      return true;
    }
    std::map<uint32_t, std::vector<uint32_t>> frames;
    size_t unresolved = 0;
    bool conditional = false;
    bool selected = false;
    for (const EffectSpawnTable::Table& each : table.tables) {
      selected = selected || !IsElement(each.selector, F("CNST")) || each.selector.args.size() != 1 ||
                 each.selector.args[0].kind != EffectValue::Kind::Word || each.selector.args[0].word != 0;
      for (const EffectSpawnTable::Frame& frame : each.frames) {
        for (const EffectSpawnTable::Spawn& spawn : frame.spawns) {
          const uint32_t type = spawn.form == F("GENP")   ? F("PART")
                                : spawn.form == F("SWSH") ? F("SWHC")
                                : spawn.form == F("ELC2") || spawn.form == F("ELSM") ? F("ELSC")
                                                                                     : 0;
          const uint32_t id = type != 0 ? AssetId(spawn.id, type) : 0;
          if (id == 0) {
            ++unresolved;
            continue;
          }
          conditional = conditional || spawn.conditional;
          if (type == F("PART")) {
            frames[frame.frame].push_back(id);
          } else {
            started.push_back({type, id, frame.frame});
          }
        }
      }
    }
    if (frames.empty() && started.empty()) {
      why = "no spawned child resolves";
      return false;
    }
    if (table.events != 0) {
      m_approximated.push_back("KSSM: " + std::to_string(table.events) + " SEVT events dropped");
    }
    if (table.tables.size() > 1) {
      m_approximated.push_back("KSSM: " + std::to_string(table.tables.size()) + " tables merged");
    }
    if (selected) {
      m_approximated.push_back("KSSM: table selector ignored");
    }
    if (conditional) {
      m_approximated.push_back("KSSM: spawn conditions ignored, every spawn always starts");
    }
    if (unresolved != 0) {
      m_approximated.push_back("KSSM: " + std::to_string(unresolved) + " spawns unresolved");
    }
    if (frames.empty()) {
      return true;
    }
    AppendBE32(out, F("CNST"));
    for (uint32_t word : {0u, 1u, table.header[2], 0u}) {
      AppendBE32(out, word);
    }
    AppendBE32(out, uint32_t(frames.size()));
    for (const auto& [frame, ids] : frames) {
      AppendBE32(out, frame);
      AppendBE32(out, uint32_t(ids.size()));
      for (uint32_t id : ids) {
        AppendBE32(out, id);
        for (int i = 0; i < 3; ++i) {
          AppendBE32(out, 0);
        }
      }
    }
    return true;
  }

  // Port-only PKSM after its FourCC: Remastered's spawn tables as its
  // CSpawnSystemKeyframeData reads them (0x293350), for CElementGen::PortSpawnTables.
  // CNST, the end frame, the table count; per table its start, 1 if the period is a
  // constant, the period (an int element) and the entry count; per entry its frame and
  // spawn count; per spawn its type, asset id, instance count (an int element) and the
  // index of the component that places it (~0 for none); then the component count and
  // each component (Component()). `node` is the generator, for RSYX.
  // False with `why` set when a table holds what the port cannot run as Remastered does.
  bool SpawnTables(const EffectProperty& property, const EffectNode& node, std::vector<uint8_t>& out,
                   std::string& why) const {
    EffectSpawnTable table;
    if (!ParseSpawnTable(m_data, property.offset + property.size, property, table)) {
      why = "spawn table does not parse";
      return false;
    }
    if (table.components.size() != table.events) {
      why = std::to_string(table.events) + " SEVT events, " + std::to_string(table.components.size()) + " parsed";
      return false;
    }
    if (table.tables.empty()) {
      return true;
    }
    std::vector<uint8_t> bytes;
    size_t unresolved = 0;
    AppendBE32(bytes, F("CNST"));
    AppendBE32(bytes, table.header[2]);
    AppendBE32(bytes, uint32_t(table.tables.size()));
    for (const EffectSpawnTable::Table& each : table.tables) {
      AppendBE32(bytes, each.word);
      AppendBE32(bytes, IsElement(each.selector, F("CNST")) ? 1u : 0u);
      if (!Element(each.selector, Type::Int, bytes, why)) {
        why = "period: " + why;
        return false;
      }
      AppendBE32(bytes, uint32_t(each.frames.size()));
      for (const EffectSpawnTable::Frame& frame : each.frames) {
        std::vector<uint8_t> spawns;
        uint32_t count = 0;
        for (const EffectSpawnTable::Spawn& spawn : frame.spawns) {
          const uint32_t type = spawn.form == F("GENP")   ? F("PART")
                                : spawn.form == F("SWSH") ? F("SWHC")
                                : spawn.form == F("ELC2") || spawn.form == F("ELSM") ? F("ELSC")
                                                                                     : 0;
          const uint32_t id = type != 0 ? AssetId(spawn.id, type) : 0;
          if (id == 0) {
            ++unresolved;
            continue;
          }
          if (spawn.component != ~0u && spawn.component >= table.components.size()) {
            why = "a spawn placed by missing component " + std::to_string(spawn.component);
            return false;
          }
          // PEOD: GetNumInstances 0x2940fc evaluates the count per spawn. A tree that needs
          // storage (CElementDep::Finish 0x24f9a4: IRND of constants is initial-only, and so is
          // a node whose inputs all are) is a ctx-0 variable CAddVarSystem::Init sets once: a
          // hidden init-only PSYS int the port-only PSYI reads. Other initial-only trees are not
          // proven.
          // Shipped counts are PEOD(e) or NONE (count 1).
          const EffectValue& instances = IsElement(spawn.instances, F("PEOD")) && spawn.instances.args.size() == 1
                                         ? spawn.instances.args[0]
                                         : spawn.instances;
          const bool latched = IsElement(instances, F("IRND")) && instances.args.size() == 2 &&
                               IsElement(instances.args[0], F("CNST")) &&
                               IsElement(instances.args[1], F("CNST"));
          if (!latched && (ContainsElement(instances, F("IRND")) || ContainsElement(instances, F("KPIN")))) {
            why = "instance instances with an initial-only " + EffectFourCCString(instances.fourcc);
            return false;
          }
          AppendBE32(spawns, type);
          AppendBE32(spawns, id);
          if (latched) {
            if (m_spawnSysItems == nullptr) {
              why = "an IRND instance instances outside a generator";
              return false;
            }
            SystemItem hidden{1, m_sysNextHiddenInt++, 1, {}};
            AppendBE32(hidden.element, F("PRAN"));
            if (!Element(instances.args[0], Type::Int, hidden.element, why) ||
                !Element(instances.args[1], Type::Int, hidden.element, why)) {
              return false;
            }
            AppendBE32(spawns, F("PSYI"));
            AppendBE32(spawns, hidden.slot);
            m_spawnSysItems->push_back(std::move(hidden));
          } else if (IsElement(instances, F("NONE"))) {
            AppendBE32(spawns, F("NONE"));
          } else if (!Element(instances, Type::Int, spawns, why)) {
            why = "instance instances: " + why;
            return false;
          }
          AppendBE32(spawns, spawn.component);
          ++count;
        }
        AppendBE32(bytes, frame.frame);
        AppendBE32(bytes, count);
        bytes.insert(bytes.end(), spawns.begin(), spawns.end());
      }
    }
    AppendBE32(bytes, uint32_t(table.components.size()));
    for (size_t i = 0; i < table.components.size(); ++i) {
      if (!Component(table.components[i], node, bytes, why)) {
        why = "component " + std::to_string(i) + ": " + why;
        return false;
      }
    }
    if (unresolved != 0) {
      m_approximated.push_back("KSSM: " + std::to_string(unresolved) + " spawns unresolved");
    }
    out.insert(out.end(), bytes.begin(), bytes.end());
    return true;
  }

  // A spawn component, SEVT(SMOV(...)), for the evaluator in CElementGen
  // (Remastered's CSpawnComponent, build/mpr/kb): its SMOV as Mover() writes it.
  bool Component(const EffectValue& value, const EffectNode& node, std::vector<uint8_t>& out,
                 std::string& why) const {
    if (!IsElement(value, F("SEVT")) || value.args.size() != 1) {
      why = "not SEVT(SMOV)";
      return false;
    }
    return Mover(value.args[0], node, out, why);
  }

  // A mover, SMOV(position, delta, rotation, scale, modifier) (CSimpleMoverElement),
  // written as the position (a vector node), the delta's three floats (it must be a
  // constant: the mover keeps its first value), the rotation (NONE or a rotation node)
  // and the modifier: NONE, or GRAV and its vector. The scale must be NONE.
  bool Mover(const EffectValue& value, const EffectNode& node, std::vector<uint8_t>& out,
             std::string& why) const {
    if (!IsElement(value, F("SMOV")) || value.args.size() != 5) {
      why = "not SMOV";
      return false;
    }
    const std::vector<EffectValue>& args = value.args;
    if (!ComponentVector(args[0], node, out, why)) {
      return false;
    }
    const EffectValue& delta = args[1];
    if (!IsElement(delta, F("CNST")) || delta.args.size() != 3) {
      why = "delta " + EffectFourCCString(delta.fourcc) + " is not a constant";
      return false;
    }
    for (const EffectValue& each : delta.args) {
      std::vector<uint8_t> word;
      if (!IsElement(each, F("CNST")) || !Literal(each, Type::Real, word, why)) {
        why = "delta is not a constant";
        return false;
      }
      out.insert(out.end(), word.begin(), word.end());
    }
    if (IsElement(args[2], F("NONE"))) {
      AppendBE32(out, F("NONE"));
    } else if (!ComponentRotation(args[2], node, out, why)) {
      return false;
    }
    if (!IsElement(args[3], F("NONE"))) {
      why = "scale " + EffectFourCCString(args[3].fourcc);
      return false;
    }
    const EffectValue& modifier = args[4];
    if (IsElement(modifier, F("NONE"))) {
      AppendBE32(out, F("NONE"));
      return true;
    }
    // GRAV(acceleration, flag, next): the flag set and a chained modifier are unseen.
    if (!IsElement(modifier, F("GRAV")) || modifier.args.size() != 3 ||
        (modifier.args[1].kind != EffectValue::Kind::Byte && modifier.args[1].kind != EffectValue::Kind::Word) ||
        modifier.args[1].word != 0 || !IsElement(modifier.args[2], F("NONE"))) {
      why = "modifier " + EffectFourCCString(modifier.fourcc);
      return false;
    }
    AppendBE32(out, F("GRAV"));
    return ComponentVector(modifier.args[0], node, out, why);
  }

  // A component's real: CNST, ADD_, MULT, IRND, RAND, VARF, or KEYE on the emitter frame.
  bool ComponentReal(const EffectValue& value, std::vector<uint8_t>& out, std::string& why) const {
    const uint32_t fourcc = value.kind == EffectValue::Kind::Element ? value.fourcc : 0;
    if (fourcc == F("CNST")) {
      AppendBE32(out, fourcc);
      return Literal(value, Type::Real, out, why);
    }
    if (fourcc == F("VARF") || (value.args.empty() && SystemRealRead(fourcc) >= 0)) {
      return Element(value, Type::Real, out, why);
    }
    if (fourcc == F("KEYE") && value.args.size() == 1) {
      const EffectValue& keys = value.args[0];
      if (keys.kind != EffectValue::Kind::Keys || keys.size < 22 || ReadLE32(m_data + keys.offset) != 0 ||
          ReadLE32(m_data + keys.offset + 18) == 0) {
        why = "KEYE on the particle's lifetime or with no keys";
        return false;
      }
      AppendBE32(out, fourcc);
      return Keyframes(keys, Type::Real, out, why);
    }
    if ((fourcc == F("ADD_") || fourcc == F("MULT") || fourcc == F("IRND") || fourcc == F("RAND")) &&
        value.args.size() == 2) {
      AppendBE32(out, fourcc);
      return ComponentReal(value.args[0], out, why) && ComponentReal(value.args[1], out, why);
    }
    why = "real " + EffectFourCCString(value.fourcc);
    return false;
  }

  // A component's vector: CNST(x, y, z), EXTT, EXTS, ADD_, MULT, ROTV(vector, rotation),
  // KPIN, or CIRC(centre, normal, angle, angle per frame, radius).
  bool ComponentVector(const EffectValue& value, const EffectNode& node, std::vector<uint8_t>& out,
                       std::string& why) const {
    const uint32_t fourcc = value.kind == EffectValue::Kind::Element ? value.fourcc : 0;
    if ((fourcc == F("EXTT") || fourcc == F("EXTS")) && value.args.empty()) {
      AppendBE32(out, fourcc);
      return true;
    }
    if (fourcc == F("KPIN") && value.args.size() == 1) {
      AppendBE32(out, fourcc);
      return ComponentVector(value.args[0], node, out, why);
    }
    if ((fourcc == F("ADD_") || fourcc == F("MULT")) && value.args.size() == 2) {
      AppendBE32(out, fourcc);
      return ComponentVector(value.args[0], node, out, why) && ComponentVector(value.args[1], node, out, why);
    }
    if (fourcc == F("ROTV") && value.args.size() == 2) {
      AppendBE32(out, fourcc);
      return ComponentVector(value.args[0], node, out, why) && ComponentRotation(value.args[1], node, out, why);
    }
    if (fourcc == F("CNST") && value.args.size() == 3) {
      AppendBE32(out, fourcc);
      return ComponentReal(value.args[0], out, why) && ComponentReal(value.args[1], out, why) &&
             ComponentReal(value.args[2], out, why);
    }
    if (fourcc == F("CIRC") && value.args.size() == 5) {
      AppendBE32(out, fourcc);
      return ComponentVector(value.args[0], node, out, why) && ComponentVector(value.args[1], node, out, why) &&
             ComponentReal(value.args[2], out, why) && ComponentReal(value.args[3], out, why) &&
             ComponentReal(value.args[4], out, why);
    }
    why = "vector " + EffectFourCCString(value.fourcc);
    return false;
  }

  // A component's rotation: EXTR, REUL(x, y, z, order 0) in degrees, RADD(a, b) = a * b,
  // or RSYX, the generator's own system rotation: with XFMD 4 and an identity SMVR the
  // emitter's (EXTR), with XFMD 0 or 2 none (written IDNT).
  bool ComponentRotation(const EffectValue& value, const EffectNode& node, std::vector<uint8_t>& out,
                         std::string& why) const {
    const uint32_t fourcc = value.kind == EffectValue::Kind::Element ? value.fourcc : 0;
    if (fourcc == F("EXTR") && value.args.empty()) {
      AppendBE32(out, fourcc);
      return true;
    }
    if (fourcc == F("RADD") && value.args.size() == 2) {
      AppendBE32(out, fourcc);
      return ComponentRotation(value.args[0], node, out, why) && ComponentRotation(value.args[1], node, out, why);
    }
    if (fourcc == F("REUL") && value.args.size() == 4) {
      const EffectValue& order = value.args[3];
      if ((order.kind != EffectValue::Kind::Byte && order.kind != EffectValue::Kind::Word) || order.word != 0) {
        why = "REUL in an unseen axis order";
        return false;
      }
      AppendBE32(out, fourcc);
      return ComponentReal(value.args[0], out, why) && ComponentReal(value.args[1], out, why) &&
             ComponentReal(value.args[2], out, why);
    }
    if (fourcc == F("RSYX") && value.args.empty()) {
      const EffectProperty* xfmd = nullptr;
      const EffectProperty* mover = nullptr;
      for (const EffectProperty& property : node.properties) {
        if (property.fourcc == F("XFMD")) {
          xfmd = &property;
        } else if (property.fourcc == F("SMVR")) {
          mover = &property;
        }
      }
      uint32_t mode = ~0u;
      if (xfmd != nullptr && SmallConst(*xfmd, mode) && mode == 4 && mover != nullptr && IdentityMover(*mover)) {
        AppendBE32(out, F("EXTR"));
        return true;
      }
      if (xfmd != nullptr && (mode == 0 || mode == 2)) {
        AppendBE32(out, F("IDNT"));
        return true;
      }
      why = "RSYX under XFMD " + (xfmd != nullptr ? std::to_string(mode) : std::string("none"));
      return false;
    }
    why = "rotation " + EffectFourCCString(value.fourcc);
    return false;
  }

  // A swoosh PROT turning about Z only: [KPIN](REUL(0, 0, z, #00)) or [KPIN](RADD(EXTR or INTR,
  // REUL(0, 0, z, #00))), as port-only PROZ flags (bit 0 keep the birth value, bit 1 start from the
  // orientation) and z in degrees. Remastered evaluates PROT at every point's birth and, unless it is
  // initial-only, again every frame for every live point (UpdateExisting 0x27d590). KPIN keeps its
  // child's value at birth (CREKeepInitial 0x297478, CElementDep::Finish 0x24f9a4); a z whose every
  // varying part sits under KPIN is therefore kept whole. SUST evaluates on the system's frame
  // (CRESolveUsingSystemTime 0x298ef8), the frame the port evaluates swoosh elements on, so it is
  // dropped; a timed SINE outside it would run on the point's age and is left out. EXTR and INTR are
  // both the orientation SetOrientation gives under XFMD 2.
  bool SwooshZRotation(const EffectValue& prot, uint32_t& flags, std::vector<uint8_t>& out, std::string& why) const {
    const EffectValue* value = &prot;
    flags = 0;
    if (IsElement(*value, F("KPIN")) && value->args.size() == 1) {
      flags |= 1;
      value = &value->args[0];
    }
    if (IsElement(*value, F("RADD")) && value->args.size() == 2 &&
        (IsElement(value->args[0], F("EXTR")) || IsElement(value->args[0], F("INTR"))) &&
        value->args[0].args.empty()) {
      flags |= 2;
      value = &value->args[1];
    }
    uint32_t word = 0;
    if (!IsElement(*value, F("REUL")) || value->args.size() != 4 || !ConstWord(value->args[0], word) ||
        (word & 0x7fffffffu) != 0 || !ConstWord(value->args[1], word) || (word & 0x7fffffffu) != 0) {
      why = "not [KPIN]([RADD(EXTR or INTR, ]REUL(0, 0, z))";
      return false;
    }
    const EffectValue& order = value->args[3];
    if ((order.kind != EffectValue::Kind::Byte && order.kind != EffectValue::Kind::Word) || order.word != 0) {
      why = "REUL in an unseen axis order";
      return false;
    }
    bool kept = false;
    bool live = false;
    std::vector<uint8_t> angle;
    if (!SwooshAngle(value->args[2], false, false, kept, live, angle, why)) {
      return false;
    }
    if (kept && live && (flags & 1) == 0) {
      why = "z mixes kept and per-frame parts";
      return false;
    }
    if (kept) {
      flags |= 1;
    }
    out.insert(out.end(), angle.begin(), angle.end());
    return true;
  }

  // A swoosh PROT's z angle as a retail real with KPIN and SUST taken out (see SwooshZRotation).
  // `kept` reports a KPIN, `live` a part varying outside one.
  bool SwooshAngle(const EffectValue& value, bool underKpin, bool underSust, bool& kept, bool& live,
                   std::vector<uint8_t>& out, std::string& why) const {
    const uint32_t fourcc = value.kind == EffectValue::Kind::Element ? value.fourcc : 0;
    if (fourcc == F("KPIN") && value.args.size() == 1) {
      kept = true;
      return SwooshAngle(value.args[0], true, underSust, kept, live, out, why);
    }
    if (fourcc == F("SUST") && value.args.size() == 1) {
      return SwooshAngle(value.args[0], underKpin, true, kept, live, out, why);
    }
    if (fourcc == F("CNST")) {
      return Element(value, Type::Real, out, why);
    }
    if (fourcc == F("KEYE") && value.args.size() == 1) {
      const EffectValue& keys = value.args[0];
      if (keys.kind != EffectValue::Kind::Keys || keys.size < 22 || ReadLE32(m_data + keys.offset) != 0) {
        why = "z KEYE on the particle's lifetime";
        return false;
      }
      live = live || !underKpin;
      return Element(value, Type::Real, out, why);
    }
    if ((fourcc == F("RAND") || fourcc == F("IRND")) && value.args.size() == 2) {
      live = live || !underKpin;
      AppendBE32(out, fourcc);
      return SwooshAngle(value.args[0], underKpin, underSust, kept, live, out, why) &&
             SwooshAngle(value.args[1], underKpin, underSust, kept, live, out, why);
    }
    if (fourcc == F("SINE") && value.args.size() == 3 && underSust) {
      live = live || !underKpin;
      AppendBE32(out, fourcc);
      return SwooshAngle(value.args[0], underKpin, underSust, kept, live, out, why) &&
             SwooshAngle(value.args[1], underKpin, underSust, kept, live, out, why) &&
             SwooshAngle(value.args[2], underKpin, underSust, kept, live, out, why);
    }
    if ((fourcc == F("ADD_") || fourcc == F("SUB_") || fourcc == F("MULT")) && value.args.size() == 2) {
      AppendBE32(out, fourcc);
      return SwooshAngle(value.args[0], underKpin, underSust, kept, live, out, why) &&
             SwooshAngle(value.args[1], underKpin, underSust, kept, live, out, why);
    }
    why = "z " + EffectFourCCString(value.fourcc) + (fourcc == F("SINE") ? " on the point's age" : "");
    return false;
  }

  // A one-byte or one-word CNST property (PBDM, ORNT, SCTR).
  static bool SmallConst(const EffectProperty& property, uint32_t& word) {
    if (property.value.size() != 1 || !IsElement(property.value[0], F("CNST")) || property.value[0].args.size() != 1) {
      return false;
    }
    const EffectValue& arg = property.value[0].args[0];
    if (arg.kind != EffectValue::Kind::Word && arg.kind != EffectValue::Kind::Byte) {
      return false;
    }
    word = arg.word;
    return true;
  }

  // LTYP and LFOT are usually stored as a byte property in Remastered, though
  // the typed integer form is also accepted by the grammar.
  static bool SmallInteger(const EffectProperty& property, uint32_t& word) {
    if (property.value.size() == 1 && (property.value[0].kind == EffectValue::Kind::Byte ||
                                       property.value[0].kind == EffectValue::Kind::Word)) {
      word = property.value[0].word;
      return true;
    }
    return SmallConst(property, word);
  }

  // SMVR as `SMOV(EXTT, CNST(0, 0, 0), EXTR, NONE, NONE)`: the emitter's own
  // translation and rotation with no offset (1114 of 1233 SMVR forms).
  static bool IdentityMover(const EffectProperty& mover) {
    if (mover.value.size() != 1 || !IsElement(mover.value[0], F("SMOV")) || mover.value[0].args.size() != 5) {
      return false;
    }
    const std::vector<EffectValue>& args = mover.value[0].args;
    const EffectValue& offset = args[1];
    if (!IsElement(args[0], F("EXTT")) || !IsElement(args[2], F("EXTR")) || !IsElement(args[3], F("NONE")) ||
        !IsElement(args[4], F("NONE")) || !IsElement(offset, F("CNST")) || offset.args.size() != 3) {
      return false;
    }
    uint32_t bits;
    return std::all_of(offset.args.begin(), offset.args.end(), [&bits](const EffectValue& each) {
      return ConstWord(each, bits) && (bits & 0x7fffffffu) == 0;
    });
  }

  // XFMD, Remastered's transform mode (build/mpr/vfx/RESOLVED-xfmd.md): 3 keeps
  // the particles in emitter space, 4 does too at unit scale with SMVR as the
  // mover. Written only for those two; 1, 2 and 5 draw as retail does. With 4,
  // PSMV carries the mover (none: the particles sit at the origin, unrotated),
  // except for the identity SMVR, which is the emitter's transform as it stands.
  void Transform(const EffectProperty& xfmd, const EffectProperty* mover, const EffectNode& node,
                 std::vector<uint8_t>& out) const {
    uint32_t mode = 2;
    if (!SmallConst(xfmd, mode)) {
      m_approximated.push_back("XFMD: not a constant, drawn as retail");
      return;
    }
    if (mode == 3 || mode == 4) {
      AppendBE32(out, F("XFMD"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, mode);
    } else if (mode != 1 && mode != 2 && mode != 5) {
      m_approximated.push_back("XFMD " + std::to_string(mode) + " drawn as retail");
    }
    if (mode != 4 || (mover != nullptr && IdentityMover(*mover))) {
      return;
    }
    if (mover == nullptr) {
      PutList(out, F("PSMV"), 0, {});
      return;
    }
    std::vector<uint8_t> items;
    std::string why;
    if (mover->value.size() != 1 || !Mover(mover->value[0], node, items, why)) {
      m_approximated.push_back("SMVR taken as the emitter's transform: " + (why.empty() ? "not SMOV" : why));
      return;
    }
    PutList(out, F("PSMV"), 1, items);
  }

  // The count-prefixed list `CNST n, items` of a port-only property.
  static void PutList(std::vector<uint8_t>& out, uint32_t fourcc, uint32_t count, const std::vector<uint8_t>& items) {
    AppendBE32(out, fourcc);
    AppendBE32(out, F("CNST"));
    AppendBE32(out, count);
    out.insert(out.end(), items.begin(), items.end());
  }

  // The port-only properties of a generator whose material instance has a
  // recipe: VMAT and the per-particle data it reads (build/mpr/vfx/DESIGN.md).
  // Writes nothing where the MATI is unavailable or its shader has no recipe.
  // A swoosh's (`swoosh`) has no SSZE or VORN, and its blend mode is SBDM.
  // An ELC2 (`electric`, with `swoosh` set) takes its blend from EBDM.
  bool Material(const EffectProperty& material, const EffectNode& node, ConvertedPart& result, bool swoosh,
                bool electric = false) const {
    if (!m_io.materialData || !m_io.vfxTexture) {
      return false;
    }
    const EffectValue* guid = nullptr;
    for (const EffectValue& value : material.value) {
      if (value.kind == EffectValue::Kind::Guid) {
        guid = &value;
      }
    }
    if (guid == nullptr) {
      return false;
    }
    const std::vector<uint8_t> data = m_io.materialData(guid->guid);
    Mati mati;
    if (!ParseMati(data, mati)) {
      result.dropped.push_back("VMAT: no MATI for its material");
      return false;
    }
    char shaderText[16];
    std::snprintf(shaderText, sizeof(shaderText), "%08x", mati.shader);
    const Recipe* recipe = RecipeOf(mati.shader);
    if (recipe == nullptr) {
      m_approximated.push_back(std::string("VMAT: shader ") + shaderText + " has no recipe");
      return false;
    }

    // The node's properties by fourcc.
    // Also takes the found property's lifetime context for the ILPTs written from it.
    auto find = [this, &node, swoosh](uint32_t fourcc) -> const EffectProperty* {
      for (const EffectProperty& property : node.properties) {
        if (property.fourcc == fourcc) {
          m_lifetimeOffset = LifetimeOffset(!swoosh, false, property);
          return &property;
        }
      }
      return nullptr;
    };
    auto findUnique = [&node](uint32_t fourcc) -> const EffectProperty* {
      const EffectProperty* found = nullptr;
      for (const EffectProperty& property : node.properties) {
        if (property.fourcc == fourcc) {
          if (found != nullptr) {
            return nullptr;
          }
          found = &property;
        }
      }
      return found;
    };
    const auto cch0 = mati.vectors.count("CCH0") ? mati.vectors.at("CCH0") : std::array<float, 4>{};
    if (recipe->unitCch0 && (cch0[2] != 1.0f || cch0[3] != 0.0f)) {
      m_approximated.push_back(std::string("VMAT: shader ") + shaderText + " has a fade gain/bias the renderer lacks");
      return false;
    }

    // Textures, in the recipe's slot order.
    struct Slot {
      FlipbookAtlas atlas;
      MatiTexture texture;
      const TexSpec* spec;
    };
    std::vector<Slot> slots;
    for (const TexSpec& spec : recipe->textures) {
      const auto found = mati.textures.find(spec.tag);
      if (found == mati.textures.end()) {
        m_approximated.push_back(std::string("VMAT: shader ") + shaderText + " has no " + spec.tag + " texture");
        return false;
      }
      const FlipbookAtlas atlas = m_io.vfxTexture(found->second.guid);
      if (atlas.id == 0) {
        m_approximated.push_back(std::string("VMAT: ") + spec.tag + " texture did not import");
        return false;
      }
      slots.push_back({atlas, found->second, &spec});
    }

    uint32_t blend = 0;
    if (const EffectProperty* ebdm = electric ? find(F("EBDM")) : nullptr) {
      // InitStaticRenderStateImpl (0xb72618) indexes a 6-entry table: 1 reads row 2 and 2 row 1 of the
      // PBDM-style rows (0 alpha, 1 premultiplied, 2 additive); 3-5 read row 0. Row meaning UNVERIFIED.
      uint32_t word = 0;
      if (SmallConst(*ebdm, word) && word <= 5) {
        blend = word == 1 ? 2 : word == 2 ? 1 : 0;
      } else {
        m_approximated.push_back("VMAT: EBDM taken as alpha blend");
      }
    } else if (electric) {
      blend = 0;
    } else if (const EffectProperty* sbdm = swoosh ? find(F("SBDM")) : nullptr) {
      uint32_t word = 0;
      if (SmallConst(*sbdm, word) && word <= 1) {
        blend = word == 1 ? 2 : 0;
      } else {
        m_approximated.push_back("VMAT: SBDM taken as alpha blend");
      }
    } else if (const EffectProperty* pbdm = swoosh ? nullptr : find(F("PBDM"))) {
      uint32_t word = 0;
      if (SmallConst(*pbdm, word) && word <= 3) {
        blend = word;
      } else {
        m_approximated.push_back("VMAT: PBDM taken as alpha blend");
      }
    }
    // The wave beam's orb is the one known particle model whose MATI header blend
    // (EBlendMode 1, Remastered's One/One) wins over PBDM, which is 0 there: the
    // model draw passes -1 and resolves it to the MATI resource's mode
    // (CParticleSceneNode::RenderModelEntries, GetMaterialInstanceBlendMode).
    // Pinned to that exact MATI; no other MATI or shader is generalised from it.
    if (!swoosh && find(F("PMDL")) != nullptr && guid->guid == kWaveOrbMati && mati.shader == 0x642dd72b &&
        mati.headerBlend == 1) {
      blend = kBlendAdditiveOne;
    }
    // Its exposure: the source scales the orb by the inverse of the auto exposure and the post
    // multiplies it back, so the light it adds to the (already exposed) HDR scene is T * COLR * ITEN,
    // net exposure 1. Validated for this MATI only; every other effect keeps the room's exposure.
    // Its own test, not read off the blend: a later blend change must not move the policy.
    uint32_t features = recipe->features;
    if (!swoosh && find(F("PMDL")) != nullptr && guid->guid == kWaveOrbMati && mati.shader == 0x642dd72b &&
        mati.headerBlend == 1) {
      features |= kAlreadyExposed;
    }
    // PowerBall's projectile/source material writes RGB multiplied by inverse
    // auto-exposure on the CPU; the source post restores that exposure. Require the exact
    // material, shader, header, model and source render-state fields: PBDM or a
    // similar ramp material alone does not prove exposure invariance.
    const EffectProperty* powerPmdl = findUnique(F("PMDL"));
    const EffectProperty* powerDvvn = findUnique(F("DVVN"));
    const EffectProperty* powerPbdm = findUnique(F("PBDM"));
    const EffectProperty* powerMtin = findUnique(F("MTIN"));
    const bool exactMatiProperty = powerMtin == &material && material.tag == 0 && material.value.size() == 2 &&
                                   material.value[0].kind == EffectValue::Kind::Byte && material.value[0].word == 0 &&
                                   material.value[1].kind == EffectValue::Kind::Guid &&
                                   material.value[1].guid == kPowerBallMati;
    const bool hasIexp = std::any_of(node.properties.begin(), node.properties.end(),
                                     [](const EffectProperty& property) { return property.fourcc == F("IEXP"); });
    const auto constantIs = [](const EffectProperty* property, uint8_t expectedTag, uint32_t expected,
                               bool requireCnst, bool requireByte) {
      if (property == nullptr || property->tag != expectedTag || property->value.size() != 1) {
        return false;
      }
      const EffectValue* value = &property->value[0];
      if (requireByte) {
        return value->kind == EffectValue::Kind::Byte && value->word == expected;
      }
      if (IsElement(*value, F("CNST")) && value->args.size() == 1) {
        value = &value->args[0];
      } else if (requireCnst) {
        return false;
      }
      return (value->kind == EffectValue::Kind::Byte || value->kind == EffectValue::Kind::Word) &&
             value->word == expected;
    };
    const auto guidIs = [](const EffectProperty* property, const EffectGuid& expected) {
      if (property == nullptr || property->tag != 3 || property->value.size() != 1) {
        return false;
      }
      const EffectValue* value = &property->value[0];
      if (!IsElement(*value, F("CNST")) || value->args.size() != 1) {
        return false;
      }
      value = &value->args[0];
      return value->kind == EffectValue::Kind::Guid && value->guid == expected;
    };
    if (!swoosh && node.id == kPowerBallChild && exactMatiProperty && mati.shader == kPowerBallShader &&
        mati.headerBlend == kNoHeaderBlend && guidIs(powerPmdl, kPowerBallModel) &&
        constantIs(powerDvvn, 1, 4, false, true) && constantIs(powerPbdm, 1, 0, true, false) && !hasIexp) {
      features |= kAlreadyExposed;
    }
    // Separately audited held-charge sphere: DVVN4 defaults inverse exposure on and PBDM2
    // resolves additive usage. Its raw MATI also has header blend -1 (LE32 at 0x61).
    // This fixes only HDR exposure cancellation; LDR appearance and full parity remain unproven.
    // Do not change blend, recipe or the still-unrepresented SMTR alpha gain/bias here.
    const bool exactChargeMatiProperty = powerMtin == &material && material.tag == 0 && material.value.size() == 2 &&
                                         material.value[0].kind == EffectValue::Kind::Byte && material.value[0].word == 0 &&
                                         material.value[1].kind == EffectValue::Kind::Guid &&
                                         material.value[1].guid == kPowerChargeMati;
    if (!swoosh && node.id == kPowerChargeChild && exactChargeMatiProperty && mati.shader == kPowerBallShader &&
        mati.headerBlend == kNoHeaderBlend && guidIs(powerPmdl, kPowerBallModel) &&
        constantIs(powerDvvn, 1, 4, false, true) && constantIs(powerPbdm, 1, 2, true, false) && !hasIexp) {
      features |= kAlreadyExposed;
    }
    if (recipe->frameBuffer) {
      if (cch0[0] != 0.0f || cch0[1] != 0.0f) {
        m_approximated.push_back(std::string("VMAT: shader ") + shaderText + "'s scene warp dropped (drawn as a multiply)");
      }
      blend = kBlendMultiply;
    }
    uint32_t spriteCenter = 0;
    if (const EffectProperty* sctr = find(F("SCTR"))) {
      uint32_t word = 0;
      if (SmallConst(*sctr, word) && word <= 8) {
        spriteCenter = word;
      } else {
        m_approximated.push_back("VMAT: SCTR taken as 0");
      }
    }

    // The blob.
    std::vector<uint8_t> blob;
    const bool opacityGainBias = mati.shader == kPowerBallShader;
    AppendBE32(blob, opacityGainBias ? 3 : 2);
    AppendBE32(blob, features);
    AppendBE32(blob, blend);
    AppendBE32(blob, uint32_t(slots.size()));
    int32_t slotOf[7] = {-1, -1, -1, -1, -1, -1, -1};
    for (size_t i = 0; i < slots.size(); ++i) {
      const Slot& slot = slots[i];
      slotOf[size_t(slot.spec->role)] = int32_t(i);
      AppendBE32(blob, slot.atlas.id);
      AppendBE32(blob, slot.texture.texCoord);
      AppendBE32(blob, WrapOf(slot.texture.wrapX));
      AppendBE32(blob, WrapOf(slot.texture.wrapY));
      AppendBE32(blob, slot.texture.filter != 0 ? 1 : 0);
      AppendBE32(blob, uint32_t(std::max(slot.atlas.cols, 1)));
      AppendBE32(blob, uint32_t(std::max(slot.atlas.rows, 1)));
      AppendBE32(blob, uint32_t(std::max(slot.atlas.frames, 1)));
      const bool warped = slot.spec->warpU >= 0;
      AppendBE32(blob, warped ? 1 : 0);
      AppendBE32(blob, FloatBits(warped ? cch0[size_t(slot.spec->warpU)] : 0.0f));
      AppendBE32(blob, FloatBits(warped ? cch0[size_t(slot.spec->warpV)] : 0.0f));
    }
    for (const Role role : {Role::Color, Role::Opacity, Role::Ramp, Role::Ramp2, Role::Threshold, Role::Indirect,
                            Role::Palette}) {
      AppendBE32(blob, uint32_t(slotOf[size_t(role)]));
    }
    AppendBE32(blob, 0);             // rampRow[2]
    AppendBE32(blob, 1);
    AppendBE32(blob, uint32_t(recipe->addRow));
    int32_t srcRow[kSrcCount], srcComp[kSrcCount];
    float srcValue[kSrcCount];
    for (int i = 0; i < kSrcCount; ++i) {
      srcRow[i] = -1;
      srcComp[i] = 0;
      srcValue[i] = i == kSrcIndexScale ? 1.0f : 0.0f;
    }
    for (const SrcSpec& src : recipe->sources) {
      srcRow[src.index] = src.row;
      srcComp[src.index] = src.comp < 0 ? 0 : src.comp;
      if (src.row < 0) {
        srcValue[src.index] = cch0[size_t(src.comp)];
      }
    }
    for (int i = 0; i < kSrcCount; ++i) {
      AppendBE32(blob, uint32_t(srcRow[i]));
      AppendBE32(blob, uint32_t(srcComp[i]));
      AppendBE32(blob, FloatBits(srcValue[i]));
    }
    AppendBE32(blob, FloatBits(1.0f));  // modulate
    AppendBE32(blob, FloatBits(0.0f));  // depthSoften
    AppendBE32(blob, spriteCenter);
    if (opacityGainBias) {
      AppendBE32(blob, FloatBits(cch0[2]));
      AppendBE32(blob, FloatBits(cch0[3]));
    }

    AppendBE32(result.part, F("VMAT"));
    AppendBE32(result.part, F("CNST"));
    AppendBE32(result.part, uint32_t(blob.size()));
    result.part.insert(result.part.end(), blob.begin(), blob.end());

    // VTMT: one TRSS per UV set (A-F), up to three.
    if (const EffectProperty* tmtr = find(F("TMTR"))) {
      std::vector<uint8_t> items;
      uint32_t count = 0;
      std::string why;
      bool ok = true;
      for (const EffectValue& transform : tmtr->value) {
        if (!IsElement(transform, F("TRSS")) || transform.args.size() != 6) {
          ok = false;
          why = "not a six-argument TRSS";
          break;
        }
        if (count == 3) {
          ok = false;
          why = "more than three UV sets";
          break;
        }
        for (const EffectValue& arg : transform.args) {
          if (!Element(arg, Type::Real, items, why)) {
            ok = false;
            break;
          }
        }
        if (!ok) {
          break;
        }
        ++count;
      }
      if (ok && count != 0) {
        PutList(result.part, F("VTMT"), count, items);
      } else if (!ok) {
        result.dropped.push_back("TMTR: " + why);
      }
    }

    // VPMT: each PMTR item to a row and component of the per-particle extras.
    if (const EffectProperty* pmtr = find(F("PMTR"))) {
      std::vector<uint8_t> items;
      uint32_t count = 0;
      for (const EffectValue& item : pmtr->value) {
        const uint32_t row = item.word & 0xff, comp = item.word >> 8 & 0xff, width = item.word >> 16 & 0xff;
        // Group 0 an int, 1 a real, 2 a vector, 4 a colour.
        uint32_t kind = 0;
        Type type = Type::Real;
        uint32_t expect = 1;
        switch (item.fourcc) {
        case 0: kind = 2; type = Type::Int; break;
        case 1: break;
        case 2: kind = 1; type = Type::Vector; expect = 3; break;
        case 4: kind = 3; type = Type::Color; expect = 4; break;
        default: expect = 0; break;
        }
        if (item.args.size() != 1 || expect == 0 || width != expect || row >= 4 || comp + expect > 4) {
          result.dropped.push_back("PMTR: an entry of group " + std::to_string(item.fourcc) + " that VPMT cannot hold");
          continue;
        }
        std::vector<uint8_t> element;
        std::string why;
        if (!Element(item.args[0], type, element, why)) {
          result.dropped.push_back("PMTR: " + why);
          continue;
        }
        AppendBE32(items, kind);
        AppendBE32(items, row);
        AppendBE32(items, comp);
        items.insert(items.end(), element.begin(), element.end());
        ++count;
      }
      if (count != 0) {
        PutList(result.part, F("VPMT"), count, items);
      }
    }

    // VSMT: the CCH0 overrides, each to the uniform and warp scales that read the component.
    if (const EffectProperty* smtr = find(F("SMTR"))) {
      std::vector<uint8_t> items;
      uint32_t count = 0;
      constexpr uint32_t kCch0 = F("CCH0");
      for (const EffectValue& item : smtr->value) {
        if (item.word != kCch0 || item.args.size() != 2) {
          result.dropped.push_back("SMTR: " + std::string(item.word == kCch0 ? "an entry" : "a parameter") +
                                   " VSMT cannot take");
          continue;
        }
        const int comp = int(item.args[1].word >> 8 & 0xff);
        std::vector<uint32_t> targets;
        if (opacityGainBias && comp == 2) {
          targets.push_back(kPortVfxOpacityGainTarget);
        } else if (opacityGainBias && comp == 3) {
          targets.push_back(kPortVfxOpacityBiasTarget);
        }
        for (const SrcSpec& src : recipe->sources) {
          if (src.row < 0 && src.comp == comp) {
            targets.push_back(uint32_t(src.index));
          }
        }
        for (size_t i = 0; i < slots.size(); ++i) {
          if (slots[i].spec->warpU == comp) {
            targets.push_back(uint32_t(11 + 2 * i));
          }
          if (slots[i].spec->warpV == comp) {
            targets.push_back(uint32_t(11 + 2 * i + 1));
          }
        }
        if (targets.empty()) {
          result.dropped.push_back("SMTR: CCH0 component " + std::to_string(comp) + " is unused by the shader");
          continue;
        }
        std::vector<uint8_t> element;
        std::string why;
        if (!Element(item.args[0], Type::Real, element, why)) {
          result.dropped.push_back("SMTR: " + why);
          continue;
        }
        for (const uint32_t target : targets) {
          AppendBE32(items, target);
          AppendBE32(items, 0);
          items.insert(items.end(), element.begin(), element.end());
          ++count;
        }
      }
      if (count != 0) {
        PutList(result.part, F("VSMT"), count, items);
      }
    }

    // SSZE and ITEN are real elements as they are; VORN is ORNT's byte.
    for (const uint32_t fourcc : {F("SSZE"), F("ITEN")}) {
      const std::string name = EffectFourCCString(fourcc);
      const EffectProperty* property = swoosh && fourcc == F("SSZE") ? nullptr : find(fourcc);
      if (property == nullptr) {
        continue;
      }
      std::vector<uint8_t> bytes;
      std::string why;
      if (property->value.size() == 1 && Element(property->value[0], Type::Real, bytes, why)) {
        AppendBE32(result.part, fourcc);
        result.part.insert(result.part.end(), bytes.begin(), bytes.end());
      } else {
        result.dropped.push_back(name + ": " + (why.empty() ? "not one element" : why));
      }
    }
    if (swoosh) {
      return true;
    }
    uint32_t orient = 0;
    if (const EffectProperty* ornt = find(F("ORNT"))) {
      if (!SmallConst(*ornt, orient) || orient > 2) {
        m_approximated.push_back("VORN: ORNT taken as 0");
        orient = 0;
      }
    }
    AppendBE32(result.part, F("VORN"));
    AppendBE32(result.part, F("CNST"));
    AppendBE32(result.part, orient);
    return true;
  }

  // One generator, swoosh or electric description, as the retail asset `type`.
  // PVAR (value.offset = the u16 id count, then 5 u16 per-kind counts, the u16 data size, the
  // 16-byte ids, then the defaults: real f32, int i32, colour 4 half, vector 3 f32, rotation 4 f32).
  void ReadVariables(const EffectValue& raw) const {
    if (raw.size < 14) {
      return;
    }
    const uint8_t* p = m_data + raw.offset;
    const size_t total = ReadLE16(p);
    size_t counts[5];
    for (int i = 0; i < 5; ++i) {
      counts[i] = ReadLE16(p + 2 + 2 * i);
    }
    const size_t dataSize = ReadLE16(p + 12);
    if (14 + 16 * total + dataSize > raw.size ||
        counts[0] + counts[1] + counts[2] + counts[3] + counts[4] != total) {
      return;
    }
    const uint8_t* id = p + 14;
    const uint8_t* data = id + 16 * total;
    const uint8_t* end = data + dataSize;
    // kind index in table order -> PVRT type (real 0, int 1, colour 2, vector 3, rotation 4)
    static constexpr size_t kSizes[5] = {4, 4, 8, 12, 16};
    for (int kind = 0; kind < 5; ++kind) {
      for (size_t i = 0; i < counts[kind]; ++i) {
        if (data + kSizes[kind] > end) {
          m_vars.clear();
          return;
        }
        PvarEntry var{};
        std::memcpy(var.guid.data(), id, 16);
        id += 16;
        var.type = uint32_t(kind);
        switch (kind) {
        case 0:
          var.def[0] = FloatFromBits(ReadLE32(data));
          break;
        case 1:
          var.def[0] = float(int32_t(ReadLE32(data)));
          break;
        case 2:
          for (int c = 0; c < 4; ++c) {
            var.def[c] = HalfToFloat(ReadLE16(data + 2 * c));
          }
          break;
        default:
          for (int c = 0; c < int(kSizes[kind] / 4); ++c) {
            var.def[c] = FloatFromBits(ReadLE32(data + 4 * c));
          }
          break;
        }
        data += kSizes[kind];
        m_vars.push_back(var);
      }
    }
  }

  ConvertedPart Generator(const EffectNode& node, uint32_t type) const {
    ConvertedPart result;
    result.id = node.id;
    result.type = type;
    result.root = node.root;
    std::vector<uint8_t>& out = result.part;
    AppendBE32(out, HeaderOf(type));
    const auto& retail = PropertiesOf(type);
    const bool part = type == F("PART");
    const bool swoosh = type == F("SWHC");
    // An ELC2 (CParticleElectric2) is written as an ELSC stream that starts with the port-only PEL2
    // marker and carries the ELC2 fields; it draws through its VMAT like a swoosh.
    const bool electric = type == F("ELSC") && node.form == F("ELC2");
    if (electric) {
      AppendBE32(out, F("PEL2"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, 1);
    }
    // This is deliberately not a general Remastered particle-light mapping:
    // source proof covers only this embedded Plasma impact child.
    std::vector<uint8_t> nestedLightInnerRadius;
    std::vector<uint8_t> nestedLightOuterRadius;
    bool nestedHdrLight = false;
    if (part && EffectGuidString(node.id) == "f911701f-ec15-458b-ae13-ca2bf65d42f6") {
      auto unique = [&node](uint32_t fourcc) -> const EffectProperty* {
        const EffectProperty* found = nullptr;
        for (const EffectProperty& property : node.properties) {
          if (property.fourcc == fourcc) {
            if (found != nullptr) {
              return nullptr;
            }
            found = &property;
          }
        }
        return found;
      };
      const EffectProperty* ltyp = unique(F("LTYP"));
      const EffectProperty* lfot = unique(F("LFOT"));
      const EffectProperty* lclr = unique(F("LCLR"));
      const EffectProperty* lint = unique(F("LINT"));
      const EffectProperty* lird = unique(F("LIRD"));
      const EffectProperty* lord = unique(F("LORD"));
      uint32_t lightType = 0;
      uint32_t falloff = 0;
      std::string why;
      std::vector<uint8_t> colorProbe;
      std::vector<uint8_t> intensityProbe;
      nestedHdrLight = ltyp != nullptr && lfot != nullptr && lird != nullptr && lord != nullptr &&
                       lclr != nullptr && lint != nullptr && lclr->value.size() == 1 &&
                       lint->value.size() == 1 &&
                       SmallInteger(*ltyp, lightType) && lightType == 2 && SmallInteger(*lfot, falloff) &&
                       falloff == 3 && lird->value.size() == 1 && lord->value.size() == 1 &&
                       Element(lclr->value[0], Type::Color, colorProbe, why) &&
                       Element(lint->value[0], Type::Real, intensityProbe, why) &&
                       Element(lird->value[0], Type::Real, nestedLightInnerRadius, why) &&
                       Element(lord->value[0], Type::Real, nestedLightOuterRadius, why);
      if (!nestedHdrLight) {
        result.dropped.push_back("PHDR: exact Plasma child lacks constant point/smooth light or supported LIRD/LORD");
      }
    }
    // Swooshes: SBDM is Remastered's blend mode, 0 or 1, which retail has as
    // AALP (additive). Taken only where there's no AALP of its own.
    bool additive = false;
    for (const EffectProperty& property : node.properties) {
      additive = additive || property.fourcc == F("AALP");
    }
    m_swoosh = node.form == F("SWSH");
    m_vars.clear();
    for (const EffectProperty& property : node.properties) {
      if (property.fourcc == F("PVAR") && property.value.size() == 1 &&
          property.value[0].kind == EffectValue::Kind::Raw) {
        ReadVariables(property.value[0]);
      }
    }
    // A generator's SVRn/SVIn/SVCn/SVVn: CAddVarSystem's own variables, which
    // PSAn and children's SPA* read. Written as the port-only PSYS, in the order
    // CElementGen evaluates them: an item after the reals it reads (PSAn of
    // another SVRn), else by property order, each after its hidden init-only
    // reals. [I] the order of RAND draws among items is not proven.
    std::vector<SystemItem> sysItems;
    if (part) {
      struct Pending {
        const EffectProperty* property;
        uint32_t type;
        uint32_t n;
        uint32_t reads;
      };
      std::vector<Pending> pending;
      uint32_t writtenReals = 0;
      for (const EffectProperty& property : node.properties) {
        const auto [varType, n] = SystemVarWrite(property.fourcc);
        if (varType == ~0u) {
          continue;
        }
        uint32_t reads = 0;
        for (const EffectValue& value : property.value) {
          reads |= SystemReadsOf(value);
        }
        pending.push_back({&property, varType, n, reads});
        if (varType == 0) {
          writtenReals |= 1u << n;
        }
      }
      m_sysNextHidden = 10;
      m_sysNextHiddenInt = 10;
      uint32_t doneReals = 0;
      std::vector<bool> done(pending.size(), false);
      for (size_t emitted = 0; emitted < pending.size(); ++emitted) {
        size_t pick = pending.size();
        for (size_t i = 0; i < pending.size() && pick == pending.size(); ++i) {
          const uint32_t self = pending[i].type == 0 ? 1u << pending[i].n : 0;
          if (!done[i] && (pending[i].reads & writtenReals & ~doneReals & ~self) == 0) {
            pick = i;
          }
        }
        for (size_t i = 0; i < pending.size() && pick == pending.size(); ++i) {
          if (!done[i]) {
            pick = i;  // a cycle: property order
          }
        }
        done[pick] = true;
        const Pending& item = pending[pick];
        if (item.type == 0) {
          doneReals |= 1u << item.n;
        }
        static constexpr Type kTypes[4] = {Type::Real, Type::Int, Type::Color, Type::Vector};
        const std::string name = EffectFourCCString(item.property->fourcc);
        std::vector<SystemItem> items;
        SystemItem visible{item.type, item.n, 0, {}};
        std::string why;
        const uint32_t hidden = m_sysNextHidden;
        m_sysItems = &items;
        const bool ok = item.property->value.size() == 1 &&
                        Element(item.property->value[0], kTypes[item.type], visible.element, why);
        m_sysItems = nullptr;
        if (!ok) {
          m_sysNextHidden = hidden;
          result.dropped.push_back(name + ": " + (why.empty() ? "not one element" : why));
          continue;
        }
        items.push_back(std::move(visible));
        for (SystemItem& sys : items) {
          sysItems.push_back(std::move(sys));
        }
      }
    }
    bool texture = false;
    uint32_t pmdl = 0;          // the PMDL's id, when it is one constant
    bool pmdlVariants = false;  // a PMDV was written (a mesh would cover only one model)
    std::vector<Started> started;  // by the spawn table
    std::set<uint32_t> written;    // retail properties
    const EffectProperty* material = nullptr;
    const EffectProperty* xfmd = nullptr;
    uint32_t spriteShape = 0;
    const EffectProperty* mover = nullptr;
    std::vector<uint32_t> portOnly;  // material data, written only with a VMAT
    const bool textured = retail.count(F("TEXR")) != 0;
    // Remastered draws nothing for a generator with no texture, material or
    // model (it only carries spawns and lights); retail would draw its
    // particles as untextured quads (0.1 wide and white without SIZE/COLR).
    // Such a generator is written with a size of 0, and so is one that draws only
    // models (retail drew a white quad round each of the Ice charge's shards).
    const auto has = [&](uint32_t fourcc) {
      return std::any_of(node.properties.begin(), node.properties.end(),
                         [&](const EffectProperty& property) { return property.fourcc == fourcc; });
    };
    // A material whose colour texture (BCLR) is nil binds Remastered's black
    // texture. These are placeholder MATIs on generators that carry spawns and
    // lights (PlasmaCharge's root among them), so their quads are taken as
    // drawing nothing too; model generators keep the black material.
    const auto nilColour = [&](const EffectProperty& property) {
      if (!m_io.materialData) {
        return false;
      }
      for (const EffectValue& value : property.value) {
        if (value.kind != EffectValue::Kind::Guid) {
          continue;
        }
        static constexpr uint8_t kTag[4] = {'B', 'C', 'L', 'R'};
        const std::vector<uint8_t> data = m_io.materialData(value.guid);
        const auto tag = std::search(data.begin(), data.end(), std::begin(kTag), std::end(kTag));
        return tag != data.end() && data.end() - tag >= 20 && std::all_of(tag + 4, tag + 20, [](uint8_t b) { return b == 0; });
      }
      return false;
    };
    const bool placeholder = part && !has(F("TEXR")) && !has(F("PMDL")) &&
                             std::any_of(node.properties.begin(), node.properties.end(), [&](const EffectProperty& property) {
                               return property.fourcc == F("MTIN") && nilColour(property);
                             });
    result.placeholder = placeholder;
    const bool drawsNothing = part && !has(F("TEXR")) && (!has(F("MTIN")) || placeholder) && !has(F("PMDL"));
    result.drawsNothing = drawsNothing;
    const bool modelsOnly = part && !has(F("TEXR")) && !has(F("MTIN")) && has(F("PMDL"));
    for (const EffectProperty& property : node.properties) {
      uint32_t fourcc = property.fourcc;
      m_lifetimeOffset = LifetimeOffset(part, swoosh, property);
      if (placeholder && fourcc == F("MTIN")) {
        result.dropped.push_back("MTIN: its colour texture is nil (the generator draws nothing)");
        continue;
      }
      if ((drawsNothing || modelsOnly) && fourcc == F("SIZE")) {
        result.dropped.push_back(drawsNothing ? "SIZE: the generator draws nothing" : "SIZE: the generator draws models only");
        continue;
      }
      // PBDM is Remastered's blend mode, from the table CParticleStaticRenderState
      // reads: 0 alpha (src alpha, 1 - src alpha), 1 premultiplied (1, 1 - src
      // alpha), 2 additive (src alpha, 1) and 3 opaque (1, 0). Retail has only
      // alpha and AAPH (additive), so 2 is AAPH and 1 and 3 go to the port-only PBDM,
      // whose factors the generator's alpha blend takes (CElementGen::PortBlendSrc/Dst).
      if (part && fourcc == F("PBDM")) {
        const EffectValue* mode = property.value.size() == 1 ? &property.value[0] : nullptr;
        if (mode != nullptr && IsElement(*mode, F("CNST")) && mode->args.size() == 1) {
          mode = &mode->args[0];
        }
        if (mode == nullptr || (mode->kind != EffectValue::Kind::Byte && mode->kind != EffectValue::Kind::Word)) {
          result.dropped.push_back("PBDM: not a constant");
        } else if (mode->word == 2) {
          AppendBE32(out, F("AAPH"));
          AppendBE32(out, F("CNST"));
          out.push_back(1);
        } else if (mode->word == 1 || mode->word == 3) {
          AppendBE32(out, F("PBDM"));
          AppendBE32(out, F("CNST"));
          AppendBE32(out, mode->word);
        } else if (mode->word != 0) {
          m_approximated.push_back("PBDM " + std::to_string(mode->word) + " taken as alpha blending");
        }
        continue;
      }
      if (part && fourcc == F("LTM2")) {
        fourcc = F("LTME");
      } else if (type == F("SWHC") && fourcc == F("SBDM") && !additive) {
        fourcc = F("AALP");
      }
      if (fourcc == F("MTIN") && (textured || electric)) {
        material = &property;
        continue;
      }
      // Retail reads KSSM, but Remastered's spawn table is laid out differently.
      if (part && fourcc == F("KSSM")) {
        std::vector<uint8_t> bytes;
        std::string why;
        // The hidden PEOD ints follow the system variables in PSYS, in table order
        // (CAddVarBuilder::AddDefinedVar 0x275188 appends as KSSM is read). [I] their draw
        // order against RAND-drawing system variables is not proven; no shipped effect has both.
        const size_t sysCount = sysItems.size();
        const uint32_t hiddenInt = m_sysNextHiddenInt;
        m_spawnSysItems = &sysItems;
        const bool exact = SpawnTables(property, node, bytes, why);
        m_spawnSysItems = nullptr;
        if (!exact) {
          sysItems.resize(sysCount);
          m_sysNextHiddenInt = hiddenInt;
        }
        if (exact) {
          if (!bytes.empty()) {
            AppendBE32(out, F("PKSM"));
            out.insert(out.end(), bytes.begin(), bytes.end());
          }
          continue;
        }
        m_approximated.push_back("KSSM: read as retail's table (" + why + ")");
        why.clear();
        if (!SpawnTable(property, bytes, started, why)) {
          result.dropped.push_back("KSSM: " + why);
          ++result.droppedRetail;
        } else if (!bytes.empty()) {
          AppendBE32(out, fourcc);
          out.insert(out.end(), bytes.begin(), bytes.end());
        }
        continue;
      }
      if (part && fourcc == F("PMRQ")) {
        std::vector<uint8_t> bytes;
        std::string why;
        if (!ModelRotation(property.value, bytes, why)) {
          result.dropped.push_back("PMRQ: " + why);
          continue;
        }
        written.insert(F("PMRT"));
        AppendBE32(out, F("PMRT"));
        out.insert(out.end(), bytes.begin(), bytes.end());
        // PMRB and PMCF (flag bits 0 and 5, CreateGPSM 0x256b7c) make the PMRQ the particle's base
        // rotation (Fixup 0x28ec98, SolveParticleTransform 0x246f14): PMOP turns with it, and the
        // system orientation is left out, except that PMCF still applies the global frame, whose
        // rotation is the system's under XFMD 3 and 4 and identity under 2. Port-only PMRF: 1 =
        // the PMRQ alone, 2 = the orientation times the PMRQ, PMOP turned by both.
        uint32_t pmrb = 0, pmcf = 0, mode = 2;
        for (const EffectProperty& other : node.properties) {
          if (other.fourcc == F("PMRB") || other.fourcc == F("PMCF") || other.fourcc == F("XFMD")) {
            uint32_t word = 0;
            if (!SmallConst(other, word)) {
              result.dropped.push_back(EffectFourCCString(other.fourcc) + ": not a constant (PMRQ frame)");
              continue;
            }
            (other.fourcc == F("PMRB") ? pmrb : other.fourcc == F("PMCF") ? pmcf : mode) = word;
          }
        }
        if (pmrb != 0 || pmcf != 0) {
          if (mode != 2 && mode != 3 && mode != 4) {
            result.dropped.push_back("PMRQ: PMRB or PMCF under XFMD " + std::to_string(int32_t(mode)));
          } else {
            AppendBE32(out, F("PMRF"));
            AppendBE32(out, F("CNST"));
            AppendBE32(out, pmcf != 0 && mode != 2 ? 2u : 1u);
          }
        }
        continue;
      }
      // Read with the PMRQ above; without a PMRQ, Fixup clears both.
      if (part && (fourcc == F("PMRB") || fourcc == F("PMCF"))) {
        continue;
      }
      // Port-only: XFMD and its mover are written after the loop, and the
      // material data is written by Material() alongside the VMAT.
      if (part && fourcc == F("XFMD")) {
        xfmd = &property;
        continue;
      }
      // A swoosh's XFMD 3 draws its points in the emitter's frame (CParticleSwoosh xPortXfmdFollow);
      // 2 and 5 keep local and global apart as retail does (build/mpr/vfx/RESOLVED-xfmd.md). 4 with
      // an identity EMVR and no SMVR births points at the emitter's transform and draws them at
      // identity and unit scale (xPortXfmdMover); the EMVR is consumed by it.
      if (swoosh && fourcc == F("XFMD")) {
        uint32_t mode = 2;
        const EffectProperty* emvr = nullptr;
        bool smvr = false;
        for (const EffectProperty& other : node.properties) {
          emvr = other.fourcc == F("EMVR") ? &other : emvr;
          smvr = smvr || other.fourcc == F("SMVR");
        }
        if (!SmallConst(property, mode)) {
          result.dropped.push_back("XFMD: not a constant");
        } else if (mode == 4 && (smvr || emvr == nullptr || !IdentityMover(*emvr))) {
          result.dropped.push_back(smvr              ? "XFMD 4 with an SMVR"
                                   : emvr == nullptr ? "XFMD 4 with no EMVR"
                                                     : "XFMD 4: EMVR not SMOV(EXTT, 0, EXTR)");
        } else if (mode == 3 || mode == 4) {
          AppendBE32(out, F("XFMD"));
          AppendBE32(out, F("CNST"));
          AppendBE32(out, mode);
        } else if (mode != 2 && mode != 5) {
          result.dropped.push_back("XFMD " + std::to_string(mode));
        }
        continue;
      }
      if (swoosh && fourcc == F("EMVR") && IdentityMover(property)) {
        uint32_t mode = 2;
        bool smvr = false;
        for (const EffectProperty& other : node.properties) {
          if (other.fourcc == F("XFMD") && !SmallConst(other, mode)) {
            mode = UINT32_MAX;
          }
          smvr = smvr || other.fourcc == F("SMVR");
        }
        if (mode == 4 && !smvr) {
          continue;
        }
      }
      if (part && fourcc == F("SMVR")) {
        mover = &property;
        continue;
      }
      if (part && (fourcc == F("TMTR") || fourcc == F("PMTR") || fourcc == F("SMTR") || fourcc == F("SSZE") ||
                   fourcc == F("ITEN") || fourcc == F("SCTR"))) {
        portOnly.push_back(fourcc);
        continue;
      }
      if ((swoosh || electric) &&
          (fourcc == F("TMTR") || fourcc == F("PMTR") || fourcc == F("SMTR") || fourcc == F("ITEN"))) {
        portOnly.push_back(fourcc);
        continue;
      }
      // Port-only ELC2 fields, written as the retail encoding under their own tags.
      if (electric && Elc2Properties().count(fourcc) != 0) {
        std::vector<uint8_t> bytes;
        std::string why;
        if (Property(fourcc, Elc2Properties().at(fourcc), property.value, bytes, why)) {
          AppendBE32(out, fourcc);
          out.insert(out.end(), bytes.begin(), bytes.end());
        } else {
          result.dropped.push_back(EffectFourCCString(fourcc) + ": " + why);
        }
        continue;
      }
      // Port-only PSRO: a swoosh's POFS of ROTV(v, EXTR) is v turned by the swoosh's orientation.
      if (swoosh && fourcc == F("POFS") && retail.count(fourcc) != 0 && property.value.size() == 1 &&
          IsElement(property.value[0], F("ROTV")) && property.value[0].args.size() == 2 &&
          IsElement(property.value[0].args[1], F("EXTR"))) {
        std::vector<uint8_t> bytes;
        std::string why;
        if (Property(fourcc, retail.at(fourcc), {property.value[0].args[0]}, bytes, why)) {
          written.insert(fourcc);
          AppendBE32(out, fourcc);
          out.insert(out.end(), bytes.begin(), bytes.end());
          AppendBE32(out, F("PSRO"));
          AppendBE32(out, F("CNST"));
          AppendBE32(out, 1);
          continue;
        }
        result.dropped.push_back("POFS: " + why);
        continue;
      }
      if (part && fourcc == F("SSHP") && property.value.size() == 1 && property.value[0].kind == EffectValue::Kind::Byte) {
        // Remastered's sprite mesh slot (desc+0x394 in RenderSpriteEntries): 0-2 are quad grids, 3-6 polygons.
        // Port-only PSHP carries the polygon shapes (kb format/SSHP.md).
        const uint32_t shape = property.value[0].word;
        if (shape >= 3 && shape <= 6) {
          spriteShape = shape;
        } else if (shape > 6) {
          result.dropped.push_back("SSHP: shape " + std::to_string(shape) + " is past Remastered's 7 meshes");
        }
        continue;
      }
      // Port-only PRAZ/PRAF: a swoosh's PROT of KPIN(RAZY(EMRV, ROTV(v, RLCL), fallback)), the ring
      // frozen at each point's birth facing the emitter's motion (CERotationFromAxis 0x29d298). Only
      // under XFMD 2, where EMRV is the system-space translation delta; ORNT with SIDE 2 replaces
      // PROT (SetupOrientToView 0x29bdbc), so it is left out there. A PROT turning about Z only
      // becomes port-only PROZ (SwooshZRotation).
      if (swoosh && fourcc == F("PROT")) {
        auto nodeWord = [&node](uint32_t tag, uint32_t fallback) {
          uint32_t word = fallback;
          for (const EffectProperty& other : node.properties) {
            if (other.fourcc == tag && !SmallConst(other, word)) {
              word = UINT32_MAX;
            }
          }
          return word;
        };
        const uint32_t xfmdMode = nodeWord(F("XFMD"), 2);
        const bool orntView = nodeWord(F("ORNT"), 0) == 1 && nodeWord(F("SIDE"), 0) == 2;
        if (orntView) {
          continue;
        }
        auto zeroReal = [](const EffectValue& value) {
          uint32_t word = 0;
          return ConstWord(value, word) && (word & 0x7fffffffu) == 0;
        };
        const EffectValue* razy = property.value.size() == 1 && IsElement(property.value[0], F("KPIN")) &&
                                          property.value[0].args.size() == 1 &&
                                          IsElement(property.value[0].args[0], F("RAZY")) &&
                                          property.value[0].args[0].args.size() == 3
                                      ? &property.value[0].args[0]
                                      : nullptr;
        const EffectValue* rotv = razy != nullptr && IsElement(razy->args[0], F("EMRV")) &&
                                          IsElement(razy->args[1], F("ROTV")) && razy->args[1].args.size() == 2 &&
                                          IsElement(razy->args[1].args[1], F("RLCL"))
                                      ? &razy->args[1]
                                      : nullptr;
        uint32_t fallback = UINT32_MAX;
        if (rotv != nullptr) {
          const EffectValue& fb = razy->args[2];
          if (IsElement(fb, F("RLCL"))) {
            fallback = 1;
          } else if (IsElement(fb, F("REUL")) && fb.args.size() == 4 && zeroReal(fb.args[0]) && zeroReal(fb.args[1]) &&
                     zeroReal(fb.args[2])) {
            fallback = 0;
          }
        }
        std::vector<uint8_t> bytes;
        std::string why;
        if (fallback == UINT32_MAX && property.value.size() == 1 && !IsElement(property.value[0], F("KPIN")) &&
            !IsElement(property.value[0], F("RADD")) && !IsElement(property.value[0], F("REUL"))) {
          result.dropped.push_back("PROT: not KPIN(RAZY(EMRV, ROTV(v, RLCL), RLCL or zero REUL))");
        } else if (fallback == UINT32_MAX) {
          uint32_t flags = 0;
          // CreateNewParticle 0x27cdc8 sets the point's orientation from var bc and then PROT's: a
          // plain REUL is the same under XFMD 3 (bc identity, the emitter's turn drawn globally) and 5
          // (bc set by SetOrientation, as retail's), and 4 (bc the EMVR's EXTR, set by both orientation
          // setters). EXTR/INTR there are not the orientation a point starts from (XFMD 3 draws it
          // again globally), so those stay dropped.
          if (xfmdMode != 2 && xfmdMode != 3 && xfmdMode != 4 && xfmdMode != 5) {
            result.dropped.push_back("PROT: turned about Z under XFMD " + std::to_string(int32_t(xfmdMode)));
          } else if (!SwooshZRotation(property.value[0], flags, bytes, why)) {
            result.dropped.push_back("PROT: " + why);
          } else if ((flags & 2) != 0 && xfmdMode != 2) {
            result.dropped.push_back("PROT: EXTR or INTR under XFMD " + std::to_string(int32_t(xfmdMode)));
          } else {
            AppendBE32(out, F("PROZ"));
            AppendBE32(out, F("CNST"));
            AppendBE32(out, flags);
            out.insert(out.end(), bytes.begin(), bytes.end());
          }
        } else if (xfmdMode != 2) {
          result.dropped.push_back("PROT: RAZY under XFMD " + std::to_string(int32_t(xfmdMode)));
        } else if (!PortVector(rotv->args[0], bytes, why)) {
          result.dropped.push_back("PROT: " + why);
        } else {
          AppendBE32(out, F("PRAZ"));
          out.insert(out.end(), bytes.begin(), bytes.end());
          AppendBE32(out, F("PRAF"));
          AppendBE32(out, F("CNST"));
          AppendBE32(out, fallback);
        }
        continue;
      }
      if (part && SystemVarWrite(property.fourcc).first != ~0u) {
        continue;  // written as PSYS
      }
      const auto found = retail.find(fourcc);
      if (found == retail.end()) {
        result.dropped.push_back(EffectFourCCString(property.fourcc) + ": Remastered only");
        continue;
      }
      std::vector<uint8_t> bytes;
      std::string why;
      // A model chosen per particle: the first as PMDL, all of them as PMDV.
      if (part && fourcc == F("PMDL") && property.value.size() == 1 && IsElement(property.value[0], F("SLCT"))) {
        std::vector<uint32_t> ids;
        if (!Variants(property.value, ids, why)) {
          result.dropped.push_back("PMDL: " + why);
          ++result.droppedRetail;
          continue;
        }
        AppendBE32(out, F("PMDL"));
        AppendBE32(out, F("CNST"));
        AppendBE32(out, ids[0]);
        if (std::all_of(ids.begin(), ids.end(), [&](uint32_t id) { return id == ids[0]; })) {
          pmdl = ids[0];  // one model, as a constant PMDL
          continue;
        }
        pmdlVariants = true;
        AppendBE32(out, F("PMDV"));
        AppendBE32(out, F("CNST"));
        AppendBE32(out, uint32_t(ids.size()));
        for (const uint32_t id : ids) {
          AppendBE32(out, F("CNST"));
          AppendBE32(out, id);
        }
        continue;
      }
      if (!Property(fourcc, found->second, property.value, bytes, why)) {
        result.dropped.push_back(EffectFourCCString(property.fourcc) + ": " + why);
        ++result.droppedRetail;
        continue;
      }
      if (fourcc == F("TEXR")) {
        texture = true;
      }
      if (fourcc == F("PMDL") && bytes.size() == 8) {
        pmdl = port::GetBE32(bytes, 4);
      }
      written.insert(fourcc);
      AppendBE32(out, fourcc);
      out.insert(out.end(), bytes.begin(), bytes.end());
    }
    if (drawsNothing || modelsOnly) {
      AppendBE32(out, F("SIZE"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, FloatBits(0.0f));
    }
    // A material instance draws with its texture where there is no TEXR.
    if (material != nullptr && !texture && !electric) {
      const EffectValue* guid = nullptr;
      for (const EffectValue& value : material->value) {
        if (value.kind == EffectValue::Kind::Guid) {
          guid = &value;
        }
      }
      const uint32_t id = guid != nullptr && m_io.materialTexture ? m_io.materialTexture(guid->guid) : 0;
      if (id != 0) {
        AppendBE32(out, F("TEXR"));
        AppendBE32(out, F("CNST"));
        AppendBE32(out, F("CNST"));
        AppendBE32(out, id);
      } else {
        result.dropped.push_back("MTIN: no texture for its material");
        ++result.droppedRetail;
      }
    } else if (material != nullptr && !electric) {
      result.dropped.push_back("MTIN: the effect has a TEXR");
    }
    const bool vmat = (part || swoosh || electric) && material != nullptr &&
                      Material(*material, node, result, swoosh || electric, electric);
    if (!vmat) {
      for (const uint32_t fourcc : portOnly) {
        result.dropped.push_back(EffectFourCCString(fourcc) + ": no VMAT");
      }
    }
    if (xfmd != nullptr) {
      Transform(*xfmd, mover, node, out);
    } else if (mover != nullptr) {
      result.dropped.push_back("SMVR: no XFMD");
    }
    if (vmat && pmdl != 0 && !pmdlVariants && m_io.modelMesh) {
      // VMSH: the converted PMDL as one mesh, so the model particle draws through the VMAT.
      const std::vector<uint8_t> mesh = m_io.modelMesh(pmdl);
      if (!mesh.empty()) {
        AppendBE32(out, F("VMSH"));
        AppendBE32(out, F("CNST"));
        AppendBE32(out, uint32_t(mesh.size()));
        out.insert(out.end(), mesh.begin(), mesh.end());
      }
    }
    // The first swoosh and electric child the spawn table starts, where the
    // generator has none of its own.
    std::vector<std::pair<uint32_t, uint32_t>> extraSwooshes;  // (id, frame), port-only PSWX
    for (const auto& [type, child, frame] : {std::tuple(F("SWHC"), F("SSWH"), F("SSSD")),
                                              std::tuple(F("ELSC"), F("SELC"), F("SESD"))}) {
      size_t count = 0;
      for (const Started& each : started) {
        if (each.type != type) {
          continue;
        }
        if (count++ == 0 && written.count(child) == 0) {
          AppendBE32(out, child);
          AppendBE32(out, F("CNST"));
          AppendBE32(out, each.id);
          if (written.count(frame) == 0) {
            AppendBE32(out, frame);
            AppendBE32(out, F("CNST"));
            AppendBE32(out, each.frame);
          } else if (each.frame != 0) {
            m_approximated.push_back("KSSM: " + EffectFourCCString(child) + " frame kept from " +
                                     EffectFourCCString(frame));
          }
        } else if (type == F("SWHC")) {
          // Port-only PSWX: retail's generator spawns one swoosh, Remastered's every one it starts.
          extraSwooshes.emplace_back(each.id, each.frame);
        } else {
          result.dropped.push_back("KSSM: a spawned " + EffectFourCCString(type) + " beyond retail's one");
          ++result.droppedRetail;
        }
      }
    }
    if (!extraSwooshes.empty()) {
      AppendBE32(out, F("PSWX"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, uint32_t(extraSwooshes.size()));
      for (const auto& [id, frame] : extraSwooshes) {
        AppendBE32(out, id);
        AppendBE32(out, frame);
      }
    }
    if (spriteShape != 0) {
      AppendBE32(out, F("PSHP"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, spriteShape);
    }
    if ((part || swoosh) && !m_vars.empty()) {
      // Port-only: the effect's PVAR table (names, kinds, defaults) for VARF/VARI/VARV/VARC.
      AppendBE32(out, F("PVRT"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, uint32_t(m_vars.size()));
      for (const PvarEntry& var : m_vars) {
        out.insert(out.end(), var.guid.begin(), var.guid.end());
        AppendBE32(out, var.type);
        for (float f : var.def) {
          AppendBE32(out, FloatBits(f));
        }
      }
    }
    if (!sysItems.empty()) {
      // Port-only: the system variables in evaluation order; each is its
      // EPortVarType, slot, init-only flag and element (CPortSysItem).
      AppendBE32(out, F("PSYS"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, uint32_t(sysItems.size()));
      for (const SystemItem& item : sysItems) {
        AppendBE32(out, item.type);
        AppendBE32(out, item.slot);
        AppendBE32(out, item.initOnly);
        out.insert(out.end(), item.element.begin(), item.element.end());
      }
    }
    // Without a PSLT, Remastered's system lifetime is 0x1000000 (CElementGen ctor 0x244cfc), which
    // an ILPT in a system property reads; retail's default is 0x7FFFFF.
    if (part && written.count(F("PSLT")) == 0) {
      AppendBE32(out, F("PSLT"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, 0x1000000u);
    }
    if (part || swoosh) {
      // Port-only: nested IRND elements are evaluated once per particle and element, not at frame 0
      // only (xPortIrnd). Marks every converted PART; retail's own PARTs do not have it.
      // On a swoosh it marks Remastered's timing: a point's colour runs over the live points
      // (KEWS), not over LENG.
      AppendBE32(out, F("PIRN"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, 1);
    }
    const uint32_t guidWord = uint32_t(node.id[0]) | uint32_t(node.id[1]) << 8 |
                              uint32_t(node.id[2]) << 16 | uint32_t(node.id[3]) << 24;
    if (part && guidWord != 0) {
      // Port-only: the first word of the effect's GUID (little-endian, as the Switch reads it). A
      // Remastered child's seed hashes it in (CElementGen::PortChildSeed); absent means 0.
      AppendBE32(out, F("PGID"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, guidWord);
    }
    if (nestedHdrLight) {
      AppendBE32(out, F("PHDR"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, 1);
      AppendBE32(out, F("PIR0"));
      out.insert(out.end(), nestedLightInnerRadius.begin(), nestedLightInnerRadius.end());
      AppendBE32(out, F("POR0"));
      out.insert(out.end(), nestedLightOuterRadius.begin(), nestedLightOuterRadius.end());
    }
    AppendBE32(out, F("_END"));
    result.approximated = std::move(m_approximated);
    m_approximated.clear();
    return result;
  }

  // The top node is the effect's own PART whatever its root flag says
  // (Remastered sets it only on effects with embedded children).
  void Collect(const EffectNode& node, bool top, std::vector<ConvertedPart>& out) const {
    if (const uint32_t type = EffectRetailType(node.form); type != 0) {
      out.push_back(Generator(node, type));
      out.back().root = top;
    }
    for (const EffectNode& child : node.children) {
      Collect(child, false, out);
    }
  }

private:
  const uint8_t* m_data;
  const EffectConvertIO& m_io;
  mutable std::vector<PvarEntry> m_vars;            // the PVAR of the generator being written
  mutable std::vector<SystemItem>* m_sysItems = nullptr;  // set while an SVR/SVI/SVC/SVV is written
  mutable uint32_t m_sysNextHidden = 10;            // the next hidden init-only real
  mutable std::vector<SystemItem>* m_spawnSysItems = nullptr;  // set while a KSSM is written
  mutable uint32_t m_sysNextHiddenInt = 10;                   // the next hidden init-only int
  mutable std::vector<std::string> m_approximated;  // for the generator being written
  mutable bool m_swoosh = false;                    // the node being written is a SWSH
  mutable bool m_mp1Space = false;                  // inside MPCB(mod vector): its vectors are retail's
  mutable int m_lifetimeOffset = -1;                // LifetimeOffset of the property being written
};


using port::ReadBE32;

// Walks a retail PART the way CParticleDataFactory reads it.
class RetailReader {
public:
  RetailReader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

  size_t At() const { return m_at; }

  bool Word(uint32_t& out) {
    if (m_at + 4 > m_size) {
      return false;
    }
    out = ReadBE32(m_data + m_at);
    m_at += 4;
    return true;
  }

  bool Skip(size_t bytes) {
    if (m_at + bytes > m_size) {
      return false;
    }
    m_at += bytes;
    return true;
  }

  bool Bool() {
    uint32_t fourcc;
    return Word(fourcc) && Skip(1);
  }

  // A keyframe block of `keySize`-byte keys.
  bool Keyframes(size_t keySize) {
    uint32_t count;
    return Skip(18) && Word(count) && Skip(size_t(count) * keySize);
  }

  bool Element(Type type) {
    uint32_t fourcc;
    if (!Word(fourcc)) {
      return false;
    }
    if (type == Type::Emitter && fourcc == F("SETR")) {
      uint32_t name;
      return Word(name) && Element(Type::Vector) && Word(name) && Element(Type::Vector);
    }
    // Port-only VARF/VARI/VARV/VARC: a PVRT index.
    if ((fourcc == F("VARF") && type == Type::Real) || (fourcc == F("VARI") && type == Type::Int) ||
        (fourcc == F("VARV") && type == Type::Vector) || (fourcc == F("VARC") && type == Type::Color)) {
      return Skip(4);
    }
    // Port-only PSYR/PSYI: a PSYS slot.
    if ((fourcc == F("PSYR") && type == Type::Real) || (fourcc == F("PSYI") && type == Type::Int)) {
      return Skip(4);
    }
    // SPA*: the parent's system variable n, then the fallback; DPV*/TPV*: a
    // PVAR guid, then the fallback.
    if (IsParentRead(fourcc, type)) {
      const bool guid = (fourcc >> 24) != 'S';
      return Skip(guid ? 16 : 4) && Element(type);
    }
    // Port-only RNDV: the magnitude as a real element.
    if (fourcc == F("RNDV") && type == Type::Vector) {
      return Element(Type::Real);
    }
    // Port-only PGRD (a swoosh's gradient): stop count, five floats per stop, the period as an
    // int element, then the repeat byte.
    if (fourcc == F("PGRD") && type == Type::Color) {
      uint32_t count;
      return Word(count) && count <= 0x10000 && Skip(size_t(count) * 20) && Element(Type::Int) && Skip(1);
    }
    // Port-only PLPT: the lifetime offset, then the percent as an int element.
    if (fourcc == F("PLPT") && type == Type::Int) {
      return Skip(4) && Element(Type::Int);
    }
    // Port-only SLCT: the index as an int element, the entry count, then the entries.
    if (fourcc == F("SLCT") && (type == Type::Real || type == Type::Vector || type == Type::Color)) {
      uint32_t count;
      if (!Element(Type::Int) || !Word(count) || count == 0 || count > 256) {
        return false;
      }
      for (uint32_t i = 0; i < count; ++i) {
        if (!Element(type)) {
          return false;
        }
      }
      return true;
    }
    const Signatures& elements = ElementsOf(type);
    const auto found = elements.find(fourcc);
    if (found == elements.end()) {
      m_error = "no " + EffectFourCCString(fourcc) + " element of that type";
      return false;
    }
    for (const char* c = found->second; *c != 0; ++c) {
      bool ok;
      switch (*c) {
      case '#':
        ok = Skip(4);
        break;
      case 'k': {
        const size_t sizes[] = {4, 4, 12, 12, 16, 0, 0, 0, 0};
        ok = Keyframes(sizes[size_t(type)]);
        break;
      }
      case 'B':
        ok = Bool();
        break;
      default:
        ok = Element(TypeOfLetter(*c));
        break;
      }
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  bool Texture() {
    uint32_t fourcc;
    if (!Word(fourcc)) {
      return false;
    }
    if (fourcc == F("NONE")) {
      return true;
    }
    // PATL: CNST id, then cols, rows, count, mode and flipX as CNST ints.
    if (fourcc == F("PATL")) {
      uint32_t sub;
      if (!Word(sub) || sub != F("CNST") || !Skip(4)) {
        return false;
      }
      for (int i = 0; i < 5; ++i) {
        if (!Word(sub) || sub != F("CNST") || !Skip(4)) {
          return false;
        }
      }
      return true;
    }
    uint32_t sub;
    if ((fourcc != F("CNST") && fourcc != F("ATEX")) || !Word(sub) || (sub != F("NONE") && !Skip(4))) {
      return false;
    }
    if (fourcc == F("CNST")) {
      return true;
    }
    for (int i = 0; i < 5; ++i) {
      if (!Element(Type::Int)) {
        return false;
      }
    }
    return Bool();
  }

  bool Asset() {
    uint32_t fourcc;
    return Word(fourcc) && (fourcc == F("NONE") || Skip(4));
  }

  // PMDV: CNST n, then n CNST ids.
  bool ModelVariants() {
    uint32_t fourcc, count;
    if (!Word(fourcc) || fourcc != F("CNST") || !Word(count) || count > 64) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      if (!Word(fourcc) || fourcc != F("CNST") || !Skip(4)) {
        return false;
      }
    }
    return true;
  }

  // KSSM: NONE, or CNST, four ints and a frame table (frame, count, 16 bytes each).
  bool SpawnTable() {
    uint32_t fourcc;
    if (!Word(fourcc)) {
      return false;
    }
    if (fourcc != F("CNST")) {
      return true;
    }
    uint32_t frames;
    if (!Skip(16) || !Word(frames)) {
      return false;
    }
    for (uint32_t i = 0; i < frames; ++i) {
      uint32_t count;
      if (!Skip(4) || !Word(count) || !Skip(size_t(count) * 16)) {
        return false;
      }
    }
    return true;
  }

  // The port-only properties (DESIGN.md): `CNST n` and then n records.
  bool CountedList(uint32_t& count) {
    uint32_t fourcc;
    return Word(fourcc) && fourcc == F("CNST") && Word(count);
  }

  // VMAT: CNST, the blob's length, the blob.
  bool Material() {
    uint32_t bytes;
    return CountedList(bytes) && Skip(bytes);
  }

  bool TextureTransforms() {
    uint32_t count;
    if (!CountedList(count) || count > 3) {
      return false;
    }
    for (uint32_t i = 0; i < count * 6; ++i) {
      if (!Element(Type::Real)) {
        return false;
      }
    }
    return true;
  }

  // VPMT: {kind, row, comp, element}; VSMT: {target, kind, element} (reals).
  bool PerParticle(bool shader) {
    uint32_t count;
    if (!CountedList(count)) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t kind = 0;
      if (shader ? !Skip(4) || !Word(kind) : !Word(kind) || !Skip(8)) {
        return false;
      }
      static constexpr Type kTypes[4] = {Type::Real, Type::Vector, Type::Int, Type::Color};
      if (kind > 3 || !Element(shader ? Type::Real : kTypes[kind])) {
        return false;
      }
    }
    return true;
  }

  // A port-only `CNST <word>` property (VORN, XFMD, PIRN).
  bool PortWord() {
    uint32_t fourcc;
    return Word(fourcc) && fourcc == F("CNST") && Skip(4);
  }

  // The port-only PVRT property: CNST, a count, then 36 bytes a variable.
  bool PortVariables() {
    uint32_t fourcc, count;
    return Word(fourcc) && fourcc == F("CNST") && Word(count) && count <= 4096 && Skip(size_t(count) * 36);
  }

  // The port-only PSYS property: CNST, a count, then per item its type, slot,
  // init-only flag and element.
  bool PortSystemVars() {
    static constexpr Type kTypes[4] = {Type::Real, Type::Int, Type::Color, Type::Vector};
    uint32_t fourcc, count;
    if (!Word(fourcc) || fourcc != F("CNST") || !Word(count) || count > 256) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t type, slot, initOnly;
      if (!Word(type) || type > 3 || !Word(slot) || slot > 255 || !Word(initOnly) || initOnly > 1 ||
          !Element(kTypes[type])) {
        if (m_error.empty()) {
          m_error = "bad PSYS";
        }
        return false;
      }
    }
    return true;
  }

  // The port-only PSWX property: CNST, a count, then an id and a frame each.
  bool PortExtraSwooshes() {
    uint32_t fourcc, count;
    return Word(fourcc) && fourcc == F("CNST") && Word(count) && count <= 64 && Skip(size_t(count) * 8);
  }

  // PKSM (Writer::SpawnTables): CNST, end, tables; per table start, const flag, period
  // element, entries; per entry frame, spawns; per spawn type, id, instance element and
  // component index; then the components.
  bool PortSpawnTables() {
    uint32_t fourcc, tables;
    if (!Word(fourcc) || fourcc != F("CNST") || !Skip(4) || !Word(tables) || tables > 64) {
      return false;
    }
    uint32_t used = 0;
    for (uint32_t t = 0; t < tables; ++t) {
      uint32_t entries;
      if (!Skip(8) || !Element(Type::Int) || !Word(entries) || entries > 1000) {
        return false;
      }
      for (uint32_t e = 0; e < entries; ++e) {
        uint32_t spawns;
        if (!Skip(4) || !Word(spawns) || spawns > 100) {
          return false;
        }
        for (uint32_t s = 0; s < spawns; ++s) {
          uint32_t component;
          if (!Skip(8) || !Element(Type::Int) || !Word(component)) {
            return false;
          }
          if (component != ~0u) {
            used = std::max(used, component + 1);
          }
        }
      }
    }
    uint32_t components;
    if (!Word(components) || components > 1000 || used > components) {
      m_error = "spawn component out of range";
      return false;
    }
    for (uint32_t c = 0; c < components; ++c) {
      if (!Mover()) {
        return false;
      }
    }
    return true;
  }

  // PSMV (Writer::Transform): CNST, a count of 0 or 1, then the mover.
  bool PortMover() {
    uint32_t fourcc, count;
    return Word(fourcc) && fourcc == F("CNST") && Word(count) && count <= 1 && (count == 0 || Mover());
  }

  // Writer::Mover's SMOV: position, delta, rotation (or NONE), modifier (NONE or GRAV).
  bool Mover() {
    uint32_t word;
    if (!ComponentVector(0) || !Skip(12) || !Word(word) || (word != F("NONE") && !ComponentRotation(word, 0)) ||
        !Word(word) || (word != F("NONE") && (word != F("GRAV") || !ComponentVector(0)))) {
      if (m_error.empty()) {
        m_error = "bad mover";
      }
      return false;
    }
    return true;
  }

  // Writer::ComponentReal/Vector/Rotation's nodes.
  bool ComponentReal(int depth) {
    uint32_t fourcc, count;
    if (depth > 64 || !Word(fourcc)) {
      return false;
    }
    if (fourcc == F("CNST") || fourcc == F("VARF") || fourcc == F("PSYR")) {
      return Skip(4);
    }
    if (fourcc == F("KEYE")) {
      return Skip(18) && Word(count) && count != 0 && count <= 0x100000 && Skip(size_t(count) * 4);
    }
    if (fourcc == F("ADD_") || fourcc == F("MULT") || fourcc == F("IRND") || fourcc == F("RAND")) {
      return ComponentReal(depth + 1) && ComponentReal(depth + 1);
    }
    m_error = "component real " + EffectFourCCString(fourcc);
    return false;
  }

  bool ComponentVector(int depth) {
    uint32_t fourcc;
    if (depth > 64 || !Word(fourcc)) {
      return false;
    }
    if (fourcc == F("EXTT") || fourcc == F("EXTS")) {
      return true;
    }
    if (fourcc == F("KPIN")) {
      return ComponentVector(depth + 1);
    }
    if (fourcc == F("ADD_") || fourcc == F("MULT")) {
      return ComponentVector(depth + 1) && ComponentVector(depth + 1);
    }
    if (fourcc == F("ROTV")) {
      uint32_t rotation;
      return ComponentVector(depth + 1) && Word(rotation) && ComponentRotation(rotation, depth + 1);
    }
    if (fourcc == F("CNST")) {
      return ComponentReal(depth + 1) && ComponentReal(depth + 1) && ComponentReal(depth + 1);
    }
    if (fourcc == F("CIRC")) {
      return ComponentVector(depth + 1) && ComponentVector(depth + 1) && ComponentReal(depth + 1) &&
             ComponentReal(depth + 1) && ComponentReal(depth + 1);
    }
    m_error = "component vector " + EffectFourCCString(fourcc);
    return false;
  }

  // A rotation node whose fourcc has been read.
  bool ComponentRotation(uint32_t fourcc, int depth) {
    if (depth > 64) {
      return false;
    }
    if (fourcc == F("EXTR") || fourcc == F("IDNT")) {
      return true;
    }
    if (fourcc == F("REUL")) {
      return ComponentReal(depth + 1) && ComponentReal(depth + 1) && ComponentReal(depth + 1);
    }
    uint32_t a, b;
    if (fourcc == F("RADD")) {
      return Word(a) && ComponentRotation(a, depth + 1) && Word(b) && ComponentRotation(b, depth + 1);
    }
    m_error = "component rotation " + EffectFourCCString(fourcc);
    return false;
  }

  const std::string& Error() const { return m_error; }

private:
  const uint8_t* m_data;
  size_t m_size;
  size_t m_at = 0;
  std::string m_error;
};

}  // namespace

uint32_t EffectRetailType(uint32_t form) {
  if (form == F("GPSM")) {
    return F("PART");
  }
  if (form == F("SWSH")) {
    return F("SWHC");
  }
  if (form == F("ELC2") || form == F("ELSM")) {
    return F("ELSC");
  }
  return 0;
}

bool SplitRetailPart(const uint8_t* data, size_t size, std::vector<RetailPartProperty>& out, std::string& error) {
  return SplitRetailEffect(F("PART"), data, size, out, error);
}

bool SplitRetailEffect(uint32_t type, const uint8_t* data, size_t size, std::vector<RetailPartProperty>& out,
                       std::string& error) {
  out.clear();
  RetailReader reader(data, size);
  uint32_t fourcc;
  if (!reader.Word(fourcc) || fourcc != HeaderOf(type)) {
    error = "not a " + EffectFourCCString(HeaderOf(type));
    return false;
  }
  const auto& retail = PropertiesOf(type);
  const bool part = type == F("PART");
  const bool electric = type == F("ELSC");
  const bool vfx = part || type == F("SWHC") || electric; // swooshes and ELC2s take the VFX material too
  for (;;) {
    const size_t start = reader.At();
    if (!reader.Word(fourcc)) {
      error = "no _END";
      return false;
    }
    if (fourcc == F("_END")) {
      return true;
    }
    bool ok;
    if (part && fourcc == F("KSSM")) {
      ok = reader.SpawnTable();
    } else if (part && fourcc == F("PMDV")) {
      ok = reader.ModelVariants();
    } else if ((vfx && fourcc == F("VMAT")) || (part && fourcc == F("VMSH"))) {
      ok = reader.Material();
    } else if (vfx && fourcc == F("VTMT")) {
      ok = reader.TextureTransforms();
    } else if (vfx && (fourcc == F("VPMT") || fourcc == F("VSMT"))) {
      ok = reader.PerParticle(fourcc == F("VSMT"));
    } else if ((part && (fourcc == F("SSZE") || fourcc == F("PIR0") || fourcc == F("POR0"))) ||
               (vfx && fourcc == F("ITEN"))) {
      ok = reader.Element(Type::Real);
    } else if ((part && (fourcc == F("VORN") || fourcc == F("XFMD") ||
                         fourcc == F("PSHP") || fourcc == F("PHDR") || fourcc == F("PGID") || fourcc == F("PBDM") ||
                         fourcc == F("PMRF"))) ||
               (vfx && fourcc == F("PIRN"))) {
      ok = reader.PortWord();
    } else if (vfx && fourcc == F("PVRT")) {
      ok = reader.PortVariables();
    } else if (part && fourcc == F("PSYS")) {
      ok = reader.PortSystemVars();
    } else if (part && fourcc == F("PSWX")) {
      ok = reader.PortExtraSwooshes();
    } else if (part && fourcc == F("PKSM")) {
      ok = reader.PortSpawnTables();
    } else if (part && fourcc == F("PSMV")) {
      ok = reader.PortMover();
    } else if (!part && vfx && (fourcc == F("PSRO") || fourcc == F("PRAF") || fourcc == F("XFMD"))) {
      ok = reader.PortWord();
    } else if (!part && vfx && fourcc == F("PRAZ")) {
      ok = reader.Element(Type::Vector);
    } else if (!part && vfx && fourcc == F("PROZ")) {
      ok = reader.PortWord() && reader.Element(Type::Real);
    } else if (electric && fourcc == F("PEL2")) {
      ok = reader.PortWord();
    } else if (electric && Elc2Properties().count(fourcc) != 0) {
      const Type elcType = Elc2Properties().at(fourcc).type;
      ok = elcType == Type::Bool ? reader.Bool() : reader.Element(elcType);
    } else if (const auto found = retail.find(fourcc); found == retail.end()) {
      error = "property " + EffectFourCCString(fourcc) + " retail does not read";
      return false;
    } else {
      switch (found->second.type) {
      case Type::Bool:
        ok = reader.Bool();
        break;
      case Type::Texture:
        ok = reader.Texture();
        break;
      case Type::Asset:
        ok = reader.Asset();
        break;
      default:
        ok = reader.Element(found->second.type);
        break;
      }
    }
    if (!ok) {
      error = EffectFourCCString(fourcc) + " does not read" + (reader.Error().empty() ? "" : ": " + reader.Error());
      return false;
    }
    out.push_back({fourcc, std::vector<uint8_t>(data + start + 4, data + reader.At())});
  }
}

std::optional<uint32_t> EffectRetailId(const EffectGuid& id) {
  // 10000000-0000-f000-f000-0000XXXXXXXX in pak order is stored here with its
  // first three groups byte-swapped.
  static const uint8_t kPrefix[12] = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xf0, 0xf0, 0x00, 0x00, 0x00};
  if (std::memcmp(id.data(), kPrefix, sizeof(kPrefix)) != 0) {
    return std::nullopt;
  }
  return port::ReadBE32(id.data() + 12);
}

std::vector<ConvertedPart> ConvertEffect(const EffectNode& effect, const uint8_t* data, const EffectConvertIO& io) {
  std::vector<ConvertedPart> out;
  Writer(data, io).Collect(effect, true, out);
  return out;
}

}  // namespace PortRemastered
