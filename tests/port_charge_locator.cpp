#include "MetroidPrime/Weapons/CGunWeapon.hpp"
#include "port_test.h"

#include <cstdio>

namespace {

bool Near(float a, float b) { return a > b - 0.0001f && a < b + 0.0001f; }

struct CGeneratorRecorder {
  explicit CGeneratorRecorder(bool remastered)
  : remastered(remastered)
  , orientationCalls(0)
  , translationCalls(0)
  , rotation{7.f, 8.f, 9.f, 10.f, 11.f, 12.f, 13.f, 14.f, 15.f}
  , translation{-1.f, -2.f, -3.f} {}

  bool PortIsRemastered() const { return remastered; }

  void SetGlobalOrientAndTrans(const CTransform4f& xf) {
    ++orientationCalls;
    rotation[0] = xf.Get00();
    rotation[1] = xf.Get01();
    rotation[2] = xf.Get02();
    rotation[3] = xf.Get10();
    rotation[4] = xf.Get11();
    rotation[5] = xf.Get12();
    rotation[6] = xf.Get20();
    rotation[7] = xf.Get21();
    rotation[8] = xf.Get22();
    SetGlobalTranslation(xf.GetTranslation());
  }

  void SetGlobalTranslation(const CVector3f& pos) {
    ++translationCalls;
    translation[0] = pos.GetX();
    translation[1] = pos.GetY();
    translation[2] = pos.GetZ();
  }

  // The generator's frame applied to a local offset, as CElementGen places its children.
  void ChildOffset(float x, float y, float z, float out[3]) const {
    out[0] = translation[0] + rotation[0] * x + rotation[1] * y + rotation[2] * z;
    out[1] = translation[1] + rotation[3] * x + rotation[4] * y + rotation[5] * z;
    out[2] = translation[2] + rotation[6] * x + rotation[7] * y + rotation[8] * z;
  }

  bool remastered;
  int orientationCalls;
  int translationCalls;
  float rotation[9];
  float translation[3];
};

const CTransform4f IdentityLocator(1.f, 0.f, 0.f, 0.f,
                                   0.f, 1.f, 0.f, 0.f,
                                   0.f, 0.f, 1.f, 0.f);

// The retail LBEAM locator at full Power charge, measured in game (gun-local).
const CTransform4f SampledPowerLocator(0.999993f, 0.003689f, -0.000819f, 0.00705f,
                                       -0.003690f, 0.999992f, -0.001245f, 1.28631f,
                                       0.000815f, 0.001248f, 0.999999f, 0.00166f);

// An exact, trig-free rotated sample, a quarter turn about the third axis, with a translation.
const CTransform4f QuarterTurnLocator(0.f, -1.f, 0.f, 0.5f,
                                      1.f, 0.f, 0.f, -0.25f,
                                      0.f, 0.f, 1.f, 0.f);

// A general Rz * Ry sample: every column nontrivial.
const CTransform4f RotatedLocator(0.48f, -0.8f, 0.36f, -2.5f,
                                  0.64f, 0.6f, 0.48f, 3.75f,
                                  -0.6f, 0.f, 0.8f, -0.125f);

void TestConvertedTakesTheSampledLocator(const CTransform4f& locator) {
  CGeneratorRecorder generator(true);
  NPortChargeFxDetail::SetLocalLocator(generator, locator);
  CHECK_MSG(generator.orientationCalls == 1 && generator.translationCalls == 1,
            "converted effect receives exactly one full locator");
  const float sampled[9] = {locator.Get00(), locator.Get01(), locator.Get02(),
                            locator.Get10(), locator.Get11(), locator.Get12(),
                            locator.Get20(), locator.Get21(), locator.Get22()};
  const float translation[3] = {locator.Get03(), locator.Get13(), locator.Get23()};
  for (int i = 0; i < 9; ++i) {
    CHECK_MSG(generator.rotation[i] == sampled[i], "converted effect keeps the sampled rotation");
  }
  for (int row = 0; row < 3; ++row) {
    CHECK_MSG(generator.translation[row] == translation[row],
              "converted effect keeps the sampled translation");
  }
  // The Power charge disc's EMTR, Remastered (0,0,.15), is the port's (0,.15,0) after the
  // import: at scale 2 it lands .30 along the sampled forward (+Y) axis.
  float out[3];
  generator.ChildOffset(0.f, 0.15f * 2.f, 0.f, out);
  for (int row = 0; row < 3; ++row) {
    CHECK_MSG(Near(out[row] - translation[row], sampled[row * 3 + 1] * 0.30f),
              "the converted disc offset goes .30 along the sampled forward axis");
  }
}

void TestRetailKeepsTranslationOnly(const CTransform4f& locator) {
  CGeneratorRecorder generator(false);
  NPortChargeFxDetail::SetLocalLocator(generator, locator);
  CHECK_MSG(generator.orientationCalls == 0, "retail effect does not receive locator orientation");
  CHECK_MSG(generator.translationCalls == 1, "retail effect keeps the legacy translation setter");
  CHECK_MSG(Near(generator.rotation[0], 7.f) && Near(generator.rotation[5], 12.f) &&
                Near(generator.rotation[8], 15.f),
            "retail effect orientation is left untouched");
  CHECK_MSG(Near(generator.translation[0], locator.Get03()) &&
                Near(generator.translation[1], locator.Get13()) &&
                Near(generator.translation[2], locator.Get23()),
            "retail effect retains locator translation");
}
} // namespace

int main() {
  for (const CTransform4f* locator :
       {&IdentityLocator, &SampledPowerLocator, &QuarterTurnLocator, &RotatedLocator}) {
    TestConvertedTakesTheSampledLocator(*locator);
    TestRetailKeepsTranslationOnly(*locator);
  }
  if (port_test::Failures() == 0) {
    std::puts("charge locator checks passed");
  }
  return PORT_TEST_RESULT();
}
