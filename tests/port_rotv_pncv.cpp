// Remastered's PNCV (CVEPortNormalizedVelocity) and ROTV outside a component tree
// (CVEPortRotateVector). PNCV is checked bit for bit against values worked by hand from ARM's
// RecipSqrtEstimate table and FRSQRTS (3 - a*b)/2: for (3, 0, 4), m2 = 25, FRSQRTE(25) =
// 0x3e4c8000, FRSQRTS(25, r*r) refines it to 0x3e4ccca2, so the result is
// (0x3f19997a, 0, 0x3f4ccca2). ROTV checks the evaluation order (rotation, then vector) and
// REUL(0, 0, 90) turning (1, 0, 0) onto (0, 1, 0) within the fast sine's error.
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/Particles/CRealElement.hpp"
#include "Kyoto/Particles/CVectorElement.hpp"
#include "Kyoto/Particles/PortParticleMath.hpp"
#include "port_test.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

uint32_t Bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

std::vector<int>* gOrder = nullptr;

class ConstReal final : public CRealElement {
public:
  ConstReal(float v, int tag) : mValue(v), mTag(tag) {}
  bool GetValue(int, float& out) const override {
    out = mValue;
    if (gOrder != nullptr) {
      gOrder->push_back(mTag);
    }
    return false;
  }

private:
  float mValue;
  int mTag;
};

class ConstVector final : public CVectorElement {
public:
  explicit ConstVector(CVector3f v) : mValue(v) {}
  bool GetValue(int, CVector3f& out) const override {
    out = mValue;
    if (gOrder != nullptr) {
      gOrder->push_back(100);
    }
    return false;
  }

private:
  CVector3f mValue;
};

void TestNormalizedVelocityMath() {
  const CVector3f n = PortParticleMath::NormalizedVelocity(CVector3f(3.f, 0.f, 4.f));
  CHECK_MSG(Bits(n.GetX()) == 0x3f19997au && n.GetY() == 0.f && Bits(n.GetZ()) == 0x3f4ccca2u,
            "PNCV (3, 0, 4) is one refined FRSQRTE step, not 0.6 / 0.8");
  const CVector3f m = PortParticleMath::NormalizedVelocity(CVector3f(1.f, 2.f, 2.f));
  CHECK_MSG(Bits(m.GetX()) == 0x3eaaaa9bu && Bits(m.GetY()) == 0x3f2aaa9bu && Bits(m.GetZ()) == 0x3f2aaa9bu,
            "PNCV (1, 2, 2)");
  // 2^-46 is skMinNormalizableMagSqd; the test is strictly greater.
  const CVector3f edge = PortParticleMath::NormalizedVelocity(CVector3f(std::ldexp(1.f, -23), 0.f, 0.f));
  CHECK_MSG(edge.GetX() == 0.f && edge.GetY() == 1.f && edge.GetZ() == 0.f, "PNCV at 2^-46 is the up vector");
  const CVector3f zero = PortParticleMath::NormalizedVelocity(CVector3f::Zero());
  CHECK_MSG(zero.GetX() == 0.f && zero.GetY() == 1.f && zero.GetZ() == 0.f, "PNCV of rest is (0, 1, 0)");
}

// The element reads the port's velocity and sums its squares in Remastered's axes: retail
// (-3, 4, 0) is Remastered (3, 0, 4).
void TestNormalizedVelocityElement() {
  CElementGen::CParticle particle;
  particle.x1c_vel = CVector3f(-3.f, 4.f, 0.f);
  CElementGen::CParticle* saved = CParticleGlobals::mCurrentParticle;
  CParticleGlobals::mCurrentParticle = &particle;
  CVEPortNormalizedVelocity element;
  CVector3f out;
  CHECK(!element.GetValue(0, out));
  CParticleGlobals::mCurrentParticle = saved;
  CHECK_MSG(Bits(out.GetX()) == 0x3f19997au && out.GetY() == 0.f && Bits(out.GetZ()) == 0x3f4ccca2u,
            "PNCV element gives Remastered's numbers");
}

void TestRotateVector() {
  std::vector<int> order;
  gOrder = &order;
  CVEPortRotateVector element(new ConstVector(CVector3f(1.f, 0.f, 0.f)), 'REUL', new ConstReal(0.f, 1),
                              new ConstReal(0.f, 2), new ConstReal(90.f, 3));
  CVector3f out;
  CHECK(!element.GetValue(0, out));
  gOrder = nullptr;
  CHECK_MSG(order == std::vector<int>({1, 2, 3, 100}), "the rotation's angles are read before the vector");
  CHECK_MSG(std::fabs(out.GetX()) < 1e-3f && std::fabs(out.GetY() - 1.f) < 1e-3f && std::fabs(out.GetZ()) < 1e-3f,
            "REUL(0, 0, 90) turns (1, 0, 0) onto (0, 1, 0)");
  CVEPortRotateVector identity(new ConstVector(CVector3f(1.f, 2.f, 3.f)), 'REUL', new ConstReal(0.f, 1),
                               new ConstReal(0.f, 2), new ConstReal(0.f, 3));
  CHECK(!identity.GetValue(0, out));
  // EulerQuat's sine is the fast approximation, so zero angles are the identity only to ~1e-5.
  CHECK_MSG(std::fabs(out.GetX() - 1.f) < 1e-4f && std::fabs(out.GetY() - 2.f) < 1e-4f &&
                std::fabs(out.GetZ() - 3.f) < 1e-4f,
            "REUL(0, 0, 0) leaves the vector");
}

} // namespace

float CMath::SqrtF(float x) { return std::sqrt(x); }
CTransform4f CGraphics::mViewMatrix = CTransform4f::Identity();
void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) { return ::operator new(size); }
void IElement::CElementAllocator::Free(void* ptr, size_t) { ::operator delete(ptr); }
void* IElement::operator new(size_t size, const char* file, const char* type) {
  return CElementAllocator::Alloc(size, file, type);
}
void IElement::operator delete(void* ptr, size_t size) { CElementAllocator::Free(ptr, size); }

int main() {
  TestNormalizedVelocityMath();
  TestNormalizedVelocityElement();
  TestRotateVector();
  return PORT_TEST_RESULT();
}
