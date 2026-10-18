#include "port_remastered_capsule_ray.h"

#include "port_remastered_ground_contacts.h" // Source::IsNormalizable/Normalize/SqrtF

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace PortRemasteredCapsuleRay {
namespace {

static_assert(sizeof(float) == 4 && std::numeric_limits< float >::is_iec559);

// 0x3727c5ac at 0xbf6fe8 (|u.dir|) and 0xbf719c (|clipped|): 1e-5, distinct from the
// quadratic solver's 0x34000000, and the two are not interchangeable at |u.dir|: this test
// selects the perpendicular branch, where a FLT_EPSILON-sized test would take the general one,
// and the tiny-clipped return-1 path has a 1e-5 test of its own. The ISA fixtures pin both
// thresholds at 1 ulp.
constexpr float kCapsuleEpsilon = 1e-5f;
// 0x34000000 at 0x699b0: FLT_EPSILON, 2^-23 = 1.1920929e-07.
constexpr float kQuadraticEpsilon = 1.1920929e-07f;

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

// Remastered is Y-up: rem = (-port.x, port.z, port.y), its own inverse.
Vec3 ToRem(const Vec3& p) { return {-p.x, p.z, p.y}; }

// The 0x6b030/0x6b0b0/0x65f70 helpers live in the contacts module and use its vector type.
PortRemasteredGroundContacts::Vec3 ToContacts(const Vec3& v) { return {v.x, v.y, v.z}; }
Vec3 FromContacts(const PortRemasteredGroundContacts::Vec3& v) { return {v.x, v.y, v.z}; }

// FRECPS: the architecture's fused reciprocal step, 2 - a*b in ONE rounding, with the
// specified zero*infinity result of exactly 2 (and the infinity result otherwise). The
// std::fma below is deliberate - fused is what the instruction computes - and it is the only
// fused operation in this file: the build disables contraction (-ffp-contract=off). Only
// lane 0 of the source's vector form is ever used, and its second lane is the dummy 1.0.
float ReciprocalStep(float a, float b) {
  if ((a == 0.f && std::isinf(b)) || (b == 0.f && std::isinf(a)))
    return 2.f;
  if (std::isinf(a) || std::isinf(b)) {
    // FPInfinity(sign(-a) XOR sign(b)): the pseudocode negates the first operand.
    const bool negative = std::signbit(a) == std::signbit(b);
    return negative ? -std::numeric_limits< float >::infinity()
                    : std::numeric_limits< float >::infinity();
  }
  return std::fma(-a, b, 2.f);
}

// RecipEstimate(): the fixed-point input a/256 in [1,2) is doubled and odd-rounded before
// the division. Both divisions round towards minus infinity (DIVRM), which is why
// FRECPE(1.0f) is 511/512 = 0.998046875, not 1.0.
int RecipEstimate(int a) {
  const int scaled = a * 2 + 1;
  const int b = (1 << 19) / scaled;
  return (b + 1) / 2;
}

// FPRecipEstimate for binary32, the architecture's default FPCR (RNE, no flush-to-zero)
// and Switch's !increasedprecision variant (8 fraction bits): a 9-bit fixed-point `scaled`
// in [256,512) of the operand's [0.5,1) mantissa, the integer estimate, and the exponent
// reconstruction, including the partial normalization the architecture specifies for
// subnormals and the |x| < 2^-128 overflow result.
float ArmReciprocalEstimate(float value) {
  const std::uint32_t bits = Bits(value);
  const int exponent = static_cast< int >((bits >> 23) & 255u);
  const std::uint32_t frac = bits & 0x7fffffu;
  if (exponent == 255) {
    if (frac)
      return FromBits(bits | 0x400000u); // NaN: quiet, payload kept
    return FromBits(bits & 0x80000000u); // infinity -> zero of the same sign
  }
  if (exponent == 0 && frac == 0)
    return FromBits((bits & 0x80000000u) | 0x7f800000u);
  // |value| < 2^-128 (raw magnitude below 2^21): the estimate overflows, and the rounded
  // result under RNE is infinity of the operand's sign. Zero was handled above.
  if ((bits & 0x7fffffffu) < 0x200000u)
    return FromBits((bits & 0x80000000u) | 0x7f800000u);

  // Scale to a fixed point value in [0.5,1) in steps of 1/512: the 52-bit fraction is the
  // binary32 fraction shifted up, and scaled = '1'::fraction[51:44].
  std::uint64_t fraction = static_cast< std::uint64_t >(frac) << 29;
  int exp = exponent;
  if (exp == 0) {
    // Subnormal: the architecture shifts by 2 or by 1 and only when the top bit is clear.
    if ((fraction >> 51) == 0) {
      exp = -1;
      fraction = (fraction << 2) & 0xfffffffffffffull;
    } else {
      fraction = (fraction << 1) & 0xfffffffffffffull;
    }
  }
  const int scaled = 256 + static_cast< int >((fraction >> 44) & 0xffu);
  const int estimate = RecipEstimate(scaled); // 256..511, i.e. [1.0,2.0)
  int resultExponent = 253 - exp;             // -1..254
  // The estimate's low 8 bits are the result mantissa (its leading 1 is implied by
  // resultExponent: the estimate represents 2^-1 * estimate/256).
  std::uint64_t resultFraction = static_cast< std::uint64_t >(estimate & 0xff) << 44;
  if (resultExponent == 0) {
    resultFraction = (static_cast< std::uint64_t >(1) << 51) | (resultFraction >> 1);
  } else if (resultExponent == -1) {
    resultFraction = (static_cast< std::uint64_t >(1) << 50) | (resultFraction >> 2);
    resultExponent = 0;
  }
  const std::uint32_t mantissa = static_cast< std::uint32_t >(resultFraction >> 29) & 0x7fffffu;
  return FromBits((bits & 0x80000000u) | (static_cast< std::uint32_t >(resultExponent) << 23) |
                  mantissa);
}

// 0xbf722c-0xbf7384: ONE inlined 0xbf69d0 sphere test against a cap centre, run for cap a
// and then cap b. Both arms carry the source's caller-side gates: the inside arm only when
// best > 0 (strict), and a hit only when t < best (strict b.mi, so a tie keeps the earlier
// producer). `best`/`code` are the source's s8/w22.
void CapCandidate(const Vec3& center, const Vec3& origin, const Vec3& direction, float radius,
                  float length, float& best, int& code) {
  const float dx = center.x - origin.x;
  const float dy = center.y - origin.y;
  const float dz = center.z - origin.z;
  const float r2 = radius * radius;
  const float m2 = (dx * dx + dy * dy) + dz * dz;
  if (m2 < r2) { // boundary m^2 == r^2 is outside
    if (0.f < best) {
      best = 0.f;
      code = 1;
    }
    return;
  }
  const float b = (dx * direction.x + dy * direction.y) + dz * direction.z;
  if (b < 0.f)
    return;
  const float disc = r2 - (m2 - b * b);
  if (!(disc > 0.f))
    return; // exact tangency misses
  const float t = b - PortRemasteredGroundContacts::Source::SqrtF(disc);
  if (!(t < length))
    return; // strict; an endpoint exactly at `length` misses
  if (t < best) {
    best = t;
    code = 2;
  }
}

} // namespace

namespace Source {

// ARM FPRecipEstimate (FRECPE) alone: the estimate Reciprocal() starts from, without its two
// refinements. Exposed because the refined sequence can mask estimate differences (its
// constant, and its subnormal partial normalization); the kernel only calls it through
// Reciprocal.
float ReciprocalEstimate(float value) { return ArmReciprocalEstimate(value); }

float Reciprocal(float value) {
  // fmov v4.2s,#1.0; mov v4.s[0],v2.s[0]; frecpe v2,v4; frecps v5,v2,v4; fmul v2,v2,v5;
  // frecps v4,v2,v4; fmul v2,v2,v4 - lane 0 is the reciprocal, lane 1 the dummy 1.0.
  // The refinements run on whatever the estimate is, as the instruction sequence does:
  // infinity gives 0, zero gives infinity (FRECPS specifies zero*infinity as exactly 2.0,
  // which leaves both fixed), and the sub-2^-128 range - where the estimate alone is
  // infinity - comes back with the refinements' sign flip. The source only reaches this
  // with |u.dir| >= 1e-5, i.e. normal inputs.
  float result = ReciprocalEstimate(value);
  result = result * ReciprocalStep(result, value);
  result = result * ReciprocalStep(result, value);
  return result;
}

int SolveQuadratic(float a, float b, float c, float (&roots)[2]) {
  // 0x699b0. Both thresholds are `b.pl` on fabs(), so an unordered compare takes the
  // "not less" side (a NaN coefficient is not treated as tiny).
  if (!(std::fabs(a) < kQuadraticEpsilon)) {
    // disc = b*b + (a * (-4.0f)) * c: fmul s4 = a * -4; fmul s2 = s4 * c; fadd with b*b.
    const float scaled = (a * -4.0f) * c;
    const float disc = b * b + scaled;
    if (!(disc >= 0.f))
      return 0; // 0x699f8 b.ge; a negative or unordered discriminant exits
    // The source inlines the 0x65f70 sequence here without its !(x > 0) guard, because
    // this path is only reached with disc > 0, where the two are bit-identical.
    const float sq = (disc > 0.f) ? PortRemasteredGroundContacts::Source::SqrtF(disc) : 0.f;
    // r1 = (0.5 / a) * (-b - sq); r2 = (0.5 / a) * (sq - b); 0.5/a is a true fdiv.
    const float inverseA = 0.5f / a;
    const float minusB = -b;
    const float r1 = inverseA * (minusB - sq);
    const float r2 = inverseA * (sq - b);
    // b.pl on fcmp a,0: a >= 0, a == -0.0 and NaN all take the "ascending" store.
    if (!(a < 0.f)) {
      roots[0] = r1;
      roots[1] = r2;
    } else {
      roots[0] = r2;
      roots[1] = r1;
    }
    return 2;
  }
  if (!(std::fabs(b) < kQuadraticEpsilon)) {
    roots[0] = (-c) / b; // fneg then fdiv
    return 1;
  }
  return 0;
}

bool InfiniteCylinder(Vec3 axisPoint, Vec3 unitAxis, float radius, Vec3 rayOrigin,
                      Vec3 unitDirection, float& nearDistance, float& farDistance) {
  // 0xbf73c0, Remastered axes: W = ray origin - axis point, A = unit axis, D = ray dir.
  const float wx = rayOrigin.x - axisPoint.x;
  const float wy = rayOrigin.y - axisPoint.y;
  const float wz = rayOrigin.z - axisPoint.z;
  // W x A and D x A, plain component crosses (every sub is a difference of exact products).
  const float wax = wy * unitAxis.z - wz * unitAxis.y;
  const float way = wz * unitAxis.x - wx * unitAxis.z;
  const float waz = wx * unitAxis.y - wy * unitAxis.x;
  const float dax = unitAxis.z * unitDirection.y - unitAxis.y * unitDirection.z;
  const float day = unitAxis.x * unitDirection.z - unitAxis.z * unitDirection.x;
  const float daz = unitAxis.y * unitDirection.x - unitAxis.x * unitDirection.y;
  // a = |D x A|^2 and c = |W x A|^2 - r*r, both grouped z^2 + (x^2 + y^2); b is
  // 2 * ((Wx*Dx + Wy*Dy) + Wz*Dz) with r*r taken first on the c path.
  const float a = daz * daz + (dax * dax + day * day);
  const float b = 2.f * ((wax * dax + way * day) + waz * daz);
  const float c = (waz * waz + (wax * wax + way * way)) - radius * radius;
  // The source zeroes its 4-float root buffer (stp xzr,xzr) before the call.
  float roots[2] = {0.f, 0.f};
  const int count = SolveQuadratic(a, b, c, roots);
  if (count == 0)
    return false; // both outputs untouched
  nearDistance = roots[0];
  farDistance = (count == 2) ? roots[1] : roots[0];
  return true;
}

} // namespace Source

int RayCapsule(const Capsule& capsule, Vec3 origin, Vec3 unitDirection, float length,
               float& distance) {
  // NCollisionRayCast::Capsule 0xbf6f10 runs in Remastered axes: convert the public
  // port-frame inputs once, before any arithmetic.
  const Vec3 a = ToRem(capsule.a);
  const Vec3 b = ToRem(capsule.b);
  const Vec3 o = ToRem(origin);
  const Vec3 d = ToRem(unitDirection);
  const float radius = capsule.radius;

  // 0xbf6f2c-0xbf6f60: axis = b - a, tested while still unnormalized.
  const Vec3 axis = {b.x - a.x, b.y - a.y, b.z - a.z};
  if (!PortRemasteredGroundContacts::Source::IsNormalizable(ToContacts(axis))) {
    // 0xbf7070: degenerate axis. The inlined 0xbf69d0 sphere test at `a` only, immediate
    // return: no caps, no cylinder, and `b` is not read.
    const float dx = a.x - o.x;
    const float dy = a.y - o.y;
    const float dz = a.z - o.z;
    const float r2 = radius * radius;
    const float m2 = (dx * dx + dy * dy) + dz * dz;
    if (m2 < r2) {
      distance = 0.f; // 0xbf70b0, written directly (no best gate on this path)
      return 1;
    }
    const float bd = (dx * d.x + dy * d.y) + dz * d.z;
    if (bd < 0.f)
      return 0;
    const float disc = r2 - (m2 - bd * bd);
    if (!(disc > 0.f))
      return 0;
    const float t = bd - PortRemasteredGroundContacts::Source::SqrtF(disc);
    if (!(t < length))
      return 0;
    distance = t;
    return 2;
  }
  // 0xbf6f6c: Normalize in place; u is the unit axis, and the dots below are all
  // grouped ((x + y) + z) in the source's order.
  const Vec3 u = FromContacts(PortRemasteredGroundContacts::Source::Normalize(ToContacts(axis)));
  const float adotd = (u.x * d.x + u.y * d.y) + u.z * d.z;
  const float adota = (u.x * a.x + u.y * a.y) + u.z * a.z;
  const float adotb = (u.x * b.x + u.y * b.y) + u.z * b.z;
  const float adoto = (u.x * o.x + u.y * o.y) + u.z * o.z;

  int code = 0;        // w22
  float best = length; // s8, initialised to len at 0xbf6ff4
  // Every non-returning path below falls through to the cap section at 0xbf722c: the
  // source's rejections (-> 0xbf7228) only clear the code, and its own caps are entered
  // either with best = len or with best = the clipped cylinder parameter.
  if (!(std::fabs(adotd) < kCapsuleEpsilon)) {
    // 0xbf70bc GENERAL: the ray is not perpendicular to the axis.
    const float inv = Source::Reciprocal(adotd);
    // fsub then fneg on the axial differences (the negated form is what the source stores,
    // including its -0 results), then two fmuls against the reciprocal.
    const float axialA = adoto - adota;
    const float axialB = adoto - adotb;
    const float tA = (-axialA) * inv;
    const float tB = (-axialB) * inv;
    // 0xbf70f4-0xbf7130 fcsel chain: tmax = min(len, max(tA,tB)), tmin = max(0, min(tA,tB)).
    const float zero = 0.f;
    const float maxA = (zero - tA >= zero) ? zero : tA;
    const float minB = (length - tB >= zero) ? tB : length;
    const float minA = (length - tA >= zero) ? tA : length;
    const float maxB = (zero - tB >= zero) ? zero : tB;
    const float tmax = (tA <= tB) ? minB : minA;
    const float tmin = (tA <= tB) ? maxA : maxB;
    // 0xbf7134: tmax <= 0 (or unordered) rejects before the cylinder. The root buffers are
    // zeroed at 0xbf70e4/0xbf7138 before the call.
    if (tmax > 0.f) {
      float t1 = 0.f;
      float s = 0.f;
      // 0xbf7160/0xbf7164/0xbf7170: a false return, `s <= tmin` (non-strict) and
      // `t1 >= tmax` (non-strict: a span-exit tie misses) all skip the lateral hit.
      if (Source::InfiniteCylinder(a, u, radius, o, d, t1, s) && s > tmin && t1 < tmax) {
        // 0xbf717c-0xbf7190: clipped = min(tmax, max(tmin, t1)), stored into `best` BEFORE
        // the length test below. That store is load-bearing: clipped <= tmax <= length, so the
        // cap arms at 0xbf722c compare their t against the clipped parameter, and a cap that is
        // nearer than `length` but farther than `clipped` must lose to the cylinder (the ISA
        // fixture's hc09_general_lateral returns the cylinder's 5, not cap b's 5.4202). The
        // cap-inside gate `best > 0` is unaffected: length > 0 here, and every `tmax <= 0` case
        // left the lateral path earlier.
        const float clippedMax = (tmin - t1 >= zero) ? tmin : t1;
        const float clipped = (clippedMax - tmax >= zero) ? tmax : clippedMax;
        best = clipped;
        if (std::fabs(clipped) < kCapsuleEpsilon) {
          return 1; // 0xbf71ac: tOut is NOT written on this path
        }
        if (clipped < length)
          code = 2; // 0xbf7220; best already holds the clipped value
      }
    }
  } else {
    // 0xbf7000 PERPENDICULAR: strict axial span test (both sides), then an immediate return.
    // Only the accepted PERP return (hit or inside) skips the `length` test and both caps; a
    // span-test rejection lands on 0xbf7228 like any other path and does run them (that is HC7
    // and the fixture's own `perp_span_hi_eq_b` row).
    if (adoto - adota > 0.f && adoto - adotb < 0.f) {
      float t1 = 0.f;
      float s = 0.f;
      if (Source::InfiniteCylinder(a, u, radius, o, d, t1, s) && s >= 0.f) {
        // 0xbf7054-0xbf7068: tOut = max(t1, 0), and only t1 < 0 counts as inside. Written as
        // a comparison rather than std::fmax, whose result for two equal zeros C leaves
        // unspecified: FMAX(-0, +0) is +0 on the ISA. For finite t1 this also fixes the
        // equal-zero sign.
        distance = t1 > 0.f ? t1 : 0.f;
        return !(t1 < 0.f) ? 2 : 1;
      }
    }
  }

  // 0xbf722c then 0xbf72e0: cap a, then cap b, both with the capsule's radius and `len`.
  CapCandidate(a, o, d, radius, length, best, code);
  CapCandidate(b, o, d, radius, length, best, code);
  // 0xbf7388: writes best only for a nonzero code; the code 1 here comes from a cap-inside
  // candidate and carries best == 0.
  if (code != 0)
    distance = best;
  return code;
}

} // namespace PortRemasteredCapsuleRay
