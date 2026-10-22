// PSLW (Remastered's SLTW): a + tween * (b - a) by the system tween, clamp(frame / lifetime, 0, 1)
// (UpdateLifetimeTweenValues 0x24ef38 on time slot 0), the reciprocal bit-exact.
#include "Kyoto/Particles/CPortSystemTween.hpp"
#include "port_test.h"

namespace {

class ConstReal : public CRealElement {
  float m_value;

public:
  explicit ConstReal(float value) : m_value(value) {}
  bool GetValue(int, float& out) const override {
    out = m_value;
    return true;
  }
};

void TestTween() {
  // 16 / 64 = 0.25 exactly: 2 + 0.25 * (10 - 2) = 4.
  CParticleGlobals::PortSetSystemTime(16, 64);
  CHECK(CParticleGlobals::xPortSystemTween == 0.25f);
  float out = -1.f;
  CHECK(!CREPortSystemTween(new ConstReal(2.f), new ConstReal(10.f)).GetValue(0, out) && out == 4.f);

  // The tween clamps to [0, 1].
  CParticleGlobals::PortSetSystemTime(200, 64);
  CHECK(CParticleGlobals::xPortSystemTween == 1.f);
  CParticleGlobals::PortSetSystemTime(-5, 64);
  CHECK(CParticleGlobals::xPortSystemTween == 0.f);

  // A lifetime of 0 reads as 1: frame 1 is the full tween, frame 0 none.
  CParticleGlobals::PortSetSystemTime(1, 0);
  CHECK(CParticleGlobals::xPortSystemTween == 1.f);
  CParticleGlobals::PortSetSystemTime(0, 0);
  CHECK(CParticleGlobals::xPortSystemTween == 0.f);

  // Remastered's no-PSLT lifetime is 0x1000000, the only one that gives 0 (0x24ef38 compares for
  // exactly that); a lifetime just under it divides through to a small tween.
  CParticleGlobals::PortSetSystemTime(100, 0x7FFFFF);
  CHECK(CParticleGlobals::xPortSystemTween > 0.f && CParticleGlobals::xPortSystemTween < 1e-4f);
  CParticleGlobals::PortSetSystemTime(100, 0x1000000);
  CHECK(CParticleGlobals::xPortSystemTween == 0.f);
}

// The reciprocal is FRECPE and two FRECPS refinements (0x24ef38), not a division: these differ
// from frame * (1 / lifetime) and from frame / lifetime in the last bit.
void TestReciprocalBits() {
  CParticleGlobals::PortSetSystemTime(1, 13);
  CHECK(CParticleGlobals::xPortSystemTween == 0x1.3b13bp-4f);
  CHECK(CParticleGlobals::xPortSystemTween != 1.f * (1.f / 13.f));
  CParticleGlobals::PortSetSystemTime(1, 15);
  CHECK(CParticleGlobals::xPortSystemTween == 0x1.11111p-4f);
  CHECK(CParticleGlobals::xPortSystemTween != 1.f * (1.f / 15.f));
  CParticleGlobals::PortSetSystemTime(1, 21);
  CHECK(CParticleGlobals::xPortSystemTween == 0x1.86186p-5f);
}

// SLTW(0, 1) is the tween itself, the form both PMSC users have.
void TestUnitBlend() {
  CParticleGlobals::PortSetSystemTime(48, 64);
  float out = -1.f;
  CREPortSystemTween(new ConstReal(0.f), new ConstReal(1.f)).GetValue(0, out);
  CHECK(out == 0.75f);
}

}  // namespace

// Heap storage for elements.
void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) { return ::operator new(size); }
void IElement::CElementAllocator::Free(void* ptr, size_t) { ::operator delete(ptr); }
void* IElement::operator new(size_t size, const char* file, const char* type) {
  return CElementAllocator::Alloc(size, file, type);
}
void IElement::operator delete(void* ptr, size_t size) { CElementAllocator::Free(ptr, size); }

int main() {
  TestTween();
  TestReciprocalBits();
  TestUnitBlend();
  return PORT_TEST_RESULT();
}
