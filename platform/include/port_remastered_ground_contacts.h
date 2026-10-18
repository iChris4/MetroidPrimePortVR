#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Geometry only, canonical Remastered coordinates (Y up). No material/owner
// or native-axis translation belongs in this module.
namespace PortRemasteredGroundContacts {
struct Vec3 { float x = 0.f, y = 0.f, z = 0.f; };
struct AABB { Vec3 min, max; };
struct Plane { Vec3 normal; float constant = 0.f; };
struct Contact {
  Vec3 primary;
  float D = 0.f, H = 0.f;
  bool hasSecondary = false;
  Vec3 secondary; // Read only when hasSecondary.
  std::size_t firstPoint = 0, lastPoint = 0; // Inclusive indices in points.
};
enum class SecondaryBranch {
  None, Contained, SingleClipPlane, ShallowCorner, ShallowEdge,
  UnnormalizableShallowEdge, NoCandidate, CandidatePlane, CandidateBlend,
  UnnormalizableCandidateBlend
};
struct Candidate { std::size_t plane = 0; float depth = 0.f; std::size_t point = 0; };
struct Provenance {
  // Order -X,+X,-Z,+Z,-Y,+Y. Interior is STRICT dot(n,p)<constant;
  // on-plane endpoints are outside but crossings are retained.
  std::array<Plane, 6> planes{};
  std::array<std::size_t, 6> crossingPlanes{};
  std::size_t crossingCount = 0;
  Vec3 lastIntersection;
  std::array<Vec3, 9> polygon{};
  std::size_t polygonSize = 0;
  // Distance-stage diagnostics below apply only to crossingCount>0.
  float Hraw = 0.f, polygonRayDistance = 0.f, cornerRayDistance = 0.f, finalDistance = 0.f;
  std::uint8_t cornerRayHits = 0, behindCorners = 0, shallowCorners = 0;
  bool usedEdgeFallback = false;
  std::array<std::uint8_t, 9> fallbackAxisHits{}; // Up/Left/Forward bits per edge.
  std::array<Candidate, 6> candidates{}; // Stable ascending depth order.
  std::size_t candidateCount = 0;
  SecondaryBranch secondaryBranch = SecondaryBranch::None;
};
struct Result {
  bool hit = false;
  Contact box, triangle;
  std::array<Vec3, 10> points{}; // Box point then triangle points.
  std::size_t pointCount = 0;
  Provenance provenance;
};
// NCollidableBoxTree::build_collision_info 0xc0a180. Preconditions as source:
// ordered finite box, finite vertices, supplied unit normal, no intermediate
// overflow. Degenerate triangles are not rejected by an invented area test.
// IEEE binary32 round-to-nearest, gradual underflow, no fast-math/contraction.
Result Build(const AABB&, const std::array<Vec3, 3>&, Vec3 normal);
// 0xc04740: inclusive SAT predicate used by the NoPostFilter tree traversal.
// Deliberately independent of Build's strict polygon clipping.
bool Intersects(const AABB&, const std::array<Vec3, 3>&);
// Source helpers exposed for independent boundary/ISA regression tests.
namespace Source {
bool IsNormalizable(Vec3);
// Normalize (unlike Unit) does not guard its input; callers follow the source
// IsNormalizable contract. Normalization preserves the source NEON operation
// order and reciprocal-square-root estimate/refinement, not sqrt/division.
Vec3 Normalize(Vec3);
// CMath::SqrtF 0x65f70: v * rsqrt(max(v, 1e-35)) with the ARM reciprocal-square-root
// estimate and TWO refinement steps, and 0 for non-positive (and NaN) input, since the
// source's guard is `fcmp` + `b.le` and AArch64 takes an unordered compare. Shared by
// 0x699b0's discriminant root and 0xbf6f10's sphere tests; not host sqrt.
float SqrtF(float);
// NormalizeWithMagnitude 0x6b100: the same pairs sum and two-refinement reciprocal as
// Normalize, writing the unit vector back IN PLACE, and returning the magnitude from
// that SAME final reciprocal (`squared * r2`, the source's `fmul v1,v2`) - never a
// recomputed sqrt. Callers follow the source IsNormalizable contract, as for Normalize.
float NormalizeWithMagnitude(Vec3&);
Vec3 Unit(Vec3);
Vec3 Corner(const AABB&, unsigned);
float ClipLineSegment(Plane, Vec3 start, Vec3 end);
bool TriangleNoBackface(Vec3 origin, Vec3 direction, float length,
                        Vec3 a, Vec3 b, Vec3 c, float& distance);
bool SweptLineLine(Vec3 origin, Vec3 direction, Vec3 sweepDirection,
                   Vec3 axisOrigin, Vec3 axisDirection, float& distance);
}
}
