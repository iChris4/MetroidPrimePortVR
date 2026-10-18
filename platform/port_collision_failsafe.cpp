#include "port_collision_failsafe.h"

#include "port_remastered_ground_contacts.h" // Source::Unit, the verified 0x6b6b8 emulation

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace PortCollisionFailsafe {
namespace {

constexpr float kEps5 = 1e-5f; // 0x3727c5ac
constexpr float kEps4 = 1e-4f; // 0x38d1b717

// ARM reciprocal-square-root estimate and its refinement step, copied verbatim from
// platform/port_remastered_ground_contacts.cpp (verified there against
// CVector3f::Normalize 0x6b0b0/0x6b238). CCollidableSphere::AABox_Sphere 0xc02d70's
// exterior distance uses the estimate with ONE frsqrts refinement, so the
// two-refinement helpers exported as PortRemasteredGroundContacts::Source::* cannot
// stand in for it; its exterior NORMAL does use the two-refinement path and does call
// that exported Unit (CUnitVector3f ctor 0x71164 -> AsNormalizedOrZero 0x6b6b8).
static_assert(sizeof(float) == 4 && std::numeric_limits< float >::is_iec559);
float FromBits(std::uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
std::uint32_t Bits(float f) { std::uint32_t b; std::memcpy(&b, &f, 4); return b; }
float ReciprocalSqrtEstimate(float v) {
  const std::uint32_t bits = Bits(v);
  int exponent = static_cast<int>((bits >> 23) & 255u);
  std::uint32_t fraction = bits & 0x7fffffu;
  if (exponent == 255) return fraction ? FromBits(bits | 0x400000u) : 0.f;
  if ((bits & 0x7fffffffu) == 0) return FromBits((bits & 0x80000000u) | 0x7f800000u);
  if (bits >> 31) return FromBits(0x7fc00000u);
  if (exponent == 0) {
    while ((fraction & 0x400000u) == 0) {
      fraction <<= 1;
      --exponent;
    }
    fraction = (fraction & 0x3fffffu) << 1;
  }
  std::uint32_t scaled = (exponent & 1) ? (128u | (fraction >> 16)) : (256u | (fraction >> 15));
  scaled = scaled < 256u ? scaled * 2u + 1u : ((scaled & ~1u) + 1u) * 2u;
  std::uint32_t b = 512u;
  while (scaled * (b + 1u) * (b + 1u) < (1u << 28)) ++b;
  return FromBits((static_cast<std::uint32_t>((380 - exponent) / 2) << 23) |
                  (((b + 1u) / 2u & 255u) << 15));
}
float ReciprocalSqrtStep(float a, float b) {
  // FRSQRTS is fused (3-a*b)/2; zero*infinity is specified to return 1.5.
  if ((a == 0.f && std::isinf(b)) || (b == 0.f && std::isinf(a))) return 1.5f;
  return std::fma(-a, b, 3.f) * .5f;
}

Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Mul(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

// Remastered is Y-up: rem = (-port.x, port.z, port.y), its own inverse
// (platform/port_remastered_ground_world.cpp).
struct R3 {
  float v[3];
};
R3 ToRem(const Vec3& p) { return {{-p.x, p.z, p.y}}; }
Vec3 FromRem(const R3& r) { return {-r.v[0], r.v[2], r.v[1]}; }
void BoxToRem(const Box& b, R3& mn, R3& mx) {
  mn = {{-b.max.x, b.min.z, b.min.y}};
  mx = {{-b.min.x, b.max.z, b.max.y}};
}

Vec3 Center(const Box& b) {
  return {(b.min.x + b.max.x) * .5f, (b.min.y + b.max.y) * .5f, (b.min.z + b.max.z) * .5f};
}

float MinHalfExtent(const Box& b) {
  return std::fmin(std::fmin((b.max.x - b.min.x) * .5f, (b.max.y - b.min.y) * .5f),
                   (b.max.z - b.min.z) * .5f);
}

} // namespace

bool SelectPush(const std::vector< Contact >& contacts, const Vec3& from, float worldDt,
                Vec3& push) {
  float best = -FLT_EPSILON;
  bool moved = false;
  push = {};
  for (const Contact& c : contacts) {
    float curMin = FLT_MAX;
    bool has = false;
    Vec3 cand;
    for (const Impact& i : c.impacts) {
      // Behaviour bit 0x20 (+0x28) skips an impact; no MP1 material maps to it.
      if (!(i.depth < curMin)) continue;
      curMin = i.depth;
      if (!i.otherCharacter) {
        cand = Mul(i.otherNormal, i.depth + kEps4);
      } else {
        // Away from the other's bounds centre, horizontally, from the ORIGINAL
        // position, at most worldDt * 50.
        Vec3 d{from.x - i.otherCenter.x, from.y - i.otherCenter.y, 0.f};
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        d = len > FLT_EPSILON ? Mul(d, 1.f / len) : Vec3{}; // NormalizeOrZero
        cand = Mul(d, std::fmin(i.depth + kEps4, worldDt * 50.f));
      }
      has = true;
      moved = true;
    }
    if (has && curMin > best) {
      best = curMin;
      push = cand;
    }
  }
  return moved;
}

bool Core(const Vec3& from, float worldDt, const CoreHooks& hooks, Vec3& out) {
  return Core(from, from, worldDt, hooks, out);
}

bool Core(const Vec3& from, const Vec3& objectState, float worldDt, const CoreHooks& hooks,
          Vec3& out) {
  out = from;
  bool moved = false;
  std::vector< Contact > list;
  for (int iter = 0; iter < 5; ++iter) {
    list.clear();
    // CheckCollision 0xc1ec60, then the pass-all result filter (vt[0x10], 0xc23a14),
    // which fails on an empty list.
    if (!hooks.collide(out, list) || list.empty()) break;
    Vec3 push;
    if (SelectPush(list, from, worldDt, push)) moved = true;
    out = Add(out, push);
  }
  Vec3 tmp = out;
  // 0xc1fc54 tunnels from the object's state transform (CPhysicsState::GetTransform),
  // while the pushes and the character centre use `from` (param_3).
  if (hooks.tunnel(objectState, out, tmp)) {
    out = tmp;
    return true;
  }
  return moved;
}

bool DoCollisionFailsafe(const Vec3& cur, bool hasFailsafeFilter,
                         const std::function< bool(bool failsafe, Vec3& out) >& core,
                         const std::function< bool(bool failsafe, const Vec3& pos) >& blocked,
                         Vec3& out) {
  out = cur;
  const bool r = core(false, out);
  if (!hasFailsafeFilter) {
    if (!r) out = cur;
    return r;
  }
  if (!r) {
    out = cur;
    return false;
  }
  if (!blocked(false, out)) return true;
  // Again from cur with the failsafe filter; the first result is discarded.
  if (!core(true, out)) {
    out = cur;
    return false;
  }
  if (!blocked(true, out)) return true;
  // Still stuck: not moved, but reported resolved.
  out = cur;
  return true;
}

bool IsFloor(float up, int orientation) {
  // 0xbecb40: up > 0.85 || orient == 0 || (up > 0.70710677 && (unsigned)(orient - 1) > 1).
  return up > 0.85f || orientation == 0 ||
         (up > 0.70710677f && unsigned(orientation - 1) > 1u);
}

bool TunnelingFailsafeApplies(const Box& bounds, const Vec3& from, const Vec3& to) {
  const Vec3 delta = Sub(to, from);
  const float len = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
  return len > MinHalfExtent(bounds);
}

bool DoTunnelingFailsafe(const Box& bounds, const Vec3& pos, const Vec3& from, const Vec3& to,
                         const TunnelHooks& hooks, Vec3& out) {
  out = to;
  const float cy = Center(bounds).z - pos.z;
  const float footZ = bounds.min.z;
  const float minHalf = MinHalfExtent(bounds);
  const Vec3 delta = Sub(to, from);
  const float len = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
  const Vec3 dir = len > FLT_EPSILON ? Mul(delta, 1.f / len) : Vec3{};
  if (len <= minHalf) return false;
  const Vec3 start{from.x, from.y, from.z + cy};
  const RayHit res = hooks.ray(start, dir, len);
  if (res.code == kRay_Inside) {
    out = from;
    return true;
  }
  if (res.code != kRay_Hit) return false;
  const float halfH =
      .5f * std::fmax(bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y);
  if (res.t < halfH) {
    out = from;
    return true;
  }
  // The box bottom lands on the ray point: pos ends cy above the swept path, as decoded.
  out = Add(start, Mul(dir, res.t - halfH));
  out.z -= footZ - pos.z;
  if (IsFloor(res.normal.z, res.orientation)) {
    Vec3 offset;
    if (hooks.safeOffset(out, minHalf + FLT_EPSILON, offset)) out = Add(out, offset);
  }
  return true;
}

namespace {
constexpr float kA = 0.70710677f;
constexpr float kB = 0.57735026f;
} // namespace

// 0x459c800 in port axes (SPEC table; Remastered (x,y,z) -> port (-x,z,y)).
const Vec3 kSafeDirections[26] = {
    {0, 1, 0},     {0, -1, 0},     {1, 0, 0},      {-1, 0, 0},     {0, 0, 1},
    {0, 0, -1},    {-kA, 0, kA},   {kA, 0, -kA},   {kA, 0, kA},    {-kA, 0, -kA},
    {-kA, kA, 0},  {kA, -kA, 0},   {kA, kA, 0},    {-kA, -kA, 0},  {0, kA, kA},
    {0, -kA, -kA}, {0, kA, -kA},   {0, -kA, kA},   {-kB, kB, kB},  {kB, kB, kB},
    {-kB, kB, -kB}, {kB, kB, -kB}, {-kB, -kB, kB}, {kB, -kB, kB},  {-kB, -kB, -kB},
    {kB, -kB, -kB},
};

const float kSafeRadii[21] = {0.01f,  0.015f, 0.02f,  0.03f,  0.045f, 0.065f, 0.095f,
                              0.14f,  0.21f,  0.315f, 0.47f,  0.705f, 1.055f, 1.58f,
                              2.37f,  3.f,    4.f,    5.f,    6.f,    7.f,    8.f};

bool CalculateOffsetToSafeCollisionPoint(const Box& box, const Vec3& pos, float maxDist,
                                         const SafeHooks& hooks, Vec3& offset) {
  offset = {};
  if (!hooks.collidesAt(pos)) return true;
  const Vec3 ctr = Center(box);
  const float minHalf = MinHalfExtent(box);
  for (int i = 0; i < 21; ++i) {
    const float d = kSafeRadii[i];
    if (d >= maxDist) break;
    for (int j = 0; j < 26; ++j) {
      const Vec3& dir = kSafeDirections[j];
      if (d >= minHalf && hooks.rayBlocked(ctr, dir, d)) continue;
      const Vec3 o = Mul(dir, d);
      if (hooks.collidesAt(Add(pos, o))) continue;
      offset = o;
      return true; // first hit wins
    }
  }
  return false;
}

namespace {
// Selection core of 0xbf61b4 in Remastered axes. `emit(axis, depth, sign)` is the helper
// 0xbf6650 after its depth > 0 test and returns whether it appended an impact; the
// fallback runs whenever no helper call appended one.
template < class Emit >
bool BoxBoxCore(const R3& aMin, const R3& aMax, const R3& bMin, const R3& bMax, Emit&& emit) {
  // DoBoundsOverlap 0x98234 (inclusive) gates the whole routine.
  for (int k = 0; k < 3; ++k) {
    if (aMax.v[k] < bMin.v[k] || bMax.v[k] < aMin.v[k]) return false;
  }
  bool any = false;
  auto add = [&](int axis, float depth, float sign) {
    if (!(depth > 0.f)) return;
    if (emit(axis, depth, sign)) any = true;
  };
  for (int k = 0; k < 3; ++k) {
    const float extB = bMax.v[k] - bMin.v[k];
    const float extA = aMax.v[k] - aMin.v[k];
    const float s0 = aMax.v[k] - bMin.v[k];
    const float s8 = bMax.v[k] - aMin.v[k];
    if (s0 < extB && s0 < extA) add(k, s0, 1.f);
    if (s8 < extA && s8 < extB) add(k, s8, -1.f);
  }
  if (!any) {
    // No helper call appended: the smallest non-negative difference, earlier axis on ties.
    const float d[6] = {aMax.v[0] - bMin.v[0], bMax.v[0] - aMin.v[0], aMax.v[1] - bMin.v[1],
                        bMax.v[1] - aMin.v[1], aMax.v[2] - bMin.v[2], bMax.v[2] - aMin.v[2]};
    float best = (d[0] < FLT_MAX && d[0] >= 0.f) ? d[0] : FLT_MAX;
    int axis = 0;
    for (int k = 1; k < 6; ++k) {
      if (d[k] < best && d[k] >= 0.f) {
        best = d[k];
        axis = k / 2;
      }
    }
    const float p = aMax.v[axis] - bMin.v[axis];
    const float n = bMax.v[axis] - aMin.v[axis];
    if (p < n) {
      add(axis, p, 1.f);
    } else {
      add(axis, n, -1.f);
    }
  }
  return any;
}

// Outward normal of a half: unit vector on `axis` (Remastered frame) scaled by `s`.
Vec3 AxisNormal(int axis, float s) {
  R3 n{{0.f, 0.f, 0.f}};
  n.v[axis] = s;
  return FromRem(n);
}

// Component-wise negation, so the zero signs flip too. 0xbf6650 builds one `n` over
// CVector3f::skZeroVector (0x1d0c200, three +0.0) with n[axis] = sign, negates all three
// components (fneg s9/s10/s11 at 0xbf66b8-0xbf66c0) and uses that negated vector both as
// the per-candidate filter argument and as the other half's normal (0xbf689c-0xbf68a4);
// the self half keeps the +0.0 zeros. Negating the converted self normal is that same
// operation (FromRem(-r) == -FromRem(r) component-wise), zeros included.
Vec3 NegatedNormal(const Vec3& n) { return {-n.x, -n.y, -n.z}; }
} // namespace

bool BoxBoxImpacts(const Box& self, const Box& other, std::vector< Impact >& out) {
  // A = self, B = other; both halves carry the depth, A's normal is +axis*sign and B's
  // (the one kept) -axis*sign. The legacy normal is AxisNormal(axis, -sign) bit for bit,
  // zero signs included, exactly as before the paired refactor: 0xbf6650's other-half
  // normal is the component-wise negation instead (see NegatedNormal), but this API's
  // output must not change.
  R3 aMin, aMax, bMin, bMax;
  BoxToRem(self, aMin, aMax);
  BoxToRem(other, bMin, bMax);
  return BoxBoxCore(aMin, aMax, bMin, bMax, [&](int axis, float depth, float sign) {
    Impact i;
    i.depth = depth;
    i.otherNormal = AxisNormal(axis, -sign);
    out.push_back(i);
    return true;
  });
}

bool BoxBoxPairedImpacts(const Box& self, const Box& other, const NormalPredicate& passes,
                         std::vector< PairedImpact >& out) {
  R3 aMin, aMax, bMin, bMax;
  BoxToRem(self, aMin, aMax);
  BoxToRem(other, bMin, bMax);
  // 0x98800 intersection: max of the minimums, then min of the maximums. The source's
  // fcsel keeps SELF on an exact tie of the two minima (fcsel s0,s0,s2,ge at
  // 0x98814/0x98820/0x98830) and OTHER on a tie of the two maxima (fcsel s4,s6,s4,ge at
  // 0x9883c/0x98858/0x98864); only zero signs can tell the two answers apart.
  R3 iMin, iMax;
  for (int k = 0; k < 3; ++k) {
    iMin.v[k] = aMin.v[k] >= bMin.v[k] ? aMin.v[k] : bMin.v[k];
    iMax.v[k] = aMax.v[k] < bMax.v[k] ? aMax.v[k] : bMax.v[k];
  }
  static const int kNext[3] = {1, 2, 0}; // 0x1d0b9b4
  return BoxBoxCore(aMin, aMax, bMin, bMax, [&](int axis, float depth, float sign) {
    // SurfaceCollisionPasses(other material, -n): the other half's outward normal, the
    // component-wise negation of the self normal (F1); the predicate gets that value.
    const Vec3 otherN = NegatedNormal(AxisNormal(axis, sign));
    if (passes && !passes(otherN)) return false;
    // 0xbf6650 points: the face is the intersection box on the other two axes, the four
    // corners summed ((c0 + c1) + c2) + c3 and scaled by 0.25; the axis component is p8
    // for the self half and p9 for the other (+1: p8 = max, p9 = min; -1: swapped).
    const int a1 = kNext[axis], a2 = kNext[a1];
    const R3& p8 = sign > 0.f ? iMax : iMin;
    const R3& p9 = sign > 0.f ? iMin : iMax;
    R3 c[4];
    for (int i = 0; i < 4; ++i) {
      c[i].v[axis] = p8.v[axis];
      c[i].v[a1] = (i < 2) ? iMin.v[a1] : iMax.v[a1];
      c[i].v[a2] = (i == 0 || i == 2) ? iMin.v[a2] : iMax.v[a2];
    }
    R3 mid;
    for (int k = 0; k < 3; ++k) mid.v[k] = (((c[0].v[k] + c[1].v[k]) + c[2].v[k]) + c[3].v[k]) * 0.25f;
    R3 otherPt = mid;
    otherPt.v[axis] = p9.v[axis];
    PairedImpact i;
    i.selfDepth = depth;
    i.otherDepth = depth;
    i.selfNormal = AxisNormal(axis, sign);
    i.otherNormal = otherN;
    i.selfPoint = FromRem(mid);
    i.otherPoint = FromRem(otherPt);
    out.push_back(i);
    return true;
  });
}

bool BoxSphereImpacts(const Box& self, const Vec3& center, float radius,
                      std::vector< Impact >& out) {
  // CCollidableSphere::AABox_Sphere 0xc02d70 (dispatcher 0xc02cec): the player's box
  // against a transformed sphere. Remastered axes x, y (up), z; both halves of the
  // contact carry the same penetration depth, and the failsafe uses the box half's
  // depth with the sphere half's normal (that normal is all this helper reports).
  R3 mn, mx;
  BoxToRem(self, mn, mx);
  const R3 c = ToRem(center);
  const float r = radius;

  // 0xc02e34-0xc02ed0: squared distance to the box, adding a squared gap only where
  // the centre is outside that axis' interval, in Rem X, Y, Z order.
  float d2 = 0.f;
  for (int k = 0; k < 3; ++k) {
    const float v = c.v[k];
    if (v < mn.v[k]) {
      const float gap = v - mn.v[k];
      d2 = d2 + gap * gap;
    } else if (mx.v[k] < v) {
      const float gap = v - mx.v[k];
      d2 = d2 + gap * gap;
    }
  }

  Impact impact;
  if (std::fabs(d2) < kEps5) {
    // 0xc02ed4-0xc03034: near/interior. Nearest signed face distance, faces visited in
    // order -X, -Y, -Z, +X, +Y, +Z; every face replaces the incumbent only when
    // STRICTLY closer, so equal distances keep the earlier face. The compares are strict
    // because the source guards each block with b.pl after FCMP: PL is N==0 and an
    // unordered FCMP result is NZCV=0011 (N=0), so the guard IS taken and the incumbent
    // is kept for an unordered candidate as well.
    R3 f{{-1.f, 0.f, 0.f}};
    float best = c.v[0] - mn.v[0];
    if (c.v[1] - mn.v[1] < best) {
      f = R3{{0.f, -1.f, 0.f}};
      best = c.v[1] - mn.v[1];
    }
    if (c.v[2] - mn.v[2] < best) {
      f = R3{{0.f, 0.f, -1.f}};
      best = c.v[2] - mn.v[2];
    }
    if (mx.v[0] - c.v[0] < best) {
      f = R3{{1.f, 0.f, 0.f}};
      best = mx.v[0] - c.v[0];
    }
    if (mx.v[1] - c.v[1] < best) {
      f = R3{{0.f, 1.f, 0.f}};
      best = mx.v[1] - c.v[1];
    }
    if (mx.v[2] - c.v[2] < best) {
      f = R3{{0.f, 0.f, 1.f}};
      best = mx.v[2] - c.v[2];
    }
    impact.depth = r + best; // 0xc0306c
    // 0xc03034-0xc03068: the sphere half's normal is the NEGATIVE of that face normal
    // (local_1b0 = (-fVar17,-fVar11,-fVar18); the box half keeps +F).
    impact.otherNormal = FromRem({{-f.v[0], -f.v[1], -f.v[2]}});
    out.push_back(impact);
    return true;
  }
  // 0xc02f40-0xc02f48: d2 >= r*r is a miss, exact tangency included.
  if (r * r <= d2) return false;

  // 0xc03188-0xc032ec: Q = component-wise clamp of the centre onto the box.
  R3 q = c;
  for (int k = 0; k < 3; ++k) {
    if (q.v[k] < mn.v[k]) {
      q.v[k] = mn.v[k];
    } else if (mx.v[k] < q.v[k]) {
      q.v[k] = mx.v[k];
    }
  }
  // 0xc032f0-0xc03330: L = d2 * (estimate refined ONCE); the fcmle/bic pair zeroes the
  // refinement when d2 <= 0 (an ordered compare, so NaN leaves it alone). Depth = r - L.
  const float estimate = ReciprocalSqrtEstimate(d2);
  float refined = estimate * ReciprocalSqrtStep(d2, estimate * estimate);
  if (d2 <= 0.f) refined = 0.f;
  const float length = d2 * refined;
  // 0xc03338: CUnitVector3f 0x71164 -> AsNormalizedOrZero 0x6b6b8, i.e. two
  // refinements and fmulx, exactly the verified PortRemasteredGroundContacts helper.
  // d2 >= 1e-5 keeps the difference Q - C normalizable.
  const PortRemasteredGroundContacts::Vec3 unit = PortRemasteredGroundContacts::Source::Unit(
      {q.v[0] - c.v[0], q.v[1] - c.v[1], q.v[2] - c.v[2]});
  impact.depth = r - length;
  impact.otherNormal = FromRem(R3{{unit.x, unit.y, unit.z}});
  out.push_back(impact);
  return true;
}

RayHit RayAABox(const Box& box, const Vec3& start, const Vec3& dir, float length) {
  RayHit miss;
  R3 mn, mx;
  BoxToRem(box, mn, mx);
  const R3 p = ToRem(start);
  const R3 d = ToRem(dir);
  R3 normal{{0.f, 1.f, 0.f}}; // skUpVector until an entry face is taken
  float tEnter = 0.f;
  float tExit = FLT_MAX;
  for (int k = 0; k < 3; ++k) {
    if (std::fabs(d.v[k]) < kEps5) {
      if (p.v[k] < mn.v[k] || mx.v[k] < p.v[k]) return miss;
      continue;
    }
    const float inv = 1.f / d.v[k];
    const bool pos = d.v[k] >= 0.f;
    const float entry = ((pos ? mn.v[k] : mx.v[k]) - p.v[k]) * inv;
    const float exit = ((pos ? mx.v[k] : mn.v[k]) - p.v[k]) * inv;
    // Axis 0 takes its entry only when non-negative; later axes when entry >= tEnter.
    if (k == 0 ? entry >= 0.f : tEnter <= entry) {
      tEnter = entry;
      normal = {{0.f, 0.f, 0.f}};
      normal.v[k] = pos ? -1.f : 1.f;
    }
    if (tExit - exit >= 0.f) tExit = exit;
  }
  if (tExit <= 0.f || tExit < tEnter) return miss;
  if (length < tEnter) return miss;
  RayHit hit;
  if (tEnter == 0.f) {
    hit.code = kRay_Inside;
    hit.t = 0.f;
    return hit;
  }
  hit.code = kRay_Hit;
  hit.t = tEnter;
  hit.normal = FromRem(normal);
  return hit;
}

// CMath::SqrtF 0x65f70 used to be file-local here; it now lives in the contacts module
// (PortRemasteredGroundContacts::Source::SqrtF, which is where the capsule kernel's
// 0x699b0 copy can share it). Same instruction sequence, same outputs.
RayHit RaySphere(const Vec3& center, float radius, const Vec3& start, const Vec3& dir,
                 float length) {
  // 0xbf6ac0 runs in Remastered axes, like the box-sphere producer.
  const R3 c = ToRem(center);
  const R3 s = ToRem(start);
  const R3 d = ToRem(dir);
  const float vx = c.v[0] - s.v[0];
  const float vy = c.v[1] - s.v[1];
  const float vz = c.v[2] - s.v[2];
  const float r2 = radius * radius; // fVar3
  const float d2 = vx * vx + vy * vy + vz * vz;
  RayHit result;
  // `r2 <= d2` leaves the strict interior: the boundary counts as outside, and the
  // ordered compare sends NaN down the inside arm, as the source branch does.
  if (r2 <= d2) {
    const float b = vx * d.v[0] + vy * d.v[1] + vz * d.v[2];
    if (0.f <= b) {
      // 0xbf6b2c: r2 - (d2 - b*b); 0.0 < disc, so exact tangency misses.
      const float disc = r2 - (d2 - b * b);
      if (0.f < disc) {
        const float t = b - PortRemasteredGroundContacts::Source::SqrtF(disc);
        if (t < length) {
          // 0xbf6b58-0xbf6b98: hit point = start + t*dir, then CUnitVector3f 0x71164
          // (AsNormalizedOrZero 0x6b6b8) of point - centre, the verified helper.
          const Vec3 point = Add(Mul(dir, t), start);
          const R3 p = ToRem(point);
          const PortRemasteredGroundContacts::Vec3 unit =
              PortRemasteredGroundContacts::Source::Unit(
                  {p.v[0] - c.v[0], p.v[1] - c.v[1], p.v[2] - c.v[2]});
          result.code = kRay_Hit;
          result.t = t;
          result.normal = FromRem(R3{{unit.x, unit.y, unit.z}});
          return result;
        }
      }
    }
    return result; // kRay_Miss
  }
  // 0xbf6ac4-0xbf6af8: strictly inside. t = 0, the point is the ray start and the
  // normal is the wrapper's skUpVector, which the helper never writes.
  result.code = kRay_Inside;
  result.t = 0.f;
  result.normal = FromRem(R3{{0.f, 1.f, 0.f}});
  return result;
}

} // namespace PortCollisionFailsafe
