#pragma once
// The airborne mover's contact decisions that do not need the game: 0xc6d460's working-normal
// selection and 0xf948d4's per-contact separation gate, so they can be regression-tested.
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace PortAirMoverContact {

struct Vec3 {
  float x = 0.f, y = 0.f, z = 0.f;
};

// is_ceiling_normal (0xbecca0 / NCollisionUtils::is_ceiling_normal): up < -0.70710677.
inline bool IsCeilingNormal(Vec3 n) { return n.z < -0.70710677f; }

// The 0.866025 (0x3f5db3d0) up test of 0xc6d658-0xc6d67c.
inline bool UpExceeds(const Vec3& n) { return n.z > 0.866025f; }

// 0xc6d63c-0xc6d65c: the presence byte at contact+0x44 alone decides which normal the response
// uses. 0xc6d624-0xc6d630 copies the primary into the working slot; the presence path then stores
// the secondary over it, and neither 0xc6d69c nor 0xc6d6b4 restores the primary. A present but
// degenerate (zero) secondary stays the working normal: the callback never tests normalizability.
inline Vec3 WorkingNormal(Vec3 primary, bool hasSecondary, Vec3 secondary) {
  return hasSecondary ? secondary : primary;
}

// Only the mover's gate cleans primary components. The callback re-reads
// raw contact normals, and a present secondary bypasses this cleanup entirely.
inline Vec3 GatePrimary(Vec3 n) {
  constexpr float threshold = 2.3841858e-7f;
  return {std::fabs(n.x) < threshold ? 0.f : n.x,
          std::fabs(n.y) < threshold ? 0.f : n.y,
          std::fabs(n.z) < threshold ? 0.f : n.z};
}

// 0xc6d658-0xc6d698 -> 0xc6d6b4 (`mov w25,#1`) when no secondary is present, otherwise w25 = 1
// only while neither up exceeds 0.866025 and neither normal is a ceiling. w25 gates the
// kMS_ApplyJump gravity/force block at 0xc6d7f4 (`cbz w25`); it never changes the working normal.
inline bool W25(bool hasSecondary, Vec3 primary, Vec3 secondary) {
  if (!hasSecondary) {
    return true; // 0xc6d63c `cbz w8, 0xc6d6b4`
  }
  return !UpExceeds(primary) && !UpExceeds(secondary) && !IsCeilingNormal(primary) &&
         !IsCeilingNormal(secondary);
}

// 0xc6d5d8: flag100 bypasses the flat-floor early return entirely.
inline bool SkipFlatFloor(float primaryUp, float velocityUp, float actorUp,
                          float contactUp, bool flag100) {
  return !flag100 && std::fabs(1.f-primaryUp) < 1e-5f &&
         (velocityUp > 0.f || actorUp <= contactUp);
}

// 0xc6d6e8-0xc6d6ec: flagged contacts use ten times the ordinary push.
inline float PushDistance(bool flag100) { return flag100 ? .025f : .0025f; }

// 0xf94b30-50 saves the original target; 0xf94f34-0xf94fe4 subtracts
// current-minus-previous and rider DISPLACEMENT from target-minus-previous.
// The common previous-origin cancels; neither velocity nor accepted-lo motion
// is the target. `current` is the PRE-STEP snapshot: ApplyMotionState is deferred
// until 0xf95420, not performed before the response loop. Static terrain cannot
// be the actor's rider.
inline Vec3 RemainingDisplacement(Vec3 fullTarget, Vec3 current) {
  return {fullTarget.x-current.x,fullTarget.y-current.y,fullTarget.z-current.z};
}

// Full CheckCollision selects lo only if impacts were produced (0xf94d00).
// SAT hit alone does not select it; response rejection does not change time.
inline bool HasImpacts(std::size_t terrainCount, std::size_t dynamicCount) {
  return terrainCount != 0 || dynamicCount != 0;
}

// 0xf94ff0-0xf95028: dot(remaining displacement - riderOffset, working) > 0 skips.
inline float SeparationDot(Vec3 displacement, Vec3 riderOffset, Vec3 working) {
  const float dx = displacement.x - riderOffset.x;
  const float dy = displacement.y - riderOffset.y;
  const float dz = displacement.z - riderOffset.z;
  return (dx * working.x + dy * working.y) + dz * working.z;
}

inline bool Acts(Vec3 displacement, Vec3 riderOffset, Vec3 working) {
  return !(SeparationDot(displacement, riderOffset, working) > 0.f);
}

// 0xf948d4:354-362: the rider offset is subtracted only for a contact whose owner (+0x30 of
// its non-self side) is the mover's rider controller (+0x1210), and only while that is valid.
// NPhysicsUtils::get_rider_controller_offset 0xc18f80 returns R+0x1100 - R+0x1128 plus a
// rotation term that is zero when R's orientations +0x1118 and +0x1140 compare equal, so
// for an unrotated rider it is the same pose delta PredictMotion 0xc23e40 carries. Owners use
// the PortRemasteredGroundRider encoding: 0 is static terrain, never a rider.
inline Vec3 RiderOffsetFor(std::uint32_t contactOwner, std::uint32_t riderOwner, bool riderActive,
                           Vec3 riderOffset) {
  if (!riderActive || contactOwner == 0u || contactOwner != riderOwner) {
    return {};
  }
  return riderOffset;
}

// 0xc6d4e8-0xc6d5d4: the flat-floor early-out (0xc6d5d8) is reached only when the other
// object's collidable (+0x168, vt+0x28 = GetObjectType) is type 0 (CCollidableAABox 0xbf60bc)
// or type 2 (CCollidableOBBox 0xc10e48) with a transform axis within 1e-5 of up; every other
// type (sphere 1, AABox tree 3, OBBox tree 4 - CCollidableOBBoxTree 0xc0fb1c, the DCLN
// platforms - capsule 5) goes straight to 0xc6d624.
enum ECollidableType { kCollidableAABox = 0, kCollidableOBBox = 2, kCollidableOBBoxTree = 4 };
inline bool FlatFloorEarlyOutApplies(int collidableType, bool obboxAxisAlignedWithUp) {
  return collidableType == kCollidableAABox ||
         (collidableType == kCollidableOBBox && obboxAxisAlignedWithUp);
}

// 0xc6d110 GroundCheckForOffGroundMoverCollider's probe and landing band, along the up axis.
// The callback gets two motion states: `hiUp` (param_4) and `targetUp` (param_5, whose
// target transform it probes from). Its two callers in 0xf948d4 pass the bisection's blocked
// hi state and the accepted lo state for a floor contact, and for the post-step probe while
// rising the state moved 0.1 down and the state itself (calls at 0xf9524c and 0xf953f4).
// `stepDown` is the vtable +0x198 call at 0xc6d230: CPlayerMP1::GetStepDownHeight (slot 51;
// -1 in kMS_Jump, 0.1 in kMS_ApplyJump, else the actor's value), the same as the port's
// CPlayer::GetStepDownHeight. hiUp is read at 0xc6d238 (`ldr s1, [x21, #4]`, Y-up).
//  - fVar13 = targetUp - stepDown; probe up = min(max(fVar13, hiUp), targetUp - 0.002)
//    (both selects are `x - y < 0` compares);
//  - a contact point lands while `boxMin < pz` is false and `pz <= low` is false, `low`
//    starting at fVar13 - 0.002; the upper bound is the box minimum at the target, which
//    the caller supplies.
struct GroundBand {
  float probeUp = 0.f; // the box is tested with its translation moved to this height
  float low = 0.f;     // exclusive lower bound of the landing band
};

inline GroundBand GroundCheckBand(float targetUp, float stepDown, float hiUp) {
  const float base = targetUp - stepDown;
  const float reach = base - hiUp < 0.f ? hiUp : base;
  const float cap = targetUp - .002f;
  return {reach - cap < 0.f ? reach : cap, base - .002f};
}

} // namespace PortAirMoverContact
