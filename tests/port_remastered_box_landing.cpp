// Remastered box-actor landing query (platform/include/port_remastered_box_landing.h)
// on fixed values: the two-way translated-material pair gate (CheckCollision 0xc1f110
// visit rule + 0xbf61b4 step 2 -> CIncludeExcludeCollisionFilterMP1::Passes 0xcb7084),
// the per-impact SurfaceCollisionPasses 0xcb7110, the 0xbf61b4/0xbf6650 face points and
// the 0xc6d110 landing walk (positive and ordered `pz <= feet && pz > low`,
// 0xc6d3d0-0xc6d3ec, with FloorEligible and is_floor 0xbecb40).
#include "port_remastered_box_landing.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <vector>

using PortCollisionFailsafe::PairedImpact;
using PortCollisionFailsafe::Vec3;
using PortRemasteredBoxLanding::Apply;
using PortRemasteredBoxLanding::Box;
using PortRemasteredBoxLanding::BoxesOverlap;
using PortRemasteredBoxLanding::Candidate;
using PortRemasteredBoxLanding::FiniteBand;
using PortRemasteredBoxLanding::FiniteOrdered;
using PortRemasteredBoxLanding::FloorEligible;
using PortRemasteredBoxLanding::LandingState;
using PortRemasteredBoxLanding::Pair;
using PortRemasteredBoxLanding::PairPasses;
using PortRemasteredBoxLanding::PairSelection;
using PortRemasteredBoxLanding::SelectionState;
using PortRemasteredCollisionMaterial::Bit;
using PortRemasteredCollisionMaterial::MaterialFilter;
using PortRemasteredCollisionMaterial::TranslateActor;
namespace List = PortRemasteredCollisionMaterial::List;

static const float kNaN = std::numeric_limits< float >::quiet_NaN();
static const float kInf = std::numeric_limits< float >::infinity();

static bool Near(float a, float b, float e = 1e-5f) { return std::fabs(a - b) <= e; }
static bool Near(const Vec3& a, const Vec3& b) {
  return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z);
}

static Box B(float x0, float y0, float z0, float x1, float y1, float z1) {
  return Box{{x0, y0, z0}, {x1, y1, z1}};
}

static uint64_t Bits(std::initializer_list<int> bits) {
  uint64_t v = 0;
  for (int b : bits) v |= Bit(b);
  return v;
}

// An actor-shaped material list without an orientation bit (source orientation -1).
static const uint64_t kActorBits = Bits({List::Solid, List::Immovable});
static const uint64_t kPlayerBits = Bits({List::Solid, List::Player, List::Character});

// A pair with everything permissive but the boxes, the materials and the band.
static Pair Permissive(const Box& self, const Box& other, uint64_t selfBits, uint64_t otherBits,
                       float feet) {
  Pair p;
  p.selfBox = self;
  p.otherBox = other;
  p.selfMaterialBits = selfBits;
  p.otherMaterialBits = otherBits;
  p.selfFilter = MaterialFilter{0, 0, 0};   // kFT_Always
  p.otherFilter = MaterialFilter{0, 0, 0};  // kFT_Always
  p.feet = feet;
  return p;
}

// ---------------------------------------------------------------------------
// The source pair gate.
// ---------------------------------------------------------------------------
static void PairGate() {
  const Box a = B(0, 0, 0, 1, 1, 1);
  CHECK_MSG(BoxesOverlap(a, B(1, 0, 0, 2, 1, 1)), "touching boxes overlap (0x98234)");
  CHECK_MSG(!BoxesOverlap(a, B(1.001f, 0, 0, 2, 1, 1)), "a separating axis is not an overlap");
  CHECK_MSG(BoxesOverlap(a, B(-1, -1, -1, 2, 2, 2)), "containment is an overlap");

  // Mode 0 (kFT_Always) passes every material pair.
  CHECK_MSG(PairPasses(Permissive(a, a, kActorBits, kActorBits, 0.f)), "mode 0 passes");

  // The other actor's OWN filter against OUR material (the CheckCollision visit rule):
  // a filter that excludes Player never sees the player's material. (CMaterialFilter::
  // MakeExclude fills the include list with all bits; mode 2 only reads the exclude.)
  {
    Pair p = Permissive(a, a, kPlayerBits, kActorBits, 0.f);
    p.otherFilter = MaterialFilter{~uint64_t{0}, Bit(List::Player), 2};  // kFT_Exclude
    CHECK_MSG(!PairPasses(p), "other filter excluding Player rejects the player material");
    p.selfMaterialBits = Bits({List::Solid});
    CHECK_MSG(PairPasses(p), "the same other filter passes a material without Player");
  }

  // Our filter against ITS material (0xbf61b4 step 2): include Solid.
  {
    Pair p = Permissive(a, a, kActorBits, Bits({List::Solid}), 0.f);
    p.selfFilter = MaterialFilter{Bit(List::Solid), 0, 1};  // kFT_Include
    CHECK_MSG(PairPasses(p), "include Solid passes a Solid other");
    p.otherMaterialBits = Bits({List::Player});
    CHECK_MSG(!PairPasses(p), "include Solid rejects a material without it");
  }

  // Channels, not masks: NoStaticCollision is channel 0x410 in the source filter.
  {
    Pair p = Permissive(a, a, kActorBits, Bits({List::Metal, List::NoStaticCollision}), 0.f);
    p.selfFilter = MaterialFilter{Bit(List::NoStaticCollision), 0, 1};
    CHECK_MSG(PairPasses(p), "include NoStaticCollision passes its translated channel");
    p.otherMaterialBits = Bits({List::Metal});
    CHECK_MSG(!PairPasses(p), "and rejects a material without it");
  }

  // Passthrough inversion (dynamic-material REPORT 5.2): Include{AIPassthrough} passes a
  // material that LACKS the AI-blocking channel, although the raw MP1 lists share no bit
  // at all. A raw-mask rule would answer the opposite way.
  {
    const uint64_t playerOnly = Bit(List::Player);
    CHECK_MSG((playerOnly & Bit(List::AIPassthrough)) == 0, "raw masks share nothing here");
    Pair p = Permissive(a, a, kActorBits, playerOnly, 0.f);
    p.selfFilter = MaterialFilter{Bit(List::AIPassthrough), 0, 1};
    CHECK_MSG(PairPasses(p), "include AIPassthrough passes through its channel");
    p.otherMaterialBits = Bits({List::Solid});  // Solid sets the AI-block channel
    CHECK_MSG(!PairPasses(p), "and refuses a material that carries it");
  }

  // Orientation lists go through the guarded material form, so a filter naming only an
  // orientation the material does not have is refused.
  {
    Pair p = Permissive(a, a, kActorBits, Bits({List::Wall}), 0.f);
    p.selfFilter = MaterialFilter{Bit(List::Floor), 0, 1};
    CHECK_MSG(!PairPasses(p), "include Floor rejects a Wall-oriented material");
    p.otherMaterialBits = Bits({List::Floor});
    CHECK_MSG(PairPasses(p), "include Floor passes a Floor-oriented material");
  }

  // A rejected pair never touches the state and never runs the walk.
  {
    Pair p = Permissive(a, a, kActorBits, Bits({List::Player}), 0.f);
    p.selfFilter = MaterialFilter{Bit(List::Solid), 0, 1};
    LandingState state{0.5f, true};
    std::vector<PairedImpact> impacts(1);
    CHECK_MSG(!Apply(p, state, impacts), "a pair our filter refuses is not a source query");
    CHECK_MSG(state.low == 0.5f && state.result && impacts.size() == 1,
              "rejected pair leaves state and impacts untouched");
  }
  {
    Pair p = Permissive(a, B(5, 5, 5, 6, 6, 6), kActorBits, kActorBits, 0.f);
    LandingState state{0.5f, true};
    std::vector<PairedImpact> impacts;
    CHECK_MSG(!Apply(p, state, impacts), "non-overlapping boxes are not a source query");
    CHECK_MSG(state.low == 0.5f && state.result, "no query leaves the state alone");
  }
}

// ---------------------------------------------------------------------------
// 0xbf61b4/0xbf6650 impact geometry and the walk order.
// ---------------------------------------------------------------------------
// Partial overlap on all three axes: one impact per axis, emitted in source order
// (Rem x = port -x, Rem y = port z, Rem z = port y), so the vector order is
// x(-port x), up, y(port y).
static void ThreeImpacts() {
  Pair p = Permissive(B(0, 0, 0, 2, 2, 2), B(1, 1, -1, 3, 3, 1), kActorBits, kActorBits, 1.f);
  LandingState state{0.998f, false};
  std::vector<PairedImpact> impacts;
  CHECK_MSG(Apply(p, state, impacts), "an admitted pair is a query");
  CHECK_MSG(impacts.size() == 3, "one impact per axis");
  if (impacts.size() == 3) {
    // Each half carries the other's face point: the intersection face's centroid with
    // the axis coordinate on the emitted half's face plane. Depth 1 on every axis.
    CHECK_MSG(Near(impacts[0].otherNormal, Vec3{-1, 0, 0}) &&
                  Near(impacts[0].otherPoint, Vec3{1, 1.5f, 0.5f}) && Near(impacts[0].selfDepth, 1),
              "first (Rem x): other plane port x=1, centroid y 1.5 z 0.5");
    CHECK_MSG(Near(impacts[1].otherNormal, Vec3{0, 0, 1}) &&
                  Near(impacts[1].otherPoint, Vec3{1.5f, 1.5f, 1}) && Near(impacts[1].selfDepth, 1),
              "second (Rem y = up): other top port z=1, centroid x 1.5 y 1.5");
    CHECK_MSG(Near(impacts[2].otherNormal, Vec3{0, -1, 0}) &&
                  Near(impacts[2].otherPoint, Vec3{1.5f, 1, 0.5f}) && Near(impacts[2].otherDepth, 1),
              "third (Rem z = port y): other plane port y=1, centroid x 1.5 z 0.5");
    for (int i = 0; i < 3; ++i) {
      CHECK_MSG(Near(impacts[i].selfNormal,
                     Vec3{-impacts[i].otherNormal.x, -impacts[i].otherNormal.y,
                          -impacts[i].otherNormal.z}),
                "halves are component-wise negations");
    }
  }
  // Band (0.998, 1.0]: only the up impact is inside it, and it is a floor
  // (is_floor 0xbecb40: up = 1 > 0.85 whatever the orientation).
  CHECK_MSG(state.result && Near(state.low, 1.f), "the up impact grounds inside the band");

  // Band (0.498, 0.5]: the x and port-y impacts both sit at 0.5, the up one is above
  // feet. The first in source order (x) sets the result and the port-y one is skipped
  // by the strict `pz <= low`.
  p.feet = 0.5f;
  LandingState tie{0.498f, true};
  std::vector<PairedImpact> second;
  CHECK_MSG(Apply(p, tie, second), "second walk");
  CHECK_MSG(!tie.result && Near(tie.low, 0.5f),
            "an orientation-free horizontal contact is not a floor and sets low=0.5");
}

// ---------------------------------------------------------------------------
// The 0xc6d110 walk: strict band, is_floor from the OTHER half, terrain priority.
// ---------------------------------------------------------------------------
// The other box's TOP face lies inside the player box: one impact with up = +1, the
// other half's point on the other's top plane (0.5, 0.5, 1).
static Pair VerticalTop(float feet) {
  return Permissive(B(0, 0, 0, 1, 1, 2), B(0.2f, 0.2f, -0.5f, 0.8f, 0.8f, 1), kPlayerBits,
                    kActorBits, feet);
}

// The other box's BOTTOM face lies inside the player box: one impact with up = -1.
static Pair VerticalBottom(float feet) {
  return Permissive(B(0, 0, 0, 1, 1, 2), B(0.2f, 0.2f, 1, 0.8f, 0.8f, 3), kPlayerBits, kActorBits,
                    feet);
}

static void LandWalk() {
  {
    const Pair p = VerticalTop(1.f);
    std::vector<PairedImpact> impacts;
    LandingState state{0.998f, false};
    CHECK_MSG(Apply(p, state, impacts), "one admitted pair");
    CHECK_MSG(impacts.size() == 1, "exactly one impact");
    if (impacts.size() == 1) {
      CHECK_MSG(Near(impacts[0].otherNormal, Vec3{0, 0, 1}) &&
                    Near(impacts[0].otherPoint, Vec3{0.5f, 0.5f, 1}) && Near(impacts[0].selfDepth, 1),
                "other top plane, up normal, depth 1");
    }
    CHECK_MSG(state.result && Near(state.low, 1.f), "a point exactly at feet is inside the band");
  }
  {
    // 1 mm higher than feet: outside, the state stays where it was.
    const Pair p = VerticalTop(0.999f);
    std::vector<PairedImpact> impacts;
    LandingState state{0.997f, false};
    CHECK_MSG(Apply(p, state, impacts), "still a query");
    CHECK_MSG(!state.result && Near(state.low, 0.997f), "a point above feet does not ground");
  }
  {
    // A terrain point already at the same height: the strict `pz <= low` keeps it, so
    // the dynamic point does not replace the terrain's answer.
    const Pair p = VerticalTop(1.f);
    std::vector<PairedImpact> impacts;
    LandingState state{1.f, false};
    CHECK_MSG(Apply(p, state, impacts), "still a query");
    CHECK_MSG(!state.result && Near(state.low, 1.f), "a tie keeps the earlier point");
  }
  {
    // A terrain point below the dynamic one: the dynamic point supersedes it.
    const Pair p = VerticalTop(1.f);
    std::vector<PairedImpact> impacts;
    LandingState state{0.5f, false};
    CHECK_MSG(Apply(p, state, impacts), "still a query");
    CHECK_MSG(state.result && Near(state.low, 1.f), "the dynamic point grounds and raises low");
  }
  {
    // The other's bottom face inside the player: up = -1, and an orientation-free
    // material is not a floor, so it overwrites a terrain floor (0xbecb40).
    const Pair p = VerticalBottom(1.2f);
    std::vector<PairedImpact> impacts;
    LandingState state{0.5f, true};
    CHECK_MSG(Apply(p, state, impacts), "still a query");
    CHECK_MSG(impacts.size() == 1 && Near(impacts[0].otherNormal, Vec3{0, 0, -1}) &&
                  Near(impacts[0].otherPoint, Vec3{0.5f, 0.5f, 1}),
              "other bottom plane, down normal");
    CHECK_MSG(!state.result && Near(state.low, 1.f), "a down face is not a floor");
  }
  {
    // The same geometry with a Floor-oriented material: orientation 0 is a floor
    // whatever the normal says.
    Pair p = VerticalBottom(1.2f);
    p.otherMaterialBits = Bits({List::Solid, List::Floor});
    std::vector<PairedImpact> impacts;
    LandingState state{0.5f, false};
    CHECK_MSG(Apply(p, state, impacts), "still a query");
    CHECK_MSG(state.result && Near(state.low, 1.f), "orientation 0 is a floor");
  }
  {
    // The adapter reuses one impacts vector across ticks, so Apply replaces its
    // contents rather than appending to them. Capacity is kept after the first call;
    // no allocation-free guarantee is claimed for that first one.
    const Pair p = VerticalTop(1.f);
    std::vector<PairedImpact> stale(3);
    LandingState state{0.998f, false};
    CHECK_MSG(Apply(p, state, stale), "a reused scratch vector is still a query");
    CHECK_MSG(stale.size() == 1, "a stale scratch vector is replaced, not appended to");
  }
}

// ---------------------------------------------------------------------------
// The walk's material eligibility, 0xc6d358-0xc6d3b8.
// ---------------------------------------------------------------------------
// `result = FloorEligible(material) && is_floor(...)`, where a behaviour 0x10 material
// cannot ground unless its Platform channel 0x2 is set. NoPlayerCollision and
// Character-without-SolidCharacter both produce 0x110, i.e. both set 0x10; an accepted
// point still advances `low` when the eligibility is false.
static void FloorEligibility() {
  // The channel values the rule reads, pinned independently of the walk.
  CHECK_MSG((TranslateActor(Bits({List::Solid, List::NoPlayerCollision})).behavior & 0x10) != 0,
            "NoPlayerCollision sets behaviour 0x10");
  CHECK_MSG((TranslateActor(Bits({List::Solid, List::Character})).behavior & 0x10) != 0,
            "Character without SolidCharacter sets behaviour 0x10");
  CHECK_MSG((TranslateActor(Bits({List::Solid, List::Character, List::SolidCharacter})).behavior &
             0x10) == 0,
            "SolidCharacter clears it");
  CHECK_MSG((TranslateActor(Bits({List::Solid, List::Platform})).filter & 0x2) != 0,
            "Platform is the filter channel 0x2");
  CHECK_MSG(FloorEligible(TranslateActor(Bits({List::Solid, List::Immovable}))),
            "a plain actor is eligible");
  CHECK_MSG(!FloorEligible(TranslateActor(Bits({List::Solid, List::NoPlayerCollision}))),
            "NoPlayerCollision is not");
  CHECK_MSG(!FloorEligible(TranslateActor(Bits({List::Solid, List::Character}))),
            "Character without SolidCharacter is not");
  CHECK_MSG(FloorEligible(TranslateActor(Bits({List::Solid, List::Character, List::SolidCharacter}))),
            "SolidCharacter is");
  CHECK_MSG(FloorEligible(
                TranslateActor(Bits({List::Solid, List::NoPlayerCollision, List::Platform}))),
            "the Platform channel is the exception");
  // The exception is a MATERIAL channel (List::Platform -> filter 0x2), not the class:
  // the adapter still keeps CScriptPlatform instances off this path entirely (they are
  // a pre-selection rejection, covered by the selection-state machine below).

  struct Case {
    std::initializer_list< int > bits;
    bool floor;  // the up = +1 contact of VerticalTop(1.f)
  };
  const Case cases[] = {
      {{List::Solid, List::Immovable}, true},
      {{List::Solid, List::NoPlayerCollision}, false},
      {{List::Solid, List::Character}, false},
      {{List::Solid, List::Character, List::SolidCharacter}, true},
      {{List::Solid, List::NoPlayerCollision, List::Platform}, true},
      {{List::Solid, List::Character, List::Platform}, true},
  };
  for (const Case& c : cases) {
    Pair p = VerticalTop(1.f);
    p.otherMaterialBits = Bits(c.bits);
    std::vector<PairedImpact> impacts;
    LandingState state{0.5f, true};  // a terrain floor below the contact
    CHECK_MSG(Apply(p, state, impacts), "still a query");
    CHECK_MSG(impacts.size() == 1 && Near(impacts[0].otherNormal, Vec3{0, 0, 1}),
              "one up-facing impact");
    CHECK_MSG(state.result == c.floor, "the eligibility decides the result");
    CHECK_MSG(Near(state.low, 1.f), "an accepted point advances low either way");
  }
  {
    // The same ineligible material with no terrain floor underneath: the result stays
    // false while low still advances.
    Pair p = VerticalTop(1.f);
    p.otherMaterialBits = Bits({List::Solid, List::NoPlayerCollision});
    std::vector<PairedImpact> impacts;
    LandingState state{0.5f, false};
    CHECK_MSG(Apply(p, state, impacts), "still a query");
    CHECK_MSG(!state.result && Near(state.low, 1.f), "not a floor, but low advanced");
  }
}

// ---------------------------------------------------------------------------
// The accept test is ordered, so an unordered band accepts nothing.
// ---------------------------------------------------------------------------
static void UnorderedBand() {
  {
    Pair p = VerticalTop(kNaN);
    std::vector<PairedImpact> impacts;
    LandingState state{0.998f, false};
    CHECK_MSG(Apply(p, state, impacts), "a NaN band is still a query");
    CHECK_MSG(impacts.size() == 1, "the impact is still reported");
    CHECK_MSG(!state.result && Near(state.low, 0.998f), "a NaN feet accepts nothing");
  }
  {
    Pair p = VerticalTop(1.f);
    std::vector<PairedImpact> impacts;
    LandingState state{kNaN, false};
    CHECK_MSG(Apply(p, state, impacts), "a NaN low is still a query");
    CHECK_MSG(!state.result && std::isnan(state.low), "a NaN low accepts nothing");
  }
  CHECK_MSG(!FiniteBand(kNaN) && !FiniteBand(kInf) && FiniteBand(0.f), "band validity");
  CHECK_MSG(!FiniteOrdered(B(kInf, 0, 0, 1, 1, 1)), "an infinite min is rejected");
  CHECK_MSG(!FiniteOrdered(B(0, 0, 0, 1, 1, -kInf)), "a negative infinite max is rejected");
}

// ---------------------------------------------------------------------------
// Bounds validity and the pre-selection state machine the adapter drives.
// ---------------------------------------------------------------------------
static void BoundsAndSelection() {
  const Box ok = B(0, 0, 0, 1, 1, 1);
  CHECK_MSG(FiniteOrdered(ok), "a normal box is usable");
  CHECK_MSG(FiniteOrdered(B(1, 1, 1, 1, 1, 1)), "a degenerate point box is usable");
  CHECK_MSG(FiniteOrdered(B(-1e30f, -1e30f, -1e30f, 1e30f, 1e30f, 1e30f)), "large finite boxes");
  CHECK_MSG(!FiniteOrdered(B(0, 0, 0, -1, 1, 1)), "an inverted x is rejected");
  CHECK_MSG(!FiniteOrdered(B(0, 0, 0, 1, 1, -1)), "an inverted z is rejected");
  CHECK_MSG(!FiniteOrdered(B(kNaN, 0, 0, 1, 1, 1)), "a NaN min is rejected");
  CHECK_MSG(!FiniteOrdered(B(0, 0, 0, kNaN, 1, 1)), "a NaN max is rejected");
  CHECK_MSG(!FiniteOrdered(B(0, 0, 0, kInf, 1, 1)), "an infinite max is rejected");

  // No candidate: 0xbf61b4 can only report a pair whose boxes overlap, so the retail
  // producer has nothing to add either.
  {
    PairSelection s(true);
    CHECK_MSG(s.State() == SelectionState::kNone, "an empty near list is kNone");
    s.Candidate(Candidate::kOutside);
    CHECK_MSG(s.State() == SelectionState::kNone, "a non-overlapping candidate is not one");
  }
  {
    PairSelection s(true);
    s.Candidate(Candidate::kOutside);
    s.Candidate(Candidate::kOutside);
    s.Candidate(Candidate::kPair);
    CHECK_MSG(s.State() == SelectionState::kPair, "exactly one overlapping candidate is the pair");
  }
  {
    PairSelection s(true);
    s.Candidate(Candidate::kPair);
    s.Candidate(Candidate::kOutside);
    CHECK_MSG(s.State() == SelectionState::kPair, "later non-candidates do not reject it");
  }
  {
    PairSelection s(true);
    s.Candidate(Candidate::kPair);
    s.Candidate(Candidate::kPair);
    CHECK_MSG(s.State() == SelectionState::kUnsupported, "a second candidate rejects the whole pair");
  }
  {
    // The adapter's conservative gate (unsupported primitive, platform, inactive actor,
    // Door/Dock/BloodFlower) arrives as one rejection.
    PairSelection s(true);
    s.Candidate(Candidate::kUnsupportedPair);
    CHECK_MSG(s.State() == SelectionState::kUnsupported, "an ineligible candidate rejects the pair");
    s.Candidate(Candidate::kPair);
    CHECK_MSG(s.State() == SelectionState::kUnsupported, "and stays rejected");
  }
  {
    // A malformed candidate is rejected although its overlap cannot be evaluated: the
    // rejection must not depend on that comparison.
    PairSelection s(true);
    s.Candidate(Candidate::kUnsupportedPair);
    s.Candidate(Candidate::kOutside);
    CHECK_MSG(s.State() == SelectionState::kUnsupported, "malformed bounds reject before overlap");
  }
  {
    PairSelection s(false);  // a non-finite band
    CHECK_MSG(s.State() == SelectionState::kUnsupported, "an unusable band rejects the pair");
    s.Candidate(Candidate::kPair);
    CHECK_MSG(s.State() == SelectionState::kUnsupported, "a candidate cannot revive it");
  }
}
// ---------------------------------------------------------------------------
// Include{Wall} on a Wall-oriented material. The pair gate passes through the
// orientation term, but is_wall 0xbecbb0 is normal-dependent: on the up = +1
// vertical candidate it is false (up > 0.85), so that candidate is dropped and the
// minimum-difference fallback runs instead. Its normal is horizontal (up = 0), where
// is_wall is true for orientation 2, so the fallback is kept.
static Pair WallOriented() {
  Pair p = VerticalTop(1.f);
  p.otherMaterialBits = Bits({List::Wall});
  p.selfFilter = MaterialFilter{Bit(List::Wall), 0, 1};
  return p;
}

static void SurfaceGate() {
  {
    const Pair p = WallOriented();
    CHECK_MSG(PairPasses(p), "the pair gate passes a Wall material for Include{Wall}");
    std::vector<PairedImpact> impacts;
    LandingState state{0.998f, true};
    CHECK_MSG(Apply(p, state, impacts), "the pair is still queried");
    CHECK_MSG(impacts.size() == 1, "the vertical candidate is dropped, the fallback is not");
    if (impacts.size() == 1) {
      CHECK_MSG(Near(impacts[0].otherNormal, Vec3{-1, 0, 0}) &&
                    Near(impacts[0].otherPoint, Vec3{0.2f, 0.5f, 0.5f}) &&
                    Near(impacts[0].selfDepth, 0.8f),
                "fallback on the x axis, horizontal normal");
    }
    CHECK_MSG(state.result && Near(state.low, 0.998f),
              "the fallback point at z=0.5 is below the band and leaves the state alone");
  }
  {
    // The same pair under a permissive filter: the vertical candidate is accepted
    // first, so no fallback runs and the single impact grounds.
    Pair q = VerticalTop(1.f);
    q.otherMaterialBits = Bits({List::Wall});
    std::vector<PairedImpact> impacts;
    LandingState state{0.998f, true};
    CHECK_MSG(Apply(q, state, impacts), "permissive filter is a query");
    CHECK_MSG(impacts.size() == 1 && Near(impacts[0].otherNormal, Vec3{0, 0, 1}) && state.result &&
                  Near(state.low, 1.f),
              "and grounds on the same geometry");
  }
  {
    // A pair whose only candidate is the up-axis fallback: Include{Wall} drops it too,
    // so the walk is empty and the state keeps the terrain's answer.
    const Pair p = Permissive(B(0, 0, 0, 1, 1, 2), B(-1, -1, 0.5f, 2, 2, 1), kPlayerBits,
                              Bits({List::Wall}), 1.f);
    Pair wall = p;
    wall.selfFilter = MaterialFilter{Bit(List::Wall), 0, 1};
    CHECK_MSG(PairPasses(wall), "the pair gate still passes");
    std::vector<PairedImpact> impacts;
    LandingState state{0.998f, true};
    CHECK_MSG(Apply(wall, state, impacts), "the pair is still queried");
    CHECK_MSG(impacts.empty(), "both candidates rejected: an empty walk");
    CHECK_MSG(state.result && Near(state.low, 0.998f), "nothing walked, nothing changed");

    std::vector<PairedImpact> permissive;
    LandingState state2{0.998f, false};
    CHECK_MSG(Apply(p, state2, permissive), "the same geometry without the filter is a query");
    CHECK_MSG(permissive.size() == 1 && Near(permissive[0].otherNormal, Vec3{0, 0, 1}) &&
                  Near(permissive[0].otherPoint, Vec3{0.5f, 0.5f, 1}) && Near(permissive[0].selfDepth, 1),
              "the fallback lands on the up axis instead");
    CHECK_MSG(state2.result && Near(state2.low, 1.f), "and grounds");
  }
}

// ---------------------------------------------------------------------------
// Face point vs the retail corner rule, incl. the terrain overwrite it causes.
// ---------------------------------------------------------------------------
// Equal minimums on every axis: the source emits no axis impact and falls back to ONE
// impact on the minimum-difference axis (x, -1 here), depth 2. The other half's point
// takes the intersection face's centroid vertically (z = 1) with the axis coordinate on
// the other's own face plane (port x = 0). The retail producer instead emits one contact
// per axis with the whole overlap box as its bounds (CollisionUtil.cpp:511-559, flags
// 11), whose -z contact walks the corners z = 0 and z = 2.
static void FacePointVsRetailCorners() {
  const Pair p = Permissive(B(0, 0, 0, 2, 2, 2), B(0, 0, 0, 3, 3, 3), kPlayerBits, kActorBits,
                            2.f);
  std::vector<PairedImpact> impacts;
  LandingState state{1.999f, true};
  CHECK_MSG(Apply(p, state, impacts), "the fallback pair is still a query");
  CHECK_MSG(impacts.size() == 1, "the fallback emits exactly one impact");
  if (impacts.size() == 1) {
    CHECK_MSG(Near(impacts[0].otherPoint, Vec3{0, 1, 1}) && Near(impacts[0].selfDepth, 2) &&
                  Near(impacts[0].otherNormal, Vec3{-1, 0, 0}) &&
                  Near(impacts[0].selfPoint, Vec3{2, 1, 1}),
              "fallback other-half point (0, 1, 1): the overlap centroid, not a box corner");
  }
  // The point sits at z = 1, below the terrain's low, so the terrain's answer stands.
  // The retail walk's -z contact would take its corner at z = 2 (inside the
  // (1.999, 2.0] band), classify it with the DOWN normal and overwrite the result with
  // false and low with 2.
  CHECK_MSG(state.result && Near(state.low, 1.999f), "terrain low/result survive the query");
}

int main() {
  PairGate();
  ThreeImpacts();
  LandWalk();
  FloorEligibility();
  UnorderedBand();
  BoundsAndSelection();
  SurfaceGate();
  FacePointVsRetailCorners();
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_box_landing: ok\n");
  }
  return PORT_TEST_RESULT();
}
