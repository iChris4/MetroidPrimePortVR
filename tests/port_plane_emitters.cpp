// Remastered's plane emitters: PLNE (CEEPortPlane), PLNV (CEEPortPlaneVariance), the spread math
// they share and PLNV's index shuffler. Positions and velocities come out in the port's space,
// B = (-x, z, y) of Remastered's. The spread is checked against a double-precision restatement
// of ModifyWithSpreadAngle, the shuffler against a hand trace.
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

struct Vec {
  double x, y, z;
};

// ModifyWithSpreadAngle in doubles: returns the velocity for a point (x, z) on a plane of half
// sizes (wx, wz), the cone half angle having sine s and cosine c.
Vec ReferenceSpread(double x, double z, double wx, double wz, double s, double c, Vec v) {
  const double ax = std::fabs(x), az = std::fabs(z);
  const bool selX = (wz - az) < (wx - ax);
  const bool useZ = wz != 0.0 && (wx == 0.0 || selX);
  const double d = useZ ? wz : wx;
  const double dimv = useZ ? az : ax;
  const double len = std::sqrt(x * x + z * z);
  if (s == 0.0 || d == 0.0 || len == 0.0) {
    return v;
  }
  const double y = len / s * c;
  const double n = std::sqrt(x * x + y * y + z * z);
  const double mag = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  double dx = mag * x / n, dy = mag * y / n, dz = mag * z / n;
  if ((v.y >= 0.0) != (dy >= 0.0)) {
    dy = -dy;
  }
  double t;
  if (wx == wz) {
    t = dimv / d;
  } else {
    const double dd = std::fabs(wx - wz);
    t = ((wx > wz) != useZ) ? (dimv - dd) / (d - dd) : dimv / d;
  }
  return {t * dx + (1 - t) * v.x, t * dy + (1 - t) * v.y, t * dz + (1 - t) * v.z};
}

void TestSpreadMath() {
  const float s = 0.70710678f;
  const struct {
    float x, z, wx, wz;
    Vec vel;
  } cases[] = {
      {1.f, 0.f, 2.f, 2.f, {0, 5, 0}},     // square: t = |x| / w
      {1.f, 0.f, 2.f, 2.f, {0, -5, 0}},    // the sign flip of dy
      {1.5f, 0.5f, 4.f, 2.f, {0, 3, 0}},   // wx > wz
      {-0.5f, 1.5f, 2.f, 4.f, {1, 3, 2}},  // wx < wz
      {-3.f, 0.25f, 4.f, 2.f, {0, 1, 0}},  // wx > wz, the z side of the plane picked
      {0.25f, -1.9f, 4.f, 2.f, {0, 2, 0}},
  };
  for (const auto& c : cases) {
    PortParticleMath::PlaneSpread spread;
    spread.enabled = true;
    spread.sin = s;
    spread.cos = s;
    const CVector3f out = PortParticleMath::PlaneModifyWithSpread(
        CVector3f(c.x, 0.f, c.z), CVector3f(float(c.vel.x), float(c.vel.y), float(c.vel.z)), c.wx, c.wz, spread);
    const Vec want = ReferenceSpread(c.x, c.z, c.wx, c.wz, s, s, c.vel);
    CHECK_MSG(Near(out.GetX(), want.x, 2.e-3) && Near(out.GetY(), want.y, 2.e-3) && Near(out.GetZ(), want.z, 2.e-3),
              "ModifyWithSpreadAngle matches the double-precision reference");
  }
  // A zero spread leaves the velocity alone.
  const PortParticleMath::PlaneSpread none = PortParticleMath::PlaneSpreadFast(0.f);
  CHECK_MSG(!none.enabled && none.sin == 0.f && none.cos == 1.f, "no spread is disabled");
  CHECK_MSG(PortParticleMath::PlaneSpreadFast(30.f).enabled && PortParticleMath::PlaneSpreadParabolic(30.f).enabled,
            "a spread is enabled");
  // Both laws put 30 degrees near sin 0.5, cos 0.866 (the parabolic one within its 1e-3).
  const PortParticleMath::PlaneSpread fast = PortParticleMath::PlaneSpreadFast(30.f);
  const PortParticleMath::PlaneSpread parabolic = PortParticleMath::PlaneSpreadParabolic(30.f);
  CHECK_MSG(Near(fast.sin, 0.5, 2.e-3) && Near(fast.cos, 0.8660254, 2.e-3), "FastSinR/FastCosR at 30 degrees");
  CHECK_MSG(Near(parabolic.sin, 0.5, 2.e-3) && Near(parabolic.cos, 0.8660254, 2.e-3), "parabolic sin/cos at 30 degrees");
}

void TestShuffler() {
  // N = 4, picks 1, 1, 1, 1, 1: 1 is free; then taken, so the first free index from 1's word start
  // (0), then 2, then 3; the fifth finds all four used, clears the set and takes its pick again.
  PortParticleMath::IndexShuffler shuffler;
  const int picks[] = {1, 1, 1, 1, 1};
  const int want[] = {1, 0, 2, 3, 1};
  for (int i = 0; i < 5; ++i) {
    const int got = shuffler.Next(4, [&] { return uint32_t(picks[i]); });
    CHECK_MSG(got == want[i], "shuffler pick follows the hand trace");
  }
  CHECK_MSG(shuffler.bits[0] == 0x2u, "the reset leaves only the fifth pick used");

  // N = 40 spans two words: every index appears once in 40 picks, whatever the draws.
  PortParticleMath::IndexShuffler wide;
  std::set<int> seen;
  uint32_t state = 12345;
  for (int i = 0; i < 40; ++i) {
    seen.insert(wide.Next(40, [&] {
      state = state * 1664525u + 1013904223u;
      return state >> 8;
    }));
  }
  CHECK_MSG(seen.size() == 40 && *seen.begin() == 0 && *seen.rbegin() == 39, "N picks cover 0..N-1");
}

void TestPlne() {
  CRandom16 rnd(777);
  CGlobalRandom global(rnd);
  // No spread, no rotation: position + (x, 0, z), velocity as given, both turned by B.
  CEEPortPlane plane(new ConstVector(CVector3f(1.f, 2.f, 3.f)), new ConstVector(CVector3f(0.f, 0.f, 0.f)),
                     new ConstVector(CVector3f(0.f, 5.f, 0.f)), new ConstReal(2.f), new ConstReal(3.f),
                     new ConstReal(0.f));
  Lcg lcg{777};
  for (int i = 0; i < 10; ++i) {
    CVector3f pos, vel;
    CHECK(!plane.GetValue(i, pos, vel));
    const double x = 4.0 * lcg.next() - 2.0;
    const double z = 6.0 * lcg.next() - 3.0;
    CHECK_MSG(Near(pos.GetX(), -(1.0 + x), 1.e-3) && Near(pos.GetY(), 3.0 + z, 1.e-3) && Near(pos.GetZ(), 2.0, 1.e-3),
              "PLNE position is B(position + (x, 0, z))");
    CHECK_MSG(Near(vel.GetX(), 0.0, 1.e-3) && Near(vel.GetY(), 0.0, 1.e-3) && Near(vel.GetZ(), 5.0, 1.e-3),
              "PLNE velocity is B(velocity)");
  }
  CHECK_MSG(rnd.GetSeed() == lcg.seed, "PLNE draws x then z");

  // Rotated 90 degrees about X: the plane stands up, +y velocity goes to +z (Remastered), so B
  // gives +y; the offset's z goes to y.
  CEEPortPlane stood(nullptr, new ConstVector(CVector3f(90.f, 0.f, 0.f)), new ConstVector(CVector3f(0.f, 5.f, 0.f)),
                     new ConstReal(2.f), new ConstReal(2.f), nullptr);
  CVector3f pos, vel;
  stood.GetValue(0, pos, vel);
  CHECK_MSG(Near(vel.GetX(), 0.0, 8.e-3) && Near(std::fabs(vel.GetY()), 5.0, 8.e-3) && Near(vel.GetZ(), 0.0, 8.e-3),
            "PLNE turns the velocity by the euler matrix");

  // With a spread the velocity bends away from +y by the point's distance from the centre.
  CRandom16 rnd2(31);
  CGlobalRandom global2(rnd2);
  CEEPortPlane spread(nullptr, nullptr, new ConstVector(CVector3f(0.f, 4.f, 0.f)), new ConstReal(2.f),
                      new ConstReal(2.f), new ConstReal(45.f));
  Lcg lcg2{31};
  for (int i = 0; i < 10; ++i) {
    CVector3f p, v;
    spread.GetValue(i, p, v);
    const double x = 4.0 * lcg2.next() - 2.0;
    const double z = 4.0 * lcg2.next() - 2.0;
    const Vec want = ReferenceSpread(x, z, 2.0, 2.0, std::sqrt(0.5), std::sqrt(0.5), {0, 4, 0});
    CHECK_MSG(Near(v.GetX(), -want.x, 8.e-3) && Near(v.GetY(), want.z, 8.e-3) && Near(v.GetZ(), want.y, 8.e-3),
              "PLNE with a spread bends the velocity (FastSinR/FastCosR)");
  }
}

void TestPlnv() {
  // Index mode (no current generator: the creation count is 0): cell 0 of a 2 x 2 grid on a
  // 4 x 4 plane, no variance, is the corner (-1, -1); the two draws are still taken.
  {
    CRandom16 rnd(5);
    CGlobalRandom global(rnd);
    CEEPortPlaneVariance grid(nullptr, nullptr, new ConstReal(3.f), new ConstReal(2.f), new ConstReal(2.f), nullptr,
                              new ConstInt(2), new ConstReal(0.f), new ConstInt(2), new ConstReal(0.f), false);
    CVector3f pos, vel;
    CHECK(!grid.GetValue(0, pos, vel));
    CHECK_MSG(Near(pos.GetX(), 1.0, 1.e-3) && Near(pos.GetY(), -1.0, 1.e-3) && Near(pos.GetZ(), 0.0, 1.e-3),
              "PLNV cell 0 centre is B(-1, 0, -1)");
    CHECK_MSG(Near(vel.GetX(), 0.0, 1.e-3) && Near(vel.GetY(), 0.0, 1.e-3) && Near(vel.GetZ(), 3.0, 1.e-3),
              "PLNV velocity is B(0, velocity y, 0)");
    Lcg lcg{5};
    lcg.next();
    lcg.next();
    CHECK_MSG(rnd.GetSeed() == lcg.seed, "PLNV draws two floats after the index");
  }
  // Variance 1 spreads a point across its whole cell: x within [cell low, cell high).
  {
    CRandom16 rnd(6);
    CGlobalRandom global(rnd);
    CEEPortPlaneVariance cells(nullptr, nullptr, new ConstReal(0.f), new ConstReal(2.f), new ConstReal(2.f), nullptr,
                               new ConstInt(2), new ConstReal(1.f), new ConstInt(2), new ConstReal(1.f), false);
    CVector3f pos, vel;
    cells.GetValue(0, pos, vel);
    const double rx = -pos.GetX(), rz = pos.GetY();
    CHECK_MSG(rx >= -2.0 && rx < 0.0 && rz >= -2.0 && rz < 0.0, "PLNV cell 0 holds its point");
  }
  // Shuffle mode over a 2 x 2 grid: four evaluations visit the four cells once each, the fifth
  // starts over. Variance 0 puts each point at its cell centre.
  {
    CRandom16 rnd(9);
    CGlobalRandom global(rnd);
    CEEPortPlaneVariance shuffled(nullptr, nullptr, new ConstReal(0.f), new ConstReal(2.f), new ConstReal(2.f), nullptr,
                                  new ConstInt(2), new ConstReal(0.f), new ConstInt(2), new ConstReal(0.f), 4);
    std::set<std::pair<int, int>> cells;
    for (int i = 0; i < 4; ++i) {
      CVector3f pos, vel;
      shuffled.GetValue(i, pos, vel);
      cells.insert({int(std::lround(-pos.GetX())), int(std::lround(pos.GetY()))});
    }
    CHECK_MSG(cells.size() == 4, "a shuffled PLNV visits every cell once");
    const std::set<std::pair<int, int>> expected = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    CHECK_MSG(cells == expected, "the cells are the four grid centres");
  }
  // A spread tilts the velocity: with spread 45 and the point off centre, vx and vz are nonzero.
  {
    CRandom16 rnd(10);
    CGlobalRandom global(rnd);
    CEEPortPlaneVariance tilted(nullptr, nullptr, new ConstReal(4.f), new ConstReal(2.f), new ConstReal(2.f),
                                new ConstReal(45.f), new ConstInt(2), new ConstReal(0.f), new ConstInt(2),
                                new ConstReal(0.f), false);
    CVector3f pos, vel;
    tilted.GetValue(0, pos, vel);
    const Vec want = ReferenceSpread(-1.0, -1.0, 2.0, 2.0, std::sqrt(0.5), std::sqrt(0.5), {0, 4, 0});
    CHECK_MSG(Near(vel.GetX(), -want.x, 2.e-2) && Near(vel.GetY(), want.z, 2.e-2) && Near(vel.GetZ(), want.y, 2.e-2),
              "PLNV with a spread uses the parabolic sin and cos");
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
  TestSpreadMath();
  TestShuffler();
  TestPlne();
  TestPlnv();
  return PORT_TEST_RESULT();
}
