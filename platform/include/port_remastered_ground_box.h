#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "port_collision_failsafe.h"
#include "port_remastered_box_landing.h"
#include "port_remastered_collision_material.h"
#include "port_remastered_ground_world.h"

// Box-only (AABX) moving platforms in the Remastered movers' producer lists (issue #117).
// The pure half: the box/box pair as the movers' contact model, and its place in the
// near-list order. The game glue is CGameCollision (PortBoxPlatformContacts and the two
// movers' TerrainContacts).
//
// Sources (Remastered 1.0.0 offsets):
//  - AABox_AABox_Boolean 0xbfce60: the self filter's Passes(other material) (vt+0x10), then
//    both GetTransformedAABox and the inclusive DoBoundsOverlap 0x98234. A box collidable's
//    GetTransformedAABox 0xbf6064 is GetTranslatedAABox by the transform's translation
//    (0xbf6070-0xbf608c), so a platform's rotation never turns its box.
//  - CheckCollision 0xc1ec60 / CheckCollision_Boolean_NoPostFilter 0xc1f110 visit the
//    object only when its own filter (+0x1270) passes us; PairPasses below applies both
//    gates on the translated materials, as the box-actor landing does.
//  - AABox_AABox 0xbf61b4 + 0xbf6650: one CCollisionInfo per pair; each impact is a 0x90
//    pair of halves. Self half: normal n, +0x14 = +0x18 = depth, the self collidable's
//    material, no secondary. Other half: -n, the same depth, the other's material and
//    ids. Each half has ONE point (the intersection face's centroid, axis coordinate at
//    that half's face). Per impact SurfaceCollisionPasses(other material, -n) (vt+0x18).
//    BoxBoxPairedImpacts (port_collision_failsafe.h) is that geometry bit for bit.
//
// The movers' contact model (PortRemasteredGroundWorld::Contact) already is that pair
// shape: a self contact followed by its other half, sharing `triangle`. A box impact
// therefore becomes the same two contacts, so every consumer (the up/down probe, OnWall,
// the snap, the grounded and airborne responses, the rider scan, the landing walk) reads
// it with no box-specific branch: the consumers pick the half whose owner is not the
// object and the response reads the depth from the self half. `triangle` is a synthetic
// key above every real triangle index (kBoxKeyBase), unique per impact.
namespace PortRemasteredGroundBox {

using PortCollisionFailsafe::Box;
using PortCollisionFailsafe::PairedImpact;
using PortRemasteredGroundWorld::Contact;
using PortRemasteredGroundWorld::Result;

// Keys of box contacts: far above any triangle index a query can produce, and spaced so
// each platform of a query owns a block (an AABox pair emits at most six impacts).
constexpr std::size_t kBoxKeyBase = std::size_t(1) << 30;
constexpr std::size_t kBoxKeyStride = 8;
inline bool IsBoxKey(std::size_t key) { return key >= kBoxKeyBase; }

// The two sides of one pair, as the adapter finds them: the boxes already placed (the
// self box at the probed position, the platform's translated box), the material LISTS
// (raw EMaterialTypes bits; the collidable material is their translation, 0xc53ccc) and
// the two collision filters (+0x1270).
using PortRemasteredBoxLanding::Pair;

// 0xbfce60 behind 0xc1f110's visit test: both filters on the translated materials, then
// the inclusive overlap. Touching boxes are blocked.
inline bool Blocked(const Pair& pair) {
  return PortRemasteredBoxLanding::PairPasses(pair) &&
         PortRemasteredBoxLanding::BoxesOverlap(pair.selfBox, pair.otherBox);
}

// One impact's half as a contact.
inline Contact Half(bool self, std::uint64_t materialBits, const PortCollisionFailsafe::Vec3& n,
                    float depth, const PortCollisionFailsafe::Vec3& point, std::size_t key,
                    std::uint32_t owner) {
  Contact c;
  c.self = self;
  c.orientation = PortRemasteredGroundWorld::Orientation(materialBits);
  c.materialBits = materialBits;
  c.normalX = n.x;
  c.normalY = n.y;
  c.normalZ = n.z;
  c.flag100 = PortRemasteredGroundWorld::Flag100(materialBits);
  c.minVertical = point.z;
  c.maxVertical = point.z;
  c.points[0] = {point.x, point.y, point.z};
  c.pointVertical[0] = point.z;
  c.pointCount = 1;
  c.D = depth;
  c.H = depth;
  c.triangle = key;
  c.ownerUid = owner;
  return c;
}

// 0xbf61b4 for one pair: appends its impacts to `out` as self/other contact pairs in
// source order (axes x, up, z; +1 before -1), keys `keyBase + i`. `owner` is the platform's
// contact owner (PortRemasteredGroundRider::OwnerFromRaw). Returns the number of impacts;
// zero when a filter rejects the pair, the boxes do not overlap, they only touch, or every
// candidate failed the per-impact surface test.
inline std::size_t AppendContacts(const Pair& pair, std::uint32_t owner, std::size_t keyBase,
                                  std::vector< Contact >& out,
                                  std::vector< PairedImpact >& scratch) {
  using namespace PortRemasteredCollisionMaterial;
  if (!Blocked(pair)) return 0;
  const CollisionMaterial otherMat = TranslateActor(pair.otherMaterialBits);
  scratch.clear();
  PortCollisionFailsafe::BoxBoxPairedImpacts(
      pair.selfBox, pair.otherBox,
      [&pair, &otherMat](const PortCollisionFailsafe::Vec3& otherNormal) {
        return SurfacePasses(pair.selfFilter, otherMat, otherNormal.z);
      },
      scratch);
  for (std::size_t i = 0; i < scratch.size(); ++i) {
    const PairedImpact& im = scratch[i];
    out.push_back(Half(true, pair.selfMaterialBits, im.selfNormal, im.selfDepth, im.selfPoint,
                       keyBase + i, 0));
    out.push_back(Half(false, pair.otherMaterialBits, im.otherNormal, im.otherDepth,
                       im.otherPoint, keyBase + i, owner));
  }
  return scratch.size();
}

// A box platform's contacts and its place in the near list: `at` is the number of
// triangles (terrain and earlier tree platforms) that precede it. `blocked` is the boolean
// twin's answer (Blocked), which touching boxes satisfy with no contacts.
struct Slot {
  std::size_t at = 0;
  bool blocked = false;
  std::vector< Contact > contacts;
};

inline bool AnyBlocked(const std::vector< Slot >& slots) {
  for (const Slot& s : slots) {
    if (s.blocked) return true;
  }
  return false;
}

inline bool AnyContacts(const std::vector< Slot >& slots) {
  for (const Slot& s : slots) {
    if (!s.contacts.empty()) return true;
  }
  return false;
}

// Interleaves the box platforms' contacts with a triangle result in near-list order
// (FindOverlap 0xc1a760: static tree first, then actors by `id & 0xffff`): a slot's
// contacts go before the first triangle contact whose index is >= its `at`, and slots
// keep their own order. A triangle pair is never split, since both halves share an index.
inline void Merge(Result& result, const std::vector< Slot >& slots) {
  if (slots.empty()) return;
  std::vector< Contact > merged;
  merged.reserve(result.contacts.size());
  std::size_t s = 0;
  for (const Contact& c : result.contacts) {
    while (s < slots.size() && slots[s].at <= c.triangle) {
      merged.insert(merged.end(), slots[s].contacts.begin(), slots[s].contacts.end());
      ++s;
    }
    merged.push_back(c);
  }
  for (; s < slots.size(); ++s) {
    merged.insert(merged.end(), slots[s].contacts.begin(), slots[s].contacts.end());
  }
  result.contacts.swap(merged);
}

} // namespace PortRemasteredGroundBox
