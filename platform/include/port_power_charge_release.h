#pragma once

namespace NPortPowerChargeRelease {

constexpr bool IsScopedRapidCharge(bool rapidChargeEnabled, bool currentBeamIsPower) {
  return rapidChargeEnabled && currentBeamIsPower;
}

template <typename Stage>
constexpr Stage ChargeStageAtAnimationStart(Stage currentStage, Stage chargedStage,
                                            bool scopedPowerRapidCharge, bool chargeAnimStarted) {
  return scopedPowerRapidCharge && chargeAnimStarted ? chargedStage : currentStage;
}

constexpr bool IsChargedRelease(int chargePhase, int fxGrowingPhase, int animAndSfxPhase,
                                bool chargeAnimStarted, bool scopedPowerRapidCharge) {
  return chargePhase >= fxGrowingPhase ||
         (scopedPowerRapidCharge && chargePhase == animAndSfxPhase && chargeAnimStarted);
}

} // namespace NPortPowerChargeRelease
