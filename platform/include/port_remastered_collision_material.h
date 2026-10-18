#pragma once

#include <cstdint>

// Game-independent Remastered MP1 collision material translation and filter
// policy (NSO addresses in comments; extracts in build/s-movement-next-20261016/
// dynamic-material/). Material-list bit numbers are the port's EMaterialTypes.
// Nothing here reads gameplay state: an adapter supplies material lists, modes
// and the contact normal's VERTICAL component as `up`. Source Y equals port Z
// (platform/port_remastered_ground_world.cpp:30-31, ToRemastered/FromRemastered,
// and the importer), so a port-space caller passes normal.z; this header only sees
// the scalar and has no basis of its own.
namespace PortRemasteredCollisionMaterial {

// CCollisionMaterial (ctor 0xbea8c8): +0 orientation, +4 type, +8 world, +0xc
// behavior, +0x10 filter. EmptyCollisionMaterial 0xbead38.
struct CollisionMaterial {
  int32_t orientation = -1;
  int32_t type = -1;
  int32_t world = -1;
  uint32_t behavior = 0;
  uint32_t filter = 0;
};

constexpr uint64_t Bit(int n) { return uint64_t{1} << n; }

// MP1 list bits (EMaterialTypes).
namespace List {
constexpr int NoStepLogic = 0, Stone = 1, Metal = 2, Grass = 3, Ice = 4, Pillar = 5,
              MetalGrating = 6, Phazon = 7, Dirt = 8, Lava = 9, LavaStone = 10,
              Snow = 11, MudSlow = 12, HalfPipe = 13, Mud = 14, Glass = 15,
              Shield = 16, Sand = 17, ProjectilePassthrough = 18, Solid = 19,
              CameraPassthrough = 21, Wood = 22, Organic = 23, SeeThrough = 26,
              ScanPassthrough = 27, AIPassthrough = 28, Ceiling = 29, Wall = 30,
              Floor = 31, Player = 32, Character = 33, Projectile = 35,
              GroundCollider = 37, NoStaticCollision = 38, Orbit = 41,
              Occluder = 42, Immovable = 43, Debris = 44, CollisionActor = 47,
              AIBlock = 48, Platform = 49, NonSolidDamageable = 50,
              SolidCharacter = 55, NoPlayerCollision = 58;
}  // namespace List

// Filter list values (+0x10) and behavior values (+0xc).
constexpr uint32_t kFilterAIBlock = 0x2000;  // shared channel, see Passthrough
constexpr uint32_t kFilterCameraBlock = 0x4000;
constexpr uint32_t kFilterProjectileBlock = 0x10000;  // shared channel
constexpr uint32_t kFilterScanBlock = 0x20000;
constexpr uint32_t kBlockChannels = 0x36000;  // 0xc59b70 synthesis mask
constexpr uint32_t kBehaviorNoPlayer = 0x110;

namespace detail {
constexpr bool Has(uint64_t list, int bit) { return (list >> bit) & 1; }
}  // namespace detail

// NGamePhysicsUtilsMP1::TranslatePositiveMP1Material, 0xc59c20. Orientation
// stores run Ceiling, Wall, Floor in order, so Floor > Wall > Ceiling; the
// material type is the first set bit in the source's chain order.
inline CollisionMaterial TranslatePositive(uint64_t list) {
  using namespace detail;
  CollisionMaterial m;
  auto f = [&](int bit, uint32_t v) { if (Has(list, bit)) m.filter |= v; };
  auto b = [&](int bit, uint32_t v) { if (Has(list, bit)) m.behavior |= v; };
  f(List::Solid, 0x8000);
  f(List::AIBlock, 0x2000);
  f(List::NonSolidDamageable, 0x10000);
  f(List::Player, 0x4);
  f(List::Character, 0x1);
  f(List::Projectile, 0x8);
  f(List::Platform, 0x2);
  f(List::CollisionActor, 0x40);
  f(List::Occluder, 0x80000);
  f(List::SeeThrough, 0x1000);
  f(List::Debris, 0x100000);
  f(List::NoStaticCollision, 0x410);
  f(List::Orbit, 0x800);
  f(List::GroundCollider, 0x100);
  b(List::HalfPipe, 0x4);
  b(List::Immovable, 0x8);
  b(List::NoPlayerCollision, 0x110);
  b(List::Pillar, 0x200);
  if (Has(list, List::Ceiling)) m.orientation = 1;
  if (Has(list, List::Wall)) m.orientation = 2;
  if (Has(list, List::Floor)) m.orientation = 0;
  struct T { int bit; int32_t type; };
  static constexpr T kTypes[] = {
      {0, 0},   {1, 0x1b}, {2, 0xc},  {3, 7},    {4, 8},    {6, 0xd},
      {7, 0x11}, {8, 3},   {9, 9},    {10, 0xa}, {11, 0x19}, {12, 0xf},
      {14, 0xb}, {15, 0x12}, {16, 0x18}, {17, 0x16}, {22, 0x1c}, {23, 0x10}};
  for (const T& t : kTypes) {
    if (Has(list, t.bit)) { m.type = t.type; break; }
  }
  return m;
}

// The four passthrough list bits as filter channels (0xc59ec0 and the middle of
// 0xc59b70).
constexpr uint32_t PassthroughChannels(uint64_t list) {
  using namespace detail;
  return (Has(list, List::AIPassthrough) ? kFilterAIBlock : 0u) |
         (Has(list, List::CameraPassthrough) ? kFilterCameraBlock : 0u) |
         (Has(list, List::ProjectilePassthrough) ? kFilterProjectileBlock : 0u) |
         (Has(list, List::ScanPassthrough) ? kFilterScanBlock : 0u);
}

// NGamePhysicsUtilsMP1::TranslateMP1Material, 0xc59b70: positive translation,
// and for a Solid list the blocking channels not named by a passthrough bit are
// OR'd into the filter (UnionWith 0xbeac80 touches only behavior/filter here).
inline CollisionMaterial TranslateMaterial(uint64_t list) {
  CollisionMaterial m = TranslatePositive(list);
  if (detail::Has(list, List::Solid)) {
    m.filter |= ~PassthroughChannels(list) & kBlockChannels;
  }
  return m;
}

// NGamePhysicsUtilsMP1::TranslateNegativeMP1Material, 0xc59ec0: only the four
// passthrough bits, unconditionally, as filter channels. Orientation/type/world
// stay -1 and behavior stays empty.
inline CollisionMaterial TranslateNegative(uint64_t list) {
  CollisionMaterial m;
  m.filter = PassthroughChannels(list);
  return m;
}

// CPhysicsActorMP1 MaterialListChanged, 0xc53ccc (ctor 0xc52288): the base
// translation, then Character && !SolidCharacter adds behavior 0x110 (the same
// value NoPlayerCollision writes). This is NOT part of TranslateMaterial.
// It intentionally does NOT model CScriptPlatformMP1::MaterialListChanged
// 0xd6d7b0 (filter &= 0xfffc9fff on a non-solid -> solid transition when the
// platform's +0x4e0 bit 0x40 is set): that rule depends on the previous state and
// a per-platform flag, so the adapter must preserve the transition itself.
inline CollisionMaterial TranslateActor(uint64_t list) {
  CollisionMaterial m = TranslateMaterial(list);
  if (detail::Has(list, List::Character) && !detail::Has(list, List::SolidCharacter)) {
    m.behavior |= kBehaviorNoPlayer;
  }
  return m;
}

// TranslateMP1OrientationFlags, 0xc5a210: Floor 1 | Wall 2 | Ceiling 4.
constexpr uint32_t OrientationFlags(uint64_t list) {
  using namespace detail;
  return (Has(list, List::Floor) ? 1u : 0u) | (Has(list, List::Wall) ? 2u : 0u) |
         (Has(list, List::Ceiling) ? 4u : 0u);
}

// COrientationCollisionFilter::Passes 0xc10c18 (orientation -1 returns true
// here; the MP1 wrapper rejects it first) and the MP1 wrapper 0xcb74c0.
constexpr bool OrientationPassesBase(uint32_t flags, int32_t o) {
  if ((flags & 1) && o == 0) return true;
  if ((flags & 2) && o == 2) return true;
  if (o == 1 && ((flags >> 2) & 1)) return true;
  if (o == -1) return true;
  return o == 3;
}
constexpr bool OrientationPasses(uint32_t flags, int32_t o) {
  if (o == -1 || o == 3) return false;
  return OrientationPassesBase(flags, o);
}

// NCollisionUtils::is_floor 0xbecb40 / is_wall 0xbecbb0 / is_ceiling 0xbecc60.
// `up` is the contact normal's vertical component. Unsigned compares as in the
// source (orientation -1 and 3 are "neither floor nor wall nor ceiling").
constexpr bool IsFloor(int32_t o, float up) {
  return up > 0.85f || o == 0 ||
         (up > 0.70710677f && static_cast<uint32_t>(o) - 1u > 1u);
}
constexpr bool IsWall(int32_t o, float up) {
  if (up > 0.85f) return false;
  return o == 2 || (up >= -0.70710677f && up <= 0.70710677f &&
                    static_cast<uint32_t>(o) > 1u);
}
constexpr bool IsCeiling(int32_t o, float up) {
  if (o == 1) return true;
  if ((static_cast<uint32_t>(o) & 0xfffffffdu) == 0) return false;  // 0 or 2
  return up < -0.70710677f;
}

// COrientationCollisionFilter::SurfaceCollisionPasses, 0xc10c68; the MP1
// wrapper 0xcb74e0 forwards to it with NO -1/3 guard.
constexpr bool OrientationSurfacePasses(uint32_t flags, int32_t o, float up) {
  return ((flags & 1) && IsFloor(o, up)) || ((flags & 2) && IsWall(o, up)) ||
         ((flags & 4) && IsCeiling(o, up));
}

// Base list filters (0xc10648 Include, 0xc10910 Exclude, 0xc116d0 ExcludeSome,
// 0xc11594 IncludeAll). Passes and SurfaceCollisionPasses are identical for
// these. `b`/`f` are the filter's behavior and filter lists.
struct ListFilter {
  uint32_t b = 0, f = 0;
};
constexpr bool IncludePasses(ListFilter l, const CollisionMaterial& m) {
  return (m.behavior & l.b) != 0 || (m.filter & l.f) != 0;
}
constexpr bool ExcludePasses(ListFilter l, const CollisionMaterial& m) {
  return (m.behavior & l.b) == 0 && (m.filter & l.f) == 0;
}
// True when the material lacks ANY channel of l, i.e. (l.b & ~m.b) != 0 or
// (l.f & ~m.f) != 0, evaluated exactly as the source's nested form. An empty
// list therefore never passes.
constexpr bool ExcludeSomePasses(ListFilter l, const CollisionMaterial& m) {
  if ((l.b & ~m.behavior) == 0) return (l.f & ~m.filter) != 0;
  return true;
}
// True when the material has ALL channels of l; an empty list always passes.
constexpr bool IncludeAllPasses(ListFilter l, const CollisionMaterial& m) {
  if ((l.b & ~m.behavior) == 0) return (l.f & ~m.filter) == 0;
  return false;
}

// CMaterialFilterMP1 (include u64 +0, exclude u64 +8, mode +0x10). Modes:
// 0 none, 1 include, 2 exclude, 3 both. The source ctor (0xcb6f0c) replaces a
// list of value < 2 that the mode does not use with an inert constant, so the
// unused list is never consulted and is ignored here.
struct MaterialFilter {
  uint64_t include = 0;
  uint64_t exclude = 0;
  uint32_t mode = 0;
};

inline ListFilter PositiveList(uint64_t list) {
  CollisionMaterial m = TranslatePositive(list);
  return {m.behavior, m.filter};
}
inline ListFilter NegativeList(uint64_t list) {
  return {0, PassthroughChannels(list)};
}

// CIncludeCollisionFilterMP1 0xcb76a8 (surface form): positive Include OR
// negative ExcludeSome OR orientation.
inline bool IncludeSurfacePasses(uint64_t list, const CollisionMaterial& m, float up) {
  return IncludePasses(PositiveList(list), m) || ExcludeSomePasses(NegativeList(list), m) ||
         OrientationSurfacePasses(OrientationFlags(list), m.orientation, up);
}
// CExcludeCollisionFilterMP1 0xcb73a4: positive Exclude AND negative IncludeAll
// AND NOT orientation.
inline bool ExcludeSurfacePasses(uint64_t list, const CollisionMaterial& m, float up) {
  return ExcludePasses(PositiveList(list), m) && IncludeAllPasses(NegativeList(list), m) &&
         !OrientationSurfacePasses(OrientationFlags(list), m.orientation, up);
}
// Non-surface forms (0xcb763c / 0xcb7338) use the guarded orientation Passes.
inline bool IncludeMaterialPasses(uint64_t list, const CollisionMaterial& m) {
  return IncludePasses(PositiveList(list), m) || ExcludeSomePasses(NegativeList(list), m) ||
         OrientationPasses(OrientationFlags(list), m.orientation);
}
inline bool ExcludeMaterialPasses(uint64_t list, const CollisionMaterial& m) {
  return ExcludePasses(PositiveList(list), m) && IncludeAllPasses(NegativeList(list), m) &&
         !OrientationPasses(OrientationFlags(list), m.orientation);
}

// CIncludeExcludeCollisionFilterMP1::SurfaceCollisionPasses, 0xcb7110. Mode 1|3
// requires Include, mode 2|3 requires Exclude, mode 0 passes everything. The
// non-surface combination is Passes below.
inline bool SurfacePasses(const MaterialFilter& fl, const CollisionMaterial& m, float up) {
  if ((fl.mode | 2) == 3 && !IncludeSurfacePasses(fl.include, m, up)) return false;
  if ((fl.mode & 0xfffffffeu) == 2 && !ExcludeSurfacePasses(fl.exclude, m, up)) return false;
  return true;
}

// CIncludeExcludeCollisionFilterMP1::Passes, 0xcb7084: the same shape as above
// through the guarded material forms (0xcb763c / 0xcb7338). Modes >= 4 pass.
inline bool Passes(const MaterialFilter& fl, const CollisionMaterial& m) {
  if ((fl.mode | 2) == 3 && !IncludeMaterialPasses(fl.include, m)) return false;
  if ((fl.mode & 0xfffffffeu) == 2 && !ExcludeMaterialPasses(fl.exclude, m)) return false;
  return true;
}

}  // namespace PortRemasteredCollisionMaterial
