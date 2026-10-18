#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "port_remastered_ground_contacts.h"

// Contact adapter between the port's collision data and Remastered's grounded
// mover. Game-independent on purpose: the mover's decisions must not depend on
// which host collision backend supplied a triangle.
//
// Frames: the port is Z-up (retail). Remastered's canonical frame is Y-up, and
// the import's own frame change is `port = (-remX, remZ, remY)`
// (kR2G and MulR2G, platform/port_remastered_room_internal.h). Contact geometry is therefore
// handed to the geometry producer in Remastered's frame and its results are
// converted back, so the producer sees exactly the numbers the shipped routine
// would (the conversion is a sign/permutation, not arithmetic). The change is
// its own inverse and has determinant +1, so winding and cross products are
// unaffected. Note `port.z == remY`, so a Remastered vertical value is the
// port's vertical value unchanged.
namespace PortRemasteredGroundWorld {

using PortRemasteredGroundContacts::Vec3;

struct Vec3f {
  float x = 0.f, y = 0.f, z = 0.f;
};
struct Aabb {
  Vec3f min, max;
};

// One terrain triangle as the port holds it (world space).
struct Triangle {
  std::array< Vec3f, 3 > verts{};
  Vec3f normal{};                 // unit plane normal, port space
  std::uint64_t materialBits = 0; // CMaterialList value, surface merged with the object's
  // The collided object: 0 for static terrain, else a platform
  // (PortRemasteredGroundRider::OwnerFromRaw). Copied to the triangle's contact.
  std::uint32_t ownerUid = 0;
};

// Remastered's CCollisionContact view of one contact, in Remastered's frame.
// Field offsets per the source layout: normal at +8/+0xc/+0x10, material at
// +0x1c, owner at +0x30, secondary at +0x38 and its presence at +0x44,
// penetration scalars at +0x14 (D) and +0x18 (H).
struct Contact {
  bool self = false;              // the queried actor's own contact
  int orientation = -1;           // ECollisionOrientation of this contact's material
  std::uint64_t materialBits = 0;
  // Primary normal in the port's frame. The port is Z-up, so normalZ is the
  // vertical the source equations compare against 0.85/0.7071 (`normal.y` in
  // Remastered's Y-up frame is the same number).
  float normalX = 0.f;
  float normalY = 0.f;
  float normalZ = 0.f;
  bool flag100 = false;           // material behavior bit 0x100 (contact+0x29 bit 0)
  bool hasSecondary = false;
  float secondaryX = 0.f;
  float secondaryY = 0.f;
  float secondaryZ = 0.f;
  // Contact point range, as the vertical extremes of its points (port z).
  // Remastered reads these through CCollisionContact::GetContactPoint over the
  // inclusive index range; every consumer takes a min and/or max over it, so the
  // extremes are the whole contract.
  float minVertical = 0.f;
  float maxVertical = 0.f;
  // Landing queries select the highest point INSIDE an interval; extrema alone
  // cannot answer that when the polygon straddles the interval's upper bound.
  std::array<float, 10> pointVertical{};
  std::array<Vec3f, 10> points{};
  std::size_t pointCount = 0;
  float D = 0.f;                  // +0x14
  float H = 0.f;                  // +0x18
  std::size_t triangle = 0;       // caller's triangle index
  // +0x30 for the triangle's side: the collided object, 0 for static terrain. The box's own
  // side (self) keeps 0; its owner is the queried actor.
  std::uint32_t ownerUid = 0;
};

// The actor's box in port world space, plus the actor's own material list: the
// producer takes the box's material from the box object.
struct Query {
  Aabb box{};
  std::uint64_t actorMaterialBits = 0;
};

// ECollisionOrientation, as produced by TranslatePositiveMP1Material
// 0xc59e04-0xc59e20: bit 29 -> 1, bit 30 -> 2, bit 31 -> 0, later store wins.
int Orientation(std::uint64_t materialBits);

// Material behavior bit 0x100, from the two proven producers:
// TranslatePositiveMP1Material ORs 0x110 for list bit 58
// (0xc59de4-0xc59df0), and CPhysicsActorMP1::TranslateMaterialList ORs 0x110
// for bit 33 while bit 55 is clear (0xc52428-0xc52450).
bool Flag100(std::uint64_t materialBits);

// Runs the source box/triangle producer over one triangle and converts its
// results out of Remastered's frame. Returns false when the triangle misses.
// Box contact first, then the triangle's; either may be absent.
bool BuildTriangleContacts(const Query& query, const Triangle& tri,
                           std::array< Contact, 2 >& contacts, std::size_t& count);

struct Result {
  // The shipped collision list starts inline in the mover's frame and grows on the
  // heap: MoveObject re-seeds it before every pass (0xf924b8-0xf92500) and frees
  // three heap blocks per iteration (0xf92508-0xf9251c, 0xf927b0-0xf927c4); there
  // is no failure path. The adapter therefore grows with the contacts it is given
  // and never drops one.
  std::vector< Contact > contacts;
  // Set when the list outgrew the shipped inline capacity. Informational only: the
  // shipped list has no failure path, so this must never change a decision.
  bool grewBeyondInlineCapacity = false;
  std::size_t hits = 0;
};
Result BuildContacts(const Query& query, const Triangle* tris, std::size_t count);

struct TerrainResult {
  bool booleanHit = false; // 0xc1f110: inclusive SAT, no response/post-filter
  Result impacts;          // full CheckCollision: SAT followed by impact production
};
TerrainResult QueryTerrain(const Query&, const Triangle*, std::size_t count,
                           bool produceImpacts);

// The contact every shipped height/normal consumer reads out of each 0x90 pair:
// the side whose +0x30 owner is NOT the queried object —
//   0xf928c0:66-70   this_00 = pCVar10 + 0x48; if (owner != object) this_00 = pCVar10;
//   0xf93c9c:311-315 and 0xf934ec:546-549, the same selection.
// Appends them in list order; the response loop (0xf92ccc:316-320) is the only
// consumer that reads both sides of a pair (the other side supplies +0x14).
void ConsumerContacts(const Result& result, std::vector< const Contact* >& out);

} // namespace PortRemasteredGroundWorld
