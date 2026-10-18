#include "Kyoto/Math/CMatrix3f.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "Kyoto/Particles/CVectorElement.hpp"
#include "port_test.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

bool Near(float a, float b, float epsilon = 2.e-5f) { return std::fabs(a - b) <= epsilon; }

CQuaternion EulerQuaternion(const CVector3f& degrees) {
  constexpr float kDegreesToRadians = 0.01745329251994329577f;
  const float x = std::remainder(degrees.GetX(), 360.f) * kDegreesToRadians * 0.5f;
  const float y = std::remainder(degrees.GetY(), 360.f) * kDegreesToRadians * 0.5f;
  const float z = std::remainder(degrees.GetZ(), 360.f) * kDegreesToRadians * 0.5f;
  return CQuaternion(std::cos(z), 0.f, 0.f, std::sin(z)) *
         CQuaternion(std::cos(y), 0.f, std::sin(y), 0.f) *
         CQuaternion(std::cos(x), std::sin(x), 0.f, 0.f);
}

CMatrix3f EulerMatrix(const CVector3f& degrees) { return EulerQuaternion(degrees).BuildTransform(); }

bool SameMatrix(const CMatrix3f& a, const CMatrix3f& b, float epsilon = 2.e-5f) {
  return Near(a.Get00(), b.Get00(), epsilon) && Near(a.Get01(), b.Get01(), epsilon) &&
         Near(a.Get02(), b.Get02(), epsilon) && Near(a.Get10(), b.Get10(), epsilon) &&
         Near(a.Get11(), b.Get11(), epsilon) && Near(a.Get12(), b.Get12(), epsilon) &&
         Near(a.Get20(), b.Get20(), epsilon) && Near(a.Get21(), b.Get21(), epsilon) &&
         Near(a.Get22(), b.Get22(), epsilon);
}

class TestVector final : public CVectorElement {
public:
  TestVector(CVector3f value, bool fastConstant, int id = 0, std::vector<int>* trace = nullptr)
  : mValue(value), mFastConstant(fastConstant), mId(id), mTrace(trace) {}

  TestVector(std::vector<CVector3f> samples, std::vector<int>* trace = nullptr, int id = 0)
  : mSamples(std::move(samples)), mId(id), mTrace(trace) {}

  bool GetValue(int frame, CVector3f& out) const override {
    ++mCalls;
    if (mTrace != nullptr) {
      mTrace->push_back(mId);
    }
    if (!mSamples.empty()) {
      const int count = int(mSamples.size());
      const int index = (frame % count + count) % count;
      out = mSamples[size_t(index)];
    } else {
      out = mValue;
    }
    return true;
  }

  bool IsFastConstant() const override { return mFastConstant; }
  int Calls() const { return mCalls; }

private:
  CVector3f mValue = CVector3f::Zero();
  std::vector<CVector3f> mSamples;
  bool mFastConstant = false;
  int mId = 0;
  std::vector<int>* mTrace = nullptr;
  mutable int mCalls = 0;
};

CMatrix3f BasisMatrix() { return CMatrix3f(-1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 0.f); }

std::unique_ptr<CVectorElement> BasisConjugated(CVectorElement* source) {
  const CVector3f basisAngles(90.f, 0.f, 180.f);
  return std::unique_ptr<CVectorElement>(new CVEOrderedEulerProduct(
      new TestVector(basisAngles, true),
      new CVEOrderedEulerProduct(source, new TestVector(basisAngles, true))));
}

CVector3f SeededAngles(uint32_t seed) {
  auto next = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return float((seed >> 8) % 36000u) * 0.01f;
  };
  return CVector3f(next(), next(), next());
}

void TestOrderedProductAndEvaluation() {
  std::vector<int> trace;
  const CVector3f firstAngles(90.f, 0.f, 0.f);
  const CVector3f secondAngles(0.f, 90.f, 0.f);
  CVEOrderedEulerProduct product(new TestVector(firstAngles, true, 1, &trace),
                                 new TestVector(secondAngles, true, 2, &trace));
  CHECK_MSG(product.IsFastConstant(), "both fast-constant operands mark the product fast-constant");

  CVector3f result(0.f, 0.f, 0.f);
  CHECK_MSG(!product.GetValue(5, result), "GetValue keeps vector-element false return convention");
  CHECK_MSG((trace == std::vector<int>{1, 2}), "operands evaluate once each in original order");

  const CMatrix3f want = (EulerQuaternion(firstAngles) * EulerQuaternion(secondAngles)).BuildTransform();
  const CMatrix3f actual = EulerMatrix(result);
  CHECK_MSG(SameMatrix(actual, want), "noncommuting X90 then Y90 returns equivalent composed orientation");
  CHECK_MSG(Near(actual.Get02(), 1.f) && Near(actual.Get12(), 0.f) && Near(actual.Get22(), 0.f),
            "X90 then Y90 rotates +Z to +X");
  const CMatrix3f reversed = (EulerQuaternion(secondAngles) * EulerQuaternion(firstAngles)).BuildTransform();
  CHECK_MSG(!SameMatrix(want, reversed, 0.1f), "the source-order discriminator differs from reversed multiplication");

  CVEOrderedEulerProduct oneDynamic(new TestVector(firstAngles, true), new TestVector(secondAngles, false));
  CVEOrderedEulerProduct otherDynamic(new TestVector(firstAngles, false), new TestVector(secondAngles, true));
  CHECK_MSG(!oneDynamic.IsFastConstant() && !otherDynamic.IsFastConstant(),
            "one non-fast-constant operand keeps the product per-particle/per-frame");
}

void TestGimbalAndAnimatedSamples() {
  for (float y : {90.f, -90.f, 89.9999f, -89.9999f}) {
    const CVector3f firstAngles(37.f, y, -71.f);
    const CVector3f secondAngles(0.f, 0.f, 0.f);
    CVEOrderedEulerProduct product(new TestVector(firstAngles, false), new TestVector(secondAngles, true));
    CVector3f result(0.f, 0.f, 0.f);
    product.GetValue(0, result);
    CHECK_MSG(SameMatrix(EulerMatrix(result), EulerMatrix(firstAngles), 5.e-5f),
               "exact and near Y gimbals preserve orientation");
  }

  for (float y : {90.f, -90.f}) {
    const CVector3f firstAngles(45.f, y, 150.f);
    const CVector3f identity(0.f, 0.f, 0.f);
    CVEOrderedEulerProduct product(new TestVector(firstAngles, false), new TestVector(identity, true));
    CVector3f result(0.f, 0.f, 0.f);
    product.GetValue(0, result);
    CHECK_MSG(SameMatrix(EulerMatrix(result), EulerMatrix(firstAngles), 5.e-5f),
              "mixed X/Z angles at either exact Y gimbal retain orientation");
  }

  for (float y : {89.9999f, -89.9999f}) {
    const CVector3f firstAngles(45.f, y, 150.f);
    const CVector3f secondAngles(-0.0002f, 0.0001f, 0.0003f);
    CVEOrderedEulerProduct product(new TestVector(firstAngles, false), new TestVector(secondAngles, false));
    CVector3f result(0.f, 0.f, 0.f);
    product.GetValue(0, result);
    const CMatrix3f expected = (EulerQuaternion(firstAngles) * EulerQuaternion(secondAngles)).BuildTransform();
    CHECK_MSG(SameMatrix(EulerMatrix(result), expected, 5.e-5f),
              "two-operand near-gimbal composition preserves its matrix");
  }

  std::vector<CVector3f> keyed;
  keyed.reserve(120);
  for (int i = 0; i < 120; ++i) {
    const float degrees = i == 119 ? 360.00003f : i == 0 ? 0.f : float(i) * 3.0252087f;
    keyed.emplace_back(0.f, degrees, 0.f);
  }
  CVEOrderedEulerProduct animated(new TestVector(CVector3f(0.f, 0.f, 0.f), false),
                                 new TestVector(keyed));
  CVector3f start(0.f, 0.f, 0.f);
  CVector3f end(0.f, 0.f, 0.f);
  CVector3f looped(0.f, 0.f, 0.f);
  animated.GetValue(0, start);
  animated.GetValue(119, end);
  animated.GetValue(120, looped);
  CHECK_MSG(SameMatrix(EulerMatrix(start), EulerMatrix(CVector3f(0.f, 0.f, 0.f))),
            "emitter-time loop starts at the first sample");
  CHECK_MSG(SameMatrix(EulerMatrix(end), EulerMatrix(keyed.back())), "the complete last key sample is evaluated");
  CHECK_MSG(SameMatrix(EulerMatrix(looped), EulerMatrix(start)), "the 120-frame emitter loop returns to sample zero");

  const CMatrix3f basis = BasisMatrix();
  std::unique_ptr<CVectorElement> converted = BasisConjugated(new TestVector(keyed));
  CVector3f convertedStart(0.f, 0.f, 0.f), convertedEnd(0.f, 0.f, 0.f), convertedLoop(0.f, 0.f, 0.f);
  converted->GetValue(0, convertedStart);
  converted->GetValue(119, convertedEnd);
  converted->GetValue(120, convertedLoop);
  CHECK_MSG(SameMatrix(EulerMatrix(convertedStart), basis * EulerMatrix(keyed.front()) * basis.GetTranspose(), 5.e-5f),
             "basis-conjugated keyed rotation reads its first frame");
  CHECK_MSG(SameMatrix(EulerMatrix(convertedEnd), basis * EulerMatrix(keyed.back()) * basis.GetTranspose(), 5.e-5f),
             "basis-conjugated keyed rotation evaluates the final frame");
  CHECK_MSG(SameMatrix(EulerMatrix(convertedLoop), EulerMatrix(convertedStart), 5.e-5f),
             "basis-conjugated keyed rotation retains its emitter-time loop");
}

void TestStableParticleRandomnessAndFiniteInputs() {
  const CVector3f fixedSeedA = SeededAngles(0x13579bdfu);
  const CVector3f fixedSeedB = SeededAngles(0x2468ace0u);
  std::unique_ptr<CVectorElement> particleA = BasisConjugated(new TestVector(fixedSeedA, false));
  std::unique_ptr<CVectorElement> particleB = BasisConjugated(new TestVector(fixedSeedB, false));
  CVector3f a1(0.f, 0.f, 0.f), a2(0.f, 0.f, 0.f), b(0.f, 0.f, 0.f);
  CVector3f aNextFrame(0.f, 0.f, 0.f);
  particleA->GetValue(23, a1);
  particleA->GetValue(23, a2);
  particleA->GetValue(24, aNextFrame);
  particleB->GetValue(23, b);
  CHECK_MSG(SameMatrix(EulerMatrix(a1), EulerMatrix(a2)), "fixed-seed random Euler values are stable on repeated draws");
  CHECK_MSG(SameMatrix(EulerMatrix(a1), EulerMatrix(aNextFrame)),
            "a particle's basis-conjugated IRND remains stable across frames");
  const CMatrix3f basis = BasisMatrix();
  CHECK_MSG(SameMatrix(EulerMatrix(a1), basis * EulerMatrix(fixedSeedA) * basis.GetTranspose()),
            "basis conjugation retains the particle's sampled random orientation");
  CHECK_MSG(!SameMatrix(EulerMatrix(a1), EulerMatrix(b), 0.01f),
            "separate basis-conjugated particle seeds are not shared or cached");

  CVEOrderedEulerProduct invalid(new TestVector(CVector3f(INFINITY, 0.f, 0.f), false),
                                new TestVector(CVector3f(0.f, 0.f, 0.f), false));
  CVector3f safe(1.f, 2.f, 3.f);
  invalid.GetValue(0, safe);
  CHECK_MSG(safe.GetX() == 0.f && safe.GetY() == 0.f && safe.GetZ() == 0.f,
            "nonfinite input produces a finite neutral rotation");
}
} // namespace

// This test supplies the element allocator with ordinary heap storage. The
// production element's behavior is unchanged; no particle factory is needed.
void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) { return ::operator new(size); }
void IElement::CElementAllocator::Free(void* ptr, size_t) { ::operator delete(ptr); }
void* IElement::operator new(size_t size, const char* file, const char* type) {
  return CElementAllocator::Alloc(size, file, type);
}
void IElement::operator delete(void* ptr, size_t size) { CElementAllocator::Free(ptr, size); }

int main() {
  TestOrderedProductAndEvaluation();
  TestGimbalAndAnimatedSamples();
  TestStableParticleRandomnessAndFiniteInputs();
  if (port_test::Failures() == 0) {
    std::puts("port_ice_rotation_element: ok");
  }
  return PORT_TEST_RESULT();
}
