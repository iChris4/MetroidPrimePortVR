#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "port_collision_failsafe.h"
#include "port_remastered_collision_material.h"

// Remastered's box-actor landing query for the MP1 airborne mover (issue #117):
// CPlayerMP1::GroundCheckForOffGroundMoverCollider 0xc6d110 for one box (AABX)
// candidate.
//
// SCOPE: this is the SOURCE BOX LANDING half of that check only. Nothing here
// reproduces the airborne mover's response half (which still runs over the port's
// native `acceptedDynamic` contacts), the terrain/triangle half of the same walk
// (which still classifies with CGameCollision's approximate raw-bits Classify), the
// rider or retained-object-state paths, or the retail fallback producer. A grounded
// verdict from this file is the box-pair landing verdict and nothing wider.
//
// Three source pieces, nothing else:
//
//  * the pair gate. CheckCollision 0xc1ec60 leaves an object out of the list
//    entirely unless the object's OWN collision filter (CPhysicsActorMP1
//    SetupPhysicsObject 0xc529f0: +0x1270) passes us (NPhysicsCollision
//    0xc1f110), and CCollidableAABox::AABox_AABox 0xbf61b4 then only counts a
//    pair when OUR filter passes its collision material (step 2). Both run
//    through CIncludeExcludeCollisionFilterMP1::Passes 0xcb7084, against the
//    TRANSLATED material (CPhysicsActorMP1::MaterialListChanged 0xc53ccc ->
//    NGamePhysicsUtilsMP1::TranslateMP1Material 0xc59b70), never against the
//    port's raw CMaterialList masks.
//    The SHIPPED failsafe query (port_remastered_movers.cpp, Failsafe::Other) has the same
//    two steps in the same order, but it feeds them the port's retail raw-mask
//    CMaterialFilter::Passes(CMaterialList); it is not a precedent for the
//    translated-material form used here, and the raw-mask versus translated
//    difference is a known open gap in the port's dynamic-material handling.
//    ONE SOURCE EXCEPTION the caller must keep out of this path: 0xc1ed44-0xc1ed4c
//    reads the near object's physics object type (+0xc) and, for type 2, branches
//    straight to the pair collision, skipping the other-filter visit test above.
//    SetupPhysicsObject 0xc529f0 gives Door, Dock and BloodFlower type 2,
//    so those classes must stay on the unchanged
//    retail fallback: PairPasses below always applies that test and must not be
//    weakened, and this header invents no native physics-type mapping.
//  * the impact set: CCollidableAABox::AABox_AABox 0xbf61b4 through 0xbf6650
//    (BoxBoxPairedImpacts in port_collision_failsafe.h), with the per-candidate
//    SurfaceCollisionPasses 0xcb7110 on the OTHER object's half.
//  * the landing walk 0xc6d110: every contact point of the other half, in source
//    order, where `pz <= feet && pz > low` sets
//    `result = FloorEligible(other material) && is_floor(other material, other
//    half's normal)` (NCollisionUtils::is_floor 0xbecb40) and `low = pz`. Both
//    compares are ordered, so a tie keeps the earlier point, and an accepted point
//    advances `low` even when the result is false. `low` starts at
//    target - GetStepDownHeight() - 0.002 (0xc6d110's `fVar13 - 0.002`, see
//    PortAirMoverContact::GroundCheckBand) and the caller
//    hands in whatever the terrain half already found, which is how terrain points
//    keep priority over dynamic ones.
//
// Everything is decided from fixed inputs: the adapter supplies the two boxes at the
// probed position, the two actor material LISTS (raw EMaterialTypes bits), the two
// CMaterialFilter mode/list triples (mode via CMaterialFilter::GetFilterType,
// EFilterType 0..3 == the source filter's mode +0x10) and the feet bound. Nothing
// here reads gameplay state, and nothing here uses CGameCollision's approximate
// raw-bits Classify.
//
// Port frame (Z up) throughout, exactly like the failsafe helpers; 0xbf61b4
// converts to Remastered's Y-up frame internally, so source Y == port Z and every
// vertical component below (the impact normal's, the face point's) is a port Z.
//
// Source decision record: pair gate 0xc1ec60 (visit test) + 0xc1f110/0xcb7084, impact
// set 0xbf61b4-0xbf6650 (0xcb7110 per candidate), landing walk 0xc6d110 with the accept
// test and the accumulating low/result at 0xc6d3d0-0xc6d3ec, is_floor 0xbecb40.
namespace PortRemasteredBoxLanding {

using PortCollisionFailsafe::Box;
using PortCollisionFailsafe::PairedImpact;

// One candidate pair, as the adapter finds it. `selfBox`/`otherBox` are the two
// collided primitives' AABBs at the probed position (0xbf61b4 transforms its own
// boxes by the transforms it is handed, so the caller passes them already placed).
struct Pair {
  Box selfBox;
  Box otherBox;
  uint64_t selfMaterialBits = 0;  // CMaterialList::GetValue() of the self actor
  uint64_t otherMaterialBits = 0;
  PortRemasteredCollisionMaterial::MaterialFilter selfFilter;   // self's +0x1270
  PortRemasteredCollisionMaterial::MaterialFilter otherFilter;  // other's +0x1270
  float feet = 0.f;  // the landing band's inclusive upper bound
};

// 0xc6d110's accumulator, as the caller carries it between the terrain half and
// the dynamic half: `low` starts at feet - 0.002 and `result` at false, and only a
// contact point accepted by `pz <= feet && pz > low` moves either.
struct LandingState {
  float low = 0.f;
  bool result = false;
};

// ---------------------------------------------------------------------------
// Bounds and band validity: a PORT-conservative precondition, not a source test.
// ---------------------------------------------------------------------------
// The kernel (0xbf61b4 in port_collision_failsafe.h) documents "all coordinates
// finite and min <= max on every axis" as its precondition and applies no epsilon or
// NaN handling. A malformed box is not a comparison the
// source query can be trusted to make, so the adapter rejects the pair to the whole
// retail fallback before the overlap test can turn a malformed candidate into a
// silent "no candidate" verdict.
inline bool FiniteOrdered(const Box& b) {
  return std::isfinite(b.min.x) && std::isfinite(b.min.y) && std::isfinite(b.min.z) &&
         std::isfinite(b.max.x) && std::isfinite(b.max.y) && std::isfinite(b.max.z) &&
         b.min.x <= b.max.x && b.min.y <= b.max.y && b.min.z <= b.max.z;
}
// The band the walk compares against has the same requirement: a non-finite feet
// bound makes every ordered compare false.
inline bool FiniteBand(float feet) { return std::isfinite(feet); }

// ---------------------------------------------------------------------------
// Material-dependent floor eligibility, 0xc6d358-0xc6d3b8.
// ---------------------------------------------------------------------------
// The MP1 Platform channel 0x2 (NGamePhysicsUtilsMP1::TranslatePositive 0xc59c20,
// `List::Platform` -> filter 0x2).
constexpr uint32_t kPlatformChannel = 0x2;
// The behaviour bit that the 0x110 producers set: NoPlayerCollision writes 0x110 and
// Character without SolidCharacter ORs the same value in (MaterialListChanged
// 0xc53ccc), so both set 0x10.
constexpr uint32_t kNoPlayerBehaviorBit = 0x10;

// The walk's material half: a material whose behaviour has 0x10 cannot ground this
// pair unless its filter carries the Platform channel 0x2, which is the explicit
// exception. Everything else still has to pass is_floor below.
constexpr bool FloorEligible(const PortRemasteredCollisionMaterial::CollisionMaterial& m) {
  return (m.behavior & kNoPlayerBehaviorBit) == 0 || (m.filter & kPlatformChannel) != 0;
}

// ---------------------------------------------------------------------------
// The pair gate and the landing query.
// ---------------------------------------------------------------------------
// 0x98234 DoBoundsOverlap (port CAABox::DoBoundsOverlap): false only when some axis
// separates, so touching counts as overlapping. This is the same inclusive test
// 0xbf61b4 applies to a pair, so the adapter can pre-select with it.
inline bool BoxesOverlap(const Box& a, const Box& b) {
  if (b.min.x > a.max.x || a.min.x > b.max.x) return false;
  if (b.min.y > a.max.y || a.min.y > b.max.y) return false;
  if (b.min.z > a.max.z || a.min.z > b.max.z) return false;
  return true;
}

// The two-way gate both boxes' filters and the pair's translated materials go
// through: the other's own filter against OUR material first (the CheckCollision
// visit rule, minus the type-2 bypass the caller must keep out of this path), then
// our filter against ITS material (0xbf61b4 step 2 - the self side's own material is
// never filtered). Both calls are the non-surface Passes 0xcb7084, so a mode-0 filter
// passes every material while an orientation-only include/exclude list is judged by
// the guarded orientation form.
inline bool PairPasses(const Pair& pair) {
  using namespace PortRemasteredCollisionMaterial;
  const CollisionMaterial selfMat = TranslateActor(pair.selfMaterialBits);
  const CollisionMaterial otherMat = TranslateActor(pair.otherMaterialBits);
  if (!Passes(pair.otherFilter, selfMat)) return false;
  return Passes(pair.selfFilter, otherMat);
}

// The source landing query for ONE pre-selected pair with FiniteOrdered boxes.
// `impacts` is filled with the source impacts in source order (Remastered axes x, y,
// z; +1 side before -1) and is reused as the caller's scratch: that avoids repeated
// growth after warm-up, but the first call (and the std::function the kernel
// receives) may allocate - there is no allocation-free guarantee.
//
// Returns false when the pair is not a query the source would make at all - a gate
// above rejects it, or the boxes do not overlap inclusively - and then `state` and
// `impacts` are untouched. Returns true when the pair is queried: `state` is then the
// result of walking its impacts. The walk may also be empty: boxes that only touch
// emit nothing, every candidate can be dropped by the per-candidate filter, and a
// candidate rejected by the depth test falls through to the minimum-difference
// fallback, so even full containment emits one impact (see the face-point order in
// port_collision_failsafe.cpp) - but a query that ends with no accepted point leaves
// the state alone.
inline bool Apply(const Pair& pair, LandingState& state,
                  std::vector< PairedImpact >& impacts) {
  using namespace PortRemasteredCollisionMaterial;
  if (!PairPasses(pair)) return false;
  if (!BoxesOverlap(pair.selfBox, pair.otherBox)) return false;
  const CollisionMaterial otherMat = TranslateActor(pair.otherMaterialBits);
  impacts.clear();
  if (!PortCollisionFailsafe::BoxBoxPairedImpacts(
          pair.selfBox, pair.otherBox,
          [&pair, &otherMat](const PortCollisionFailsafe::Vec3& otherNormal) {
            // 0xbf61b4 calls SurfaceCollisionPasses on the OTHER object with the
            // component-wise negation of the self half's normal, so the predicate's
            // vector is the other half's own outward normal (and therefore carries
            // the up component the landing walk reads). One call per candidate,
            // including the minimum-difference fallback candidate.
            return SurfacePasses(pair.selfFilter, otherMat, otherNormal.z);
          },
          impacts)) {
    // Nothing to walk: no impact qualified on any axis and the fallback candidate
    // was rejected too (or the boxes only touch).
    return true;
  }
  for (std::size_t i = 0; i < impacts.size(); ++i) {
    const float pz = impacts[i].otherPoint.z;
    // The source's accept test, positive and ordered (0xc6d3d0 fcmp/cset ls against
    // feet, 0xc6d3d8 fcmp/cset gt against low, 0xc6d3e0 tst and 0xc6d3ec csel):
    // `pz <= feet` (LS) and `pz > low` (GT). GT is the N/V encoding and so
    // unordered-false, meaning an unordered pz or band fails both compares and the
    // point is skipped; the negated disjunction `pz > feet || pz <= low` (HI, the
    // C/Z encoding, unordered-true) would accept it instead.
    if (pz <= pair.feet && pz > state.low) {
      state.result = FloorEligible(otherMat) &&
                     IsFloor(otherMat.orientation, impacts[i].otherNormal.z);
      // An accepted point advances the running low even when it is not a floor.
      state.low = pz;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Pre-selection: the adapter's decision, in the near list's order.
// ---------------------------------------------------------------------------
// Every value below is a PORT-conservative gate, not a source physics-type test. The
// adapter alone owns the classifications (it is the only side that can see
// CPhysicsActor, CScriptPlatform or the port's Door/Dock/BloodFlower classes); this
// state machine owns only the ORDER and the counting the source's near-list walk
// implies.
enum class Candidate {
  // The whole pair is rejected: bounds that cannot be compared, or a class,
  // primitive or native state this query does not claim. The adapter then runs the
  // unchanged retail block.
  kUnsupportedPair,
  // Valid bounds that do not overlap the probed box: not a candidate at all.
  kOutside,
  // One eligible, bounds-overlapping candidate.
  kPair,
};
enum class SelectionState { kNone, kPair, kUnsupported };

// Exactly one kPair candidate and no kUnsupportedPair admits the source query; no
// candidate at all is kNone (0xbf61b4 can only report a pair whose boxes overlap);
// any rejection is kUnsupported. A malformed candidate must be reported as
// kUnsupportedPair BEFORE the overlap comparison, so a comparison accident cannot
// turn it into kNone.
class PairSelection {
public:
  explicit PairSelection(bool bandUsable) : mUnsupported(!bandUsable) {}

  void Candidate(PortRemasteredBoxLanding::Candidate c) {
    if (mUnsupported) return;
    if (c == Candidate::kUnsupportedPair) {
      mUnsupported = true;
      return;
    }
    if (c == Candidate::kOutside) return;
    if (mHave) {
      mUnsupported = true;  // a second overlapping candidate
      return;
    }
    mHave = true;
  }

  SelectionState State() const {
    if (mUnsupported) return SelectionState::kUnsupported;
    return mHave ? SelectionState::kPair : SelectionState::kNone;
  }

private:
  bool mUnsupported = false;
  bool mHave = false;
};

}  // namespace PortRemasteredBoxLanding
