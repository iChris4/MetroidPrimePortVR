// Remastered's SPEV (CEEPortSphereVariance) and ELPS (CEEPortEllipsoid) emitters. Positions and
// velocities come out in the port's space, B = (-x, z, y) of Remastered's. Hand-computed values
// for the band centre (cp = FastCosR(pi) = 0xbf7fff2e, not -1), the cell walk and the ellipsoid's
// direction, plus the number of draws each takes.
#include "Kyoto/CRandom16.hpp"
#include "Kyoto/Particles/CEmitterElement.hpp"
#include "Kyoto/Particles/CIntElement.hpp"
#include "Kyoto/Particles/CRealElement.hpp"
#include "Kyoto/Particles/CVectorElement.hpp"
#include "Kyoto/Particles/PortParticleMath.hpp"
#include "port_test.h"

#include <cmath>
#include <cstdint>
#include <set>

namespace {

class ConstReal final : public CRealElement {
public:
  explicit ConstReal(float v) : mValue(v) {}
  bool GetValue(int, float& out) const override {
    out = mValue;
    return false;
  }

private:
  float mValue;
};

class ConstInt final : public CIntElement {
public:
  explicit ConstInt(int v) : mValue(v) {}
  bool GetValue(int, int& out) const override {
    out = mValue;
    return false;
  }

private:
  int mValue;
};

class ConstVector final : public CVectorElement {
public:
  explicit ConstVector(CVector3f v) : mValue(v) {}
  bool GetValue(int, CVector3f& out) const override {
    out = mValue;
    return false;
  }

private:
  CVector3f mValue;
};

// The LCG behind CRandom16::PortFloat28, kept apart from the code under test.
struct Lcg {
  uint32_t seed;
  double next() {
    seed = seed * 0x41c64e6du + 0x3039u;
    return double(seed & 0xfffffffu) / 268435456.0;
  }
};

bool Near(double a, double b, double epsilon) { return std::fabs(a - b) <= epsilon; }

void TestSphereMath() {
  using namespace PortParticleMath;
  CHECK_MSG(BitsFloat(0xbf7fff2e) == -0.9999875f, "cp is FastCosR(pi), -0.9999875");
  // One band, one sector, no variance, cell 0: c = st1/2 + cp = 0.99999375 + cp = 6.25e-6, the
  // azimuth is (0 + 180 + 0 - 180) degrees = 0, so the point is nearly +z on the sphere.
  {
    const SpherePoint p = SphereWithVariance(CVector3f(1.f, 2.f, 3.f), 2.f, 3.f, 1, 0.f, 1, 0.f, 0, 0.5f, 0.5f);
    CHECK_MSG(Near(p.position.GetY(), 2.0 + 6.25e-6 * 2.0, 1.e-5), "y = P.y + c * radius");
    CHECK_MSG(Near(p.position.GetX(), 1.0, 1.e-5) && Near(p.position.GetZ(), 5.0, 1.e-3), "z = P.z + radius * cos 0");
    CHECK_MSG(Near(p.velocity.GetY(), 6.25e-6 * 3.0, 1.e-5) && Near(p.velocity.GetZ(), 3.0, 1.e-3),
              "velocity is the unit direction times the speed");
  }
  // Two bands: the second starts at cp + st1 with st1 = 0.99999375, so its centre is 0.500003125.
  {
    const SpherePoint p = SphereWithVariance(CVector3f::Zero(), 1.f, 0.f, 2, 0.f, 1, 0.f, 1, 0.f, 0.f);
    CHECK_MSG(Near(p.position.GetY(), 0.500003125, 1.e-6), "band 1 of 2 has its centre at 0.500003125");
  }
  // Variance 1 spans the whole band: the draw 0 gives the band's bottom, 1 (almost) its top.
  {
    const SpherePoint lo = SphereWithVariance(CVector3f::Zero(), 1.f, 0.f, 2, 1.f, 1, 0.f, 0, 0.f, 0.f);
    CHECK_MSG(Near(lo.position.GetY(), -0.9999875, 1.e-6), "draw 0 with variance 1 is the bottom of the band");
  }
  // An azimuth sector 1 of 2 at the centre of its half: (180 * 1 + 90 + 0 - 180) = 90 degrees.
  {
    const SpherePoint p = SphereWithVariance(CVector3f::Zero(), 1.f, 0.f, 1, 0.f, 2, 0.f, 1, 0.f, 0.f);
    CHECK_MSG(Near(p.position.GetX(), 1.0, 1.e-3), "sector 1 of 2 points at +x on the unit circle");
  }
}

void TestSphereElement() {
  // Index mode without a generator is index 0; the two draws are taken, polar first.
  {
    CRandom16 rnd(5);
    CGlobalRandom global(rnd);
    CEEPortSphereVariance sphere(new ConstVector(CVector3f(1.f, 2.f, 3.f)), new ConstReal(2.f), new ConstReal(3.f),
                                 new ConstInt(1), new ConstReal(0.f), new ConstInt(1), new ConstReal(0.f), 0);
    CVector3f pos, vel;
    CHECK(!sphere.GetValue(0, pos, vel));
    // Remastered (1, 2 + 1.25e-5, 5) turned by B = (-x, z, y).
    CHECK_MSG(Near(pos.GetX(), -1.0, 1.e-5) && Near(pos.GetY(), 5.0, 1.e-3) && Near(pos.GetZ(), 2.0, 1.e-4),
              "SPEV position is B(position + radius * direction)");
    CHECK_MSG(Near(vel.GetY(), 3.0, 1.e-3) && Near(vel.GetZ(), 1.875e-5, 1.e-5), "SPEV velocity is B(direction * speed)");
    Lcg lcg{5};
    lcg.next();
    lcg.next();
    CHECK_MSG(rnd.GetSeed() == lcg.seed, "SPEV draws two floats after the index");
  }
  // A shuffler over 2 x 2: four evaluations visit the four cells once each (no variance).
  {
    CRandom16 rnd(9);
    CGlobalRandom global(rnd);
    CEEPortSphereVariance sphere(nullptr, new ConstReal(1.f), new ConstReal(0.f), new ConstInt(2), new ConstReal(0.f),
                                 new ConstInt(2), new ConstReal(0.f), 4);
    std::set<std::pair<int, int>> cells;
    for (int i = 0; i < 4; ++i) {
      CVector3f pos, vel;
      sphere.GetValue(i, pos, vel);
      // B(pos): y is Remastered z, and -x is Remastered x: the azimuth -90 or +90 degrees.
      cells.insert({pos.GetZ() > 0.0 ? 1 : 0, pos.GetX() > 0.0 ? 1 : 0});
    }
    CHECK_MSG(cells.size() == 4, "a shuffled SPEV visits every cell once");
  }
}

void TestEllipsoid() {
  using namespace PortParticleMath;
  // Draws 0.75 give d = (0.5, 0.5, 0.5), normalized (0.57735, ...); the size (2, 4, 6) stretches
  // it to (1.1547, 2.3094, 3.4641); the velocity is the unit (2, 4, 6) / sqrt(56) times 0.5.
  {
    const float draw[3] = {0.75f, 0.75f, 0.75f};
    const SpherePoint p = EllipsoidPoint(CVector3f(1.f, 2.f, 3.f), CVector3f(2.f, 4.f, 6.f), .5f, draw);
    CHECK_MSG(Near(p.position.GetX(), 1.0 + 1.154700538, 1.e-4) && Near(p.position.GetY(), 2.0 + 2.309401077, 1.e-4) &&
                  Near(p.position.GetZ(), 3.0 + 3.464101615, 1.e-4),
              "ELPS position is centre + normalized d * size");
    CHECK_MSG(Near(p.velocity.GetX(), 0.133630621, 1.e-4) && Near(p.velocity.GetY(), 0.267261242, 1.e-4) &&
                  Near(p.velocity.GetZ(), 0.400891863, 1.e-4),
              "ELPS velocity is the normalized point times the speed");
  }
  // Draws 0.5 give d = 0, which stays 0: the point is the centre and the velocity zero.
  {
    const float draw[3] = {0.5f, 0.5f, 0.5f};
    const SpherePoint p = EllipsoidPoint(CVector3f(1.f, 2.f, 3.f), CVector3f(2.f, 4.f, 6.f), .5f, draw);
    CHECK_MSG(p.position.GetX() == 1.f && p.position.GetY() == 2.f && p.position.GetZ() == 3.f,
              "a zero direction puts the point at the centre");
    CHECK_MSG(p.velocity.GetX() == 0.f && p.velocity.GetY() == 0.f && p.velocity.GetZ() == 0.f,
              "and the velocity is zero");
  }
  // IsNormalizable 0x6b030: threshold 1e-28 (0x10fd87b5) with >=, not 1.42e-14, and non-finite
  // lanes fail. A tiny d (draws 0.5 + 5e-15 lanes cannot be formed), so probe the predicate
  // directly at, below and above the threshold, and through the size scaling in the element.
  {
    const float t = BitsFloat(0x10fd87b5);
    CHECK(IsNormalizableVec(CVector3f(std::sqrt(t) * 1.0001f, 0.f, 0.f)));
    CHECK(!IsNormalizableVec(CVector3f(std::sqrt(t) * .9999f, 0.f, 0.f)));
    CHECK_MSG(IsNormalizableVec(CVector3f(1.e-10f, 0.f, 0.f)), "1e-20 is normalizable (it is below the old 1.42e-14)");
    CHECK(!IsNormalizableVec(CVector3f(0.f, 0.f, 0.f)));
    CHECK(!IsNormalizableVec(CVector3f(INFINITY, 0.f, 0.f)));
    CHECK(!IsNormalizableVec(CVector3f(1.f, NAN, 0.f)));
    // A direction with magnitude 1e-10 (squared 1e-20) must be normalized in the point scaling:
    // size (1e-10, 0, 0) with d = (1,0,0) gives q = (1e-10,0,0), whose velocity is the unit x.
    const float draw[3] = {1.f, 0.5f, 0.5f};
    const SpherePoint p = EllipsoidPoint(CVector3f(0.f, 0.f, 0.f), CVector3f(1.e-10f, 0.f, 0.f), 2.f, draw);
    CHECK_MSG(Near(p.velocity.GetX(), 2.0, 1.e-4), "a 1e-10 point still gets a unit direction");
    // Below 1e-28 the point is left as is.
    const SpherePoint r = EllipsoidPoint(CVector3f(0.f, 0.f, 0.f), CVector3f(1.e-15f, 0.f, 0.f), 2.f, draw);
    CHECK_MSG(Near(r.velocity.GetX(), 2.0e-15, 1.e-18), "a point under 1e-28 squared keeps its length");
  }
  // The element: surface-only takes 3 draws, otherwise 6, and the output is turned by B.
  {
    CRandom16 rnd(21);
    CGlobalRandom global(rnd);
    CEEPortEllipsoid surface(new ConstVector(CVector3f(0.f, 0.f, 0.f)), new ConstVector(CVector3f(3.f, 0.f, 3.f)),
                             new ConstReal(1.f), true);
    CVector3f pos, vel;
    CHECK(!surface.GetValue(0, pos, vel));
    Lcg lcg{21};
    double d[3];
    for (double& v : d) {
      v = (lcg.next() * 200.0 + -100.0) * 0.01;
    }
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const double qx = d[0] / len * 3.0, qy = 0.0, qz = d[2] / len * 3.0;
    CHECK_MSG(Near(pos.GetX(), -qx, 1.e-4) && Near(pos.GetY(), qz, 1.e-4) && Near(pos.GetZ(), qy, 1.e-4),
              "ELPS position is B(q)");
    CHECK_MSG(rnd.GetSeed() == lcg.seed, "a surface-only ELPS draws three floats");
    CRandom16 rnd2(21);
    CGlobalRandom global2(rnd2);
    CEEPortEllipsoid solid(nullptr, new ConstVector(CVector3f(3.f, 3.f, 3.f)), new ConstReal(1.f), false);
    solid.GetValue(0, pos, vel);
    Lcg lcg2{21};
    for (int i = 0; i < 6; ++i) {
      lcg2.next();
    }
    CHECK_MSG(rnd2.GetSeed() == lcg2.seed, "a solid ELPS draws six floats");
  }
}

} // namespace

// Heap storage for elements.
void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) { return ::operator new(size); }
void IElement::CElementAllocator::Free(void* ptr, size_t) { ::operator delete(ptr); }
void* IElement::operator new(size_t size, const char* file, const char* type) {
  return CElementAllocator::Alloc(size, file, type);
}
void IElement::operator delete(void* ptr, size_t size) { CElementAllocator::Free(ptr, size); }

int main() {
  TestSphereMath();
  TestSphereElement();
  TestEllipsoid();
  return PORT_TEST_RESULT();
}
