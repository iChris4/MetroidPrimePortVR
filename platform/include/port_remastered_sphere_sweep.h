#pragma once

#include <vector>

#include "port_collision_failsafe.h" // Vec3 (port frame, Z up)

// NCollidablePairs::Sphere_Sphere_Swept 0xc012d0, over NCollisionBoolean::Sphere_Sphere
// 0xc042b0 (the overlap gate), CCollidableSphere::Sphere_Sphere 0xc029f0 (the overlap
// contact), NCollisionRayCast::Sphere 0xbf69d0 (the sweep) and
// CVector3f::NormalizeWithMagnitude 0x6b100 (the unit delta and the distance it
// returns): one sphere (the mover) against one near-list sphere, swept along a unit
// direction with a shared `t`. Geometry only: materials, physics ids and any filter
// policy beyond the pair gate are the caller's.
//
// API frame is the port's (Z up, PortCollisionFailsafe::Vec3). Every ordered arithmetic
// step - including the point sums and the normal negation - runs in the source's
// Remastered Y-up frame and is converted back at the end, exactly as
// platform/port_remastered_ground_world.cpp:30-31 defines it (signed zeros included).
namespace PortRemasteredSphereSweep {

using Vec3 = PortCollisionFailsafe::Vec3;

// World centre already transformed by the caller (the source's GetTransformedSphere
// 0xc027e8 done outside). Offset-shape local centres (obj+0x34/0x24) are out of scope.
struct Sphere {
  Vec3 center;
  float radius = 0.f;
};

// One pair contact as the source's 0xc029f0 / 0xc012d0 emits it. Both halves carry the
// ECollisionSurfaceType::POINT feature (1, written at 0xc0159c/0xc015a0) and both depth
// slots hold the same `depth`; there is no secondary normal, and the flag at +0x44 stays
// 0. Materials and physics ids are deliberately absent: the caller's to attach.
// Normals come from the START centres, never from the hit point.
struct Impact {
  Vec3 selfPoint;    // P0 = c0 + t*d + r0*n (overlap: c0 + r0*n)
  Vec3 otherPoint;   // P1 = c1 - r1*n
  Vec3 selfNormal;   // n = unit(c1 - c0)
  Vec3 otherNormal;  // component-wise negation of n, zeros included (per-lane fneg)
  float depth = 0.f; // (r0 + r1) - |c1 - c0|
};

// 0xc012d0's return codes.
constexpr int kMiss = 0;    // nothing touched: `distance` and `impacts` unchanged
constexpr int kOverlap = 1; // started overlapping: `impacts` cleared, `distance` = 0
constexpr int kSweep = 2;   // swept hit: `impacts` cleared, `distance` = the hit t

// 0xc012d0. `self` is swept along `unitDirection` against the stationary `other` for at
// most `distance` (in/out: the source's shared `t`, used verbatim as the ray length,
// with no epsilon added). `pairPasses` is the entry ICollisionFilter test 0xc01300 and
// the re-test 0xc029f0 runs; it is one caller-supplied decision, so it is collapsed to
// one bool and asked once. `impacts` is the caller's shared list: an accepted return
// REPLACES its contents (the source's inlined Clear() runs even when no contact is
// emitted - coincident centres), a rejected return leaves the list and `distance`
// bit-unchanged. It is the caller's CCollisionInfoList; material/id attachment and any
// pairing policy stay outside.
//
// Preconditions (as the source, no guard is added here): finite world centres,
// non-negative finite radii, a finite unit direction, a non-negative finite
// `distance`, and no overflow in the intermediate products. NaN and overflow
// behaviour is not claimed by this port.
int SphereSphereSwept(const Sphere& self, const Sphere& other, Vec3 unitDirection,
                      bool pairPasses, float& distance, std::vector< Impact >& impacts);

} // namespace PortRemasteredSphereSweep
