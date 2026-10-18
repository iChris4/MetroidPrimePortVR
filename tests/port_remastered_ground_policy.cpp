#include "port_mouse.h"
#include "port_remastered_ground_policy.h"
#include "port_test.h"

#include <cmath>
#include <initializer_list>
#include <limits>

using namespace PortRemasteredGround;

// A motion state at `p` with identity orientation and no momentum; a translation-only delta.
static MotionSnapshot At(const Vec3& p) { return {p, {1.f, 0.f, 0.f, 0.f}, {}, {}}; }
static GroundFrame Top(const Vec3& actor, const Vec3& delta) {
  return FrameAtLoopTop(At(actor), MotionSnapshot{delta, {0.f, 0.f, 0.f, 0.f}, {}, {}});
}
static bool Same(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
static bool Same(const Quat4& a, const Quat4& b) {
  return a.w == b.w && a.x == b.x && a.y == b.y && a.z == b.z;
}

int main() {
  REQUIRE(Orientation(false, false, false) == -1);
  REQUIRE(Orientation(false, false, true) == 1);
  REQUIRE(Orientation(false, true, true) == 2);
  REQUIRE(Orientation(true, true, true) == 0);
  // Explicit material orientation and normal-only fallback are distinct.
  REQUIRE(IsFloor(0, -1.f));
  REQUIRE(!IsFloor(1, 0.8f));
  REQUIRE(!IsFloor(2, 0.8f));
  REQUIRE(IsFloor(-1, 0.8f));
  REQUIRE(!IsFloor(-1, 0.70710677f));
  REQUIRE(IsFloor(1, 0.851f));
  REQUIRE(!IsFloor(1, 0.85f));
  REQUIRE(IsWall(2, -1.f));
  REQUIRE(IsWall(2, 0.85f));
  REQUIRE(!IsWall(2, 0.851f));
  REQUIRE(!IsWall(0, 0.f));
  REQUIRE(!IsWall(1, 0.f));
  REQUIRE(IsWall(-1, -0.70710677f));
  REQUIRE(IsWall(-1, 0.70710677f));
  REQUIRE(!IsWall(-1, 0.708f));

  // Shipped player step-up/down: 1 and 0.8, respectively.
  auto p = StepProbe(0.f, 2.f, 0.5f, true, 0.f, 0.2f, 1.f, 0.8f);
  REQUIRE(p.allowed && std::fabs(p.offset - 0.502f) < 1.e-6f);
  REQUIRE(!StepProbe(0.f, 2.f, 1.f, true, 0.f, 0.2f, 1.f, 0.8f).allowed);
  REQUIRE(!StepProbe(0.f, 2.f, 0.f, true, 0.f, -1.f, 1.f, 0.8f).allowed);
  REQUIRE(!StepProbe(0.f, 2.f, 0.5f, true, 0.f, 0.6f, 1.f, 0.8f).allowed);
  // Bounds endpoint, not origin: underside clearance chooses a downward probe.
  p = StepProbe(0.f, 2.f, 0.5f, true, 1.5f, 3.f, 1.f, 0.8f);
  REQUIRE(p.allowed && std::fabs(p.offset + 0.502f) < 1.e-6f);
  // No non-floor contact: the offset raises the box bottom to 0.002 above the
  // highest floor point, and the step-up height is the cap (0xf92bb0-0xf92bb8).
  p = StepProbe(0.f, 2.f, 0.f, false, 0.f, 0.f, 1.f, 0.8f);
  REQUIRE(p.allowed && std::fabs(p.offset - 0.002f) < 1.e-6f);
  p = StepProbe(0.f, 2.f, 1.5f, false, 0.f, 0.f, 1.f, 0.8f);
  REQUIRE(p.allowed && std::fabs(p.offset - 1.f) < 1.e-6f);
  p = StepProbe(0.f, 2.f, -std::numeric_limits<float>::max(), false,
                0.f, 0.f, 1.f, 0.8f);
  REQUIRE(p.allowed && p.offset < -1.e30f);

  Contact floor = {0, 0.f, 1.f, 0.f, false, false, 0.f, 0.f, 0.f};
  Contact wall = {2, 0.f, 0.f, 0.1f, false, false, 0.f, 0.f, 0.5f};
  Contact contacts[] = {floor, wall};
  REQUIRE(!OnWall(false, contacts, 2));
  REQUIRE(OnWall(true, contacts, 2));
  REQUIRE(!OnWall(true, &floor, 1));
  // Height priority is independent of contact ordering when heights differ.
  contacts[1].maxHeight = -0.1f;
  REQUIRE(!OnWall(true, contacts, 2));
  Contact reversed[] = {contacts[1], floor};
  REQUIRE(!OnWall(true, reversed, 2));
  Contact axisWall = wall;
  axisWall.normalX = 1.f;
  axisWall.scalar14 = 0.f;
  contacts[1] = axisWall;
  REQUIRE(!OnWall(true, contacts, 2));
  axisWall.scalar14 = 0.1f;
  contacts[1] = axisWall;
  REQUIRE(OnWall(true, contacts, 2));

  auto snap = SelectSnap(0.f, 1.f, &floor, 1);
  REQUIRE(snap.result == SnapResult::kGrounded && std::fabs(snap.offset - 0.002f) < 1.e-6f);
  Contact flaggedFloor = floor;
  flaggedFloor.flag100 = true;
  // A floor with behavior bit 0x100 cleared leaves floorAtTop false: no floor.
  REQUIRE(SelectSnap(0.f, 1.f, &flaggedFloor, 1).result == SnapResult::kNoFloor);
  contacts[0] = floor;
  contacts[1] = wall;
  REQUIRE(SelectSnap(0.f, 1.f, contacts, 2).result == SnapResult::kNoFloor);
  contacts[1].maxHeight = -0.1f;
  REQUIRE(SelectSnap(0.f, 1.f, contacts, 2).result == SnapResult::kGrounded);
  // Bias is for selecting the surface, not positioning the actor.
  contacts[1].maxHeight = 0.003f;
  snap = SelectSnap(0.f, 1.f, contacts, 2);
  REQUIRE(snap.result == SnapResult::kGrounded && std::fabs(snap.offset - 0.002f) < 1.e-6f);
  floor.minHeight = floor.maxHeight = 1.f;
  // offset >= stepUp (equality included) is the refused outcome: the source jumps
  // to its epilogue and applies nothing at all (0xf93a4c -> 0xf93b04).
  REQUIRE(SelectSnap(0.002f, 1.f, &floor, 1).result == SnapResult::kRefused);
  REQUIRE(SelectSnap(0.f, 0.9f, &floor, 1).result == SnapResult::kRefused);
  REQUIRE(SelectSnap(0.f, 1.1f, &floor, 1).result == SnapResult::kGrounded);
  REQUIRE(SelectSnap(0.f, 1.f, nullptr, 0).result == SnapResult::kNoFloor);

  // A secondary normal can make an otherwise wall-like contact a floor.
  Contact edge = wall;
  edge.orientation = -1;
  edge.hasSecondary = true;
  edge.secondaryUp = 1.f;
  edge.minHeight = edge.maxHeight = 0.4f;
  REQUIRE(HasFloorNormal(edge));
  snap = SelectSnap(0.f, 1.f, &edge, 1);
  REQUIRE(snap.result == SnapResult::kGrounded && std::fabs(snap.offset - 0.402f) < 1.e-6f);
  edge.hasSecondary = false;
  REQUIRE(!HasFloorNormal(edge));
  REQUIRE(SelectSnap(0.f, 1.f, &edge, 1).result == SnapResult::kNoFloor);
  floor.minHeight = floor.maxHeight = 0.6f;
  contacts[0] = floor;
  contacts[1] = wall;
  p = StepProbe(0.f, 2.f, contacts, 2, 1.f, 0.8f);
  REQUIRE(p.allowed && std::fabs(p.offset - 0.602f) < 1.e-6f);

  float samples[4] = {};
  int calls = 0;
  float distance = -1.f;
  // Threshold 0.5: full blocked, half free, three-quarter blocked,
  // five-eighth blocked => keep five-eighth.
  REQUIRE(SnapProbe(0.8f, [&](float d) {
     samples[calls++] = d;
     return d >= 0.5f;
   }, distance));
  REQUIRE(calls == 4);
  REQUIRE(samples[0] == 0.8f && samples[1] == 0.4f);
  REQUIRE(std::fabs(samples[2] - 0.6f) < 1.e-6f);
  REQUIRE(std::fabs(samples[3] - 0.5f) < 1.e-6f);
  REQUIRE(std::fabs(distance - 0.5f) < 1.e-6f);
  calls = 0;
  REQUIRE(!SnapProbe(0.8f, [&](float) { ++calls; return false; }, distance));
  REQUIRE(calls == 1);
  calls = 0;
  REQUIRE(SnapProbe(0.8f, [&](float d) { ++calls; return d >= 0.7f; }, distance));
  REQUIRE(calls == 4 && std::fabs(distance - 0.7f) < 1.e-6f);
  // A final free sample must not replace the previous blocked candidate.
  REQUIRE(SnapProbe(0.8f, [](float d) { return d >= 0.71f; }, distance));
  REQUIRE(distance == 0.8f);

  calls = 0;
  auto search = SearchGroundTime(1.f, [&](float t) {
    samples[calls++] = t;
    return t < 0.7f;
  });
  REQUIRE(calls == 2 && samples[0] == 0.5f && samples[1] == 0.75f);
  REQUIRE(search.anyFree && search.freeSample == 0.5f);
  REQUIRE(search.blockedSample == 0.75f && search.consumed == 0.75f);
  search = SearchGroundTime(1.f, [](float) { return false; });
  REQUIRE(!search.anyFree && search.freeSample == 0.f);
  REQUIRE(search.blockedSample == 0.25f && search.consumed == 0.f);
  search = SearchGroundTime(1.f, [](float) { return true; });
  REQUIRE(search.anyFree && search.freeSample == 0.75f);
  REQUIRE(search.blockedSample == 0.5f && search.consumed == 0.875f);
  search = SearchGroundTime(1.f, [](float t) { return t < 0.4f; });
  REQUIRE(search.anyFree && search.freeSample == 0.25f);
  REQUIRE(search.blockedSample == 0.5f && search.consumed == 0.375f);

  // R1/R4: no free sample still responds (current position, return 0).
  auto pick = DecideGroundSample(false, true, 0.f);
  REQUIRE(!pick.useBlockedSample && pick.applyResponse && pick.keepCurrentPosition &&
          pick.returnedTime == 0.f);
  const ResponseContact rwall{1.f, 0.f, 0.f, 2, 0.f};
  auto r = ResolveGroundResponse(&rwall, 1, {-3.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 100.f);
  REQUIRE(r.wallAccepted && r.velocityImpulse.x == 3.f && std::fabs(r.momentumless.x - 15.f) < 1e-4f);
  // R2: a blocked sample that collides with nothing is applied, no response.
  pick = DecideGroundSample(true, false, 0.75f);
  REQUIRE(pick.useBlockedSample && !pick.applyResponse && !pick.keepCurrentPosition &&
          pick.returnedTime == 0.75f);
  pick = DecideGroundSample(false, false, 0.f);
  REQUIRE(pick.useBlockedSample && pick.keepCurrentPosition && pick.returnedTime == 0.f);
  // R3: floor-only contacts, first floor only, factor 0.
  const ResponseContact rfloors[2] = {{0.f, 0.f, 1.f, -1, 0.f}, {0.f, 0.6f, 0.8f, 0, 0.f}};
  r = ResolveGroundResponse(rfloors, 2, {0.f, 0.f, -2.f}, {0.f, 0.f, -5.f}, 100.f);
  REQUIRE(!r.wallAccepted && r.floorUsed);
  REQUIRE(std::fabs(r.velocityImpulse.z - 2.f) < 1e-5f && r.velocityImpulse.y == 0.f);
  REQUIRE(std::fabs(r.momentumless.z - 5.f) < 1e-5f);
  // R4: nothing qualifies -> no response, still a response record the caller clears after.
  const ResponseContact rceil{0.f, 0.f, -1.f, 1, 0.f};
  r = ResolveGroundResponse(&rceil, 1, {0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}, 100.f);
  REQUIRE(!r.wallAccepted && !r.floorUsed && r.velocityImpulse.z == 0.f);

  // G3/R5 (dispatch audit): Pre = impulse only; Post = dt*(force+persistent) after the move.
  {
    const float dt = 1.f / 60.f, m = 100.f, g = -980.f;
    const Vec3 persistent{0.f, 0.f, g}, zero{};
    // Pre never adds gravity or force.
    Vec3 mom = PreMomentum(zero, zero);
    REQUIRE(mom.z == 0.f);
    // Predictions are read-only: three samples sum to dt*v, momentum unchanged.
    const Vec3 v{0.f, 0.f, 3.f};
    float sum = 0.f;
    for (int k = 0; k < 3; ++k) sum += PredictTranslation(dt / 3.f, v, zero, 1.f / m).z;
    REQUIRE(std::fabs(sum - dt * v.z) < 1e-7f);
    // Impulse enters once at Pre; a cleared impulse adds nothing on the next tick.
    Vec3 m2 = PreMomentum(zero, {0.f, 0.f, 50.f});
    REQUIRE(m2.z == 50.f);
    m2 = PreMomentum(m2, zero);
    REQUIRE(m2.z == 50.f);
    // Post: force and persistent once each, after the move.
    const Vec3 f{10.f, 0.f, 0.f};
    Vec3 p = PostMomentum(zero, f, persistent, dt);
    REQUIRE(std::fabs(p.x - dt * 10.f) < 1e-6f && std::fabs(p.z - dt * g) < 1e-5f);
    // Move impulse enters translation only, never the momentum.
    const Vec3 jm{6.f, 0.f, 0.f};
    float tx = 0.f;
    for (int k = 0; k < 3; ++k) tx += PredictTranslation(dt / 3.f, zero, jm, 1.f / m).x;
    REQUIRE(std::fabs(tx - dt * 6.f / m) < 1e-7f);
  }
  // Full force-law recurrence with the production AxisForce (tweak defaults: max 16.5,
  // friction 1.0, mass 200, accel 35000): drag, force on the damped speed, move at the
  // pre-force damped speed, Post folds the force. Steady position speed = max - A = 10.842857,
  // NOT 11.842857 (which a Pre-folded force would give).
  {
    const float dt = 1.f / 60.f, mass = 200.f, maxSpeed = 16.5f, fric = 1.f, accel = 35000.f;
    Vec3 momentum{};
    float step = 0.f;
    for (int t = 0; t < 1200; ++t) {
      const float damped = momentum.y / mass - fric * dt * 60.f;
      momentum.y = damped * mass;
      const float force = PortMouse::AxisForce(1.f, damped, maxSpeed, fric, mass, dt, accel);
      momentum = PreMomentum(momentum, {});
      step = PredictTranslation(dt, {0.f, momentum.y / mass, 0.f}, {}, 1.f / mass).y / dt;
      momentum = PostMomentum(momentum, {0.f, force, 0.f}, {}, dt);
    }
    REQUIRE(std::fabs(step - 10.842857f) < 0.002f);
    REQUIRE(std::fabs(step - 11.842857f) > 0.5f);
  }
  // G2: the failsafe seam the mover calls on every loop exit, before the snap.
  {
    const Vec3 actor{10.f, 20.f, 30.f}, delta{1.f, 2.f, -3.f};
    Vec3 seenFrom, seenState;
    int calls = 0;
    GroundFrame fr = Top(actor, delta);
    FailsafeApply a = ApplyGroundFailsafe(actor, fr, [&](const Vec3& f, const Vec3& st, Vec3&) {
      ++calls;
      seenFrom = f;
      seenState = st;
      return false;
    });
    REQUIRE(calls == 1 && !a.resolved);
    REQUIRE(seenFrom.x == 11.f && seenFrom.y == 22.f && seenFrom.z == 27.f); // target
    REQUIRE(seenState.x == 10.f && seenState.y == 20.f && seenState.z == 30.f); // object state
    // Resolved: only target + saved motion translation take `out`; actor is not an output.
    a = ApplyGroundFailsafe(actor, fr, [&](const Vec3&, const Vec3&, Vec3& out) {
      out = {5.f, 6.f, 7.f};
      return true;
    });
    REQUIRE(a.resolved && a.out.x == 5.f && a.out.z == 7.f);
    FrameFailsafeResolved(fr, a.out);
    REQUIRE(fr.target.z == 7.f && fr.motion.translation.z == 7.f);
    // Zero delta still runs it, from the actor's own spot.
    calls = 0;
    GroundFrame z = Top(actor, Vec3{});
    ApplyGroundFailsafe(actor, z, [&](const Vec3& f, const Vec3&, Vec3&) {
      ++calls;
      REQUIRE(f.x == 10.f && f.z == 30.f);
      return false;
    });
    REQUIRE(calls == 1);
  }
  // D1/D2: the mover's shared frame sequence (the same helpers MoveGroundedRemastered calls).
  {
    const Vec3 actor{10.f, 20.f, 30.f};
    const Vec3 up{0.f, 0.f, 1.f};
    // Nonzero-time collision, then a loop-limit exit: the pass moved the actor to the free
    // sample and wrote the target there, so the exit delta is 0 (not the old stale delta).
    GroundFrame f = Top(actor, {4.f, 0.f, 0.f});
    const Vec3 moved{12.f, 20.f, 30.f};
    FrameAfterPass(f, true, moved);
    REQUIRE(f.target.x == 12.f && f.motion.translation.x == 14.f); // motion keeps the loop-top value
    REQUIRE(SubVec(f.target, moved).x == 0.f);
    // Zero-time collision: actor stays, target = actor.
    f = Top(actor, {4.f, 0.f, 0.f});
    FrameAfterPass(f, true, actor);
    REQUIRE(f.target.x == 10.f && f.motion.translation.x == 14.f);
    // No-free / non-colliding blocked sample: param_6 untouched, absolute target kept even
    // though the actor moved.
    f = Top(actor, {4.f, 0.f, 0.f});
    FrameAfterPass(f, false, moved);
    REQUIRE(f.target.x == 14.f && f.motion.translation.x == 14.f);
    // Step resolved raises only the target.
    FrameStepResolved(f, {0.f, 0.f, 0.5f});
    REQUIRE(f.target.z == 30.5f && f.motion.translation.z == 30.f);
    // Failsafe-resolved + each snap outcome.
    f = Top(actor, {4.f, 0.f, 0.f});
    FrameAfterPass(f, true, moved);
    FrameFailsafeResolved(f, {50.f, 60.f, 70.f});
    SnapApply g = ApplySnapOutcome(SnapResult::kGrounded, f, {0.f, 0.f, 0.25f});
    REQUIRE(g.moveActor && g.state.translation.x == 50.f && g.state.translation.z == 70.25f);
    SnapApply n = ApplySnapOutcome(SnapResult::kNoFloor, f, up);
    REQUIRE(n.moveActor && n.state.translation.x == 50.f && n.state.translation.z == 70.f);
    SnapApply r = ApplySnapOutcome(SnapResult::kRefused, f, up);
    REQUIRE(!r.moveActor); // no translation, no failsafe push
    // No-floor without a failsafe applies the loop-top motion, grounded the pass target.
    f = Top(actor, {4.f, 0.f, 0.f});
    FrameAfterPass(f, true, moved);
    n = ApplySnapOutcome(SnapResult::kNoFloor, f, up);
    g = ApplySnapOutcome(SnapResult::kGrounded, f, {});
    REQUIRE(n.state.translation.x == 14.f && g.state.translation.x == 12.f);
  }
  // D3: the WHOLE saved loop-top state, through the same sequence MoveGroundedRemastered runs:
  // save at the loop top, a colliding pass whose wall response changes the momentum and which
  // applies its own orientation/angular step to the actor, failsafe, then the snap outcome.
  {
    const float m = 2.f;
    MotionSnapshot actor{{10.f, 20.f, 30.f}, {0.9f, 0.1f, 0.2f, 0.3f}, {m * 6.f, m * 1.f, m * -3.f},
                         {0.5f, -0.25f, 0.125f}};
    // Predict's delta: translation, orientation step and angular momentum step; linear 0.
    const MotionSnapshot delta{{0.1f, 0.f, 0.f}, {-0.01f, 0.02f, 0.03f, -0.04f}, {}, {0.25f, 0.f, -0.5f}};
    GroundFrame f = FrameAtLoopTop(actor, delta);
    const Quat4 savedQ{0.9f + -0.01f, 0.1f + 0.02f, 0.2f + 0.03f, 0.3f + -0.04f};
    const Vec3 savedP{m * 6.f, m * 1.f, m * -3.f};
    const Vec3 groundedP{m * 6.f, m * 1.f, 0.f}; // 0xf93ab0-0xf93ad4: vertical zeroed, factor 1
    const Vec3 savedL{0.5f + 0.25f, -0.25f, 0.125f + -0.5f};
    REQUIRE(Same(f.motion.orientation, savedQ) && Same(f.motion.momentum, savedP));
    REQUIRE(Same(f.motion.angularMomentum, savedL) && Same(f.target, f.motion.translation));
    // The pass: the actor moves, takes the wall response (x momentum removed) and its own
    // orientation/angular step.
    actor.translation = {10.05f, 20.f, 30.f};
    actor.momentum = {0.f, m * 1.f, m * -4.f};
    actor.orientation = savedQ;
    actor.angularMomentum = savedL;
    FrameAfterPass(f, true, actor.translation);
    // The saved state is untouched by the pass.
    REQUIRE(Same(f.motion.momentum, savedP) && Same(f.motion.orientation, savedQ));
    const MotionSnapshot postPass = actor;
    const Vec3 up{0.f, 0.f, 0.5f};
    // No floor: the saved state whole, i.e. the PRE-response momentum, and the saved
    // orientation/angular momentum (not the pass's step added a second time).
    SnapApply n = ApplySnapOutcome(SnapResult::kNoFloor, f, up);
    REQUIRE(n.moveActor && Same(n.state.translation, Vec3{10.1f, 20.f, 30.f}));
    REQUIRE(Same(n.state.momentum, savedP) && Same(n.state.orientation, savedQ));
    REQUIRE(Same(n.state.angularMomentum, savedL));
    REQUIRE(n.state.momentum.z == m * -3.f); // no floor keeps the saved vertical momentum
    // Grounded: the same, but the translation is the snapped target and the saved vertical
    // momentum is zeroed (horizontal kept: factor 1.0).
    SnapApply g = ApplySnapOutcome(SnapResult::kGrounded, f, up);
    REQUIRE(g.moveActor && Same(g.state.translation, Vec3{10.05f, 20.f, 30.5f}));
    REQUIRE(Same(g.state.momentum, groundedP) && Same(g.state.orientation, savedQ));
    REQUIRE(Same(g.state.angularMomentum, savedL));
    // Refused: nothing applied; the actor keeps its post-pass (post-response) state.
    SnapApply r = ApplySnapOutcome(SnapResult::kRefused, f, up);
    if (r.moveActor) actor = r.state;
    REQUIRE(Same(actor.momentum, postPass.momentum) && Same(actor.translation, postPass.translation));
    REQUIRE(actor.momentum.z == m * -4.f); // post-pass vertical momentum kept
    // A resolved failsafe replaces only the saved translation (and the target).
    FrameFailsafeResolved(f, {1.f, 2.f, 3.f});
    n = ApplySnapOutcome(SnapResult::kNoFloor, f, up);
    REQUIRE(Same(n.state.translation, Vec3{1.f, 2.f, 3.f}) && Same(n.state.momentum, savedP));
    REQUIRE(Same(n.state.orientation, savedQ) && Same(n.state.angularMomentum, savedL));
    g = ApplySnapOutcome(SnapResult::kGrounded, f, up);
    REQUIRE(Same(g.state.translation, Vec3{1.f, 2.f, 3.5f}) && Same(g.state.momentum, groundedP));
    // A later loop top re-saves from the actor there (the post-response momentum).
    f = FrameAtLoopTop(actor, delta);
    REQUIRE(Same(f.motion.momentum, actor.momentum));
  }
  // Snap velocity: N grounded ticks on flat ground with gravity folded in at entry; the
  // vertical momentum never accumulates and the horizontal one is unchanged.
  {
    const float m = 2.f, dt = 1.f / 60.f;
    const Vec3 gravity{0.f, 0.f, m * -35.f};
    MotionSnapshot actor{{0.f, 0.f, 5.f}, {1.f, 0.f, 0.f, 0.f}, {m * 3.f, m * -4.f, 0.f}, {}};
    for (int tick = 0; tick < 120; ++tick) {
      // Pre adds nothing (no impulse); last tick's Post gravity is still in z here.
      const float zIn = actor.momentum.z;
      actor.momentum = PreMomentum(actor.momentum, {});
      REQUIRE(actor.momentum.z == zIn);
      GroundFrame f = FrameAtLoopTop(actor, MotionSnapshot{{}, {0.f, 0.f, 0.f, 0.f}, {}, {}});
      SnapApply g = ApplySnapOutcome(SnapResult::kGrounded, f, {});
      REQUIRE(g.moveActor);
      actor = g.state;
      REQUIRE(actor.momentum.z == 0.f); // PRE-post: the snap reset, gravity not yet folded
      REQUIRE(actor.momentum.x == m * 3.f && actor.momentum.y == m * -4.f);
      REQUIRE(actor.translation.z == 5.f);
      // Post, on every snap outcome, then creates next tick's vertical speed.
      for (SnapResult out : {SnapResult::kGrounded, SnapResult::kNoFloor, SnapResult::kRefused}) {
        Vec3 pm = PostMomentum(actor.momentum, {}, gravity, dt);
        REQUIRE(pm.z == dt * gravity.z && pm.x == actor.momentum.x);
        (void)out;
      }
      actor.momentum = PostMomentum(actor.momentum, {}, gravity, dt);
      REQUIRE(actor.momentum.z < 0.f);
    }
  }
  // G6/G6b: post-snap state policy, both retained entry states.
  {
    const RetainedResult grounded{RetainedState::kGrounded, true};
    const RetainedResult noFloor{RetainedState::kNoFloor, true};
    for (const RetainedResult& retained : {grounded, noFloor}) {
      StateActions g = DecideStateActions(SnapResult::kGrounded, retained);
      REQUIRE(g.onFloor && !g.falling && !g.removeGroundCollider && !g.resendOnFloor && g.moveActor);
      StateActions n = DecideStateActions(SnapResult::kNoFloor, retained);
      REQUIRE(n.falling && n.removeGroundCollider && n.clearFloorNormal && !n.onFloor && n.moveActor);
    }
    StateActions r = DecideStateActions(SnapResult::kRefused, grounded);
    REQUIRE(r.resendOnFloor && !r.onFloor && !r.falling && !r.removeGroundCollider && !r.moveActor);
    REQUIRE(!r.clearFloorNormal);
    // An airborne entry (0) into the ground mover refuses: the source resends Falling and
    // drops back to the off-ground mover (ENTRY-STATE.md §C, 0xc54ad4).
    r = DecideStateActions(SnapResult::kRefused, noFloor);
    REQUIRE(r.falling && r.removeGroundCollider && !r.resendOnFloor && !r.onFloor && !r.moveActor);
    REQUIRE(!r.clearFloorNormal);
  }
  // The retained Remastered persisted object state (CPhysicsState+0x13c): construction is the
  // source ctor's 0 and valid (MOVER-SELECTION.md §2), and only mover outcomes write it.
  {
    const RetainedResult initial;
    REQUIRE(initial.state == RetainedState::kNoFloor && initial.valid);
    REQUIRE(!RetainedIsGrounded(initial));
    REQUIRE(RetainedAllowsGroundMover(initial));

    // A verified airborne tick (0xf9450c) writes 0 and restores validity from any state.
    const RetainedResult air = RetainedAirborneEntry(RetainedInvalidate());
    REQUIRE(air.state == RetainedState::kNoFloor && air.valid && RetainedAllowsGroundMover(air));
    // An ordinary airborne tick keeps 0; a landing that sent kSM_OnFloor stores 1.
    const RetainedResult stillAir = RetainedAirborneLanding(air, false);
    REQUIRE(stillAir.state == RetainedState::kNoFloor && stillAir.valid);
    const RetainedResult landed = RetainedAirborneLanding(air, true);
    REQUIRE(landed.state == RetainedState::kGrounded && landed.valid && RetainedIsGrounded(landed));

    // air -> grounded -> refused: the refusals retain 1, so OnFloor is resent on any number
    // of refused ticks (SnapToGround 0xf93b04 writes nothing).
    RetainedResult afterGroundedSnap = RetainedGroundOutcome(air, SnapResult::kGrounded);
    REQUIRE(RetainedIsGrounded(afterGroundedSnap));
    RetainedResult refused = RetainedGroundOutcome(afterGroundedSnap, SnapResult::kRefused);
    REQUIRE(refused.state == RetainedState::kGrounded && refused.valid);
    REQUIRE(RetainedIsGrounded(refused));
    REQUIRE(DecideStateActions(SnapResult::kRefused, refused).resendOnFloor);
    // A later no-floor snap clears it (0xf93af4), and a later refused tick then resends
    // Falling / removes the material.
    const RetainedResult cleared = RetainedGroundOutcome(refused, SnapResult::kNoFloor);
    REQUIRE(cleared.state == RetainedState::kNoFloor && cleared.valid);
    const StateActions afterClearedRefusal = DecideStateActions(SnapResult::kRefused, cleared);
    REQUIRE(afterClearedRefusal.falling && afterClearedRefusal.removeGroundCollider);
    REQUIRE(!afterClearedRefusal.resendOnFloor);

    // air (0) -> ground mover entry -> refused: retains 0, so the tick resends Falling and
    // the off-ground mover takes over (the material-ground entry of ENTRY-STATE.md §C).
    const RetainedResult airRefused = RetainedGroundOutcome(air, SnapResult::kRefused);
    REQUIRE(airRefused.state == RetainedState::kNoFloor && airRefused.valid);
    const StateActions airRefusedActions = DecideStateActions(SnapResult::kRefused, airRefused);
    REQUIRE(airRefusedActions.falling && airRefusedActions.removeGroundCollider && !airRefusedActions.onFloor);

    // Invalid (a retail mover ran: morph ball, the dynamic fallback, a save-state load): the
    // ground mover is not entered, NO outcome writes the value, and only an airborne tick
    // restores validity. Nothing ever turns invalid back into 0/1 silently.
    const RetainedResult invalid = RetainedInvalidate();
    REQUIRE(!invalid.valid && !RetainedAllowsGroundMover(invalid));
    REQUIRE(!RetainedIsGrounded(invalid));
    for (SnapResult snap : {SnapResult::kGrounded, SnapResult::kNoFloor, SnapResult::kRefused}) {
      const RetainedResult written = RetainedGroundOutcome(invalid, snap);
      REQUIRE(!written.valid && written.state == RetainedState::kInvalid);
    }
    REQUIRE(RetainedAirborneEntry(invalid).valid);
    REQUIRE(RetainedAllowsGroundMover(RetainedAirborneEntry(invalid)));
  }
  // MovePlayer's tick mover gate (MoverGate): the retained value survives a tick only when
  // that tick ran a Remastered mover, so any tick outside the gate leaves it invalid and no
  // amount of retail ground ticking (or the gate reopening) turns it valid again. Exercised as
  // a truth table over the getter values the entry samples, never by re-deriving the
  // predicate.
  {
    const RetainedResult grounded{RetainedState::kGrounded, true};
    const RetainedResult noFloor{RetainedState::kNoFloor, true};
    // A tick served by a Remastered mover: unmorphed, mouse controls allowed, rm1. It passes
    // the entry value through untouched (the movers write it later in the tick).
    const MoverGate served{true, true, true};
    REQUIRE(UsesRemasteredMover(served));
    REQUIRE(RetainedIsGrounded(RetainedEntryForTick(served, grounded)));
    REQUIRE(RetainedEntryForTick(served, noFloor).state == RetainedState::kNoFloor);
    REQUIRE(RetainedEntryForTick(served, noFloor).valid);
    REQUIRE(RetainedEntryForTick(served, RetainedInvalidate()).state == RetainedState::kInvalid);
    // One closed term is enough: classic movement (rm0), mouse controls not allowed (direct
    // aim / pointer eligibility off), morph ball (never kMS_Unmorphed). The ball needs no
    // invalidation of its own.
    const MoverGate classic{true, true, false};
    const MoverGate noMouse{true, false, true};
    const MoverGate ball{false, true, true};
    const MoverGate ballClassic{false, false, false};
    for (const MoverGate& gate : {classic, noMouse, ball, ballClassic}) {
      REQUIRE(!UsesRemasteredMover(gate));
      for (const RetainedResult& entry : {grounded, noFloor, RetainedInvalidate()}) {
        const RetainedResult tick = RetainedEntryForTick(gate, entry);
        REQUIRE(tick.state == RetainedState::kInvalid && !tick.valid);
        REQUIRE(!RetainedAllowsGroundMover(tick));
        REQUIRE(!RetainedIsGrounded(tick));
      }
    }
    // rm1 -> rm0 -> rm1: the classic tick invalidates, and the gate reopening on a later tick
    // does not revive it; only a verified airborne tick does (0xf9450c writes 0).
    const RetainedResult afterClassic = RetainedEntryForTick(classic, grounded);
    REQUIRE(!afterClassic.valid && !RetainedAllowsGroundMover(afterClassic));
    RetainedResult carried = afterClassic;
    for (int tick = 0; tick < 3; ++tick) {
      carried = RetainedEntryForTick(served, carried);
      REQUIRE(!carried.valid && !RetainedAllowsGroundMover(carried));
      for (SnapResult snap : {SnapResult::kGrounded, SnapResult::kNoFloor, SnapResult::kRefused}) {
        const RetainedResult retailOutcome = RetainedGroundOutcome(carried, snap);
        REQUIRE(retailOutcome.state == RetainedState::kInvalid && !retailOutcome.valid);
      }
      carried = RetainedEntryForTick(classic, RetainedGroundOutcome(carried, SnapResult::kGrounded));
      REQUIRE(!carried.valid);
    }
    // The airborne mover is inside the gate and its entry restores validity from invalid.
    const RetainedResult restored = RetainedAirborneEntry(carried);
    REQUIRE(restored.valid && restored.state == RetainedState::kNoFloor);
    REQUIRE(RetainedIsGrounded(RetainedGroundOutcome(restored, SnapResult::kGrounded)));
    // ... and the gate keeps vouching for it on the next served tick.
    const RetainedResult kept = RetainedEntryForTick(served, restored);
    REQUIRE(kept.valid && kept.state == RetainedState::kNoFloor);
    REQUIRE(RetainedAllowsGroundMover(kept));
  }
  // The stop against the Landing Site runway wall (SLOPE-DELTA.md §5, room
  // 39F2DE28:B2701146): the tick that reaches it
  // meets exactly one non-floor contact, face 3061 n = (-0.062,-0.967,+0.248) (up 0.248 ->
  // IsWall, not IsFloor), over the ramp floor face 4664. The response must remove the inward
  // component of the entry velocity and keep the tangential; the retail mover leaves the actor
  // 2 mm from the wall with no positional re-aim (a full stop, CGroundMovement's minimum
  // translation), so a nonzero tangent is what tells the two contracts apart.
  {
    // Entry velocity of the tick before the stop, from the frozen trial trace
    // (final-runtime/csv/gf117E2_slopef.csv, S rel 282 -> 283): +y at 11.842 m/s.
    const Vec3 v{-0.00509f, 11.84206f, 0.f};
    const float mass = 200.f;
    const ResponseContact stop[2] = {
        {-0.062f, -0.967f, 0.248f, 2, 0.0035f}, // face 3061, the near-vertical wall
        {-0.201f, -0.341f, 0.918f, 0, 0.f},     // face 4664, the ramp floor under it
    };
    // Expected value derived independently of the policy: a contact normal with up > 0 is
    // flattened into the horizontal plane and renormalized (0xf92ccc:305-401), and the response
    // is the plane projection, v - (v.n)n, i.e. the impulse -n(v.n) added to the velocity.
    float nx = -0.062f, ny = -0.967f; // z dropped: up > 0
    const float len = std::sqrt(nx * nx + ny * ny);
    nx /= len;
    ny /= len;
    const float vd = nx * v.x + ny * v.y; // negative: the entry runs into the wall
    REQUIRE(vd < -11.f);
    const Vec3 expectedImpulse{-nx * vd, -ny * vd, 0.f};
    const Vec3 tangential{v.x + expectedImpulse.x, v.y + expectedImpulse.y, 0.f};
    REQUIRE(std::fabs(tangential.x + 0.7612f) < 0.002f);
    REQUIRE(std::fabs(tangential.y - 0.0494f) < 0.002f);
    // The tangent is nonzero, along -x as the measured slide is, and has no inward part left.
    REQUIRE(tangential.x < -0.7f);
    REQUIRE(std::sqrt(tangential.x * tangential.x + tangential.y * tangential.y) > 0.7f);
    REQUIRE(std::fabs(nx * tangential.x + ny * tangential.y) < 1.e-4f);
    const GroundResponse wall = ResolveGroundResponse(stop, 2, v, {0.f, 0.f, 0.f}, mass);
    // The wall pass takes it and the floor pass never runs (0xf93300 is reached only when no
    // wall was accepted).
    REQUIRE(wall.wallAccepted && !wall.floorUsed);
    REQUIRE(std::fabs(wall.velocityImpulse.x - expectedImpulse.x) < 1.e-5f);
    REQUIRE(std::fabs(wall.velocityImpulse.y - expectedImpulse.y) < 1.e-5f);
    REQUIRE(wall.velocityImpulse.z == 0.f);
    // The caller adds the impulse to the velocity (MoveGroundedRemastered,
    // platform/port_remastered_movers.cpp): the result is the tangential above, NOT zero.
    REQUIRE(std::fabs((v.x + wall.velocityImpulse.x) - tangential.x) < 1.e-5f);
    REQUIRE(std::fabs((v.y + wall.velocityImpulse.y) - tangential.y) < 1.e-5f);
    // momentumless seed: the flattened normal at mu = 0 (mass * 0.15), and no accumulator term
    // for a zero accumulator.
    REQUIRE(std::fabs(wall.momentumless.x - nx * mass * 0.15f) < 1.e-4f);
    REQUIRE(std::fabs(wall.momentumless.y - ny * mass * 0.15f) < 1.e-4f);
    REQUIRE(wall.momentumless.z == 0.f);
  }
  // Snap skin (LANDING-DELTA.md): both movers rest on the SAME contact, each at its own source
  // constant. SnapToGround places the box bottom at the selected contact's height + 0.002
  // (0xf938c4/0xf93964); the retail step-down snap ends at contact + 0.0005
  // (CGroundMovement.cpp:564-595). The 1.5 mm is each mover's own skin and is not tuned away.
  {
    // The measured contact height of the Observatory flat runway (LANDING-DELTA.md §3,
    // room A8BE6291:3FB4A34E) — the Observatory ring, NOT the Landing Site ramp that
    // SLOPE-DELTA.md measures.
    const float contactHeight = 101.476872f;
    Contact rest = floor;
    rest.minHeight = rest.maxHeight = contactHeight;
    const Snap snapRest = SelectSnap(contactHeight, 1.f, &rest, 1);
    REQUIRE(snapRest.result == SnapResult::kGrounded);
    const float skin = (contactHeight + 0.002f) - contactHeight; // this mover's own constant
    REQUIRE(std::fabs(snapRest.offset - skin) < 1.e-6f);
    REQUIRE(std::fabs(snapRest.offset - 0.002f) < 2.e-5f);
    REQUIRE(std::fabs(snapRest.offset - 0.0005f) > 0.001f); // not the retail resting skin
    // The refusal boundary is the same offset: equality is refused and applies nothing
    // (0xf93a4c -> 0xf93b04), one hair more is grounded.
    REQUIRE(SelectSnap(contactHeight, skin, &rest, 1).result == SnapResult::kRefused);
    REQUIRE(SelectSnap(contactHeight, skin + 1.e-3f, &rest, 1).result == SnapResult::kGrounded);
  }
}
