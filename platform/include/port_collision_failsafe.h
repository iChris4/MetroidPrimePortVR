#pragma once

#include <cstddef>
#include <functional>
#include <vector>

// Remastered's collision failsafe for the MP1 stepped mover (issue #117):
// CPhysicsCollisionFailsafe::DoCollisionFailsafe 0xc1fa68 (core 0xc1fc54),
// DoTunnelingFailsafe 0xc20214 and NPhysicsCollision::CalculateOffsetToSafeCollisionPoint
// 0xc21740/0xc217a0, plus the two primitive routines they need for box actors:
// CCollidableAABox::AABox_AABox 0xbf61b4 (impacts via 0xbf6650) and
// NCollisionRayCast::AABox 0xbf7e50. Game-independent: the caller supplies the
// collision queries as hooks. Port frame (Z up) throughout; the box routines
// convert to Remastered's Y-up frame internally so axis order and ties match.
// Spec: build/s-failsafe-20261016/spec/SPEC.md.
namespace PortCollisionFailsafe {

struct Vec3 {
  float x = 0.f, y = 0.f, z = 0.f;
};
struct Box {
  Vec3 min, max;
};

// One impact of a contact, as the core reads it: the self half's +0x14 depth and the
// other half's normal (+0x8) and filter flags (+0x2c & 0x41).
struct Impact {
  float depth = 0.f;
  Vec3 otherNormal;
  bool otherCharacter = false; // Character or CollisionActor: horizontal push
  Vec3 otherCenter;            // other object's collision bounds centre
};
// One CCollisionInfo {first,last} range: one per collided object (a whole area tree
// for terrain).
struct Contact {
  std::vector< Impact > impacts;
};

// 0xc1fc54 inner loop, one iteration: the deepest contact's shallowest impact.
// Returns true when any impact was considered ("moved").
bool SelectPush(const std::vector< Contact >& contacts, const Vec3& from, float worldDt,
                Vec3& push);

struct CoreHooks {
  // CheckCollision 0xc1ec60 at `pos` with the core's filter, through the pass-all result
  // filter: false (nothing collided) ends the loop.
  std::function< bool(const Vec3& pos, std::vector< Contact >& out) > collide;
  // DoTunnelingFailsafe(from, to, out).
  std::function< bool(const Vec3& from, const Vec3& to, Vec3& out) > tunnel;
};
// 0xc1fc54: up to five pushes from `from`, then the tunneling check from the state
// (`from`) to the result.
bool Core(const Vec3& from, float worldDt, const CoreHooks& hooks, Vec3& out);
// Same, with the tunneling test's origin (the object's state transform) apart from `from`
// (the explicit start of the 0xc1fb88 wrapper). The pushes still reference `from`.
bool Core(const Vec3& from, const Vec3& objectState, float worldDt, const CoreHooks& hooks,
          Vec3& out);

// 0xc1fa68. `core(failsafe, out)` runs Core from `cur` with the collision filter
// (0x1270) or the failsafe filter (0x1290); `blocked(failsafe, pos)` is
// CheckGlobalCollision_Boolean_NoPostFilter 0xc1f240 with that filter. `out` is the
// translation to set (== cur when the object is not moved); the return value is the
// failsafe's result.
bool DoCollisionFailsafe(const Vec3& cur, bool hasFailsafeFilter,
                         const std::function< bool(bool failsafe, Vec3& out) >& core,
                         const std::function< bool(bool failsafe, const Vec3& pos) >& blocked,
                         Vec3& out);

// ERayResult.
enum RayCode { kRay_Miss = 0, kRay_Inside = 1, kRay_Hit = 2 };
struct RayHit {
  int code = kRay_Miss;
  float t = 0.f;
  Vec3 normal;
  int orientation = -1; // ECollisionOrientation of the hit material
};

// NCollisionUtils::is_floor 0xbecb40.
bool IsFloor(float up, int orientation);

struct TunnelHooks {
  // RayIntersection 0xc20810 over the swept near list (no Character/CollisionActor).
  std::function< RayHit(const Vec3& start, const Vec3& dir, float length) > ray;
  // CalculateOffsetToSafeCollisionPoint at `pos` with `maxDist`; false = no valid offset.
  std::function< bool(const Vec3& pos, float maxDist, Vec3& offset) > safeOffset;
};
// 0xc20214's entry test: |to - from| > the bounds' smallest half-extent. False means
// DoTunnelingFailsafe returns false with out = to; lets the caller skip building hooks.
bool TunnelingFailsafeApplies(const Box& bounds, const Vec3& from, const Vec3& to);
// 0xc20214. `bounds`/`pos` are the object's CURRENT collision bounds and translation.
bool DoTunnelingFailsafe(const Box& bounds, const Vec3& pos, const Vec3& from, const Vec3& to,
                         const TunnelHooks& hooks, Vec3& out);

// 0x459c800 (26, port frame) and 0x1d1f9d8 (21).
extern const Vec3 kSafeDirections[26];
extern const float kSafeRadii[21];

struct SafeHooks {
  // GetCollision_Boolean of the object's box placed at `pos` against the near list.
  std::function< bool(const Vec3& pos) > collidesAt;
  // A ray from `start` along `dir` hits a near object with t < length.
  std::function< bool(const Vec3& start, const Vec3& dir, float length) > rayBlocked;
};
// 0xc217a0. `box` is the object's box at `pos`. Returns valid; `offset` is the push.
bool CalculateOffsetToSafeCollisionPoint(const Box& box, const Vec3& pos, float maxDist,
                                         const SafeHooks& hooks, Vec3& offset);

// 0xbf61b4: impacts of `self` against `other`, normals the OTHER side's (push self
// along them). Returns false when the boxes do not overlap.
bool BoxBoxImpacts(const Box& self, const Box& other, std::vector< Impact >& out);

// Full AABox_AABox geometry (0xbf61b4 wrapper + 0xbf6650 helper), port frame. Both
// halves of a source impact: each carries its owner's OUTWARD face normal, the same depth
// and one face-centre point; no secondary normal. Materials and ids are the caller's.
struct PairedImpact {
  float selfDepth = 0.f;  // self half's depth
  float otherDepth = 0.f; // other half's depth (== selfDepth in source)
  Vec3 selfNormal;        // self's outward face normal
  Vec3 otherNormal;       // other's: component-wise negation of selfNormal (zero signs too)
  Vec3 selfPoint;         // centroid of the intersection face, axis at self's face plane
  Vec3 otherPoint;        // same, axis at other's face plane
};
// SurfaceCollisionPasses on the OTHER's material with the other half's normal (port
// frame), asked once per candidate impact. An empty function accepts everything.
using NormalPredicate = std::function< bool(const Vec3& otherNormal) >;
// Appends the impacts of `self` against `other` in source order (Remastered axes x, y(up),
// z; +1 side before -1) and returns true when any were appended. Every impact one call
// appends belongs to the SAME pair contact: the append is flat and carries no end marker,
// so the caller must snapshot out.size() before the call (or hand in a fresh contact's
// vector) and group exactly the appended range as one contact, as port_remastered_movers.cpp does
// for the legacy API below. A candidate rejected by the depth > 0 test or by `passes` is
// dropped; when every primary candidate was dropped the minimum-difference fallback
// candidate is tried (also gated by `passes`). Returns false (appending nothing) when the
// boxes do not overlap (inclusive) or nothing passed. The entry Passes(otherMaterial)
// check is the caller's, as is hoisting `passes` out of the tick loop. Preconditions: all
// coordinates finite and min <= max on every axis; no epsilon is applied.
bool BoxBoxPairedImpacts(const Box& self, const Box& other, const NormalPredicate& passes,
                         std::vector< PairedImpact >& out);

// 0xc02cec dispatcher / 0xc02d70 CCollidableSphere::AABox_Sphere: impacts of `self`
// against a sphere at `center` with `radius`, normal the sphere half's (push self
// along it) and depth the box half's (both halves carry the same depth in source).
// Appends ONE impact, like BoxBoxImpacts; leaves otherCharacter/otherCenter to the
// caller. Returns false when the sphere is clear of the box or exactly tangent.
bool BoxSphereImpacts(const Box& self, const Vec3& center, float radius,
                      std::vector< Impact >& out);

// 0xbf7e50.
RayHit RayAABox(const Box& box, const Vec3& start, const Vec3& dir, float length);

// NCollisionRayCast::Sphere 0xbf6ac0 through the CCollidableSphere::RayCast wrapper
// 0xc0289c: `center` and `radius` are the sphere after the wrapper's transform (the
// radius is passed through untouched) and `dir` is the ray's unit direction, as the
// source's CUnitVector3f. A ray starting STRICTLY inside the sphere
// returns code 1 with t = 0 and the wrapper's untouched skUpVector normal; the boundary
// is not inside. Otherwise the entry point (t = B - sqrt, `t < length`, an endpoint
// exactly at `length` is a miss) is code 2 with the normal normalized from the hit
// point, and a backward ray or exact tangency (disc <= 0) misses.
RayHit RaySphere(const Vec3& center, float radius, const Vec3& start, const Vec3& dir,
                 float length);

} // namespace PortCollisionFailsafe
