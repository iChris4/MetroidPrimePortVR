// Port scalar elements: ABS_ (real, int, vector), COSE, RSGN, PIRS (Remastered's IRSN) and PMXV.
// COSE is checked against std::cos (FastCosR is its polynomial, within ~1e-4), the signs against
// an independent CRandom16 sequence.
#include "Kyoto/CRandom16.hpp"
#include "Kyoto/Particles/CIntElement.hpp"
#include "Kyoto/Particles/CPortParticleVars.hpp"
#include "Kyoto/Particles/CRealElement.hpp"
#include "Kyoto/Particles/CVectorElement.hpp"
#include "port_test.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

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

bool Near(float a, float b, float epsilon) { return std::fabs(a - b) <= epsilon; }

void TestAbs() {
  float r = 1.f;
  CHECK(!CREPortAbs(new ConstReal(-2.5f)).GetValue(0, r) && r == 2.5f);
  CHECK(!CREPortAbs(new ConstReal(3.f)).GetValue(0, r) && r == 3.f);
  int i = 0;
  CHECK(!CIEPortAbs(new ConstInt(-7)).GetValue(0, i) && i == 7);
  CHECK(!CIEPortAbs(new ConstInt(7)).GetValue(0, i) && i == 7);
  // 0 - INT_MIN wraps back to INT_MIN, as the shipped code's does.
  CHECK(!CIEPortAbs(new ConstInt(INT_MIN)).GetValue(0, i) && i == INT_MIN);
  CVector3f v;
  CHECK(!CVEPortAbs(new ConstVector(CVector3f(-1.f, 2.f, -3.f))).GetValue(0, v));
  CHECK(v.GetX() == 1.f && v.GetY() == 2.f && v.GetZ() == 3.f);
}

void TestCosine() {
  // frequency 90 deg/frame, amplitude 2, phase 45 deg: angles beyond +-pi and negative ones.
  const CREPortCosineWave wave(new ConstReal(90.f), new ConstReal(2.f), new ConstReal(45.f));
  for (int frame = 0; frame < 40; ++frame) {
    float out = 0.f;
    CHECK(!wave.GetValue(frame, out));
    const double angle = (45.0 + 90.0 * frame) * 3.14159265358979323846 / 180.0;
    CHECK_MSG(Near(out, float(2.0 * std::cos(angle)), 2.e-3f), "COSE follows 2 cos(phase + f * frame)");
  }
  const CREPortCosineWave negative(new ConstReal(-300.f), new ConstReal(-1.f), new ConstReal(-1000.f));
  for (int frame = 0; frame < 10; ++frame) {
    float out = 0.f;
    CHECK(!negative.GetValue(frame, out));
    const double angle = (-1000.0 - 300.0 * frame) * 3.14159265358979323846 / 180.0;
    CHECK_MSG(Near(out, float(-std::cos(angle)), 2.e-3f), "COSE with negative angles");
  }
}

void TestRandomSign() {
  const CREPortRandomSign sign(new ConstReal(3.f));
  CRandom16 rnd(12345);
  CRandom16 want(12345);
  CGlobalRandom global(rnd);
  int positive = 0;
  for (int frame = 0; frame < 64; ++frame) {
    float out = 0.f;
    CHECK(!sign.GetValue(frame, out));
    const bool plus = (want.Next() & 1) != 0;
    positive += plus;
    CHECK_MSG(out == (plus ? 3.f : -3.f), "RSGN draws on every evaluation, bit 0 set is positive");
  }
  CHECK_MSG(positive > 8 && positive < 56, "the sequence has both signs");
}

void TestInitialRandomSign() {
  SPortKeep keep{};
  CPortKeepScope scope(&keep);
  const CREPortInitialRandomSign sign(2, new ConstReal(5.f));
  CRandom16 rnd(777);
  CRandom16 want(777);
  CGlobalRandom global(rnd);
  float out = 1.f;
  // Never stored (first read after frame 0): Remastered's zeroed variable memory.
  CHECK(!sign.GetValue(3, out) && out == 0.f);
  const float expected = (want.Next() & 1) != 0 ? 5.f : -5.f;
  CHECK(!sign.GetValue(0, out) && out == expected);
  CHECK(keep.latched == 0);  // it draws at frame 0 only; the birth latch never touches it
  for (int frame = 1; frame < 5; ++frame) {
    CHECK(!sign.GetValue(frame, out) && out == expected);
  }
  CHECK_MSG(rnd.Next() == want.Next(), "later frames draw nothing");
  // Another particle gets its own draw.
  SPortKeep other{};
  CPortKeepScope inner(&other);
  const float second = (want.Next() & 1) != 0 ? 5.f : -5.f;
  CHECK(!sign.GetValue(0, out) && out == second);
  // No per-particle slots: the element keeps the sign itself.
  CPortKeepScope none(nullptr);
  const CREPortInitialRandomSign solo(CREPortInitialRandomSign::kNoSlot, new ConstReal(2.f));
  const float soloWant = (want.Next() & 1) != 0 ? 2.f : -2.f;
  CHECK(!solo.GetValue(0, out) && out == soloWant);
  CHECK(!solo.GetValue(1, out) && out == soloWant);
}

const float* gExternalVars = nullptr;

void TestExternalVar() {
  float vars[16];
  for (int i = 0; i < 16; ++i) {
    vars[i] = float(i) * 10.f + 1.f;
  }
  float out = 0.f;
  // No external block: the fallback (the index is not evaluated).
  gExternalVars = nullptr;
  CHECK(!CREPortExternalVar(new ConstInt(3), new ConstReal(0.25f)).GetValue(0, out) && out == 0.25f);
  gExternalVars = vars;
  CHECK(!CREPortExternalVar(new ConstInt(0), new ConstReal(0.25f)).GetValue(0, out) && out == 1.f);
  CHECK(!CREPortExternalVar(new ConstInt(1), new ConstReal(0.25f)).GetValue(0, out) && out == 11.f);
  CHECK(!CREPortExternalVar(new ConstInt(11), new ConstReal(0.25f)).GetValue(0, out) && out == 111.f);
  // max(i, 0) % 12: negative reads 0, 12 wraps to 0, 13 to 1, and the vars past 12 are never read.
  CHECK(!CREPortExternalVar(new ConstInt(-4), new ConstReal(0.25f)).GetValue(0, out) && out == 1.f);
  CHECK(!CREPortExternalVar(new ConstInt(INT_MIN), new ConstReal(0.25f)).GetValue(0, out) && out == 1.f);
  CHECK(!CREPortExternalVar(new ConstInt(12), new ConstReal(0.25f)).GetValue(0, out) && out == 1.f);
  CHECK(!CREPortExternalVar(new ConstInt(13), new ConstReal(0.25f)).GetValue(0, out) && out == 11.f);
  CHECK(!CREPortExternalVar(new ConstInt(15), new ConstReal(0.25f)).GetValue(0, out) && out == 31.f);
  gExternalVars = nullptr;
}

} // namespace

const float* PortCurrentExternalVars() { return gExternalVars; }

// Heap storage for elements.
void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) { return ::operator new(size); }
void IElement::CElementAllocator::Free(void* ptr, size_t) { ::operator delete(ptr); }
void* IElement::operator new(size_t size, const char* file, const char* type) {
  return CElementAllocator::Alloc(size, file, type);
}
void IElement::operator delete(void* ptr, size_t size) { CElementAllocator::Free(ptr, size); }

int main() {
  TestAbs();
  TestCosine();
  TestRandomSign();
  TestInitialRandomSign();
  TestExternalVar();
  return PORT_TEST_RESULT();
}
