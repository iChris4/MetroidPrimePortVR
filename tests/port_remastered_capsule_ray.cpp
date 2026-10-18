// Pure geometry of NCollisionRayCast::Capsule 0xbf6f10
// (platform/port_remastered_capsule_ray.cpp) with its two dependencies,
// NCollisionRayCast::InfiniteCylinder 0xbf73c0 and CMath::SolveQuadratic 0x699b0, and the
// FRECPE + 2*FRECPS reciprocal at 0xbf70c4-0xbf70e8.
//
// The kernel cases are SPEC.md §8's hand cases HC1-HC12
// (build/s-movement-next-20261016/ball-capsule-kernel/SPEC.md), given here in Remastered
// coordinates and converted to the public port frame by the documented mapping. Where the
// spec's prose and the f32 arithmetic disagree the ISA fixture wins (HC10).
//
// THE ORACLE IS tests/fixtures/port_remastered_capsule_ray_isa.h: 421 rows of the game's own
// AArch64 instruction bodies run under qemu-aarch64, plus 87 rows of the bare `frecpe`
// instruction from the same run (see that file's provenance comment for the version, the
// source addresses, the evidence TSV digests and the selection rule). Every row is compared for
// the exact integer code and the exact output bits, including the rows where the kernel must
// leave the caller's sentinel untouched. Nothing below is a tolerance on an expected value.
//
// The Source::* sections between the hand cases and IsaFixtures() are the readable,
// hand-computed boundary checks (thresholds, signed zeros, linear/negative-a quadratics);
// IsaFixtures() is the exhaustive oracle subset, and the hand cases are rows of it too.
#include "port_remastered_capsule_ray.h"

#include "fixtures/port_remastered_capsule_ray_isa.h" // the AArch64 ISA oracle subset

#include "port_remastered_ground_contacts.h" // Source::SqrtF, the shared 0x65f70
#include "port_test.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using PortRemasteredCapsuleRay::Capsule;
using PortRemasteredCapsuleRay::RayCapsule;
using PortRemasteredCapsuleRay::Vec3;
using PortRemasteredCapsuleRay::Source::InfiniteCylinder;
using PortRemasteredCapsuleRay::Source::Reciprocal;
using PortRemasteredCapsuleRay::Source::ReciprocalEstimate;
using PortRemasteredCapsuleRay::Source::SolveQuadratic;

namespace {
constexpr float kEstimate = 1e-5f; // Absolute bound for values on the ARM estimate path.

std::uint32_t Bits(float f) {
  std::uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
bool SameBits(float a, float b) { return Bits(a) == Bits(b); }
bool Near(float a, float b, float e = kEstimate) { return std::fabs(a - b) <= e; }
float FromBits(std::uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
// A Remastered-frame vector (fixture bits or literals) handed straight to the Source::*
// functions, which are Remastered like the source's own CVector3f arguments.
Vec3 RemFrame(const std::uint32_t (&v)[3]) {
  return {FromBits(v[0]), FromBits(v[1]), FromBits(v[2])};
}
// The same bits as the PORT-frame vector RayCapsule takes. Both negations cancel, so the
// kernel sees the fixture's own components, NaN signs and zeros included.
Vec3 PortBits(const std::uint32_t (&v)[3]) {
  return {-FromBits(v[0]), FromBits(v[2]), FromBits(v[1])};
}

// Remastered -> port frame, and its own inverse (platform/port_remastered_ground_world.cpp
// :22-24): the source's coordinates `(x, y, z)` become the port vector `(-x, z, y)`.
Vec3 Rem(float x, float y, float z) { return {-x, z, y}; }
Vec3 FromPort(const Vec3& p) { return {-p.x, p.z, p.y}; }

// A capsule and a ray in REMASTERED coordinates (what the spec's cases are written in).
Capsule Cap(float ax, float ay, float az, float bx, float by, float bz, float radius) {
  return {Rem(ax, ay, az), Rem(bx, by, bz), radius};
}
// The value the kernel was handed before the call: distinguishing it from any written
// output (including 0) is how "tOut untouched" is checked.
constexpr float kSentinel = 12345.f;

struct Result {
  int code = 0;
  float t = 0.f;
  bool wrote = false;
};
Result Cast(const Capsule& capsule, float ox, float oy, float oz, float dx, float dy, float dz,
            float length, float sentinel = kSentinel) {
  float out = sentinel;
  const int code = RayCapsule(capsule, Rem(ox, oy, oz), Rem(dx, dy, dz), length, out);
  return {code, out, !SameBits(out, sentinel)};
}

float Distance(Vec3 a, Vec3 b) {
  const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The hit point of a reported `t` must lie on the capsule's boundary. The ISA fixture pins
// arbitrary axes too; this geometric check is the readable cross-check that would survive a
// change in the fixture's provenance.
bool OnBoundary(const Capsule& capsule, float ox, float oy, float oz, float dx, float dy, float dz,
                float t) {
  // The arguments and the result of `Cast` are already Remastered coordinates.
  const Vec3 o = {ox, oy, oz};
  const Vec3 d = {dx, dy, dz};
  const Vec3 p = {o.x + t * d.x, o.y + t * d.y, o.z + t * d.z};
  const Vec3 a = FromPort(capsule.a), b = FromPort(capsule.b);
  const Vec3 ab = {b.x - a.x, b.y - a.y, b.z - a.z};
  const Vec3 ap = {p.x - a.x, p.y - a.y, p.z - a.z};
  const float lengthSquared = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
  float axial = (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / lengthSquared;
  axial = axial < 0.f ? 0.f : (axial > 1.f ? 1.f : axial);
  const Vec3 closest = {a.x + axial * ab.x, a.y + axial * ab.y, a.z + axial * ab.z};
  const float tolerance = 2e-4f;
  return std::fabs(Distance(p, closest) - capsule.radius) < tolerance ||
         std::fabs(Distance(p, a) - capsule.radius) < tolerance ||
         std::fabs(Distance(p, b) - capsule.radius) < tolerance;
}

// --- SPEC.md §8, the capsule's hand cases (Remastered coordinates) ---------------------
void HandCases() {
  const Capsule axis = Cap(0, 0, 0, 0, 0, 4, 1.f);
  const Capsule degenerate = Cap(3, 0, 0, 3, 0, 0, 2.f);

  // HC1: zero axis, start strictly inside -> inside, t = 0 (b is never read).
  {
    const Result r = Cast(degenerate, 2, 0, 0, 1, 0, 0, 10.f);
    REQUIRE(r.code == 1 && r.wrote && Bits(r.t) == 0);
  }
  // HC2: zero axis, start exactly on the surface. The boundary is not inside, so the ray
  // arm runs: b = 2, disc = 4, SqrtF(4) = 2 exactly (asserted below) -> t = +0.
  {
    const Result r = Cast(degenerate, 1, 0, 0, 1, 0, 0, 10.f);
    REQUIRE(r.code == 2 && r.wrote && Bits(r.t) == 0);
  }
  // HC3: zero axis, start behind -> miss, and tOut is NOT touched.
  {
    const Result r = Cast(degenerate, 5, 0, 0, 1, 0, 0, 10.f);
    REQUIRE(r.code == 0 && !r.wrote);
  }
  // HC4: perpendicular, entry beyond the ray length. This branch has no length test and no
  // caps, so t = 4 > length = 2 is returned: t^2 - 10t + 24 = 0 -> 4, 6 (5 and 1 -> exact
  // coefficients: |D x A|^2 = 1, b = -10, c = 24, SqrtF(4) = 2).
  {
    const Result r = Cast(axis, -5, 0, 2, 1, 0, 0, 2.f);
    REQUIRE(r.code == 2 && SameBits(r.t, 4.f) && r.t > 2.f);
  }
  // HC5: perpendicular, start inside the span -> inside, tOut = max(t_near, 0) = +0 (the
  // only inside path that writes a value other than by the cap arms).
  {
    const Result r = Cast(axis, 0, 0, 2, 1, 0, 0, 10.f);
    REQUIRE(r.code == 1 && r.wrote && Bits(r.t) == 0);
  }
  // HC6: perpendicular, start exactly on the lateral surface: t_near = 0 is not < 0, so
  // this is a hit at t = 0, not an inside.
  {
    const Result r = Cast(axis, -1, 0, 2, 1, 0, 0, 10.f);
    REQUIRE(r.code == 2 && r.wrote && Bits(r.t) == 0);
  }
  // HC7: perpendicular, the strict span test rejects the lateral cylinder (adoto - adotb
  // = 0.75 >= 0), the caps run: cap a's discriminant is negative and cap b gives t = 4.
  {
    const Capsule r = Cap(0, 0, 0, 0, 0, 4, 1.25f);
    const Result run = Cast(r, -5, 0, 4.75f, 1, 0, 0, 10.f);
    REQUIRE(run.code == 2 && SameBits(run.t, 4.f));
  }
  // HC8: general, direction along the axis. D x A = 0, so a = b = 0 and 0x699b0 returns 0
  // (the linear arm needs |b| >= 0x34000000); the lateral test is skipped and cap a gives
  // t = 3 - sqrt(0.75). The spec's inputs are exact, and the ISA fixture returns 0x4008930a
  // (2.13397455), one f32 rounding below the real-arithmetic 2.1339746.
  {
    const Result r = Cast(axis, 0.5f, 0, -3, 0, 0, 1, 10.f);
    REQUIRE(r.code == 2 && Bits(r.t) == 0x4008930au);
  }
  // HC9: general lateral hit inside the span: 0.64t^2 - 8t + 24 = 0 -> t_near = 5 exactly
  // (0.5/0.64 = 0.78125, 8 - SqrtF(2.56) = 6.4 in f32), and cap b's 5.4202 does not win.
  {
    const Result r = Cast(axis, -5, 0, 0, 0.8f, 0, 0.6f, 10.f);
    REQUIRE(r.code == 2 && SameBits(r.t, 5.f));
  }
  // HC10: general span clipping. The clipped cylinder parameter is max(tmin, t_near) with
  // tmin = t_A = 4/0.6, then cap a's sat value wins. The SPEC's 5.4202041 is a
  // real-arithmetic number: 0.8f/0.6f are inexact, so b, b*b and disc round before the
  // square root. The ISA fixture returns 0x40ad724d (5.42020273), 3 ulp lower, and that is
  // what is pinned here. Skipping the clamp would return t = 5 instead.
  {
    const Result r = Cast(axis, -5, 0, -4, 0.8f, 0, 0.6f, 10.f);
    REQUIRE(r.code == 2 && Bits(r.t) == 0x40ad724du);
  }
  // HC11: general span-exit tie (t_near == t_max). The compare is between independently
  // rounded values, so this is settled by the ISA fixture, not by hand: the oracle row
  // returns code 0 with tOut untouched, i.e. the lateral hit is rejected non-strictly at
  // `t_near >= t_max` and both cap discriminants are negative.
  {
    const Result r = Cast(axis, -5, 0, 3, 0.8f, 0, 0.6f, 10.f);
    REQUIRE(r.code == 0 && !r.wrote);
  }
  // HC12: no hit: the perpendicular span test passes but the quadratic discriminant is
  // negative (t^2 + 3), and both caps are outside (m^2 = 13 and 5 against r^2 = 1).
  {
    const Result r = Cast(axis, 2, 0, 3, 0, 1, 0, 10.f);
    REQUIRE(r.code == 0 && !r.wrote);
  }
}

// --- the two 1e-5 thresholds and FLT_EPSILON -------------------------------------------
void Thresholds() {
  const Capsule axis = Cap(0, 0, 0, 0, 0, 4, 1.f);
  // Branch selection is on |u.dir| against 0x3727c5ac = 1e-5, not FLT_EPSILON (0x34000000)
  // and not the quadratic's 0x34000000:
  //  |u.dir| = 1e-6 (< 1e-5) -> PERPENDICULAR, which ignores `length`: the lateral entry is
  //  t = 4 (exact coefficients: a-coeff = 1, b = -10, c = 24) while length is only 2. A
  //  FLT_EPSILON reading would take the general branch instead and miss (return 0).
  {
    const Result r = Cast(axis, -5, 0, 2, 1, 0, 1e-6f, 2.f);
    REQUIRE(r.code == 2 && SameBits(r.t, 4.f) && r.t > 2.f);
  }
  //  |u.dir| = 2e-5 -> general. t_A = -2/(2e-5) and t_B = 2/(2e-5), so tmin = 0 and tmax =
  //  min(2, ...) = 2; the cylinder entry 4 is at/beyond t_max and rejects before the caps.
  {
    const Result r = Cast(axis, -5, 0, 2, 1, 0, 2e-5f, 2.f);
    REQUIRE(r.code == 0 && !r.wrote);
  }
  //  |u.dir| == 1e-5 exactly is GENERAL: the source's test is `|u.dir| >= eps` (b.pl on the
  //  fcmp), so the equality belongs to the general branch, which misses here as above.
  {
    const Result r = Cast(axis, -5, 0, 2, 1, 0, 1e-5f, 2.f);
    REQUIRE(r.code == 0 && !r.wrote);
  }
  // The clipped cylinder parameter has its own 1e-5 threshold (0xbf719c): |clipped| < 1e-5
  // returns INSIDE without writing tOut at all. Here the ray starts 1e-6 outside the lateral
  // surface (port-frame x offset), so clipped = 1.25e-6 -> code 1, output untouched.
  {
    const Result r = Cast(axis, -1.000001f, 0, 0, 0.8f, 0, 0.6f, 10.f);
    REQUIRE(r.code == 1 && !r.wrote);
  }
  // The 1e-3 mirror is far above either epsilon and reports the lateral hit: 1e-3/0.8 =
  // 1.25e-3 (measured 0.0012501142; the offset is not exactly representable).
  {
    const Result r = Cast(axis, -1.001f, 0, 0, 0.8f, 0, 0.6f, 10.f);
    REQUIRE(r.code == 2 && Near(r.t, 1.25e-3f, 1e-6f));
  }
}

// --- the output sentinel and signed zeros ---------------------------------------------
void Sentinels() {
  const Capsule axis = Cap(0, 0, 0, 0, 0, 4, 1.f);
  const float negativeZeroSentinel = -0.f;
  // A miss leaves even a -0.0 output alone (a written +0 would be indistinguishable from a
  // zeroed buffer, so the negative zero is the useful sentinel).
  {
    const Result r = Cast(axis, 2, 0, 3, 0, 1, 0, 10.f, negativeZeroSentinel);
    REQUIRE(r.code == 0 && Bits(r.t) == 0x80000000u);
  }
  // So does the tiny-clipped inside return: 0xbf71ac branches straight to the epilogue.
  {
    const Result r = Cast(axis, -1.000001f, 0, 0, 0.8f, 0, 0.6f, 10.f, negativeZeroSentinel);
    REQUIRE(r.code == 1 && Bits(r.t) == 0x80000000u);
  }
  // The perpendicular inside DOES write: max(t_near, 0) is +0 (not -0, and not the -0 the
  // caller passed), which is what separates it from the untouched paths.
  {
    const Result r = Cast(axis, 0, 0, 2, 1, 0, 0, 10.f, negativeZeroSentinel);
    REQUIRE(r.code == 1 && Bits(r.t) == 0u);
  }
  // And a hit whose t is exactly zero also writes +0 (degenerate axis, boundary start).
  {
    const Result r = Cast(Cap(3, 0, 0, 3, 0, 0, 2.f), 1, 0, 0, 1, 0, 0, 10.f, negativeZeroSentinel);
    REQUIRE(r.code == 2 && Bits(r.t) == 0u);
  }
}

// --- length == 0 -----------------------------------------------------------------------
void ZeroLength() {
  const Capsule axis = Cap(0, 0, 0, 0, 0, 4, 1.f);
  // The perpendicular branch never reads `length`: with length = 0 the ray still reports
  // inside/hit and writes max(t_near, 0).
  {
    const Result r = Cast(axis, 0, 0, 2, 1, 0, 0, 0.f);
    REQUIRE(r.code == 1 && r.wrote && Bits(r.t) == 0);
  }
  // The general branch seeds its running best with `length`, and both cap arms are gated by
  // `best > 0`, so with length = 0 a start strictly inside cap a is NOT reported: t_max =
  // min(0, 3.9) = 0 rejects the cylinder, cap a detects the inside but skips the update, and
  // cap b's entry is beyond the (zero) length -> miss with tOut untouched.
  {
    const Result r = Cast(axis, 0.5f, 0, 0.1f, 0, 0, 1, 0.f);
    REQUIRE(r.code == 0 && !r.wrote);
  }
  // Every hit arm is strict against `length` (`t < len`), so a hit exactly at `length`
  // misses: the degenerate-axis sphere test gives t = 0 here (b - SqrtF(4) = 2 - 2).
  {
    const Result r = Cast(Cap(3, 0, 0, 3, 0, 0, 2.f), 1, 0, 0, 1, 0, 0, 0.f);
    REQUIRE(r.code == 0 && !r.wrote);
  }
}

// --- Source::SolveQuadratic 0x699b0 ----------------------------------------------------
void QuadraticCases() {
  float roots[2] = {77.f, 88.f};
  // Exact tangency: disc == 0 is not rejected (only disc < 0 is), sq = 0, and both roots are
  // the same. No sqrt path is involved, so this is bit-exact.
  REQUIRE(SolveQuadratic(1.f, -2.f, 1.f, roots) == 2);
  REQUIRE(SameBits(roots[0], 1.f) && SameBits(roots[1], 1.f));
  // disc == 0 with b == +0: the source's fneg makes -b - sq = -0 - 0 = -0 and sq - b = +0,
  // so the pair is {-0, +0} in that order (a >= 0 keeps r1 first).
  roots[0] = 77.f;
  roots[1] = 88.f;
  REQUIRE(SolveQuadratic(1.f, 0.f, 0.f, roots) == 2);
  REQUIRE(Bits(roots[0]) == 0x80000000u && Bits(roots[1]) == 0u);
  // Linear: |a| < 0x34000000, root = -c/b, and roots[1] is left alone.
  roots[0] = 77.f;
  roots[1] = 88.f;
  REQUIRE(SolveQuadratic(0.f, 2.f, -4.f, roots) == 1);
  REQUIRE(SameBits(roots[0], 2.f) && SameBits(roots[1], 88.f));
  // Signed zero through the source's fneg/fdiv: -(-0)/2 is +0.
  roots[0] = 77.f;
  REQUIRE(SolveQuadratic(0.f, 2.f, -0.f, roots) == 1);
  REQUIRE(Bits(roots[0]) == 0);
  // Both |a| and |b| below the threshold: no roots, buffer untouched.
  roots[0] = 77.f;
  roots[1] = 88.f;
  REQUIRE(SolveQuadratic(0.f, 0.f, 5.f, roots) == 0);
  REQUIRE(SameBits(roots[0], 77.f) && SameBits(roots[1], 88.f));
  // Negative discriminant: no roots, buffer untouched.
  roots[0] = 77.f;
  roots[1] = 88.f;
  REQUIRE(SolveQuadratic(1.f, 0.f, 1.f, roots) == 0);
  REQUIRE(SameBits(roots[0], 77.f) && SameBits(roots[1], 88.f));
  // a < 0 stores {r2, r1}, which is still ascending: -t^2 + 1 -> {-1, 1} exactly (disc = 4).
  roots[0] = 77.f;
  roots[1] = 88.f;
  REQUIRE(SolveQuadratic(-1.f, 0.f, 1.f, roots) == 2);
  REQUIRE(SameBits(roots[0], -1.f) && SameBits(roots[1], 1.f));
  // The quadratic threshold is 0x34000000 = FLT_EPSILON (1.1920929e-07), not the
  // capsule's 1e-5:
  // |a| = 1e-8 takes the linear arm, |a| = 2e-7 takes the quadratic arm.
  roots[0] = 77.f;
  roots[1] = 88.f;
  REQUIRE(SolveQuadratic(1e-8f, 2.f, -4.f, roots) == 1);
  REQUIRE(SameBits(roots[0], 2.f) && SameBits(roots[1], 88.f));
  //  |a| = 2e-7 >= 0x34000000 takes the quadratic arm: 2e-7 t^2 - 8 = 0 gives the symmetric
  //  pair sqrt(8/2e-7) = 6324.5553 (no cancellation, so a tight absolute bound is safe).
  roots[0] = 77.f;
  roots[1] = 88.f;
  REQUIRE(SolveQuadratic(2e-7f, 0.f, -8.f, roots) == 2);
  REQUIRE(roots[0] < 0.f && Near(roots[1], 6324.5553f, 1.f));
}

// --- Source::InfiniteCylinder 0xbf73c0 -------------------------------------------------
// Remastered-frame literals, handed to the Source API as-is (no port conversion here).
void CylinderCases() {
  const Vec3 axisPoint = {0.f, 0.f, 0.f};
  const Vec3 unitAxis = {0.f, 0.f, 1.f};
  // Miss: the source returns before writing either output, so both are left alone.
  {
    float nearDistance = 7.f, farDistance = 8.f;
    REQUIRE(!InfiniteCylinder(axisPoint, unitAxis, 1.f, {2.f, 0.f, 3.f}, {0.f, 1.f, 0.f},
                              nearDistance, farDistance));
    REQUIRE(SameBits(nearDistance, 7.f) && SameBits(farDistance, 8.f));
  }
  // Quadratic arm: HC4's cylinder, near = 4 and far = 6 exactly, ascending.
  {
    float nearDistance = 7.f, farDistance = 8.f;
    REQUIRE(InfiniteCylinder(axisPoint, unitAxis, 1.f, {-5.f, 0.f, 2.f}, {1.f, 0.f, 0.f},
                             nearDistance, farDistance));
    REQUIRE(SameBits(nearDistance, 4.f) && SameBits(farDistance, 6.f) &&
            nearDistance <= farDistance);
  }
  // Linear arm (|D x A|^2 = 1e-12 < 0x34000000): BOTH outputs receive root[0]. The ray is
  // 1e-6 off the axis direction, so 1e-12 t^2 - 1e-5 t + 24 = 0 gives t = 24/1e-5.
  {
    float nearDistance = 7.f, farDistance = 8.f;
    REQUIRE(InfiniteCylinder(axisPoint, unitAxis, 1.f, {-5.f, 0.f, 2.f}, {1e-6f, 0.f, 1.f},
                             nearDistance, farDistance));
    // -c/b with c = 24 exactly and b = -1e-5f: the same two f32 operands the source
    // divides, so this is bit-exact rather than a bound, and the ISA fixture's own
    // linear-arm rows pin the same path.
    REQUIRE(SameBits(nearDistance, farDistance) && SameBits(nearDistance, 24.f / 1e-5f));
  }
  // r = 0 is an accepted (degenerate) cylinder radius: the disc reduces to |W x A|^2.
  {
    float nearDistance = 7.f, farDistance = 8.f;
    REQUIRE(!InfiniteCylinder(axisPoint, unitAxis, 0.f, {2.f, 0.f, 3.f}, {0.f, 1.f, 0.f},
                              nearDistance, farDistance));
    REQUIRE(SameBits(nearDistance, 7.f) && SameBits(farDistance, 8.f));
  }
}

// --- Source::Reciprocal 0xbf70c4-0xbf70e8 (FRECPE + 2*FRECPS) ---------------------------
void ReciprocalCases() {
  // The architecture's estimate is 511/512 for 1.0, and the two fused refinements then give
  // the exact f32 reciprocal of these powers of two (each step is exact here).
  REQUIRE(SameBits(Reciprocal(1.f), 1.f));
  REQUIRE(SameBits(Reciprocal(2.f), 0.5f));
  REQUIRE(SameBits(Reciprocal(4.f), 0.25f));
  REQUIRE(SameBits(Reciprocal(-2.f), -0.5f));
  // 1/3 and 1/0.6 are not representable: the two refinements land within 1 ULP of the
  // correctly rounded value (1/3 is bit-exact here, 1/0.6 is one ULP below it).
  REQUIRE(Bits(Reciprocal(3.f)) == 0x3eaaaaabu);
  REQUIRE(Near(Reciprocal(0.6f), 1.f / 0.6f, 1e-6f));
  // Special cases follow FPRecipEstimate: infinity -> zero of the same sign, zero ->
  // infinity of the same sign (the refinements then leave those fixed, because FRECPS
  // specifies zero*infinity as exactly 2.0).
  REQUIRE(SameBits(Reciprocal(0.f), std::numeric_limits< float >::infinity()));
  REQUIRE(SameBits(Reciprocal(-0.f), -std::numeric_limits< float >::infinity()));
  REQUIRE(Bits(Reciprocal(std::numeric_limits< float >::infinity())) == 0u);
  REQUIRE(Bits(Reciprocal(-std::numeric_limits< float >::infinity())) == 0x80000000u);
  // |x| < 2^-128 is the estimate's overflow range: the estimate alone is +infinity there,
  // and the two refinements (which the instruction sequence always applies) sign-flip it.
  // The source cannot reach this range (|u.dir| >= 1e-5), so it is not special-cased.
  REQUIRE(Bits(Reciprocal(1e-40f)) == 0xff800000u);
  // Subnormals at or above 2^-128 take the architecture's partial normalization: 2^-127 is
  // a subnormal whose reciprocal is representable, and the estimate comes back within its
  // ~8-bit accuracy (1.696e38 against 2^127 = 1.7014e38).
  const float subnormal = std::ldexp(1.f, -127);
  const float subnormalReciprocal = Reciprocal(subnormal);
  const float expected = std::ldexp(1.f, 127);
  REQUIRE(std::isfinite(subnormalReciprocal));
  REQUIRE(Near(subnormalReciprocal, expected, 0.01f * expected));
}

// --- arbitrary, reversed axes ----------------------------------------------------------
void ObliqueAxes() {
  // A capsule whose axis is oblique in the Remastered frame, hit laterally: the coefficients
  // are not exact, so the value is checked geometrically rather than against a number.
  const Capsule oblique = Cap(1, 0, 1, 3, 0, 5, 1.f);
  const Capsule reversed = Cap(3, 0, 5, 1, 0, 1, 1.f);
  {
    const Result r = Cast(oblique, -2, 0, 2, 1, 0, 0, 10.f);
    REQUIRE(r.code == 2 && r.t > 0.f && OnBoundary(oblique, -2, 0, 2, 1, 0, 0, r.t));
    // Swapping a and b is the same cylinder and the same caps, and both caps miss here, so
    // the two orders agree to one f32 ULP (measured 0x40187222 vs 0x40187221: the roots come
    // from W = o - a, which rounds differently for the two endpoint orders).
    const Result s = Cast(reversed, -2, 0, 2, 1, 0, 0, 10.f);
    REQUIRE(s.code == 2 && Near(s.t, r.t, 1e-6f));
  }
  // A cap hit on the oblique axis, again in both endpoint orders (identical bits). The
  // direction must be unit here: the sphere test's disc = r^2 - (m^2 - b^2) is the tangent
  // discriminant only for |dir| = 1, so a (1, 0, -1) direction would report a point that is
  // not on the capsule at all.
  {
    const Result r = Cast(oblique, 1, 0, 8, 0.6f, 0, -0.8f, 10.f);
    REQUIRE(r.code == 2 && OnBoundary(oblique, 1, 0, 8, 0.6f, 0, -0.8f, r.t));
    const Result s = Cast(reversed, 1, 0, 8, 0.6f, 0, -0.8f, 10.f);
    REQUIRE(s.code == 2 && SameBits(s.t, r.t));
  }
  // Reversing the axis of a hand case: the two caps swap roles but the same physical
  // sphere wins, so the reported code and t are unchanged.
  {
    const Result forward = Cast(Cap(0, 0, 0, 0, 0, 4, 1.f), 0, 0, 2, 1, 0, 0, 10.f);
    const Result reverse = Cast(Cap(0, 0, 4, 0, 0, 0, 1.f), 0, 0, 2, 1, 0, 0, 10.f);
    REQUIRE(forward.code == reverse.code && SameBits(forward.t, reverse.t));
  }
  {
    const Result forward = Cast(Cap(0, 0, 0, 0, 0, 4, 1.25f), -5, 0, 4.75f, 1, 0, 0, 10.f);
    const Result reverse = Cast(Cap(0, 0, 4, 0, 0, 0, 1.25f), -5, 0, 4.75f, 1, 0, 0, 10.f);
    REQUIRE(forward.code == 2 && reverse.code == 2 && SameBits(forward.t, reverse.t));
  }
  // A zero-radius capsule is a line: a perpendicular ray through the axis reports the
  // entry t = 5 exactly. The cap spheres then have r^2 = 0, so their strict `disc > 0` test
  // rejects everything (the boundary would need disc == 0), which is why the lateral
  // cylinder is the only producer here.
  {
    const Result r = Cast(Cap(0, 0, 0, 0, 0, 4, 0.f), -5, 0, 2, 1, 0, 0, 10.f);
    REQUIRE(r.code == 2 && SameBits(r.t, 5.f));
  }
}

// --- the PERP negative-zero row (named ISA case) ----------------------------------------
// The one perpendicular path whose raw cylinder entry is NEGATIVE ZERO, so its reported tOut
// is the equal-zeros case of max(t_near, 0): AArch64 FMAX(-0, +0) is +0, while C leaves the
// sign of std::fmax for equal zeros unspecified. Numeric values only, from the game's own
// instruction bodies run under qemu-aarch64 10.2.1 (same provenance as
// tests/fixtures/port_remastered_capsule_ray_isa.h); the kernel must match them bit-for-bit:
//   SolveQuadratic(1, +0, +0) -> 2 roots, {-0, +0}
//   InfiniteCylinder -> true, near = -0, far = +0
//   RayCapsule: capsule a=(0,0,0) b=(0,0,4) r=1, o=(0,1,2), dir=(1,0,0), len=10
//               -> return 2, tOut = +0, inputs unchanged
void PerpNegativeZero() {
  // The geometry: the origin lies exactly on the lateral surface (c-coefficient 0) with the
  // axial span strictly interior, so |u.dir| = 0 selects PERP.
  const Capsule axis = Cap(0, 0, 0, 0, 0, 4, 1.f);
  const std::uint32_t seed = 0x5eed0000u; // the oracle run's caller sentinel
  float roots[2] = {FromBits(seed), FromBits(seed)};
  REQUIRE(SolveQuadratic(1.f, 0.f, 0.f, roots) == 2);
  REQUIRE(Bits(roots[0]) == 0x80000000u && Bits(roots[1]) == 0u);
  {
    float nearDistance = FromBits(seed), farDistance = FromBits(seed);
    REQUIRE(InfiniteCylinder({0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, 1.f, {0.f, 1.f, 2.f}, {1.f, 0.f, 0.f},
                             nearDistance, farDistance));
    REQUIRE(Bits(nearDistance) == 0x80000000u && Bits(farDistance) == 0u);
  }
  {
    // PERP continues because the far root is >= 0, writes max(t_near, 0) = +0 and returns 2
    // because t_near is not < 0.
    const Result r = Cast(axis, 0, 1, 2, 1, 0, 0, 10.f, FromBits(seed));
    REQUIRE(r.code == 2 && Bits(r.t) == 0u);
  }
}

// --- the ISA oracle subset: exact code and exact output bits, row by row ------------------
// tests/fixtures/port_remastered_capsule_ray_isa.h carries the provenance (qemu-aarch64
// version, source addresses, evidence TSV digests, selection rule). Nothing here is a bound:
// every comparison is on the integer code and on the bit pattern of every output, and a
// row whose slot must not have been written is checked against the fixture's own sentinel.
void IsaFixtures() {
  using namespace mpr_capsule_ray_isa;
  std::size_t capsuleRows = 0, quadraticRows = 0, cylinderRows = 0, reciprocalRows = 0;
  std::size_t outOfContract = 0;

  for (const CapsuleCase& c : kCapsuleCases) {
    ++capsuleRows;
    if (!c.inContract)
      ++outOfContract;
    const Capsule capsule{PortBits(c.a), PortBits(c.b), FromBits(c.radius)};
    float t = FromBits(c.tSeed);
    const int code =
        RayCapsule(capsule, PortBits(c.origin), PortBits(c.dir), FromBits(c.length), t);
    if (code != c.code) {
      std::fprintf(stderr, "ISA capsule %s: code %d, expected %d\n", c.name, code, c.code);
      REQUIRE(code == c.code);
    }
    const std::uint32_t want = c.touched ? c.tOut : c.tSeed;
    if (Bits(t) != want) {
      std::fprintf(stderr, "ISA capsule %s: tOut 0x%08x, expected 0x%08x (touched %d)\n", c.name,
                   Bits(t), want, (int)c.touched);
      REQUIRE(Bits(t) == want);
    }
  }

  for (const QuadraticCase& c : kQuadraticCases) {
    ++quadraticRows;
    if (!c.inContract)
      ++outOfContract;
    float roots[2] = {FromBits(c.seed[0]), FromBits(c.seed[1])};
    const int code = SolveQuadratic(FromBits(c.a), FromBits(c.b), FromBits(c.c), roots);
    if (code != c.code) {
      std::fprintf(stderr, "ISA quadratic %s: code %d, expected %d\n", c.name, code, c.code);
      REQUIRE(code == c.code);
    }
    for (int k = 0; k < 2; ++k) {
      const std::uint32_t want = c.touched[k] ? c.out[k] : c.seed[k];
      if (Bits(roots[k]) != want) {
        std::fprintf(stderr, "ISA quadratic %s: root[%d] 0x%08x, expected 0x%08x\n", c.name, k,
                     Bits(roots[k]), want);
        REQUIRE(Bits(roots[k]) == want);
      }
    }
  }

  for (const CylinderCase& c : kCylinderCases) {
    ++cylinderRows;
    if (!c.inContract)
      ++outOfContract;
    float nearDistance = FromBits(c.tSeed[0]), farDistance = FromBits(c.tSeed[1]);
    // The Source API is Remastered frame: the fixture vectors go in unconverted.
    const bool ok = InfiniteCylinder(RemFrame(c.axisPoint), RemFrame(c.unitAxis),
                                     FromBits(c.radius), RemFrame(c.rayOrigin),
                                     RemFrame(c.unitDirection), nearDistance, farDistance);
    // The bool collapses the source's 1 (linear root) and 2 (two roots): both write both
    // outputs, which the per-slot bits below still distinguish.
    if (ok != (c.code != 0)) {
      std::fprintf(stderr, "ISA cylinder %s: ok %d, expected %d\n", c.name, (int)ok, c.code != 0);
      REQUIRE(ok == (c.code != 0));
    }
    const float got[2] = {nearDistance, farDistance};
    for (int k = 0; k < 2; ++k) {
      const std::uint32_t want = c.touched[k] ? c.t[k] : c.tSeed[k];
      if (Bits(got[k]) != want) {
        std::fprintf(stderr, "ISA cylinder %s: t[%d] 0x%08x, expected 0x%08x\n", c.name, k,
                     Bits(got[k]), want);
        REQUIRE(Bits(got[k]) == want);
      }
    }
  }

  for (const ReciprocalCase& c : kReciprocalCases) {
    ++reciprocalRows;
    if (!c.inContract)
      ++outOfContract;
    const std::uint32_t got = Bits(Reciprocal(FromBits(c.in)));
    if (got != c.out) {
      std::fprintf(stderr, "ISA reciprocal %s: 0x%08x, expected 0x%08x\n", c.name, got, c.out);
      REQUIRE(got == c.out);
    }
  }

  // The bare `frecpe` rows: the estimate the source's reciprocal starts from, which the
  // refined reciprocal rows above cannot pin. Every row is asserted exactly; there is no NaN
  // row, so there is nothing to hold outside a contract.
  std::size_t estimateRows = 0;
  for (const EstimateCase& c : kEstimateCases) {
    ++estimateRows;
    const std::uint32_t got = Bits(ReciprocalEstimate(FromBits(c.in)));
    if (got != c.out) {
      std::fprintf(stderr, "ISA frecpe %s: 0x%08x, expected 0x%08x\n", c.name, got, c.out);
      REQUIRE(got == c.out);
    }
  }

  // The selection is fixed (see gen_test_subset.log); a change here means the fixture was
  // regenerated, not that the kernel drifted.
  REQUIRE(capsuleRows == 197 && quadraticRows == 73 && cylinderRows == 99 && reciprocalRows == 52);
  REQUIRE(outOfContract == 85 + 5 + 4 + 10);
  REQUIRE(estimateRows == 87 && kEstimateCaseCount == 87);
  std::printf("ISA fixture rows: %zu capsule, %zu quadratic, %zu cylinder, %zu reciprocal,"
              " %zu bare frecpe (%zu outside the public finite contract)\n",
              capsuleRows, quadraticRows, cylinderRows, reciprocalRows, estimateRows,
              outOfContract);
}

// --- the shared prerequisites the kernel's exactness rests on --------------------------
void SharedSqrtPrerequisite() {
  using PortRemasteredGroundContacts::Source::SqrtF;
  // 0x65f70 is now shared (it used to be file-local in port_collision_failsafe.cpp): its
  // outputs are unchanged, and the exact ones the kernel pins above depend on are here.
  REQUIRE(SameBits(SqrtF(4.f), 2.f));
  REQUIRE(SameBits(SqrtF(1.f), 1.f));
  REQUIRE(SameBits(SqrtF(0.5625f), 0.75f)); // 0.75^2, also exact through the estimate
  // 2.56 and 0.96 are not perfect squares in f32, and the estimate chain is not correctly
  // rounded (2.56 comes back 1 ULP low). The kernel's own values for those inputs are pinned
  // by the ISA fixture rows instead of by these bounds.
  REQUIRE(Near(SqrtF(2.56f), 1.6f, 1e-6f));
  REQUIRE(Near(SqrtF(0.96f), 0.9797959f, 1e-6f));
  // The guard is `!(v > 0)`: zero, negatives and NaN all return 0.
  REQUIRE(Bits(SqrtF(0.f)) == 0u && Bits(SqrtF(-1.f)) == 0u &&
          Bits(SqrtF(std::numeric_limits< float >::quiet_NaN())) == 0u);
}
} // namespace

int main() {
  HandCases();
  Thresholds();
  Sentinels();
  ZeroLength();
  QuadraticCases();
  CylinderCases();
  ReciprocalCases();
  PerpNegativeZero();
  ObliqueAxes();
  IsaFixtures();
  SharedSqrtPrerequisite();
  std::puts("Remastered capsule ray: all checks passed");
  return 0;
}
