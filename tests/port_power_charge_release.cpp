#include "port_power_charge_release.h"
#include "port_test.h"

namespace {

// Numeric values of the charge-stage enum and CPlayerGun::EChargePhase.
constexpr int kNormalStage = 0;
constexpr int kChargedStage = 1;
constexpr int kNotCharging = 0;
constexpr int kChargeRequested = 1;
constexpr int kAnimAndSfx = 2;
constexpr int kFxGrowing = 3;

void CheckNoEarlyPowerRapidCorrection(bool rapidChargeEnabled, bool currentBeamIsPower) {
  const bool scopedPowerRapidCharge =
      NPortPowerChargeRelease::IsScopedRapidCharge(rapidChargeEnabled, currentBeamIsPower);
  int chargeStage = kNormalStage;

  CHECK(!scopedPowerRapidCharge);
  CHECK_EQ(NPortPowerChargeRelease::ChargeStageAtAnimationStart(
               chargeStage, kChargedStage, scopedPowerRapidCharge, false),
           kNormalStage);
  CHECK(!NPortPowerChargeRelease::IsChargedRelease(kAnimAndSfx, kFxGrowing, kAnimAndSfx, false,
                                                  scopedPowerRapidCharge));

  chargeStage = NPortPowerChargeRelease::ChargeStageAtAnimationStart(
      chargeStage, kChargedStage, scopedPowerRapidCharge, true);
  CHECK_EQ(chargeStage, kNormalStage);
  CHECK(!NPortPowerChargeRelease::IsChargedRelease(kAnimAndSfx, kFxGrowing, kAnimAndSfx, true,
                                                  scopedPowerRapidCharge));

  // The prior phase>=FxGrowing rule remains unconditional for all beams and settings.
  CHECK(NPortPowerChargeRelease::IsChargedRelease(kFxGrowing, kFxGrowing, kAnimAndSfx, false,
                                                 scopedPowerRapidCharge));
}

void TestPowerRapidChargeAnimationStartRelease() {
  const bool scopedPowerRapidCharge =
      NPortPowerChargeRelease::IsScopedRapidCharge(true, true);
  int chargeStage = kNormalStage;

  CHECK(scopedPowerRapidCharge);
  CHECK_EQ(NPortPowerChargeRelease::ChargeStageAtAnimationStart(
               chargeStage, kChargedStage, scopedPowerRapidCharge, false),
           kNormalStage);
  CHECK(!NPortPowerChargeRelease::IsChargedRelease(kNotCharging, kFxGrowing, kAnimAndSfx, true,
                                                  scopedPowerRapidCharge));
  CHECK(!NPortPowerChargeRelease::IsChargedRelease(kChargeRequested, kFxGrowing, kAnimAndSfx, true,
                                                  scopedPowerRapidCharge));
  CHECK(!NPortPowerChargeRelease::IsChargedRelease(kAnimAndSfx, kFxGrowing, kAnimAndSfx, false,
                                                  scopedPowerRapidCharge));

  // UpdateChargeState sets the animation-start bit before release processing in this update.
  chargeStage = NPortPowerChargeRelease::ChargeStageAtAnimationStart(
      chargeStage, kChargedStage, scopedPowerRapidCharge, true);
  CHECK_EQ(chargeStage, kChargedStage);
  CHECK(NPortPowerChargeRelease::IsChargedRelease(kAnimAndSfx, kFxGrowing, kAnimAndSfx, true,
                                                 scopedPowerRapidCharge));

  // The following FX phase retains the old unconditional release boundary and charged stage.
  CHECK(NPortPowerChargeRelease::IsChargedRelease(kFxGrowing, kFxGrowing, kAnimAndSfx, false,
                                                 scopedPowerRapidCharge));
  CHECK_EQ(chargeStage, kChargedStage);
}

void TestRapidChargeScopePreservesOtherCases() {
  // Rapid-charge disabled includes the classic/OriginalExperience path, where the option is
  // suppressed; rapid charge on a non-Power beam also gets no early-stage/release correction.
  CheckNoEarlyPowerRapidCorrection(false, false);
  CheckNoEarlyPowerRapidCorrection(false, true);
  CheckNoEarlyPowerRapidCorrection(true, false);
}

} // namespace

int main() {
  TestPowerRapidChargeAnimationStartRelease();
  TestRapidChargeScopePreservesOtherCases();
  return PORT_TEST_RESULT();
}
