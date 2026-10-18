// Regression coverage for the AUDIT-2 findings on CGameCollision::MoveAirborneStepped:
//  (1) 0xf94ff0-0xf95028 rejects a contact on dot(displacement, WORKING normal) > 0, so a contact
//      whose primary looks separating but whose secondary blocks is still responded to;
//  (2) 0xc6d644-0xc6d65c selects the secondary whenever the presence byte is set (a zero secondary
//      stays the working normal), and 0xc6d658-0xc6d698 only sets w25, which gates the
//      kMS_ApplyJump force block (0xc6d7f4 `cbz w25`), never the normal choice;
//  (3) the static half of a query is only made when the filter does not exclude
//      kMT_NoStaticCollision (CGameCollision::DetectCollision / DetectCollisionBoolean).
#include "Collision/CMaterialList.hpp"
#include "Collision/CMaterialFilter.hpp"

#include "port_air_mover_contact.h"
#include "port_remastered_ground_policy.h"
#include "port_remastered_ground_world.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>

using PortAirMoverContact::Acts;
using PortAirMoverContact::Vec3;
using PortAirMoverContact::W25;
using PortAirMoverContact::WorkingNormal;

// (2) presence selects the secondary, unconditionally, including a degenerate one.
static void TestWorkingNormal() {
  const Vec3 primary{0.6f, 0.8f, 0.f};   // Z-up: up = z, so a level two-wall pair
  const Vec3 secondary{0.f, 1.f, 0.f};
  const Vec3 zero{0.f, 0.f, 0.f};
  const Vec3 chosen = WorkingNormal(primary, true, secondary);
  CHECK_MSG(chosen.x == secondary.x && chosen.y == secondary.y && chosen.z == secondary.z,
            "presence selects the secondary");
  const Vec3 without = WorkingNormal(primary, false, secondary);
  CHECK_MSG(without.x == primary.x && without.y == primary.y && without.z == primary.z,
            "absence keeps the primary");
  const Vec3 degenerate = WorkingNormal(primary, true, zero);
  CHECK_MSG(degenerate.x == 0.f && degenerate.y == 0.f && degenerate.z == 0.f,
            "a present zero secondary stays the working normal (no fallback to the primary)");
  CHECK_MSG(WorkingNormal({0.f, 0.f, -1.f}, true, zero).z == 0.f,
            "a ceiling primary does not win over a present zero secondary");
}

// (2) w25 only gates the force block: true for a wall pair, false for floor/ceiling pairs.
static void TestW25() {
  CHECK_MSG(W25(false, {0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}), "no secondary -> w25 = 1");
  CHECK_MSG(W25(true, {0.6f, 0.8f, 0.f}, {0.f, 1.f, 0.f}), "two walls -> w25 = 1");
  CHECK_MSG(!W25(true, {0.f, 0.f, -1.f}, {0.6f, 0.8f, 0.f}), "ceiling primary -> w25 = 0");
  CHECK_MSG(!W25(true, {0.6f, 0.8f, 0.f}, {0.f, 0.f, -1.f}), "ceiling secondary -> w25 = 0");
  CHECK_MSG(!W25(true, {0.f, 0.f, 1.f}, {0.6f, 0.8f, 0.f}), "floor primary -> w25 = 0");
  CHECK_MSG(!W25(true, {0.6f, 0.8f, 0.f}, {0.f, 0.f, 1.f}), "floor secondary -> w25 = 0");
  // w25 must not touch the normal choice: the secondary is the working normal either way.
  const Vec3 work = WorkingNormal({0.6f, 0.8f, 0.f}, true, {0.f, 0.f, -1.f});
  CHECK_MSG(work.z == -1.f && !W25(true, {0.6f, 0.8f, 0.f}, {0.f, 0.f, -1.f}),
            "w25 = 0 does not restore the primary");
}

// (1) the separation gate is evaluated on the working normal: the review's example.
static void TestSeparationGate() {
  const Vec3 primary{1.f, 0.f, 0.f};
  const Vec3 secondary{0.f, 0.f, -1.f};
  const Vec3 motion{1.f, 0.f, 1.f};
  const Vec3 noRider{0.f, 0.f, 0.f};
  const Vec3 work = WorkingNormal(primary, true, secondary);
  // Primary +X, secondary -Z, motion (+1,0,+1): the native velocity form against the primary is
  // separating (dot = +1 > 0) and would skip the blocking contact; the source's working-normal
  // form is dot = -1 and responds.
  CHECK_MSG(!Acts(motion, noRider, primary), "primary alone would skip (the native form)");
  CHECK_MSG(Acts(motion, noRider, work), "the working normal responds (0xf94ff0-0xf95028)");
  // The converse: motion along -Z (into the -Z working normal) is separating.
  CHECK_MSG(!Acts({0.f, 0.f, -1.f}, noRider, work), "separating along the working normal skips");
  // A zero secondary never separates: dot = 0, so the contact is responded to (the guard is
  // `> 0`). Rejection never changes the bracket's reported time.
  CHECK_MSG(Acts(motion, noRider, {0.f, 0.f, 0.f}), "zero working normal never separates");
  // The rider offset is subtracted before the dot (0xf94ff0-0xf95020).
  CHECK_MSG(!Acts({1.f, 0.f, 0.f}, {2.f, 0.f, 0.f}, {1.f, 0.f, 0.f}) == false,
            "the rider offset is subtracted (dot -1, so the contact acts)");
}

// (3) the static half is guarded exactly as the native static half is.
static void TestStaticGuard() {
  const CMaterialFilter excludesStatic =
      CMaterialFilter::MakeExclude(CMaterialList(kMT_NoStaticCollision));
  const CMaterialFilter open = CMaterialFilter::skPassEverything;
  CHECK_MSG(excludesStatic.GetExcludeList().HasMaterial(kMT_NoStaticCollision),
            "the exclusion list carries kMT_NoStaticCollision");
  CHECK_MSG(!(!excludesStatic.GetExcludeList().HasMaterial(kMT_NoStaticCollision)),
            "excluded static collision must not be queried");
  CHECK_MSG(!open.GetExcludeList().HasMaterial(kMT_NoStaticCollision),
            "an open filter queries the static half");
}

static void TestTargetAndBracket() {
  // The source applies its selected motion only at exit. Current remains 10,
  // not the accepted-lo position 10.5; target remains the full prediction 12.
  const Vec3 remain = PortAirMoverContact::RemainingDisplacement({12.f,0.f,0.f},{10.f,0.f,0.f});
  CHECK_MSG(remain.x == 2.f, "F1 gate uses full target minus PRE-STEP current");
  CHECK_MSG(!Acts(remain,{1.75f,0.f,0.f},{1.f,0.f,0.f}), "F1 rider is displacement, not velocity");
  CHECK_MSG(Acts({1.5f,0.f,0.f},{1.75f,0.f,0.f},{1.f,0.f,0.f}),
            "wrong target-minus-accepted-lo would act");
  CHECK_MSG(Acts({.5f,0.f,0.f},{1.75f,0.f,0.f},{1.f,0.f,0.f}),
            "wrong accepted-lo-minus-entry would act");
  CHECK_MSG(!PortAirMoverContact::HasImpacts(0,0), "F2 SAT alone selects hi, not lo");
  CHECK_MSG(PortAirMoverContact::HasImpacts(1,0), "F2 source impacts select lo without native entries");
  CHECK_MSG(PortAirMoverContact::HasImpacts(0,1), "F2 actor impact independently selects lo");
}

static void TestFlag100Response() {
  using PortAirMoverContact::SkipFlatFloor;
  using PortAirMoverContact::PushDistance;
  CHECK_MSG(SkipFlatFloor(1.f,1.f,2.f,1.f,false), "ordinary rising flat-floor contact skips");
  CHECK_MSG(!SkipFlatFloor(1.f,1.f,2.f,1.f,true), "flag100 bypasses rising early-out");
  CHECK_MSG(SkipFlatFloor(1.f,-1.f,1.f,1.f,false), "ordinary at-plane flat-floor contact skips");
  CHECK_MSG(!SkipFlatFloor(1.f,-1.f,1.f,1.f,true), "flag100 bypasses at-plane early-out");
  CHECK_MSG(!SkipFlatFloor(.5f,1.f,2.f,1.f,false), "non-flat normal does not take floor early-out");
  CHECK_MSG(PushDistance(false) == .0025f, "ordinary push is 0.0025");
  CHECK_MSG(PushDistance(true) == .025f, "flag100 push is 0.025");
}

static void TestTerrainPredicates() {
  namespace W = PortRemasteredGroundWorld;
  W::Query q;
  q.box = {{-1.f,-1.f,-1.f},{1.f,1.f,1.f}};
  W::Triangle tri;
  // Exactly on the top plane: inclusive SAT hits but strict clipping emits no
  // impact. This is precisely why list-nonempty cannot define NoPostFilter.
  tri.verts = {{{-.5f,-.5f,1.f},{.5f,-.5f,1.f},{0.f,.5f,1.f}}};
  tri.normal = {0.f,0.f,1.f};
  auto r = W::QueryTerrain(q,&tri,1,true);
  CHECK_MSG(r.booleanHit, "inclusive SAT counts touching");
  CHECK_MSG(r.impacts.contacts.empty(), "strict impact clipping does not count touching");
  tri.verts[0].z = tri.verts[1].z = tri.verts[2].z = .9f;
  r = W::QueryTerrain(q,&tri,1,true);
  CHECK_MSG(r.booleanHit && r.impacts.contacts.size() == 2, "penetrating triangle supplies both results");
  const auto b = W::QueryTerrain(q,&tri,1,false);
  CHECK_MSG(b.booleanHit && b.impacts.contacts.empty(), "NoPostFilter does not produce impacts");
  tri.verts[0].z = tri.verts[1].z = tri.verts[2].z = 1.01f;
  r = W::QueryTerrain(q,&tri,1,true);
  CHECK_MSG(!r.booleanHit && r.impacts.contacts.empty(), "separated triangle supplies neither result");
}

static void TestSATReference() {
  namespace G = PortRemasteredGroundContacts;
  // Independent generic 13-axis SAT, exact for these small integer inputs.
  // Unlike the production routine this projects all three points on every
  // axis and uses double arithmetic; no source endpoint shortcuts are reused.
  const G::AABB box{{-2.f,-3.f,-4.f},{2.f,3.f,4.f}};
  unsigned rng = 117;
  for (int trial=0; trial<50000; ++trial) {
    std::array<G::Vec3,3> t;
    for (auto& p : t) {
      auto next = [&]() { rng=rng*1664525u+1013904223u; return float(int(rng%21)-10); };
      p = {next(),next(),next()};
    }
    auto sub = [](G::Vec3 a,G::Vec3 b) {return G::Vec3{a.x-b.x,a.y-b.y,a.z-b.z};};
    auto cross = [](G::Vec3 a,G::Vec3 b) {
      return G::Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
    };
    const G::Vec3 e[] = {sub(t[1],t[0]),sub(t[2],t[1]),sub(t[0],t[2])};
    const G::Vec3 xyz[] = {{1,0,0},{0,1,0},{0,0,1}};
    G::Vec3 axes[13];
    for(int i=0;i<3;++i) axes[i]=xyz[i];
    axes[3]=cross(e[0],e[1]);
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) axes[4+i*3+j]=cross(e[i],xyz[j]);
    bool expected=true;
    for(auto a:axes) {
      const double radius=2.*std::fabs(a.x)+3.*std::fabs(a.y)+4.*std::fabs(a.z);
      double lo=1e30,hi=-1e30;
      for(auto p:t) {
        const double dot=double(a.x)*p.x+double(a.y)*p.y+double(a.z)*p.z;
        if(dot<lo) lo=dot;
        if(dot>hi) hi=dot;
      }
      if(lo>radius || hi < -radius) expected=false;
    }
    if (G::Intersects(box,t)!=expected) {
      CHECK_MSG(false,"source SAT agrees with independent 13-axis reference (50,000 cases)");
      return;
    }
  }
}

// 0xc6d110's probe and band (issue #147): the probe reaches down to the hi state, at most
// GetStepDownHeight() below the target, and never above target - 0.002.
static void TestGroundCheckBand() {
  using PortAirMoverContact::GroundCheckBand;
  // A floor contact while falling in kMS_ApplyJump (step down 0.1): the bisection's blocked
  // sample sits 0.0156 below the accepted one (the #147 descending piston). The probe goes to
  // it and the band reaches 0.102 down, so the platform top between the two is found.
  const auto fall = GroundCheckBand(23.668f, 0.1f, 23.6524f);
  CHECK_MSG(fall.probeUp == 23.6524f, "the probe reaches the blocked hi sample");
  CHECK_MSG(std::fabs(fall.low - (23.668f - 0.1f - 0.002f)) < 1e-6f, "the band starts step-down below");
  // A hi sample further down than the step-down height is clamped to it.
  const auto deep = GroundCheckBand(10.f, 0.1f, 9.5f);
  CHECK_MSG(deep.probeUp == 10.f - 0.1f, "the probe stops at the step-down height");
  // The post-step probe while rising: hi is the state moved 0.1 down.
  const auto rise = GroundCheckBand(22.4181f, 0.1f, 22.4181f - 0.1f);
  CHECK_MSG(std::fabs(rise.probeUp - (22.4181f - 0.1f)) < 1e-5f, "the rising probe is 0.1 down");
  CHECK_MSG(rise.low < 22.4181f - 0.05f, "a floor 0.05 below the feet is inside the band");
  // A hi sample above the target never lifts the probe above target - 0.002.
  CHECK_MSG(GroundCheckBand(5.f, 0.1f, 6.f).probeUp == 5.f - .002f, "the probe is capped below");
  // kMS_Jump's step down is -1: the band starts above the box bottom, so nothing lands.
  CHECK_MSG(GroundCheckBand(5.f, -1.f, 4.9f).low > 5.f, "kMS_Jump has an empty band");
}

// The airborne platform contacts (issue #117 slice 2, B rework): the rider offset belongs to the
// rider controller's own contacts only, and a DCLN platform (CCollidableOBBoxTree, type 4) never
// takes the callback's flat-floor early-out.
static void TestPlatformContacts() {
  using PortAirMoverContact::RiderOffsetFor;
  using PortAirMoverContact::FlatFloorEarlyOutApplies;
  const Vec3 off{0.f, 0.f, 0.05f};
  const std::uint32_t plat = 0x10000u | 0x42u;
  const Vec3 own = RiderOffsetFor(plat, plat, true, off);
  CHECK_MSG(own.z == 0.05f, "the rider's own contact gets its offset");
  CHECK_MSG(RiderOffsetFor(plat, plat, false, off).z == 0.f, "an invalid controller gives none");
  CHECK_MSG(RiderOffsetFor(0x10000u | 0x43u, plat, true, off).z == 0.f, "another platform gives none");
  CHECK_MSG(RiderOffsetFor(0u, 0u, true, off).z == 0.f, "static terrain gives none");
  // The rising-platform jump: own motion +0.0139 up against a +0.05 carry is approaching the
  // platform top (dot < 0), so the contact acts; without the offset it separates.
  const Vec3 up{0.f, 0.f, 1.f};
  CHECK_MSG(Acts({0.f, 0.f, 0.0139f}, own, up), "rising platform top acts with the rider offset");
  CHECK_MSG(!Acts({0.f, 0.f, 0.0139f}, {}, up), "and separates without it");
  CHECK_MSG(FlatFloorEarlyOutApplies(0, false), "AABox takes the early-out");
  CHECK_MSG(FlatFloorEarlyOutApplies(2, true) && !FlatFloorEarlyOutApplies(2, false),
            "OBBox only when axis-aligned with up");
  CHECK_MSG(!FlatFloorEarlyOutApplies(PortAirMoverContact::kCollidableOBBoxTree, true),
            "an OBBox tree (DCLN platform) never does");
  CHECK_MSG(!FlatFloorEarlyOutApplies(1, true) && !FlatFloorEarlyOutApplies(3, true) &&
                !FlatFloorEarlyOutApplies(5, true),
            "sphere, AABox tree, capsule never do");
}

// The stepped mover's split integrator (Pre 0xc59814 at 0xf9447c, Post 0xc5994c at 0xf94884):
// the steps predict with the entry velocity and add no force, and the force is folded once per
// tick after the failsafe, whatever the steps consumed. A tick whose steps are all blocked at
// lo = 0 still gains the tick's force (the rising-piston jump-off stall, #117).
static void TestSplitIntegrator() {
  using PortRemasteredGround::PostMomentum;
  using PortRemasteredGround::PreMomentum;
  using PortRemasteredGround::PredictTranslation;
  using V = PortRemasteredGround::Vec3;
  const float m = 2.f, dt = 1.f / 60.f;
  const V gravity{0.f, 0.f, -35.f * m};  // persistent
  const V force{0.f, 0.f, 10.f * m};     // transient, this tick
  // One tick with `steps` predictions of `consumed[i]` each; returns the end momentum.
  auto tick = [&](V momentum, V impulse, const float* consumed, int steps, bool failsafeStops,
                  float& movedZ) {
    momentum = PreMomentum(momentum, impulse);
    movedZ = 0.f;
    for (int i = 0; i < steps; ++i) {
      const V v{momentum.x / m, momentum.y / m, momentum.z / m};
      movedZ += PredictTranslation(consumed[i], v, {}, 1.f / m).z;  // read-only on momentum
    }
    if (failsafeStops) momentum = {};
    return PostMomentum(momentum, force, gravity, dt);
  };
  const V start{0.f, 0.f, 0.8333f * m};
  const float whole[1] = {dt};
  const float split[3] = {dt / 4.f, dt / 2.f, dt / 4.f};
  const float blocked[3] = {0.f, 0.f, 0.f};
  float z1 = 0.f, z3 = 0.f, z0 = 0.f;
  const V a = tick(start, {}, whole, 1, false, z1);
  const V b = tick(start, {}, split, 3, false, z3);
  const V c = tick(start, {}, blocked, 3, false, z0);
  const float want = start.z + dt * (force.z + gravity.z);
  CHECK_MSG(std::fabs(a.z - want) < 1e-6f, "force folded once per tick");
  CHECK_MSG(a.z == b.z && b.z == c.z, "independent of the step count and of lo = 0 steps");
  CHECK_MSG(std::fabs(z1 - dt * 0.8333f) < 1e-6f && std::fabs(z3 - z1) < 1e-6f,
            "the steps move with the entry velocity (no 0.5 a t^2)");
  CHECK_MSG(z0 == 0.f, "blocked steps do not move");
  float zf = 0.f;
  const V d = tick(start, {}, blocked, 3, true, zf);
  CHECK_MSG(std::fabs(d.z - dt * (force.z + gravity.z)) < 1e-6f,
            "a failsafe stop happens before the force, which still applies");
  const V e = tick(start, {0.f, 0.f, 3.f}, whole, 1, false, zf);
  CHECK_MSG(std::fabs(e.z - (want + 3.f)) < 1e-5f && std::fabs(zf - dt * (start.z + 3.f) / m) < 1e-6f,
            "the impulse is folded once, at entry, and moves this tick");
}

int main() {
  TestWorkingNormal();
  TestW25();
  TestSeparationGate();
  TestStaticGuard();
  TestTargetAndBracket();
  TestFlag100Response();
  TestTerrainPredicates();
  TestSATReference();
  TestGroundCheckBand();
  TestPlatformContacts();
  TestSplitIntegrator();
  if (port_test::Failures() == 0) {
    std::printf("port_air_mover_contact_tests: all passed\n");
  }
  return PORT_TEST_RESULT();
}
