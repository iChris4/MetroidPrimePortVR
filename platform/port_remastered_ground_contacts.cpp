#include "port_remastered_ground_contacts.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace PortRemasteredGroundContacts {
namespace {
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
constexpr float Epsilon = std::numeric_limits<float>::epsilon();
// 0x0554ad2e: the fmax floor inside CMath::SqrtF 0x65f70 (and 0x699b0's inlined copy).
constexpr float kSqrtFloor = 1e-35f;
float FromBits(std::uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
std::uint32_t Bits(float f) { std::uint32_t b; std::memcpy(&b, &f, 4); return b; }
Vec3 Add(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3 Neg(Vec3 a) { return {-a.x, -a.y, -a.z}; }
Vec3 Mul(Vec3 a, float b) { return {a.x*b, a.y*b, a.z*b}; }
float Dot(Vec3 a, Vec3 b) { return (a.x*b.x + a.y*b.y) + a.z*b.z; }
Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
float Component(Vec3 a, unsigned i) { return i == 0 ? a.x : (i == 1 ? a.y : a.z); }
Vec3 SupportMin(const AABB& b, Vec3 v) {
  // ClosestPointAlongVector 0x98b64; zero chooses min.
  return {v.x >= 0.f ? b.min.x : b.max.x, v.y >= 0.f ? b.min.y : b.max.y,
          v.z >= 0.f ? b.min.z : b.max.z};
}
bool Close(Vec3 a, Vec3 b) {
  // close_enough 0x94680: strict component comparisons.
  return std::fabs(a.x-b.x) < .0001f && std::fabs(a.y-b.y) < .0001f &&
         std::fabs(a.z-b.z) < .0001f;
}
// ARM RecipSqrtEstimate's specified integer construction, not host rsqrt.
// Switch predates FEAT_RPRES; source uses the 8-fraction-bit estimate.
float ReciprocalSqrtEstimate(float v) {
  const auto bits = Bits(v);
  int exponent = static_cast<int>((bits >> 23) & 255);
  auto fraction = bits & 0x7fffff;
  if (exponent == 255) return fraction ? FromBits(bits | 0x400000) : 0.f;
  if ((bits & 0x7fffffff) == 0) return FromBits((bits & 0x80000000) | 0x7f800000);
  if (bits >> 31) return FromBits(0x7fc00000);
  if (exponent == 0) {
    while ((fraction & 0x400000) == 0) { fraction <<= 1; --exponent; }
    fraction = (fraction & 0x3fffff) << 1;
  }
  unsigned scaled = (exponent & 1) ? (128 | (fraction >> 16)) : (256 | (fraction >> 15));
  scaled = scaled < 256 ? scaled*2+1 : ((scaled & ~1u)+1)*2;
  unsigned b = 512;
  while (scaled*(b+1)*(b+1) < (1u << 28)) ++b;
  return FromBits((static_cast<unsigned>((380-exponent)/2) << 23) | (((b+1)/2 & 255) << 15));
}
float ReciprocalSqrtStep(float a, float b) {
  // FRSQRTS is fused (3-a*b)/2; zero*infinity is specified to return 1.5.
  if ((a == 0.f && std::isinf(b)) || (b == 0.f && std::isinf(a))) return 1.5f;
  return std::fma(-a, b, 3.f)*.5f;
}
float MulExtended(float a, float b) {
  // FMULX special zero*infinity result.
  if ((a == 0.f && std::isinf(b)) || (b == 0.f && std::isinf(a)))
    return std::copysign(2.f, std::signbit(a) != std::signbit(b) ? -1.f : 1.f);
  return a*b;
}
void Secondary(Result& r, Vec3 v, SecondaryBranch branch) {
  r.triangle.hasSecondary = true;
  r.triangle.secondary = v;
  r.provenance.secondaryBranch = branch;
}
}
namespace Source {
bool IsNormalizable(Vec3 v) {
  // 0x6b030: finite COMPONENTS and square length >= raw 0x10fd87b5.
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
         Dot(v,v) >= FromBits(0x10fd87b5);
}
Vec3 Normalize(Vec3 v) {
  // 0x6b0b0 / 0x6b238: NEON pairs sum (x*x+z*z)+(y*y+0).
  const float squared = (v.x*v.x + v.z*v.z) + (v.y*v.y + 0.f);
  float reciprocal = ReciprocalSqrtEstimate(squared);
  reciprocal *= ReciprocalSqrtStep(squared, reciprocal*reciprocal);
  reciprocal *= ReciprocalSqrtStep(squared, reciprocal*reciprocal);
  return {MulExtended(v.x,reciprocal), MulExtended(v.y,reciprocal), MulExtended(v.z,reciprocal)};
}
Vec3 Unit(Vec3 v) { return IsNormalizable(v) ? Normalize(v) : Vec3{}; }
float SqrtF(float v) {
  // 0x65f70: `fcmp v,0` + `b.le` is the machine guard, so the rule is !(v > 0) and NaN
  // returns 0 like the non-positive inputs. The reciprocal square root is the ARM estimate
  // with TWO frsqrts refinements, then multiplied into the original v.
  if (!(v > 0.f)) return 0.f;
  const float x = std::fmax(v, kSqrtFloor);
  const float estimate = ReciprocalSqrtEstimate(x);
  const float first = estimate * ReciprocalSqrtStep(x, estimate * estimate);
  const float second = first * ReciprocalSqrtStep(x, first * first);
  return v * second;
}
float NormalizeWithMagnitude(Vec3& v) {
  // 0x6b100: the same pairs sum as Normalize ((x*x+z*z)+y*y: the source squares
  // x,y in one NEON pair, moves z down and faddp's) and the same two frsqrts
  // refinements; the components are written back with fmulx, and the returned
  // magnitude is the source's `s0 = fmul v1,v2` - squared * that same final
  // reciprocal, so m is not an independently computed |v|.
  const float squared = (v.x*v.x + v.z*v.z) + v.y*v.y;
  float reciprocal = ReciprocalSqrtEstimate(squared);
  reciprocal *= ReciprocalSqrtStep(squared, reciprocal*reciprocal);
  reciprocal *= ReciprocalSqrtStep(squared, reciprocal*reciprocal);
  v = {MulExtended(v.x,reciprocal), MulExtended(v.y,reciprocal), MulExtended(v.z,reciprocal)};
  return squared*reciprocal;
}
Vec3 Corner(const AABB& b, unsigned i) {
  // 0x98908: X/Z/Y bit order.
  return {i & 1 ? b.max.x : b.min.x, i & 4 ? b.max.y : b.min.y, i & 2 ? b.max.z : b.min.z};
}
float ClipLineSegment(Plane p, Vec3 a, Vec3 b) {
  // 0x978bc, including denominator threshold and clamp order.
  const float denominator = Dot(Sub(b,a),p.normal);
  if (std::fabs(denominator) < 1e-20f) return 1.f;
  const float t = -(Dot(a,p.normal)-p.constant)/denominator;
  const float lower = 0.f-t < 0.f ? t : 0.f;
  return lower-1.f < 0.f ? lower : 1.f;
}
bool TriangleNoBackface(Vec3 o, Vec3 direction, float length, Vec3 a, Vec3 b, Vec3 c, float& distance) {
  // 0xbf9360; 0xbf94d0 adds an unused output normal only for return code 2.
  const Vec3 e1 = Sub(b,a), e2 = Sub(c,a), p = Cross(direction,e2);
  const float determinant = e1.z*p.z + (e1.x*p.x + e1.y*p.y);
  if (determinant < 1.1920929e-6f) return false;
  const Vec3 t = Sub(o,a);
  const float u = Dot(p,t);
  if (!(u >= 0.f && u <= determinant)) return false;
  const Vec3 q = Cross(t,e1);
  const float v = direction.z*q.z + (direction.x*q.x + direction.y*q.y);
  if (!(v >= 0.f && u+v <= determinant)) return false;
  const float hit = (1.f/determinant)*(e2.z*q.z + (e2.x*q.x + e2.y*q.y));
  if (hit < 0.f || length < hit) return false;
  distance = hit;
  return true;
}
bool SweptLineLine(Vec3 o, Vec3 direction, Vec3 sweep, Vec3 axisOrigin, Vec3 axisDirection, float& distance) {
  // 0xbed4a0: infinite CLines, NOT segment tests. Deliberately no division
  // by dot(normal,axisDirection); checked in disassembly 0xbed548..0xbed58c.
  Vec3 normal = Cross(direction,sweep);
  if (!IsNormalizable(normal)) return false;
  normal = Normalize(normal);
  if (std::fabs(Dot(normal,axisDirection)) < Epsilon) return false;
  const float offset = Dot(normal,Sub(axisOrigin,o));
  const Vec3 delta = Sub(Add(axisOrigin,Mul(axisDirection,offset)),o);
  const Vec3 cross = Cross(direction,delta);
  const float hit = normal.z*cross.z + (normal.x*cross.x + normal.y*cross.y);
  if (!(hit >= 0.f)) return false;
  distance = hit;
  return true;
}
}

bool Intersects(const AABB& b, const std::array<Vec3,3>& t) {
  const Vec3 center = Mul(Add(b.min,b.max),.5f);
  const Vec3 extent = Mul(Sub(b.max,b.min),.5f);
  const Vec3 a = Sub(t[0],center), c = Sub(t[2],center), v = Sub(t[1],center);
  for (unsigned i=0; i<3; ++i) {
    const float x = Component(a,i), y = Component(v,i), z = Component(c,i);
    const float lo = x-y < 0.f ? x : y;
    const float hi = x-y < 0.f ? y : x;
    if ((lo-z < 0.f ? lo : z) > Component(extent,i) ||
        (hi-z < 0.f ? z : hi) < -Component(extent,i)) return false;
  }
  const Vec3 e0 = Sub(v,a), e1 = Sub(c,v), e2 = Sub(a,c);
  const Vec3 n = Cross(e0,e1);
  if (!(std::fabs(a.z*n.z + (a.x*n.x + a.y*n.y)) <=
        extent.z*std::fabs(n.z) + (extent.x*std::fabs(n.x) + extent.y*std::fabs(n.y))))
    return false;
  // Each edge's two distinct projections: its first vertex and the opposite
  // vertex. Keep the source's endpoint choice/order, including equality.
  const Vec3 edges[] = {e0,e1,e2};
  const Vec3 first[] = {a,a,a};
  const Vec3 opposite[] = {c,c,v};
  for (unsigned i=0; i<3; ++i) {
    const Vec3 e = edges[i], p = first[i], q = opposite[i];
    // The source uses the second vertex for edge 0's Z-axis projection,
    // and vertices 1/2 for edge 2's. Algebraically equal edge endpoints
    // need not be bit-equal in binary32, so do not substitute vertex 0.
    const Vec3 zFirst = i == 1 ? a : v;
    const float u[] = {p.y*e.z-p.z*e.y, p.z*e.x-p.x*e.z,
                       zFirst.x*e.y-e.x*zFirst.y};
    const float w[] = {e.z*q.y-e.y*q.z, e.x*q.z-e.z*q.x, e.y*c.x-e.x*c.y};
    const float radius[] = {extent.z*std::fabs(e.y)+extent.y*std::fabs(e.z),
                            extent.z*std::fabs(e.x)+extent.x*std::fabs(e.z),
                            extent.y*std::fabs(e.x)+extent.x*std::fabs(e.y)};
    for (unsigned j=0; j<3; ++j) {
      const float hi = u[j]-w[j] < 0.f ? w[j] : u[j];
      const float lo = u[j]-w[j] < 0.f ? u[j] : w[j];
      if (!((-hi-lo < 0.f ? lo : -hi) <= radius[j])) return false;
    }
  }
  return true;
}

Result Build(const AABB& b, const std::array<Vec3,3>& t, Vec3 n) {
  Result r;
  auto& p = r.provenance;
  p.planes = {{{{-1.f,-0.f,-0.f},-b.min.x}, {{1.f,0.f,0.f},b.max.x},
               {{-0.f,-0.f,-1.f},-b.min.z}, {{0.f,0.f,1.f},b.max.z},
               {{-0.f,-1.f,-0.f},-b.min.y}, {{0.f,1.f,0.f},b.max.y}}};
  std::array<Vec3,9> polygon{};
  std::size_t size = 3;
  for (std::size_t i=0; i<size; ++i) polygon[i]=t[i];
  // 0xc0a328..0xc0a59c: CURRENT inside vertex then OUTGOING crossing.
  for (std::size_t j=0; j<6; ++j) {
    if (size == 0) return r;
    const Plane plane = p.planes[j];
    std::array<Vec3,9> next{};
    std::size_t count=0;
    bool crossed=false, outside=plane.constant <= Dot(plane.normal,polygon[0]);
    for (std::size_t i=0; i<size; ++i) {
      const Vec3 a=polygon[i], z=polygon[i+1 == size ? 0 : i+1];
      if (!outside && count != 9) next[count++]=a;
      const bool nextOutside=plane.constant <= Dot(plane.normal,z);
      if (outside != nextOutside) {
        const float fraction=Source::ClipLineSegment(plane,a,z);
        p.lastIntersection=Add(a,Mul(Sub(z,a),fraction));
        if (count != 9) next[count++]=p.lastIntersection;
        crossed=true;
      }
      outside=nextOutside;
    }
    if (crossed) p.crossingPlanes[p.crossingCount++]=j;
    polygon=next;
    size=count;
    p.polygon=polygon;
    p.polygonSize=size;
    if (size == 0) return r;
  }
  r.hit=true;
  r.triangle.primary=n;
  r.box.firstPoint=r.box.lastPoint=0;
  r.triangle.firstPoint=1;
  if (p.crossingCount == 0) {
    // 0xc0ad3c..0xc0af14; tie table at 0x1d0c4a8.
    constexpr unsigned table[]={0,1,0,2,1,1,2,2};
    const unsigned index=unsigned(std::fabs(n.x)<std::fabs(n.y)) |
      (unsigned(std::fabs(n.y)<std::fabs(n.z))<<1) | (unsigned(std::fabs(n.x)<std::fabs(n.z))<<2);
    const unsigned axis=table[index];
    const float sign=Component(n,axis)<0.f ? 1.f : -1.f;
    r.box.primary=axis==0 ? Vec3{sign,0.f,0.f} : (axis==1 ? Vec3{0.f,sign,0.f} : Vec3{0.f,0.f,sign});
    r.points[0]=SupportMin(b,r.box.primary);
    r.box.D=r.box.H=-Dot(Sub(r.points[0],t[0]),n); // NO clamp.
    for (std::size_t i=0; i<3; ++i) r.points[i+1]=t[i];
    r.pointCount=4;
    r.triangle.lastPoint=3;
    p.secondaryBranch=SecondaryBranch::Contained;
    return r;
  }
  // 0xc0a5a0..0xc0ad30: three distance constructions in source order.
  const Vec3 support=SupportMin(b,n);
  p.Hraw=Dot(Sub(p.lastIntersection,support),n);
  float distance=0.f;
  for (std::size_t i=0; i<size; ++i) {
    float exit=std::numeric_limits<float>::max();
    for (unsigned axis=0; axis<3; ++axis) {
      const float component=Component(n,axis);
      if (component>Epsilon || component<-Epsilon) {
        const float boundary=Component(component>Epsilon ? b.min : b.max,axis);
        const float value=(boundary-Component(polygon[i],axis))/-component;
        exit=exit-value<0.f ? exit : value;
      }
    }
    distance=exit-distance<0.f ? distance : exit;
  }
  p.polygonRayDistance=distance;
  for (unsigned i=0; i<8; ++i) {
    float hit=0.f;
    if (Source::TriangleNoBackface(Source::Corner(b,i),n,p.Hraw+.1f,t[2],t[1],t[0],hit)) {
      p.cornerRayHits |= std::uint8_t(1u<<i);
      if (distance-hit<0.f) distance=hit;
    }
  }
  p.cornerRayDistance=distance;
  if (std::fabs(distance)<1e-5f) {
    p.usedEdgeFallback=true;
    constexpr Vec3 axes[]={{0.f,1.f,0.f},{1.f,0.f,0.f},{0.f,0.f,1.f}};
    for (std::size_t i=0; i<size; ++i) {
      const Vec3 previous=polygon[i==0 ? size-1 : i-1];
      if (Close(previous,polygon[i])) continue;
      const Vec3 direction=Source::Unit(Sub(polygon[i],previous));
      bool success=false;
      float minimum=0.f;
      for (unsigned axis=0; axis<3; ++axis) {
        float hit=0.f;
        if (Source::SweptLineLine(previous,direction,Neg(n),support,axes[axis],hit)) {
          p.fallbackAxisHits[i] |= std::uint8_t(1u<<axis);
          if (!success || !(minimum<=hit)) minimum=hit;
          success=true;
        }
      }
      if (success && distance-minimum<0.f) distance=minimum;
    }
  }
  p.finalDistance=distance;
  const float D=std::fabs(distance)<1e-5f ? p.Hraw : distance;
  r.box.D=D<=0.f ? 0.f : D;
  r.box.H=p.Hraw<=0.f ? 0.f : p.Hraw;
  r.box.primary=p.planes[p.crossingPlanes[p.crossingCount-1]].normal;
  r.points[0]=p.lastIntersection;
  for (std::size_t i=0; i<size; ++i) r.points[i+1]=polygon[i];
  r.pointCount=size+1;
  r.triangle.lastPoint=size;
  if (p.crossingCount==1) {
    Secondary(r,Neg(p.planes[p.crossingPlanes[0]].normal),SecondaryBranch::SingleClipPlane);
    return r;
  }
  // 0xc0b0a8..0xc0b598: shallow corner / shallow box edge.
  std::array<Vec3,8> behind{};
  unsigned behindCount=0, shallowCount=0;
  for (unsigned i=0; i<8; ++i) {
    const Vec3 corner=Source::Corner(b,i), delta=Sub(corner,t[0]);
    const float depth=n.z*delta.z+(n.x*delta.x+n.y*delta.y);
    if (depth<0.f) {
      behind[behindCount++]=corner;
      p.behindCorners |= std::uint8_t(1u<<i);
      if (depth>-.05f) { ++shallowCount; p.shallowCorners |= std::uint8_t(1u<<i); }
    }
  }
  if (behindCount==1 && shallowCount==1) {
    Secondary(r,n,SecondaryBranch::ShallowCorner);
    return r;
  }
  if (behindCount==2 && shallowCount==2) {
    const Vec3 edge=Sub(behind[0],behind[1]), cross=Cross(edge,n);
    if (Source::IsNormalizable(cross))
      Secondary(r,Source::Unit(Source::Normalize(Cross(Source::Normalize(cross),edge))),SecondaryBranch::ShallowEdge);
    else p.secondaryBranch=SecondaryBranch::UnnormalizableShallowEdge;
    return r;
  }
  // 0xc0b59c..0xc0b7ec and 0xc0b8d0..0xc0b964: plane-depth candidates.
  for (std::size_t j=0; j<6; ++j) {
    Candidate c{j,0.f,0};
    for (std::size_t i=0; i<size; ++i) {
      const float depth=-(Dot(p.planes[j].normal,polygon[i])-p.planes[j].constant);
      if (!(depth<=c.depth)) { c.depth=depth; c.point=i; }
    }
    if (c.depth<std::fabs(Dot(Sub(b.max,b.min),p.planes[j].normal))-Epsilon)
      p.candidates[p.candidateCount++]=c;
  }
  // 0xc0b970 insertion path (<=6 entries): ties keep original plane order.
  for (std::size_t i=1; i<p.candidateCount; ++i) {
    const Candidate key=p.candidates[i];
    std::size_t j=i;
    while (j>0 && key.depth<p.candidates[j-1].depth) { p.candidates[j]=p.candidates[j-1]; --j; }
    p.candidates[j]=key;
  }
  if (p.candidateCount==0) { p.secondaryBranch=SecondaryBranch::NoCandidate; return r; }
  const Candidate c0=p.candidates[0];
  const Plane a0=p.planes[c0.plane];
  if (p.candidateCount==1 || c0.point==p.candidates[1].point) {
    Secondary(r,Neg(a0.normal),SecondaryBranch::CandidatePlane);
    return r;
  }
  const Candidate c1=p.candidates[1];
  const Plane a1=p.planes[c1.plane];
  if (!(std::fabs(Dot(a0.normal,polygon[c1.point])-a0.constant)<1e-5f &&
        std::fabs(Dot(a1.normal,polygon[c0.point])-a1.constant)<1e-5f)) {
    Secondary(r,Neg(a0.normal),SecondaryBranch::CandidatePlane);
    return r;
  }
  const Vec3 sum=Add(Mul(a1.normal,c0.depth),Mul(a0.normal,c1.depth));
  if (Source::IsNormalizable(sum)) Secondary(r,Source::Unit(Neg(sum)),SecondaryBranch::CandidateBlend);
  else p.secondaryBranch=SecondaryBranch::UnnormalizableCandidateBlend;
  return r;
}
}
