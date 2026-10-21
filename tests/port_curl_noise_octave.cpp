// Port curl noise with octaves (CRLN, octave class): CurlNoiseOctave3d and the PCRO element.
// The octave sum is linear and its gradient scales with the frequency, so the expected values are
// the single-octave curl at p and at 2p (CurlNoise3d, hand-checked in port_curl_noise.cpp) combined
// with the octave weights: curl_oct(2, gain) = curl(p) + 2 * gain * curl(2p).
#include "Kyoto/Particles/CPortCurlNoise.hpp"
#include "port_test.h"

#include <cmath>

namespace {

bool Near(float a, float b, float epsilon = 1.e-3f) { return std::fabs(a - b) <= epsilon; }

class TestVector final : public CVectorElement {
public:
  explicit TestVector(CVector3f v) : mValue(v) {}
  bool GetValue(int, CVector3f& out) const override {
    out = mValue;
    return false;
  }

private:
  CVector3f mValue;
};

class TestReal final : public CRealElement {
public:
  explicit TestReal(float v) : mValue(v) {}
  bool GetValue(int, float& out) const override {
    out = mValue;
    return false;
  }

private:
  float mValue;
};

class TestInt final : public CIntElement {
public:
  explicit TestInt(int v) : mValue(v) {}
  bool GetValue(int, int& out) const override {
    out = mValue;
    return false;
  }

private:
  int mValue;
};

constexpr float kEps = 1.e-2f;

void TestMath() {
  const float x = 0.3f, y = 0.6f, z = 0.9f;
  // Zero octaves is zero.
  const CVector3f none = PortCurlNoise::CurlNoiseOctave3d(0, x, y, z, 1.f, 0.5f, kEps);
  CHECK(none.GetX() == 0.f && none.GetY() == 0.f && none.GetZ() == 0.f);
  // One octave at frequency 1 is CurlNoise3d: (0.575285, -0.778021, 0.202736).
  const CVector3f one = PortCurlNoise::CurlNoiseOctave3d(1, x, y, z, 1.f, 0.5f, kEps);
  CHECK(Near(one.GetX(), 0.575285295f, 2.e-3f));
  CHECK(Near(one.GetY(), -0.778021286f, 2.e-3f));
  CHECK(Near(one.GetZ(), 0.202735991f, 2.e-3f));
  // Gain 0 leaves only the first octave.
  const CVector3f g0 = PortCurlNoise::CurlNoiseOctave3d(3, x, y, z, 1.f, 0.f, kEps);
  CHECK(Near(g0.GetX(), one.GetX()) && Near(g0.GetY(), one.GetY()) && Near(g0.GetZ(), one.GetZ()));
  // Two octaves: curl(p) + 2 * gain * curl(2p).
  const CVector3f c1 = PortCurlNoise::CurlNoise3d(x, y, z, kEps, 256);
  const CVector3f c2 = PortCurlNoise::CurlNoise3d(2.f * x, 2.f * y, 2.f * z, kEps, 256);
  const CVector3f two = PortCurlNoise::CurlNoiseOctave3d(2, x, y, z, 1.f, 0.5f, kEps);
  CHECK(Near(two.GetX(), c1.GetX() + c2.GetX(), 2.e-2f));
  CHECK(Near(two.GetY(), c1.GetY() + c2.GetY(), 2.e-2f));
  CHECK(Near(two.GetZ(), c1.GetZ() + c2.GetZ(), 2.e-2f));
  // The frequency scales the probe: freq 2, one octave is 2 * curl(2p).
  const CVector3f f2 = PortCurlNoise::CurlNoiseOctave3d(1, x, y, z, 2.f, 0.5f, kEps);
  CHECK(Near(f2.GetX(), 2.f * c2.GetX(), 2.e-2f));
  CHECK(Near(f2.GetY(), 2.f * c2.GetY(), 2.e-2f));
  CHECK(Near(f2.GetZ(), 2.f * c2.GetZ(), 2.e-2f));
}

void TestElement() {
  const CVector3f amp(2.f, 0.5f, -1.f);
  const CVector3f p(0.3f, 0.6f, 0.9f);
  const CVector3f ref = PortCurlNoise::CurlNoiseOctave3d(2, p.GetX(), p.GetY(), p.GetZ(), 1.f, 0.5f, 1.e-5f);
  CVEPortCurlNoiseOctave e(new TestVector(p), new TestReal(1.f), new TestInt(2), new TestReal(0.5f),
                           new TestVector(amp));
  CVector3f out;
  CHECK(!e.GetValue(0, out));
  CHECK(out.GetX() == amp.GetX() * ref.GetX());
  CHECK(out.GetY() == amp.GetY() * ref.GetY());
  CHECK(out.GetZ() == amp.GetZ() * ref.GetZ());
  // The amplitude of 1 passes the octave curl through, and the 1e-5 step is near the 1e-2 one.
  CVEPortCurlNoiseOctave one(new TestVector(p), new TestReal(1.f), new TestInt(1), new TestReal(1.f),
                             new TestVector(CVector3f(1.f, 1.f, 1.f)));
  one.GetValue(0, out);
  CHECK(Near(out.GetX(), 0.575285295f, 5.e-2f));
}

} // namespace

void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) { return ::operator new(size); }
void IElement::CElementAllocator::Free(void* ptr, size_t) { ::operator delete(ptr); }
void* IElement::operator new(size_t size, const char* file, const char* type) {
  return CElementAllocator::Alloc(size, file, type);
}
void IElement::operator delete(void* ptr, size_t size) { CElementAllocator::Free(ptr, size); }
const CVectorField* CVEPortCurlNoise::ResolveField() const { return nullptr; }

int main() {
  TestMath();
  TestElement();
  return PORT_TEST_RESULT();
}
