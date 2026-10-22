#include "port_gyro_motion.h"

// GamepadMotion.hpp uses M_PI, which MSVC only defines on request.
#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "../extern/GamepadMotionHelpers/GamepadMotion.hpp"

namespace PortGyro {
namespace {
constexpr float kDegPerRad = 57.29577951308232f;
constexpr float kStandardGravity = 9.80665f;
// How far back the smoothing averages.
constexpr float kSmoothWindow = 0.125f;
} // namespace

Motion::Motion() : mMotion(std::make_unique< GamepadMotion >()) {}
Motion::~Motion() = default;

void Motion::Reset() {
  mMotion->Reset();
  mMotion->PauseContinuousCalibration();
  mHasMode = false;
  mManual = false;
}

void Motion::BeginManualCalibration() {
  // While it samples, GamepadMotionHelpers skips its auto calibration.
  mMotion->ResetContinuousCalibration();
  mMotion->StartContinuousCalibration();
  mManual = true;
  mManualSamples = 0;
}

void Motion::EndManualCalibration() {
  if (!mManual) {
    return;
  }
  mMotion->PauseContinuousCalibration();
  // Full confidence, so auto calibration refines the average instead of
  // replacing it with its first still moment. No samples (the gyro was not
  // read meanwhile) leaves nothing to trust.
  if (mManualSamples > 0) {
    mMotion->SetAutoCalibrationConfidence(1.f);
  }
  mManual = false;
}

float Motion::Confidence() const { return mMotion->GetAutoCalibrationConfidence(); }

bool Motion::Steady() const { return mMotion->GetAutoCalibrationIsSteady(); }

void Motion::Process(const float gyro[3], const float* accel, float dt, int space, bool autoCalibrate,
                     float& pitch, float& yaw) {
  // SDL's rates are counter-clockwise positive, so a turn right of a flat pad
  // is a negative y, and of an upright one (its -z up) a positive z;
  // GamepadMotionHelpers uses the same Y-up axes. Without an accelerometer the
  // raw local rates pass through.
  float x = gyro[0] * kDegPerRad;
  float y = gyro[1] * kDegPerRad;
  float z = gyro[2] * kDegPerRad;
  if (accel != nullptr) {
    if (!mHasMode || autoCalibrate != mAutoCalibrate) {
      // Only the mode changes: turning calibration off keeps the drift learned
      // so far (Recalibrate or a source change replaces it).
      mMotion->SetCalibrationMode(autoCalibrate ? GamepadMotionHelpers::Stillness | GamepadMotionHelpers::SensorFusion
                                                : GamepadMotionHelpers::Manual);
      mAutoCalibrate = autoCalibrate;
      mHasMode = true;
    }
    if (dt > 0.f) {
      mManualSamples += mManual ? 1 : 0;
      mMotion->ProcessMotion(x, y, z, accel[0] / kStandardGravity, accel[1] / kStandardGravity,
                             accel[2] / kStandardGravity, dt);
    }
    mMotion->GetCalibratedGyro(x, y, z);
  }
  float turn = -y;
  if (space == kLocalRoll) {
    turn = z;
  } else if (space == kLocalBoth) {
    const float dominant = std::fabs(y) > std::fabs(z) ? -y : z;
    turn = std::copysign(std::hypot(y, z), dominant);
  } else if (accel != nullptr && (space == kPlayer || space == kWorld)) {
    if (space == kPlayer) {
      mMotion->GetPlayerSpaceGyro(x, y);
    } else {
      mMotion->GetWorldSpaceGyro(x, y);
    }
    turn = -y;
  }
  pitch = x / kDegPerRad;
  yaw = turn / kDegPerRad;
}

void Filter::Reset() {
  mSamples = {};
  mNext = 0;
}

void Filter::Apply(float& pitch, float& yaw, float dt, float smoothDeg, float tightenDeg) {
  if (smoothDeg > 0.f && dt > 0.f) {
    const float speed = std::hypot(pitch, yaw) * kDegPerRad;
    const float direct = std::clamp((speed - smoothDeg) / smoothDeg, 0.f, 1.f);
    mSamples[mNext] = {pitch * (1.f - direct), yaw * (1.f - direct), dt};
    mNext = (mNext + 1) % int(mSamples.size());
    // Average the newest samples over the window, weighted by their time.
    float sumPitch = 0.f;
    float sumYaw = 0.f;
    float time = 0.f;
    for (int i = 1; i <= int(mSamples.size()) && time < kSmoothWindow; ++i) {
      const Sample& sample = mSamples[(mNext - i + int(mSamples.size())) % int(mSamples.size())];
      if (sample.dt <= 0.f) {
        break;
      }
      const float w = std::min(sample.dt, kSmoothWindow - time);
      sumPitch += sample.pitch * w;
      sumYaw += sample.yaw * w;
      time += w;
    }
    pitch = pitch * direct + sumPitch / time;
    yaw = yaw * direct + sumYaw / time;
  } else if (smoothDeg <= 0.f) {
    Reset();
  }
  if (tightenDeg > 0.f) {
    const float speed = std::hypot(pitch, yaw) * kDegPerRad;
    if (speed < tightenDeg) {
      pitch *= speed / tightenDeg;
      yaw *= speed / tightenDeg;
    }
  }
}

} // namespace PortGyro
