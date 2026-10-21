// Remastered's CRCV as the port-only PCRC (CVEPortCircle): the ring of n steps with its
// jitter, the axis frame, the one draw and the shuffler's pick. Values are hand-computed; the
// parabolic sine differs from std::sin by a few 1e-3, so points use that tolerance.
#include "Kyoto/CRandom16.hpp"
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

bool Near(double a, double b, double epsilon) { return std::fabs(a - b) <= epsilon; }

constexpr double kPi = 3.14159265358979323846;

CVEPortCircle* Circle(CVector3f centre, CVector3f axis, float start, int n, float variance, float radius,
                      u8 shuffle) {
  return new CVEPortCircle(new ConstVector(centre), new ConstVector(axis), new ConstReal(start), new ConstInt(n),
                           new ConstReal(variance), new ConstReal(radius), shuffle);
}

// 360 / n degrees a step from the start, the jitter var * step / 2 either side of the draw.
void TestAngle() {
  // n = 4: step 90, index 1, no variance: 90 degrees = 1.5707964 rad.
  CHECK_MSG(Near(PortParticleMath::CircleAngle(0.f, 4, 0.f, 1, 0.3f), kPi / 2, 1e-6), "index 1 of 4 is 90 degrees");
  // start 10, n = 8 (step 45), var 1 (h = 22.5), index 3: 10 + 135 = 145 degrees at d = .5
  // (jitter 45 * .5 - 22.5 = 0), 122.5 at d = 0 and 167.5 at d = 1.
  const double deg = kPi / 180.0;
  CHECK_MSG(Near(PortParticleMath::CircleAngle(10.f, 8, 1.f, 3, 0.5f), 145.0 * deg, 1e-6), "mid draw");
  CHECK_MSG(Near(PortParticleMath::CircleAngle(10.f, 8, 1.f, 3, 0.f), 122.5 * deg, 1e-6), "low draw");
  CHECK_MSG(Near(PortParticleMath::CircleAngle(10.f, 8, 1.f, 3, 1.f), 167.5 * deg, 1e-6), "high draw");
  // 270 + 45 degrees wraps below pi: 315 degrees is -45.
  CHECK_MSG(Near(PortParticleMath::CircleAngle(0.f, 8, 0.f, 7, 0.f), -45.0 * deg, 1e-6), "wraps into [-pi, pi]");
}

// u is (0, z, -y) below x = .8 and (-z, 0, x) above; v is axis x u.
void TestFrame() {
  const PortParticleMath::CircleFrame up = PortParticleMath::MakeCircleFrame(CVector3f(0.f, 1.f, 0.f));
  CHECK_MSG(Near(up.u.GetX(), 0, 1e-6) && Near(up.u.GetY(), 0, 1e-6) && Near(up.u.GetZ(), -1, 1e-6), "Y axis: u = -z");
  CHECK_MSG(Near(up.v.GetX(), -1, 1e-6) && Near(up.v.GetY(), 0, 1e-6) && Near(up.v.GetZ(), 0, 1e-6), "Y axis: v = -x");
  const PortParticleMath::CircleFrame right = PortParticleMath::MakeCircleFrame(CVector3f(1.f, 0.f, 0.f));
  CHECK_MSG(Near(right.u.GetZ(), 1, 1e-6) && Near(right.v.GetY(), -1, 1e-6), "X axis: u = z, v = -y");
  const PortParticleMath::CircleFrame forward = PortParticleMath::MakeCircleFrame(CVector3f(0.f, 0.f, 1.f));
  CHECK_MSG(Near(forward.u.GetY(), 1, 1e-6) && Near(forward.v.GetX(), -1, 1e-6), "Z axis: u = y, v = -x");
}

// No variance, start 0, 4 steps, axis Y, radius 2 around (1, 2, 3): the first point is
// centre + 2 u = (1, 2, 1), and the element drew once.
void TestPoint() {
  CRandom16 rnd(4242);
  CGlobalRandom global(rnd);
  const CVEPortCircle* element = Circle(CVector3f(1.f, 2.f, 3.f), CVector3f(0.f, 1.f, 0.f), 0.f, 4, 0.f, 2.f, 0);
  CVector3f out;
  element->GetValue(0, out);
  CHECK_MSG(Near(out.GetX(), 1, 1e-6) && Near(out.GetY(), 2, 1e-6) && Near(out.GetZ(), 1, 2e-3),
            "first point of the ring is the centre plus the radius along u");
  CRandom16 ref(4242);
  ref.PortFloat28();
  CHECK_MSG(rnd.GetSeed() == ref.GetSeed(), "one draw without a shuffler");
  delete element;
}

// A shuffler of n cells (here with no generator, the element's own) takes one draw for the pick and
// one for the jitter, and visits all four quadrants of a 4-step unit circle once.
void TestShuffle() {
  CRandom16 rnd(77);
  CGlobalRandom global(rnd);
  const CVEPortCircle* element = Circle(CVector3f(0.f, 0.f, 0.f), CVector3f(0.f, 1.f, 0.f), 0.f, 4, 0.f, 1.f, 4);
  std::set<int> seen;
  for (int i = 0; i < 4; ++i) {
    CVector3f out;
    element->GetValue(0, out);
    seen.insert(int(std::lround(out.GetX())) * 3 + int(std::lround(out.GetZ())));
  }
  CHECK_MSG(seen.size() == 4, "four calls visit four distinct steps");
  CRandom16 ref(77);
  for (int i = 0; i < 8; ++i) {
    ref.Next();
  }
  CHECK_MSG(rnd.GetSeed() == ref.GetSeed(), "two draws a call with a shuffler");
  delete element;
}

} // namespace

int main() {
  TestAngle();
  TestFrame();
  TestPoint();
  TestShuffle();
  return PORT_TEST_RESULT();
}

void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) { return ::operator new(size); }
void IElement::CElementAllocator::Free(void* ptr, size_t) { ::operator delete(ptr); }
void* IElement::operator new(size_t size, const char* file, const char* type) {
  return CElementAllocator::Alloc(size, file, type);
}
void IElement::operator delete(void* ptr, size_t size) { CElementAllocator::Free(ptr, size); }
