// PKYF (Remastered's KEYF, CREKeyframeInput 0x296490 / 0x29661c): hand-computed values for keys
// {0, 10, 20, 40}, A (loop start) 1.0, B (start) 0.5, R (rate) 2.
#include "Kyoto/Particles/CPortKeyframeInput.hpp"
#include "port_test.h"

#include <vector>

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

float Eval(bool loop, float t) {
  const CREPortKeyframeInput element(loop, 4, 1.0f, 0.5f, 2.0f, {0.f, 10.f, 20.f, 40.f}, nullptr);
  return element.Evaluate(t);
}

void TestNoLoop() {
  // t = 1.5: u = 1, scaled = 2, k = 2, f = 2 * (1 - 2 * 0.5) = 0 -> key 2.
  CHECK(Eval(false, 1.5f) == 20.f);
  // t = 1.25: u = 0.75, scaled = 1.5, k = 1, f = 2 * (0.75 - 0.5) = 0.5 -> 10 * 0.5 + 20 * 0.5.
  CHECK(Eval(false, 1.25f) == 15.f);
  // Before the start: index < 0 clamps to the first key.
  CHECK(Eval(false, 0.f) == 0.f);
  // Past the last key: the last key.
  CHECK(Eval(false, 10.f) == 40.f);
}

void TestLoop() {
  // end = 0.5 + 0.5 * 3 = 2; t = 2.5: 0.5 past the end, span 1, wrapped to A + 0.5 = 1.5 -> key 2.
  CHECK(Eval(true, 2.5f) == 20.f);
  // t = 4.25: 2.25 past the end, two spans leave 0.25, A + 0.25 = 1.25 -> 15.
  CHECK(Eval(true, 4.25f) == 15.f);
  // Before the end the loop changes nothing.
  CHECK(Eval(true, 1.25f) == 15.f);
}

void TestInput() {
  float out = -1.f;
  const CREPortKeyframeInput element(false, 4, 1.0f, 0.5f, 2.0f, {0.f, 10.f, 20.f, 40.f},
                                     new ConstReal(1.25f));
  CHECK(!element.GetValue(0, out) && out == 15.f);
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
  TestNoLoop();
  TestLoop();
  TestInput();
  return PORT_TEST_RESULT();
}
