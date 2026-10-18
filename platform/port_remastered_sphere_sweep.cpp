#include "port_remastered_sphere_sweep.h"

#include "port_remastered_ground_contacts.h" // Source::IsNormalizable, NormalizeWithMagnitude

namespace PortRemasteredSphereSweep {
namespace {

// Port (Z up) -> source/Remastered canonical (Y up), and its own inverse: the change is
// symmetric, so the same expression converts both ways, signed zeros included
// (platform/port_remastered_ground_world.cpp:30-31, 63-64).
struct SourceVec {
  float x = 0.f, y = 0.f, z = 0.f;
};
SourceVec ToSource(const Vec3& v) { return {-v.x, v.z, v.y}; }
Vec3 FromSource(const SourceVec& v) { return {-v.x, v.z, v.y}; }

namespace GroundContacts = PortRemasteredGroundContacts;

// The shared helpers take the ground-contacts module's own Vec3: the source frame's
// components by value, copied rather than reinterpreted.
bool IsNormalizable(const SourceVec& v) {
  return GroundContacts::Source::IsNormalizable({v.x, v.y, v.z});
}
float NormalizeWithMagnitude(SourceVec& v) {
  GroundContacts::Vec3 unit{v.x, v.y, v.z};
  const float magnitude = GroundContacts::Source::NormalizeWithMagnitude(unit);
  v = {unit.x, unit.y, unit.z};
  return magnitude;
}

} // namespace

int SphereSphereSwept(const Sphere& self, const Sphere& other, Vec3 unitDirection,
                      bool pairPasses, float& distance, std::vector< Impact >& impacts) {
  // 0xc01300-0xc01338: the pair's filter test, and the producer's re-test later. A
  // rejection returns 0 immediately and touches neither output.
  if (!pairPasses) {
    return kMiss;
  }

  const SourceVec c0 = ToSource(self.center);
  const SourceVec c1 = ToSource(other.center);
  const SourceVec d = ToSource(unitDirection);
  const float r0 = self.radius;
  const float r1 = other.radius;
  const float radiusSum = r0 + r1;
  // Delta from the START centres (0xc02aec/0xc02b20 and 0xc01424): the sweep's normal
  // and depth use it, never `c1 - (c0 + t*d)`.
  const SourceVec delta{c1.x - c0.x, c1.y - c0.y, c1.z - c0.z};
  // NCollisionBoolean::Sphere_Sphere 0xc042b0: strict, sum order ((dx^2 + dy^2) + dz^2),
  // radius term (r0 + r1)^2. Equality is false, and an unordered compare is not mi.
  const float d2 = ((delta.x * delta.x) + (delta.y * delta.y)) + (delta.z * delta.z);
  const float radiusSquared = radiusSum * radiusSum;

  if (d2 < radiusSquared) {
    // 0xc01380-0xc013b0 clears the list, 0xc013b4 calls CCollidableSphere::Sphere_Sphere
    // 0xc029f0, 0xc013b8 stores t = 0 and 0xc013bc returns 1 - unconditionally, even
    // when the producer emits no contact (coincident centres: IsNormalizable fails).
    // This port writes `distance` before running the producer rather than after it, as
    // the source stores t; only the state a completed call leaves is claimed equal, not
    // the interleaving (the producer never reads `t`, and the list's own allocation is
    // the caller's).
    impacts.clear();
    distance = 0.f;

    // 0xc02b18: an inclusive d2 <= R^2 re-test; b.hi takes the no-contact exit, and so
    // does an unordered compare. Unreachable after the strict test above, kept as the
    // producer has it.
    if (!(d2 <= radiusSquared)) {
      return kOverlap;
    }
    SourceVec n{delta.x, delta.y, delta.z};
    if (!IsNormalizable(n)) {
      return kOverlap;
    }
    // 0x6b100: the vector becomes the unit delta in place and the returned magnitude
    // comes from that same final reciprocal.
    const float m = NormalizeWithMagnitude(n);
    const float depth = radiusSum - m;
    const SourceVec p0{c0.x + r0 * n.x, c0.y + r0 * n.y, c0.z + r0 * n.z};
    const SourceVec p1{c1.x - r1 * n.x, c1.y - r1 * n.y, c1.z - r1 * n.z};
    const SourceVec negatedNormal{-n.x, -n.y, -n.z};
    impacts.push_back(Impact{FromSource(p0), FromSource(p1), FromSource(n),
                             FromSource(negatedNormal), depth});
    return kOverlap;
  }

  // 0xc013c4-0xc0140c: ray = origin c0, dir d, len = *t verbatim (no 1e-5 append);
  // target = {c1, r0 + r1}. NCollisionRayCast::Sphere 0xbf69d0's finite branch is
  // PortCollisionFailsafe::RaySphere, called through the same port/source conversion;
  // its `normal` belongs to the neighbouring routine and is not read here.
  const PortCollisionFailsafe::RayHit ray = PortCollisionFailsafe::RaySphere(
      other.center, radiusSum, self.center, unitDirection, distance);
  if (ray.code == PortCollisionFailsafe::kRay_Miss) {
    return kMiss;
  }
  // 0xc0141c: accepted only strictly inside the incoming t; an equal t and a NaN leave
  // both outputs untouched. (Code 1 sets t = 0 and is unreachable here: the pair gate
  // just ran the same predicate on the same values.)
  const float tp = ray.t;
  if (!(tp < distance)) {
    return kMiss;
  }

  // 0xc0145c: 0x6b030 on the start-centre delta. A real hit with a non-normalizable
  // delta is a silent no-op.
  SourceVec n{delta.x, delta.y, delta.z};
  if (!IsNormalizable(n)) {
    return kMiss;
  }

  // 0xc01464-0xc014dc: p = c0 + tp*d componentwise (0xc01464-0xc01470, 0xc0147c,
  // 0xc014a4), clear the list (0xc01474-0xc01498), *t = tp (0xc014a8), delta <-
  // NormalizeWithMagnitude (0xc014ac), depth = (r0 + r1) - m (0xc014dc).
  const SourceVec p{c0.x + tp * d.x, c0.y + tp * d.y, c0.z + tp * d.z};
  impacts.clear();
  distance = tp;
  const float m = NormalizeWithMagnitude(n);
  const float depth = radiusSum - m;
  const SourceVec p0{p.x + r0 * n.x, p.y + r0 * n.y, p.z + r0 * n.z};
  const SourceVec p1{c1.x - r1 * n.x, c1.y - r1 * n.y, c1.z - r1 * n.z};
  const SourceVec negatedNormal{-n.x, -n.y, -n.z};
  impacts.push_back(
      Impact{FromSource(p0), FromSource(p1), FromSource(n), FromSource(negatedNormal), depth});
  return kSweep;
}

} // namespace PortRemasteredSphereSweep
