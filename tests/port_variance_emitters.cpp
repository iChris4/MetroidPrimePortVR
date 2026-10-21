// Remastered's ASPV and LINV as the port-only PASV (CEEPortAngleSphereVariance) and PLWV (CVEPortLine).
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


bool Near(double a, double b, double e) { return std::fabs(a - b) <= e; }

struct Lcg {
  uint32_t seed;
  double next() {
    seed = seed * 0x41c64e6du + 0x3039u;
    return double(seed & 0xfffffffu) / 268435456.0;
  }
};

void TestMath() {
  using namespace PortParticleMath;
  const Quat id = EulerQuat(0.f, 0.f, 0.f);
  // Polar 180..0, 1 band, no variance: the band centre is c = ~0 (up to FastCosR(pi)), azimuth 360 / 1
  // at the cell centre is 0: direction (0, 0, 1) in Remastered, (0, 1, 0) in the port.
  const SpherePoint p = AngleSphereWithVariance(CVector3f::Zero(), id, 180.f, 0.f, 1, 0.f, 360.f, 1, 0.f, 2.f, 3.f, 0, .5f, .5f);
  CHECK_MSG(Near(p.position.GetX(), 0.0, 1.e-3) && Near(p.position.GetY(), 2.0, 1.e-3) && Near(p.position.GetZ(), 0.0, 1.e-3),
            "one band, one sector: radius along the port's up");
  CHECK_MSG(Near(p.velocity.GetY(), 3.0, 1.e-3), "velocity is speed along the direction");
  // Azimuth 2 sectors, index 1 of a 180..0 band: azimuth 45 degrees, c ~ 0, direction (.7071, 0, .7071).
  const SpherePoint q = AngleSphereWithVariance(CVector3f::Zero(), id, 180.f, 0.f, 1, 0.f, 180.f, 2, 0.f, 1.f, 0.f, 1, .5f, .5f);
  CHECK_MSG(Near(q.position.GetX(), -0.7071, 2.e-3) && Near(q.position.GetY(), 0.7071, 2.e-3) && Near(q.position.GetZ(), 0.0, 2.e-3),
            "sector 1 of 2 over 180 degrees is 45 degrees");
  // A line of 10 in 2 cells: cell centres 2.5 and 7.5; variance 1 and draw 0 reach the cell's edge.
  const CVector3f s(0.f, 0.f, 0.f), e(10.f, 0.f, 0.f);
  CHECK_MSG(Near(LinePoint(s, e, 2, 0.f, 0, .3f, 10.f).GetX(), 2.5, 1.e-5), "cell 0 centre");
  CHECK_MSG(Near(LinePoint(s, e, 2, 0.f, 1, .3f, 10.f).GetX(), 7.5, 1.e-5), "cell 1 centre");
  CHECK_MSG(Near(LinePoint(s, e, 2, 1.f, 1, 0.f, 10.f).GetX(), 5.0, 1.e-5), "draw 0, variance 1: the cell's start");
}

void TestElements() {
  // PLWV: two draws' worth of state: one draw for a long segment, none for a degenerate one.
  {
    CRandom16 rnd(7);
    CGlobalRandom global(rnd);
    CVEPortLine line(new ConstVector(CVector3f(0.f, 0.f, 0.f)), new ConstVector(CVector3f(10.f, 0.f, 0.f)),
                     new ConstInt(2), new ConstReal(0.f), 0);
    CVector3f v;
    CHECK(!line.GetValue(0, v));
    CHECK_MSG(Near(v.GetX(), 2.5, 1.e-5), "PLWV without a generator is cell 0");
    Lcg lcg{7};
    lcg.next();
    CHECK_MSG(rnd.GetSeed() == lcg.seed, "PLWV takes one draw");
    CVEPortLine same(new ConstVector(CVector3f(1.f, 2.f, 3.f)), new ConstVector(CVector3f(1.f, 2.f, 3.f)),
                     new ConstInt(2), new ConstReal(0.f), 0);
    const uint32_t before = rnd.GetSeed();
    same.GetValue(0, v);
    CHECK_MSG(Near(v.GetX(), 1.0, 0.0) && Near(v.GetZ(), 3.0, 0.0) && rnd.GetSeed() == before,
              "a degenerate segment is its start and takes no draw");
  }
  // PLWV shuffler over 3 cells: three evaluations visit the three cells once each.
  {
    CRandom16 rnd(11);
    CGlobalRandom global(rnd);
    CVEPortLine line(new ConstVector(CVector3f(0.f, 0.f, 0.f)), new ConstVector(CVector3f(9.f, 0.f, 0.f)),
                     new ConstInt(3), new ConstReal(0.f), 3);
    std::set<int> cells;
    for (int i = 0; i < 3; ++i) {
      CVector3f v;
      line.GetValue(i, v);
      cells.insert(int(v.GetX() / 3.0));
    }
    CHECK_MSG(cells.size() == 3, "a shuffler of n cells visits every cell once");
  }
  // PASV: origin B-turned position, two draws after the index, polar first.
  {
    CRandom16 rnd(5);
    CGlobalRandom global(rnd);
    CEEPortAngleSphereVariance sphere(new ConstVector(CVector3f(1.f, 2.f, 3.f)), new ConstReal(0.f), new ConstReal(0.f),
                                      new ConstReal(0.f), new ConstReal(180.f), new ConstReal(0.f), new ConstInt(1),
                                      new ConstReal(0.f), new ConstReal(360.f), new ConstInt(1), new ConstReal(0.f),
                                      new ConstReal(2.f), new ConstReal(3.f), 0);
    CVector3f pos, vel;
    CHECK(!sphere.GetValue(0, pos, vel));
    CHECK_MSG(Near(pos.GetX(), 1.0, 1.e-3) && Near(pos.GetY(), 4.0, 1.e-3) && Near(pos.GetZ(), 3.0, 1.e-3),
              "PASV position is origin + radius * direction");
    CHECK_MSG(Near(vel.GetY(), 3.0, 1.e-3), "PASV velocity is speed * direction");
    Lcg lcg{5};
    lcg.next();
    lcg.next();
    CHECK_MSG(rnd.GetSeed() == lcg.seed, "PASV draws two floats");
  }
}

}  // namespace

int main() {
  TestMath();
  TestElements();
  return PORT_TEST_RESULT();
}

void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) { return ::operator new(size); }
void IElement::CElementAllocator::Free(void* ptr, size_t) { ::operator delete(ptr); }
void* IElement::operator new(size_t size, const char* file, const char* type) {
  return CElementAllocator::Alloc(size, file, type);
}
void IElement::operator delete(void* ptr, size_t size) { CElementAllocator::Free(ptr, size); }
