#pragma once

#include <cmath>
#include <cstddef>
#include <limits>

// Pure decisions from CPhysicsGroundMoverColliderMP1. Keep these independent of
// the retail collision backend: adapting contacts must not change the decisions.
namespace PortRemasteredGround {

// TranslatePositiveMP1Material, 0xc59e04-0xc59e20: later stores win.
inline int Orientation(bool floor, bool wall, bool ceiling) {
  return floor ? 0 : wall ? 2 : ceiling ? 1 : -1;
}

// NCollisionUtils::is_floor, 0xbecb40. The subtraction/comparison is unsigned.
inline bool IsFloor(int orientation, float up) {
  return up > 0.85f || orientation == 0 ||
         (up > 0.70710677f && static_cast<unsigned>(orientation) - 1u > 1u);
}

// NCollisionUtils::is_wall, 0xbecbb0.
inline bool IsWall(int orientation, float up) {
  if (up > 0.85f) {
    return false;
  }
  return orientation == 2 ||
         (up >= -0.70710677f && up <= 0.70710677f &&
          static_cast<unsigned>(orientation) > 1u);
}

struct Probe {
  bool allowed;
  float offset;
};

// CanMoveUpAndDownFromPosition, 0xf928c0. Heights are world-space contact
// extrema and the candidate transform's AABox endpoints, NOT player origin.
// Caller must classify all contact points, including an optional secondary
// surface normal, before using this policy, then test the returned transform.
inline Probe StepProbe(float lower, float upper, float floorMax,
                       bool hasNonFloor, float nonFloorMin, float nonFloorMax,
                       float stepUp, float stepDown) {
  // Player SetupPhysicsObject (0xc5311c) supplies zero travel multipliers.
  // Do not substitute the state-dependent player step-height accessors here.
  if (hasNonFloor) {
    if (upper > nonFloorMin && upper < nonFloorMin + stepDown) {
      return {true, (nonFloorMin - upper) - 0.002f};
    }
    // 0xf928c0:153-161. Equality at the step-up limit is refused.
    if (floorMax + 1.e-5f <= nonFloorMax || floorMax <= lower ||
        lower <= floorMax - stepUp) {
      return {false, 0.f};
    }
    return {true, (floorMax - lower) + 0.002f};
  }
  // 0xf92b98-0xf92bb8: offset = (floorMax - lower) + 0.002, then the step-up
  // height wins when it is smaller (fcsel takes the offset only while
  // stepUp - offset >= 0). No floor altitude at all (an empty contact list) leaves
  // the huge negative offset the source leaves; callers reach this only with
  // contacts present.
  const float offset = (floorMax - lower) + 0.002f;
  return {true, stepUp < offset ? stepUp : offset};
}

// Intermediate contact view. The optional secondary surface normal is NOT
// the opposite body's normal. Retain it and the full contact height range.
// scalar14 is contact+0x14: keep this source field explicit until its producer
// contract is resolved, rather than replacing it with normal.z or actor kind.
struct Contact {
  int orientation;
  float normalX;
  float normalUp;
  float scalar14;
  bool flag100;
  bool hasSecondary;
  float secondaryUp;
  float minHeight;
  float maxHeight;
};

inline bool HasFloorNormal(const Contact& c) {
  return IsFloor(c.orientation, c.normalUp) ||
         (c.hasSecondary && IsFloor(c.orientation, c.secondaryUp));
}

inline Probe StepProbe(float lower, float upper, const Contact* contacts,
                       std::size_t count, float stepUp, float stepDown) {
  float floorMax = -std::numeric_limits<float>::max();
  float nonFloorMin = std::numeric_limits<float>::max();
  float nonFloorMax = -std::numeric_limits<float>::max();
  bool hasNonFloor = false;
  for (std::size_t i = 0; i < count; ++i) {
    const auto& c = contacts[i];
    if (HasFloorNormal(c)) {
      if (c.maxHeight > floorMax) {
        floorMax = c.maxHeight;
      }
    } else {
      hasNonFloor = true;
      if (c.minHeight < nonFloorMin) {
        nonFloorMin = c.minHeight;
      }
      if (c.maxHeight > nonFloorMax) {
        nonFloorMax = c.maxHeight;
      }
    }
  }
  return StepProbe(lower, upper, floorMax, hasNonFloor, nonFloorMin,
                   nonFloorMax, stepUp, stepDown);
}

// IsOnWallAtPosition, 0xf93c9c: caller first queries at candidate - stepDown.
// The result depends on the highest contact, not simply the presence of a wall.
inline bool OnWall(bool collision, const Contact* contacts, std::size_t count) {
  if (!collision) {
    return false;
  }
  float highest = -std::numeric_limits<float>::max();
  bool floorAtTop = false;
  for (std::size_t i = 0; i < count; ++i) {
    const auto& c = contacts[i];
    // 0xf93c9c:317-318. +0x14 is separate from the primary normal.
    if (std::fabs(std::fabs(c.normalX) - 1.f) < 1.e-5f &&
        std::fabs(c.scalar14) < 1.e-5f) {
      continue;
    }
    if (IsFloor(c.orientation, c.normalUp)) {
      const float height = c.maxHeight + 1.e-5f + 0.002f;
      floorAtTop = highest < height || floorAtTop;
      if (height > highest) {
        highest = height;
      }
    } else {
      floorAtTop = floorAtTop && c.maxHeight <= highest;
      if (c.maxHeight > highest) {
        highest = c.maxHeight;
      }
    }
  }
  return !floorAtTop;
}

// SnapToGround's three outcomes, from 0xf93a30-0xf93b04:
//   1. a floor was taken and offset < GetStepUpHeight: the snapped motion state is
//      applied and the object state set to 1 (0xf93a50-0xf93ae4);
//   2. no floor at the probe: the object state is set to 0 and the caller's motion
//      state applied (0xf93aec-0xf93b00, 0xf934ec:647-648);
//   3. refused: offset >= GetStepUpHeight branches to the epilogue (0xf93a4c
//      b.pl 0xf93b04), which touches nothing at all — the object keeps whatever
//      MoveAlongGround/the prediction left it with. Equality is refused.
enum class SnapResult { kRefused, kNoFloor, kGrounded };

struct Snap {
  SnapResult result = SnapResult::kNoFloor;
  float offset = 0.f;
  // Index of the contact the selection took (the port sends its material on a
  // landing; the source's SnapToGround itself only sets the object state).
  std::size_t contact = 0;
};

struct GroundSearch {
  float freeSample;
  float blockedSample;
  float consumed;
  bool anyFree;
};

// MoveAlongGround 0xf92ccc:163-213. The consumed time is NOT the time
// belonging to the last free snapshot: a successful sample adds half the
// current search interval before recording the consumed time.
// `free(t)` means (no collision OR step resolves) AND !IsOnWallAtPosition.
template <class Free>
GroundSearch SearchGroundTime(float dt, Free&& free) {
  float sample = dt * 0.5f;
  float interval = sample;
  GroundSearch result{0.f, sample, 0.f, false};
  while (dt * 0.2f < interval) {
    const float tested = sample;
    if (free(tested)) {
      sample += interval * 0.5f;
      result.freeSample = tested;
      result.consumed = sample;
      result.anyFree = true;
    } else {
      result.blockedSample = tested;
      sample -= interval * 0.5f;
    }
    interval *= 0.5f;
  }
  return result;
}

struct Vec3 {
  float x = 0.f, y = 0.f, z = 0.f;
};

// G3/R5 (corrected by ground/DISPATCH-AUDIT.md): the PLAYER's integrator is the local class
// with vptr 0x20e7340, not CPhysicsEulerIntegrator:
//  - Pre  0xc59814 (once at MoveObject entry): momentum += impulse         (v = momentum / m)
//  - Post 0xc5994c (AFTER the loop and the snap, BEFORE the transient clear and the state messages):
//    momentum += dt * (force + persistent)
// Every PredictMotion 0xc23e40 in between is READ-ONLY on the linear state: translation =
// t * (v + moveImpulse / m). So the tick moves with the pre-force velocity and the force
// raises the next tick's. persistentMomentum (gravity) is never modified; force, impulse and
// moveImpulse are transient and cleared by the caller at the tick epilogue.
// Angular integration is NOT covered here (the port's existing angular path is kept).
inline Vec3 PreMomentum(Vec3 momentum, Vec3 impulse) {
  return {momentum.x + impulse.x, momentum.y + impulse.y, momentum.z + impulse.z};
}
inline Vec3 PostMomentum(Vec3 momentum, Vec3 force, Vec3 persistent, float dt) {
  return {momentum.x + dt * (force.x + persistent.x), momentum.y + dt * (force.y + persistent.y),
          momentum.z + dt * (force.z + persistent.z)};
}

inline Vec3 PredictTranslation(float t, Vec3 velocity, Vec3 moveImpulse, float massRecip) {
  return {t * (velocity.x + massRecip * moveImpulse.x), t * (velocity.y + massRecip * moveImpulse.y),
          t * (velocity.z + massRecip * moveImpulse.z)};
}

// MoveAlongGround 0xf92ccc:225-423, which sample is applied and what follows.
//  - the blocked sample (0xf92ccc:229) is applied when it collides with nothing
//    (blocked only by IsOnWall/step refusal): no response, no impulses added;
//  - otherwise the FREE sample (t = 0 when none was free) takes the response;
//  - with no free sample the translation is the current one and 0 is returned
//    (0xf92ccc:409-417);
//  - impulses, angular impulse and the extra are cleared in every case.
struct GroundSample {
  bool useBlockedSample;
  bool applyResponse;
  bool keepCurrentPosition;
  float returnedTime;
};

inline GroundSample DecideGroundSample(bool anyFree, bool blockedCollides, float consumed) {
  return {!blockedCollides, blockedCollides, !anyFree, anyFree ? consumed : 0.f};
}

// One terrain contact as the response reads it: the normal already chosen
// (secondary when present), the material orientation and the own-side depth.
struct ResponseContact {
  float nx, ny, nz;
  int orientation;
  float depth;
};

struct GroundResponse {
  Vec3 velocityImpulse;
  Vec3 momentumless;
  bool wallAccepted = false;
  bool floorUsed = false;
};

// 0xf92ccc:305-401 wall pass, then the 0xf93300 floor pass when no wall was
// accepted. e = mu = 0 (verified: the player's state scalars are 0, FORCES.md R6).
// `accX/Y/Z` is the momentumless accumulator (+0xa4 wall, +0x118c floor, both
// read from the same port accumulator), `velocity` the state velocity after the entry integration (R5).
inline GroundResponse ResolveGroundResponse(const ResponseContact* contacts, std::size_t count,
                                            Vec3 velocity, Vec3 acc, float mass) {
  GroundResponse out;
  auto dot = [](float ax, float ay, float az, Vec3 v) { return ax * v.x + ay * v.y + az * v.z; };
  auto normalize = [](float& x, float& y, float& z) {
    const float m = std::sqrt(x * x + y * y + z * z);
    if (!(m > 1.1920929e-07f)) {
      return false;
    }
    x /= m;
    y /= m;
    z /= m;
    return true;
  };
  float priority = std::numeric_limits<float>::max();
  float bestMagSq = 0.f;
  for (std::size_t i = 0; i < count; ++i) {
    const ResponseContact& c = contacts[i];
    if (!IsWall(c.orientation, c.nz) && !(-0.996f < c.nz && c.nz < 0.f)) {
      continue;
    }
    float x = c.nx, y = c.ny, z = c.nz;
    if (c.nz > 0.f) {
      z = 0.f; // flattened into the horizontal plane
    }
    if (!normalize(x, y, z)) {
      continue;
    }
    const float vd = dot(x, y, z, velocity);
    Vec3 imp;
    if (vd <= 1.1920929e-07f) {
      imp = {-x * vd, -y * vd, -z * vd};
    }
    const float ad = dot(x, y, z, acc);
    out.momentumless = {x * mass * 0.15f, y * mass * 0.15f, z * mass * 0.15f};
    if (ad < 0.f) {
      out.momentumless.x += -x * ad;
      out.momentumless.y += -y * ad;
      out.momentumless.z += -z * ad;
    }
    const float mag = imp.x * imp.x + imp.y * imp.y + imp.z * imp.z;
    const bool zero = std::fabs(imp.x) < 0.0001f && std::fabs(imp.y) < 0.0001f &&
                      std::fabs(imp.z) < 0.0001f; // close_enough(imp, 0, 1e-4)
    if (zero || std::fabs(c.depth - priority) < 1.e-5f) {
      if (bestMagSq < mag) {
        out.velocityImpulse = imp;
        bestMagSq = mag;
      }
    } else {
      out.velocityImpulse = imp;
      bestMagSq = mag;
    }
    if (c.depth < priority) {
      priority = c.depth;
    }
    out.wallAccepted = true;
  }
  if (!out.wallAccepted) {
    // 0xf93300-0xf93378: the first floor-class contact only, no flatten, factor 0.
    for (std::size_t i = 0; i < count; ++i) {
      const ResponseContact& c = contacts[i];
      if (!IsFloor(c.orientation, c.nz)) {
        continue;
      }
      float x = c.nx, y = c.ny, z = c.nz;
      const float vd = dot(x, y, z, velocity);
      if (vd <= 1.1920929e-07f) {
        out.velocityImpulse = {-x * vd, -y * vd, -z * vd};
      }
      const float ad = dot(x, y, z, acc);
      if (ad < 0.f) {
        out.momentumless = {-x * ad, -y * ad, -z * ad};
      }
      out.floorUsed = true;
      break;
    }
  }
  return out;
}

// 0xf934ec:460-524. `blocked(d)` queries the unchanged candidate transform
// translated down by d. Retain the last known blocked distance; do not replace
// this ladder with a swept collision or the last sampled midpoint.
template <class Blocked>
bool SnapProbe(float reach, Blocked&& blocked, float& distance) {
  if (!blocked(reach)) {
    return false;
  }
  float high = reach;
  float half = reach * 0.5f;
  float sample = reach - half;
  const bool middleBlocked = blocked(sample);
  if (middleBlocked) {
    high = sample;
  }
  const float quarter = half * 0.5f;
  sample += middleBlocked ? -quarter : quarter;
  if (blocked(sample)) {
    high = sample;
    sample -= quarter * 0.5f;
  } else {
    sample += quarter * 0.5f;
  }
  if (blocked(sample)) {
    high = sample;
  }
  distance = high;
  return true;
}

// SnapToGround, 0xf934ec:537-642. Caller supplies contacts from the exact
// downward probe ladder. Selection uses biased heights but placement does not.
inline Snap SelectSnap(float lower, float stepUp, const Contact* contacts,
                       std::size_t count) {
  float rank = -std::numeric_limits<float>::max();
  float target = rank;
  bool floorAtTop = false;
  std::size_t taken = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const auto& c = contacts[i];
    // +0xc is normal.y, +0x14 is NOT normal.z. See raw contact layout.
    if (std::fabs(c.normalUp) < 1.e-5f && std::fabs(c.scalar14) < 1.e-5f) {
      continue;
    }
    if (HasFloorNormal(c)) {
      const float height = c.maxHeight + 0.0040100003f;
      if (rank < height) {
        target = c.maxHeight + 0.002f;
        floorAtTop = !c.flag100 || floorAtTop;
        rank = height;
        taken = i;
      }
    } else if (rank < c.maxHeight) {
      target = c.maxHeight + 0.002f;
      floorAtTop = c.flag100 && floorAtTop;
      rank = c.maxHeight;
      taken = i;
    }
  }
  if (floorAtTop) {
    const float offset = target - lower;
    // Equality is explicitly refused and applies nothing (0xf934ec:621-623,
    // 0xf93a4c -> 0xf93b04).
    if (offset < stepUp) {
      return {SnapResult::kGrounded, offset, taken};
    }
    return {SnapResult::kRefused, 0.f, taken};
  }
  // No floor contact at the probe: the caller's motion state is applied unchanged.
  return {SnapResult::kNoFloor, 0.f, taken};
}

// The persisted Remastered physics object state (CPhysicsState+0x13c, `EObjectState`): the
// ONLY quantity PostPhysicsUpdate 0xc545bc derives the ground/air messages from, and the
// only mover-written value an actor keeps across ticks (ENTRY-STATE.md §A, MOVER-SELECTION.md
// §2). `CPhysicsState::CPhysicsState` 0xc14f7c initialises it ONCE, to 0 (0xc151d4, the
// caller CPhysicsObject 0xc19f50 stores wzr): every physics object starts at "no floor", and
// nothing in the load or save path ever writes it. Written only by mover outcomes:
// ground SnapToGround 0xf93ae4 (1) / 0xf93af4 (0), refused 0xf93b04 writes NOTHING; stepped
// MoveObject 0xf9450c writes 0 at entry, DoMovementStep 0xf95414 raises it to 1 on a grounded
// step; the ball's swept mover 0xc261a8 (1, with a rider) / 0xc26234 (0 or 1 by the pinned
// entry/exit vertical travel of RETAINED-STATE-SEAM.md §1). Morph / unmorph / grapple /
// teleport only touch the material bit and the mover: they must never re-derive it.
//
// The port needs one extra bit the source does not have: validity. The native movers are not
// all source movers (the morph ball is the retail standard collider, the ground mover steps
// aside for dynamic contacts, a save state is rebuilt), so there are ticks whose source value
// the port cannot produce. Those INVALIDATE the value, and the consumers treat it as absent
// (retail mover, no writes, no message from it) until a verified airborne tick writes 0 again.
enum class RetainedState : unsigned char {
  kNoFloor = 0,   // source 0: "no floor"
  kGrounded = 1,  // source 1: "has floor"
  kInvalid = 2,   // no source-retained result exists
};
struct RetainedResult {
  RetainedState state = RetainedState::kNoFloor; // source ctor value 0
  bool valid = true;                             // construction/default: 0 and valid
};
// 1 exactly when the value is trustworthy AND grounded. Everything that reads the value as a
// boolean (the refused-snap resend) goes through this.
inline bool RetainedIsGrounded(const RetainedResult& r) {
  return r.valid && r.state == RetainedState::kGrounded;
}
// Whether the Remastered grounded mover may run at all on this tick. False: the caller must
// take the port's retail grounded mover and leave the value invalid (no guessing).
inline bool RetainedAllowsGroundMover(const RetainedResult& r) { return r.valid; }
inline RetainedResult RetainedInvalidate() { return {RetainedState::kInvalid, false}; }
// The tick's mover gate: the condition CGameCollision::MovePlayer's grounded and airborne
// Remastered dispatch shares, field for field, sampled once at the top of the tick. Every
// field is a live port getter, so the gate is not a second copy of a source formula:
//  - `unmorphed`: CPlayer::GetMorphballTransitionState() == CPlayer::kMS_Unmorphed. A morph
//    ball is outside the gate by construction (it is the retail standard collider);
//  - `mouseControlsAllowed`: CPlayer::MouseControlsAllowed(mgr), the port's pointer/twin-stick
//    eligibility term the dispatch already required;
//  - `remasteredMovement`: the remastered_movement setting (PortDebug::RemasteredMovement()).
struct MoverGate {
  bool unmorphed = false;
  bool mouseControlsAllowed = false;
  bool remasteredMovement = false;
};
inline bool UsesRemasteredMover(const MoverGate& gate) {
  return gate.unmorphed && gate.mouseControlsAllowed && gate.remasteredMovement;
}
// The retained value a tick's entry leaves behind. The movers that can write the value are
// exactly the ticks UsesRemasteredMover selects, so a tick outside the gate runs a retail
// mover and cannot vouch for the value: it is invalidated instead of being carried over, and
// stays invalid across any number of such ticks and of retail ground outcomes. A tick inside
// the gate passes the value through untouched (its writes happen later, and a refused snap
// writes nothing). Classic movement (rm0) never reads the value, so there the invalidation
// only keeps the tracked value honest.
inline RetainedResult RetainedEntryForTick(const MoverGate& gate, const RetainedResult& current) {
  return UsesRemasteredMover(gate) ? current : RetainedInvalidate();
}
// CPhysicsSteppedMoverColliderMP1::MoveObject 0xf9450c: entry writes 0 unconditionally, so a
// verified airborne tick (inside the gate) is what restores validity after any invalidating
// path. No ground or retail outcome can: RetainedGroundOutcome passes an invalid value on.
inline RetainedResult RetainedAirborneEntry(const RetainedResult&) {
  return {RetainedState::kNoFloor, true};
}
// The airborne mover's landing outcome: 1 exactly when that tick actually sent kSM_OnFloor
// (the port's airborne landing message, CGameCollision::SendScriptMessages), and the entry's
// 0/valid otherwise, so an ordinary airborne tick stays at 0.
inline RetainedResult RetainedAirborneLanding(const RetainedResult&, bool onFloorSent) {
  return {onFloorSent ? RetainedState::kGrounded : RetainedState::kNoFloor, true};
}
// SnapToGround's write: grounded 1, no floor 0, refused writes nothing. An invalid value is
// never written (the ground mover is not entered with one).
inline RetainedResult RetainedGroundOutcome(const RetainedResult& r, SnapResult snap) {
  if (!r.valid) {
    return r;
  }
  switch (snap) {
  case SnapResult::kGrounded:
    return {RetainedState::kGrounded, true};
  case SnapResult::kNoFloor:
    return {RetainedState::kNoFloor, true};
  case SnapResult::kRefused:
    return r;
  }
  return r;
}

// G6/G6b: the ground-to-air state messages of PostPhysicsUpdate 0xc545bc, which the source
// derives from the persisted object state (set by SnapToGround: grounded 1, no floor 0,
// refused untouched) every tick. `retainedAtEntry` is that value, retained by CPlayer across
// ticks (see RetainedResult). It replaces the earlier proxy (the native kMT_GroundCollider
// material, ENTRY-STATE.md §D: identically true inside this mover, so the refused/air case was
// unreachable). Never inferred from velocity; invalid never reaches here (the caller falls
// back to the retail mover first).
struct StateActions {
  bool onFloor = false;           // kSM_OnFloor + surface material + last floor normal
  bool falling = false;           // kSM_Falling
  bool removeGroundCollider = false; // RemoveMaterial(kMT_GroundCollider): airborne mover next tick
  bool clearFloorNormal = false;
  bool resendOnFloor = false;     // kSM_OnFloor only (no material/normal update)
  bool moveActor = true;          // false: the snap applied nothing
};
inline StateActions DecideStateActions(SnapResult snap, const RetainedResult& retainedAtEntry) {
  StateActions a;
  switch (snap) {
  case SnapResult::kGrounded:
    a.onFloor = true;
    break;
  case SnapResult::kNoFloor:
    a.falling = a.removeGroundCollider = a.clearFloorNormal = true;
    break;
  case SnapResult::kRefused:
    // Nothing is written, so the previous state's message is sent again.
    a.moveActor = false;
    if (RetainedIsGrounded(retainedAtEntry)) {
      a.onFloor = false;
      a.resendOnFloor = true;
    } else {
      a.falling = a.removeGroundCollider = true;
    }
    break;
  }
  return a;
}

// G2/D1/D2/D3: the two LOCAL values 0xf923d8 carries into SnapToGround, both ABSOLUTE
// (PredictMotion 0xc23e40 / ApplyMotionState 0xc165c0 are absolute):
//  - `target`: the loop transform x29-0xe8. Written at the loop top from the prediction
//    (0xf92594-0xf925a8), raised by CanMoveUpAndDown, and overwritten by MoveAlongGround's
//    param_6 on its colliding path (0xf92ffc-0xf9301c: the free sample's target, which IS
//    the position the pass applied to the actor);
//  - `motion`: the WHOLE saved loop-top motion state x29-0x88 (translation, orientation,
//    linear momentum, angular momentum). MoveAlongGround never writes it, so the passes'
//    responses (velocity) and their applied orientation steps do not reach it; only a
//    resolved DoCollisionFailsafe replaces its translation.
// SnapToGround applies `motion` whole on the no-floor path (0xf93afc), every field but the
// translation on the grounded path (0xf93a70-0xf93aa4; the translation is the snapped
// target), and nothing when refused. The actor is moved only there.
struct Quat4 {
  float w, x, y, z;
};
// The native CMotionState layout: `momentum` is CMotionState::GetVelocity(), which
// CPhysicsActor stores as xfc (mass * velocity; SetMotionState/ComputeDerivedQuantities).
// The orientation is kept unnormalized; CPhysicsActor::SetMotionState normalizes it
// (CQuaternion::FromNUQuaternion), as AddMotionState does after its addition.
struct MotionSnapshot {
  Vec3 translation;
  Quat4 orientation;
  Vec3 momentum;
  Vec3 angularMomentum;
};
struct GroundFrame {
  Vec3 target;
  MotionSnapshot motion;
};
inline Vec3 AddVec(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 SubVec(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

// Loop top: the absolute prediction = the actor's current motion state + the predicted
// delta, field by field as AddMotionState would add it (orientation summed unnormalized).
// Both locals start there; the delta's linear momentum is 0 (Predict is read-only on it).
inline GroundFrame FrameAtLoopTop(const MotionSnapshot& current, const MotionSnapshot& delta) {
  MotionSnapshot abs;
  abs.translation = AddVec(current.translation, delta.translation);
  abs.orientation = {current.orientation.w + delta.orientation.w,
                     current.orientation.x + delta.orientation.x,
                     current.orientation.y + delta.orientation.y,
                     current.orientation.z + delta.orientation.z};
  abs.momentum = AddVec(current.momentum, delta.momentum);
  abs.angularMomentum = AddVec(current.angularMomentum, delta.angularMomentum);
  return {abs.translation, abs};
}
// CanMoveUpAndDown resolved: the target is raised, the saved motion state is not.
inline void FrameStepResolved(GroundFrame& f, const Vec3& raise) { f.target = AddVec(f.target, raise); }
// After a MoveAlongGround pass: only the colliding path (the blocked sample collided,
// DecideGroundSample().applyResponse) writes param_6, and then with the position the
// pass applied to the actor (the current position when no sample was free), so the
// target delta from the moved actor is zero. A pass whose blocked sample collides with
// nothing leaves the target untouched.
inline void FrameAfterPass(GroundFrame& f, bool passWroteTarget, const Vec3& actorAfter) {
  if (passWroteTarget) f.target = actorAfter;
}
// DoCollisionFailsafe resolved (0xf926ec-0xf92718): only the local snap transform and the
// saved motion state's translation take `out`; the actor stays where it is.
struct FailsafeApply {
  bool resolved = false;
  Vec3 out;
};
template <class Run>
inline FailsafeApply ApplyGroundFailsafe(const Vec3& actorTranslation, const GroundFrame& f,
                                         Run&& run) {
  Vec3 out;
  if (run(f.target, actorTranslation, out)) return {true, out};
  return {};
}
inline void FrameFailsafeResolved(GroundFrame& f, const Vec3& out) {
  f.target = out;
  f.motion.translation = out;
}
// The actor's final ABSOLUTE motion state by the SnapToGround outcome: grounded = the saved
// state with the snapped target as translation, no floor = the saved state whole, refused =
// nothing (the post-pass state stays, also without the failsafe push).
// Grounded also rewrites the saved velocity (0xf93ab0-0xf93ad4): horizontal *= the state's
// base ground velocity damp factor (S+0x11f4), vertical = 0. SetupPhysicsObject 0xc53bc8
// sets that factor to 1.0 for every CPhysicsActorMP1, so for the player only the vertical
// component is zeroed (Remastered Y-up = native Z; momentum m*v, so zeroing it is the same).
struct SnapApply {
  bool moveActor = false;
  MotionSnapshot state;
};
inline SnapApply ApplySnapOutcome(SnapResult r, const GroundFrame& f, const Vec3& snapRaise) {
  SnapApply a;
  switch (r) {
  case SnapResult::kGrounded:
    a.moveActor = true;
    a.state = f.motion;
    a.state.translation = AddVec(f.target, snapRaise);
    a.state.momentum.z = 0.f;
    break;
  case SnapResult::kNoFloor:
    a.moveActor = true;
    a.state = f.motion;
    break;
  case SnapResult::kRefused:
    break;
  }
  return a;
}

} // namespace PortRemasteredGround
