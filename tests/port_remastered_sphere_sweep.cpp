// Pure geometry of NCollidablePairs::Sphere_Sphere_Swept 0xc012d0
// (platform/port_remastered_sphere_sweep.cpp) and of the CVector3f::NormalizeWithMagnitude
// helper 0x6b100 it shares with CCollidableSphere::Sphere_Sphere 0xc029f0.
// The geometry cases are the kernel's hand-computable ones (S1/S2/S3/S7, O1/O2, S4/S5,
// the tangent start and the tie), read against the raw disassembly of 0xc012d0,
// 0xc042b0, 0xbf69d0 and 0xc029f0; three of the spec prose claims they come from are
// corrected rather than transcribed: O2's "same numbers as S2" cross-reference (its own
// radii 1+1 at dx = 2 is the true strict-boundary case; S2's radii sum to 1 and that
// start is simply separated), "every tp is exact" (S3's disc = 3.75 is irrational), and
// "P0 == P1 exactly", which holds to within an ulp rather than bit for bit.
// The fixture rows at the bottom pin 0x6b100 itself against recorded AArch64 results.
#include "port_remastered_sphere_sweep.h"

#include "port_remastered_ground_contacts.h"
#include "port_test.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using PortRemasteredSphereSweep::Impact;
using PortRemasteredSphereSweep::Sphere;
using PortRemasteredSphereSweep::SphereSphereSwept;
using PortRemasteredSphereSweep::kMiss;
using PortRemasteredSphereSweep::kOverlap;
using PortRemasteredSphereSweep::kSweep;
using Vec3 = PortRemasteredSphereSweep::Vec3;

namespace {
// The spec's numbers are source/Remastered-frame (Y up); the API is the port's Z up.
// This is the documented mapping and its own inverse
// (platform/port_remastered_ground_world.cpp:30-31):
// Remastered = (-port.x, port.z, port.y). `Rem` builds a port-frame value from
// Remastered coordinates; `ToRem` brings a port-frame result back for comparison.
Vec3 Rem(float x, float y, float z) { return {-x, z, y}; }
Vec3 ToRem(const Vec3& port) { return {-port.x, port.z, port.y}; }

// Values that come out of the ARM estimate path are bounded rather than compared bit
// for bit, because this repo holds no recorded AArch64 fixture for the *sphere* kernel's
// own arithmetic: its tp goes through CMath::SqrtF 0x65f70, and 0x6b100's fixture only
// covers the normalizer (pinned below, where bit-equality IS used). Observed on this
// tree: <= 1 ULP of the value (e.g. |c1-c0| = sqrt(25.25) comes back 5.02493763 vs
// 5.02493781056), so 1e-6 is about 4 ULP at magnitude 5 - a bound, not a rounding claim.
constexpr float kEstimate = 1e-6f;
// tp = b - CMath::SqrtF(disc) goes through that same estimate path; only the irrational
// discriminants differ from exact f32 arithmetic (S3 is 1 ULP below, S1/S2/S7 and the
// tangent start are exact).
constexpr float kSqrt = 1e-6f;

bool Near(float a, float b, float e = kEstimate) { return std::fabs(a - b) <= e; }
bool Near(const Vec3& a, const Vec3& b, float e = kEstimate) {
  return Near(a.x, b.x, e) && Near(a.y, b.y, e) && Near(a.z, b.z, e);
}
std::uint32_t Bits(float f) {
  std::uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
float FromBits(std::uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
bool SameBits(float a, float b) { return Bits(a) == Bits(b); }

// A caller's list already holding a previous pair's contacts.
std::vector< Impact > Seeded() {
  Impact dummy;
  dummy.selfPoint = {1.f, 2.f, 3.f};
  dummy.otherPoint = {-4.f, -5.f, -6.f};
  dummy.selfNormal = {.5f, .25f, -.125f};
  dummy.otherNormal = {-.5f, -.25f, .125f};
  dummy.depth = .75f;
  return {dummy, dummy};
}
bool SameImpacts(const std::vector< Impact >& a, const std::vector< Impact >& b) {
  return a.size() == b.size() &&
         std::memcmp(a.data(), b.data(), a.size() * sizeof(Impact)) == 0;
}
// Every rejection must leave BOTH outputs untouched, bit for bit.
void CheckUntouched(int code, float t, const std::vector< Impact >& got,
                    const std::vector< Impact >& seed, float seededT) {
  REQUIRE(code == kMiss);
  REQUIRE(SameBits(t, seededT));
  REQUIRE(SameImpacts(got, seed));
}

const Sphere kSelfOrigin{Rem(0.f, 0.f, 0.f), 1.f};
const Vec3 kDirX = Rem(1.f, 0.f, 0.f);
const Sphere kOtherX5{Rem(5.f, 0.f, 0.f), 1.f};

void Overlap() {
  // O1: radii 1+1 with centres 1 apart. The strict boolean is true, so the list is
  // cleared and t = 0 first, then the producer emits one impact from the START centres.
  {
    std::vector< Impact > impacts = Seeded();
    float t = 7.f;
    REQUIRE(SphereSphereSwept(kSelfOrigin, {Rem(1.f, 0.f, 0.f), 1.f}, kDirX, true, t,
                              impacts) == kOverlap);
    REQUIRE(t == 0.f);
    REQUIRE(impacts.size() == 1); // the seed is gone, nothing appended to it
    const Impact& i = impacts[0];
    REQUIRE(Near(ToRem(i.selfNormal), {1.f, 0.f, 0.f}));
    REQUIRE(Near(ToRem(i.otherNormal), {-1.f, 0.f, 0.f}));
    REQUIRE(Near(ToRem(i.selfPoint), {1.f, 0.f, 0.f}));  // c0 + r0*n
    REQUIRE(Near(ToRem(i.otherPoint), {0.f, 0.f, 0.f})); // c1 - r1*n
    REQUIRE(Near(i.depth, 1.f));                         // (r0+r1) - |c1-c0| = 2 - 1
  }
  // Strict-boundary start: centres 2 apart with radii 1+1 makes 4 < 4 false, so this is
  // NOT an overlap (the spec's O2 cross-reference to S2 is wrong; these are O2's own
  // numbers). The sweep runs with target radius 2 and hits at tp = 2 - SqrtF(4) = 0.
  {
    std::vector< Impact > impacts = Seeded();
    float t = 7.f;
    REQUIRE(SphereSphereSwept(kSelfOrigin, {Rem(2.f, 0.f, 0.f), 1.f}, kDirX, true, t,
                              impacts) == kSweep);
    REQUIRE(Near(t, 0.f, kSqrt));
    REQUIRE(impacts.size() == 1);
    const Impact& i = impacts[0];
    REQUIRE(Near(ToRem(i.selfNormal), {1.f, 0.f, 0.f}));
    REQUIRE(Near(ToRem(i.selfPoint), {1.f, 0.f, 0.f}));
    REQUIRE(Near(ToRem(i.otherPoint), {1.f, 0.f, 0.f})); // P0 and P1 coincide at r0*n
    REQUIRE(Near(i.depth, 0.f, kEstimate));              // the exact-tangent start: s = 0
  }
  // S4 coincident centres, radii 1+1: the boolean is true, so the list is cleared and
  // t = 0, but IsNormalizable(delta = 0) fails and NO contact is emitted.
  {
    std::vector< Impact > impacts = Seeded();
    float t = 7.f;
    REQUIRE(SphereSphereSwept(kSelfOrigin, {Rem(0.f, 0.f, 0.f), 1.f}, kDirX, true, t,
                              impacts) == kOverlap);
    REQUIRE(t == 0.f);
    REQUIRE(impacts.empty()); // cleared unconditionally, not left holding the seed
  }
  // S5 coincident centres with radii 0+0: 0 < 0 is false, and the zero-radius ray has
  // b = 0 and disc = 0, so it misses. Nothing is touched.
  {
    std::vector< Impact > impacts = Seeded();
    const std::vector< Impact > seed = impacts;
    float t = 7.f;
    const int code = SphereSphereSwept({Rem(0.f, 0.f, 0.f), 0.f},
                                       {Rem(0.f, 0.f, 0.f), 0.f}, kDirX, true, t, impacts);
    CheckUntouched(code, t, impacts, seed, 7.f);
  }
  // A false pair gate returns 0 before anything else.
  {
    std::vector< Impact > impacts = Seeded();
    const std::vector< Impact > seed = impacts;
    float t = 7.f;
    const int code =
        SphereSphereSwept(kSelfOrigin, {Rem(1.f, 0.f, 0.f), 1.f}, kDirX, false, t, impacts);
    CheckUntouched(code, t, impacts, seed, 7.f);
  }
}

float gReferenceT = 0.f;
float gReferenceDepth = 0.f;

void Sweep() {
  // S1 axis hit: tp = 5 - SqrtF(4), m = |c1-c0| = 5, depth = 2 - 5 = -3, and both
  // points sit at d*|t| because the centre line is parallel to the direction.
  {
    std::vector< Impact > impacts = Seeded();
    float t = 10.f;
    REQUIRE(SphereSphereSwept(kSelfOrigin, kOtherX5, kDirX, true, t, impacts) == kSweep);
    REQUIRE(Near(t, 3.f, kSqrt));
    REQUIRE(impacts.size() == 1);
    const Impact& i = impacts[0];
    REQUIRE(Near(ToRem(i.selfNormal), {1.f, 0.f, 0.f}, 1e-6f));
    REQUIRE(Near(ToRem(i.otherNormal), {-1.f, 0.f, 0.f}, 1e-6f));
    REQUIRE(Near(ToRem(i.selfPoint), {4.f, 0.f, 0.f}, 1e-6f));
    REQUIRE(Near(ToRem(i.otherPoint), {4.f, 0.f, 0.f}, 1e-6f));
    REQUIRE(Near(i.selfPoint, i.otherPoint, 1e-6f)); // structural: P0 == P1 here
    REQUIRE(i.depth < 0.f);
    REQUIRE(Near(i.depth, -3.f, kEstimate));
    REQUIRE(Near(i.depth, 2.f - std::sqrt(5.f * 5.f), kEstimate));
    // Signed zeros: the negation is done on the Remastered normal (fneg, so every zero
    // lane becomes -0) and the conversion back negates x again, which is why the
    // otherNormal's zero lanes carry the sign bit while selfNormal's do not.
    REQUIRE(Bits(i.selfNormal.x) == Bits(-1.f) && Bits(i.selfNormal.y) == 0u &&
            Bits(i.selfNormal.z) == 0u);
    REQUIRE(Bits(i.otherNormal.x) == Bits(1.f) && Bits(i.otherNormal.y) == 0x80000000u &&
            Bits(i.otherNormal.z) == 0x80000000u);
    gReferenceT = t;
    gReferenceDepth = i.depth;
  }
  // S1b exact tie: the source accepts only tp < *t, so an equal tp rejects with both
  // outputs untouched (this kernel has no 1e-5 tie window). Using the run's own tp
  // keeps the test independent of the SqrtF estimate's last ulp.
  {
    std::vector< Impact > impacts = Seeded();
    const std::vector< Impact > seed = impacts;
    float t = gReferenceT;
    const int code = SphereSphereSwept(kSelfOrigin, kOtherX5, kDirX, true, t, impacts);
    CheckUntouched(code, t, impacts, seed, gReferenceT);
  }
  // A window strictly inside leaves the equal case a rejection but accepts any larger t.
  {
    std::vector< Impact > impacts = Seeded();
    float t = gReferenceT + 1e-4f;
    REQUIRE(SphereSphereSwept(kSelfOrigin, kOtherX5, kDirX, true, t, impacts) == kSweep);
    REQUIRE(SameBits(t, gReferenceT));
  }
  // S1c: tp > t (t = 2). Nothing touched.
  {
    std::vector< Impact > impacts = Seeded();
    const std::vector< Impact > seed = impacts;
    float t = 2.f;
    const int code = SphereSphereSwept(kSelfOrigin, kOtherX5, kDirX, true, t, impacts);
    CheckUntouched(code, t, impacts, seed, 2.f);
  }
  // S1d: the ray points away from the other sphere (b = -5 < 0). Nothing touched.
  {
    std::vector< Impact > impacts = Seeded();
    const std::vector< Impact > seed = impacts;
    float t = 10.f;
    const int code =
        SphereSphereSwept(kSelfOrigin, kOtherX5, Rem(-1.f, 0.f, 0.f), true, t, impacts);
    CheckUntouched(code, t, impacts, seed, 10.f);
  }
  // S2 Y axis, tangent-radius start (radii .5+.5 at two apart: separated, not the
  // boundary the spec prose claims): disc = 1 - (4 - 4) = 1, tp = 1, m = 2, depth = -1,
  // and the contact sits at (0, 1.5, 0).
  {
    std::vector< Impact > impacts = Seeded();
    float t = 4.f;
    REQUIRE(SphereSphereSwept({Rem(0.f, 0.f, 0.f), .5f}, {Rem(0.f, 2.f, 0.f), .5f},
                              Rem(0.f, 1.f, 0.f), true, t, impacts) == kSweep);
    REQUIRE(Near(t, 1.f, kSqrt));
    REQUIRE(impacts.size() == 1);
    const Impact& i = impacts[0];
    REQUIRE(Near(ToRem(i.selfNormal), {0.f, 1.f, 0.f}, 1e-6f));
    REQUIRE(Near(ToRem(i.otherNormal), {0.f, -1.f, 0.f}, 1e-6f));
    REQUIRE(Near(ToRem(i.selfPoint), {0.f, 1.5f, 0.f}, 1e-6f));
    REQUIRE(Near(ToRem(i.otherPoint), {0.f, 1.5f, 0.f}, 1e-6f));
    REQUIRE(Near(i.depth, -1.f, kEstimate));
  }
  // S7 diagonal: b = 10 and disc = 4 are exact, tp = 8, m = 10, depth = -8, contact at
  // 8*d + n = (5.4, 7.2, 0).
  {
    std::vector< Impact > impacts = Seeded();
    float t = 20.f;
    REQUIRE(SphereSphereSwept(kSelfOrigin, {Rem(6.f, 8.f, 0.f), 1.f}, Rem(.6f, .8f, 0.f),
                              true, t, impacts) == kSweep);
    REQUIRE(Near(t, 8.f, kSqrt));
    REQUIRE(impacts.size() == 1);
    const Impact& i = impacts[0];
    REQUIRE(Near(ToRem(i.selfNormal), {.6f, .8f, 0.f}, 1e-6f));
    REQUIRE(Near(ToRem(i.selfPoint), {5.4f, 7.2f, 0.f}, 1e-5f));
    REQUIRE(Near(ToRem(i.otherPoint), {5.4f, 7.2f, 0.f}, 1e-5f));
    REQUIRE(Near(i.depth, -8.f, kEstimate));
  }
  // S3 start-centre normal (the TOI discriminator): the normal and the depth come from
  // delta = c1 - c0, never from c1 - (c0 + tp*d). tp = 5 - SqrtF(3.75) is irrational,
  // so it is bounded rather than bit-compared (the "every tp is exact" claim it comes
  // from is wrong for an irrational discriminant).
  {
    std::vector< Impact > impacts = Seeded();
    float t = 10.f;
    REQUIRE(SphereSphereSwept(kSelfOrigin, {Rem(5.f, .5f, 0.f), 1.f}, kDirX, true, t,
                              impacts) == kSweep);
    REQUIRE(Near(t, 3.0635083f, kSqrt));
    REQUIRE(impacts.size() == 1);
    const Impact& i = impacts[0];
    const Vec3 n = ToRem(i.selfNormal);
    REQUIRE(Near(n, {0.9950372f, 0.09950373f, 0.f}, 1e-6f));
    REQUIRE(Near(ToRem(i.otherNormal), {-0.9950372f, -0.09950373f, 0.f}, 1e-6f));
    // What a TOI-based normal would have produced: unit(c1 - (c0 + tp*d)) =
    // (0.96824586, 0.25, 0). Orders apart from the correct answer, so a few ULP of
    // tolerance cannot hide such an implementation.
    const Vec3 toiNormal{0.96824586f, 0.25f, 0.f};
    REQUIRE(std::fabs(n.x - toiNormal.x) > 1e-3f);
    REQUIRE(std::fabs(n.y - toiNormal.y) > 1e-3f);
    REQUIRE(Near(ToRem(i.selfPoint), {4.0585456f, 0.09950373f, 0.f}, kEstimate));
    REQUIRE(Near(ToRem(i.otherPoint), {4.004963f, 0.40049627f, 0.f}, kEstimate));
    REQUIRE(Near(i.depth, -3.0249376f, kEstimate));
  }
  // Exact tangency on the swept target misses: 4 - (29 - 25) = 0, and 0 < disc is false.
  {
    std::vector< Impact > impacts = Seeded();
    const std::vector< Impact > seed = impacts;
    float t = 10.f;
    const int code = SphereSphereSwept(kSelfOrigin, {Rem(5.f, 2.f, 0.f), 1.f}, kDirX, true,
                                       t, impacts);
    CheckUntouched(code, t, impacts, seed, 10.f);
  }
  // Non-origin positions: an integer translation leaves delta exactly (5, 0, 0), so the
  // distance and the depth must be BIT-identical to the origin case above, while the
  // points move with the geometry.
  {
    const Vec3 c0 = Rem(1000.f, -256.f, 7.f);
    const Vec3 step = Rem(5.f, 0.f, 0.f);
    const Vec3 c1{c0.x + step.x, c0.y + step.y, c0.z + step.z};
    std::vector< Impact > impacts = Seeded();
    float t = 10.f;
    REQUIRE(SphereSphereSwept({c0, 1.f}, {c1, 1.f}, kDirX, true, t, impacts) == kSweep);
    REQUIRE(SameBits(t, gReferenceT));
    REQUIRE(impacts.size() == 1);
    REQUIRE(SameBits(impacts[0].depth, gReferenceDepth));
    REQUIRE(Near(ToRem(impacts[0].selfNormal), {1.f, 0.f, 0.f}, 1e-6f));
    REQUIRE(Near(ToRem(impacts[0].selfPoint), {1004.f, -256.f, 7.f}, 1e-3f));
    REQUIRE(Near(ToRem(impacts[0].otherPoint), {1004.f, -256.f, 7.f}, 1e-3f));
  }
  // Axis permutations and zero components: the same 5-apart radii 1+1 geometry along
  // each of the six port axes. The Remastered framing permutes which lanes the squared
  // sum and the fmulx touch, but delta is 5 on one axis with zeros on the others, so
  // the squared sum stays exactly 25: distance and depth are bit-identical to the
  // reference case, and the vectors are checked as values plus the negation contract's
  // signed zeros.
  {
    const Vec3 axes[6] = {{1.f, 0.f, 0.f}, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f},
                          {0.f, -1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f, -1.f}};
    for (const Vec3& axis : axes) {
      const Vec3 far{axis.x * 5.f, axis.y * 5.f, axis.z * 5.f};
      std::vector< Impact > impacts = Seeded();
      float t = 10.f;
      REQUIRE(SphereSphereSwept({Rem(0.f, 0.f, 0.f), 1.f}, {far, 1.f}, axis, true, t,
                                impacts) == kSweep);
      REQUIRE(SameBits(t, gReferenceT));
      REQUIRE(impacts.size() == 1);
      const Impact& i = impacts[0];
      REQUIRE(SameBits(i.depth, gReferenceDepth));
      REQUIRE(Near(i.selfNormal, axis, 1e-6f));
      REQUIRE(Near(i.otherNormal, {-axis.x, -axis.y, -axis.z}, 1e-6f));
      const Vec3 point{axis.x * 4.f, axis.y * 4.f, axis.z * 4.f};
      REQUIRE(Near(i.selfPoint, point, 1e-5f));
      REQUIRE(Near(i.otherPoint, point, 1e-5f));
      // otherNormal is the component-wise negation of selfNormal, zeros included.
      REQUIRE(Bits(i.otherNormal.x) == Bits(-i.selfNormal.x));
      REQUIRE(Bits(i.otherNormal.y) == Bits(-i.selfNormal.y));
      REQUIRE(Bits(i.otherNormal.z) == Bits(-i.selfNormal.z));
    }
  }
}

void SharedList() {
  // The caller's `impacts` is the source's CCollisionInfoList: an accepted return
  // REPLACES what was in it, a rejection leaves it exactly as it was.
  std::vector< Impact > shared = Seeded();
  float t = 10.f;
  REQUIRE(SphereSphereSwept(kSelfOrigin, kOtherX5, kDirX, true, t, shared) == kSweep);
  REQUIRE(shared.size() == 1);
  const std::vector< Impact > closest = shared;
  REQUIRE(SameBits(t, gReferenceT));
  // An equal-tie rejection keeps the earlier (first) contact, not a new one.
  {
    float tie = gReferenceT;
    REQUIRE(SphereSphereSwept(kSelfOrigin, kOtherX5, kDirX, true, tie, shared) == kMiss);
    REQUIRE(SameBits(tie, gReferenceT));
    REQUIRE(SameImpacts(shared, closest));
  }
  // A miss keeps it too: the list stays as the previous pair left it.
  {
    float miss = 10.f;
    REQUIRE(SphereSphereSwept(kSelfOrigin, kOtherX5, Rem(-1.f, 0.f, 0.f), true, miss,
                              shared) == kMiss);
    REQUIRE(SameBits(miss, 10.f));
    REQUIRE(SameImpacts(shared, closest));
  }
  // A closer hit replaces it outright, with no prefix of the previous contact left.
  {
    std::vector< Impact > bigger = Seeded();
    float far = 10.f;
    REQUIRE(SphereSphereSwept(kSelfOrigin, {Rem(9.f, 0.f, 0.f), 1.f}, kDirX, true, far,
                              bigger) == kSweep);
    REQUIRE(Near(far, 7.f, kSqrt)); // 9 - SqrtF(4)
    REQUIRE(bigger.size() == 1);
    float closer = far;
    REQUIRE(SphereSphereSwept(kSelfOrigin, kOtherX5, kDirX, true, closer, bigger) == kSweep);
    REQUIRE(closer < far);
    REQUIRE(bigger.size() == 1);
    REQUIRE(Near(ToRem(bigger[0].selfPoint), {4.f, 0.f, 0.f}, 1e-6f));
  }
}

void NormalizeHelper() {
  using PortRemasteredGroundContacts::Source::IsNormalizable;
  using PortRemasteredGroundContacts::Source::Normalize;
  using PortRemasteredGroundContacts::Source::NormalizeWithMagnitude;
  using GVec3 = PortRemasteredGroundContacts::Vec3;

  const GVec3 cases[] = {
      {3.f, 4.f, 0.f},   {.6f, .8f, 0.f}, {1.f, 2.f, 3.f},     {-1.f, 0.f, 0.f},
      {0.f, -2.f, 0.f},  {5.f, .5f, 0.f}, {6.f, 8.f, 0.f},      {1000.f, 1.f, -7.f},
      {.001f, -.002f, .003f}, {1e-14f, 0.f, 0.f}};
  for (const GVec3 v : cases) {
    REQUIRE(IsNormalizable(v));
    GVec3 n = v;
    const float m = NormalizeWithMagnitude(n);
    // 0x6b100 runs the same pairs sum, estimate and two frsqrts refinements as
    // 0x6b0b0/0x6b238, so the components are bit-identical to the port's Normalize.
    const GVec3 reference = Normalize(v);
    REQUIRE(SameBits(n.x, reference.x));
    REQUIRE(SameBits(n.y, reference.y));
    REQUIRE(SameBits(n.z, reference.z));
    // The input is unit-length afterwards and m is that length. These are loose bounds
    // against host IEEE arithmetic - deliberately not an oracle: the recorded AArch64
    // values, magnitude included, are asserted bit-for-bit in the fixture block below,
    // and the implementation never derives m from a sqrt.
    const double length =
        std::sqrt(double(reference.x) * reference.x + double(reference.y) * reference.y +
                  double(reference.z) * reference.z);
    REQUIRE(std::fabs(length - 1.0) < 1e-5);
    const double sourceLength =
        std::sqrt(double(v.x) * v.x + double(v.y) * v.y + double(v.z) * v.z);
    REQUIRE(std::fabs(double(m) - sourceLength) <= 1e-6 * std::fmax(sourceLength, 1.0));
    if (sourceLength > 1e-3) { // tiny inputs have no meaningful relative direction check
      REQUIRE(Near(n.x, v.x / float(sourceLength), 1e-5f));
      REQUIRE(Near(n.y, v.y / float(sourceLength), 1e-5f));
      REQUIRE(Near(n.z, v.z / float(sourceLength), 1e-5f));
    }
  }
  // 0x6b100 against recorded AArch64 results: 23 selected rows of an independently
  // generated 1000-row fp32 fixture whose oracle is the game's own 21 instructions for
  // 0x6b100 (frsqrte + 2x frsqrts, paired faddp sum, fmulx component scale, magnitude
  // `fmul v1,v2`) assembled and executed under qemu-aarch64 with FPCR/FFSR reset, i.e.
  // round-to-nearest with FZ=0. The inputs are synthetic (splitmix64, no game data), and
  // frsqrte is architecture-defined-but-recommended, so this pins the values QEMU's ARM
  // pseudocode model produces, NOT real hardware; no host sqrt/rsqrt stands in for the
  // instructions anywhere in that oracle. Rows are the fixture's finite, in-domain ones
  // (its gate tag = IsNormalizable 0x6b030), chosen for non-symmetric components, the
  // 1-ULP 1e-14 case, signed zeros, large-but-finite sums, and rounding boundaries; the
  // fixture's out-of-domain rows (zero sum, sum that underflows or overflows) end in
  // NaN magnitudes and are deliberately not asserted here. Every expected value,
  // including the magnitude, is the recorded bit pattern.
  {
    struct FixtureRow {
      std::uint32_t x, y, z, nx, ny, nz, magnitude;
    };
    static const FixtureRow kRows[] = {
        // non-symmetric components
        {0xbd57a2b0u, 0x3d0a62edu, 0xbd39d518u, 0xbf2e6816u, 0x3edfdacau, 0xbf164d3fu,
         0x3d9e4234u},
        {0x2f90f941u, 0xaf9c848du, 0xafb938c4u, 0x3f035c76u, 0xbf0dd244u, 0xbf27d483u,
         0x300d43a1u},
        {0xaceef4b7u, 0x2cc3d9a1u, 0x2cd433a2u, 0xbf23351eu, 0x3f05c420u, 0x3f10ef2du,
         0x2d3b6866u},
        {0xcf62683cu, 0xcf155bd1u, 0x4f111277u, 0xbf3c6e50u, 0xbef89cb7u, 0x3ef17a24u,
         0x4f99cc0cu},
        {0xb53508c6u, 0xb51b158du, 0x35101bb3u, 0xbf266081u, 0xbf0e8725u, 0x3f0470cau,
         0x358b46c3u},
        {0xce1edd5bu, 0x4e795d32u, 0x4e52f26fu, 0xbedff236u, 0x3f2fc290u, 0x3f14aeb3u,
         0x4eb59a73u},
        // 1e-14, one ULP above it, and the pairs either side of the 0x6b030 threshold
        {0x283424dcu, 0x00000000u, 0x00000000u, 0x3f800001u, 0x00000000u, 0x00000000u,
         0x283424ddu},
        {0x283424dcu, 0x283424dcu, 0x00000000u, 0x3f3504f3u, 0x3f3504f3u, 0x00000000u,
         0x287ec316u},
        {0x283424dbu, 0x283424dbu, 0x00000000u, 0x3f3504f2u, 0x3f3504f2u, 0x00000000u,
         0x287ec314u},
        {0x283424ddu, 0x283424ddu, 0x00000000u, 0x3f3504f3u, 0x3f3504f3u, 0x00000000u,
         0x287ec317u},
        // signed zeros: fmulx keeps the sign of a zero component
        {0x80000000u, 0x3f800000u, 0x3f800000u, 0x80000000u, 0x3f3504f4u, 0x3f3504f4u,
         0x3fb504f4u},
        {0x3f800000u, 0x80000000u, 0x3f800000u, 0x3f3504f4u, 0x80000000u, 0x3f3504f4u,
         0x3fb504f4u},
        {0x3f800000u, 0x3f800000u, 0x80000000u, 0x3f3504f4u, 0x3f3504f4u, 0x80000000u,
         0x3fb504f4u},
        // large but finite sums (no overflow in the squared sum)
        {0x5d5e0b6bu, 0x5cde0b6bu, 0x00000000u, 0x3f64f92eu, 0x3ee4f92eu, 0x00000000u,
         0x5d7840e0u},
        {0xdd5e0b6bu, 0x5d5e0b6bu, 0x5d5e0b6bu, 0xbf13cd3bu, 0x3f13cd3bu, 0x3f13cd3bu,
         0x5dc04bd8u},
        {0x5e800000u, 0x5e800000u, 0x5e800000u, 0x3f13cd3au, 0x3f13cd3au, 0x3f13cd3au,
         0x5eddb3d7u},
        // rounding boundaries and equal ratios at three magnitudes
        {0x283424dcu, 0x283424ddu, 0x283424dbu, 0x3f13cd3au, 0x3f13cd3bu, 0x3f13cd39u,
         0x289c025fu},
        {0x283424ddu, 0x283424ddu, 0x283424dcu, 0x3f13cd3bu, 0x3f13cd3bu, 0x3f13cd3au,
         0x289c0260u},
        {0x283424dcu, 0x283424dbu, 0x283424ddu, 0x3f13cd3au, 0x3f13cd39u, 0x3f13cd3bu,
         0x289c025fu},
        {0x3fc00000u, 0x3fa00000u, 0x3fb00000u, 0x3f20cbbcu, 0x3f05ff1cu, 0x3f13656cu,
         0x4018d6fcu},
        {0x4c400000u, 0x4c200000u, 0x4c300000u, 0x3f20cbbcu, 0x3f05ff1cu, 0x3f13656cu,
         0x4c98d6fcu},
        // an exact power-of-two sum, and a plain axis
        {0x35800000u, 0x00000000u, 0x35800000u, 0x3f3504f4u, 0x00000000u, 0x3f3504f4u,
         0x35b504f4u},
        {0x40000000u, 0x00000000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x00000000u,
         0x40000000u},
    };
    for (const FixtureRow& row : kRows) {
      GVec3 v{FromBits(row.x), FromBits(row.y), FromBits(row.z)};
      REQUIRE(IsNormalizable(v)); // the fixture's own in-domain tag
      const float m = NormalizeWithMagnitude(v);
      REQUIRE(Bits(v.x) == row.nx && Bits(v.y) == row.ny && Bits(v.z) == row.nz);
      REQUIRE(Bits(m) == row.magnitude);
    }
  }
  // The returned magnitude is `squared * the SAME final reciprocal` the components use
  // (the source's `s0 = fmul v1,v2`): for a unit axis and for (1,1,0) the reciprocal is
  // exactly the returned component, so these are bit-exact identities rather than
  // bounds. A `Normalize(v) * sqrt(squared)` recomputation would not have to satisfy
  // the last one.
  {
    GVec3 x{1.f, 0.f, 0.f}, y{0.f, 1.f, 0.f}, z{0.f, 0.f, 1.f}, xy{1.f, 1.f, 0.f};
    const float mx = NormalizeWithMagnitude(x);
    const float my = NormalizeWithMagnitude(y);
    const float mz = NormalizeWithMagnitude(z);
    const float mxy = NormalizeWithMagnitude(xy);
    REQUIRE(SameBits(mx, x.x) && SameBits(my, y.y) && SameBits(mz, z.z));
    REQUIRE(SameBits(xy.x, xy.y));      // the pairs sum treats x and y alike
    REQUIRE(SameBits(mxy, 2.f * xy.x)); // squared = 2, and doubling is exact
  }
}
} // namespace

int main() {
  Overlap();
  Sweep();
  SharedList();
  NormalizeHelper();
  std::puts("Remastered sphere sweep: all checks passed");
  return 0;
}
