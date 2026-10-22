#pragma once

#include <array>
#include <memory>

class GamepadMotion;

namespace PortGyro {

// Tracks the Gyro aim action once per input frame. A mode change starts toggle
// mode off and requires a fresh press, even if the button is already held.
class Activation {
public:
  void Update(int mode, bool held) {
    if (mode != mMode) {
      mMode = mode;
      mToggled = false;
    } else if (mode == 4 && held && !mHeld) {
      mToggled = !mToggled;
    }
    mHeld = held;
  }

  bool Active() const {
    switch (mMode) {
    case 1: return mHeld;
    case 2: return true;
    case 3: return !mHeld;
    case 4: return mToggled;
    default: return false;
    }
  }

private:
  int mMode = 0;
  bool mHeld = false;
  bool mToggled = false;
};

// Gyro space: which turn of the pad steers yaw. Local spaces use the pad's own
// axes: yaw (held flat), roll (held upright), or both, the larger one's sign
// with their combined size (as Overwatch). Player and world turn about gravity.
enum Space { kLocal = 0, kPlayer = 1, kWorld = 2, kLocalRoll = 3, kLocalBoth = 4, kSpaceCount = 5 };

// Turns raw motion samples into aim rates, through GamepadMotionHelpers for
// drift calibration and gravity (player/world space). Inputs use SDL's sensor
// axes (x right, y up, z toward the player; rad/s and m/s^2, gravity included);
// outputs are the port's pitch (positive tilts up) and yaw (positive turns
// right) in rad/s. No SDL here, so it can be tested on its own.
class Motion {
public:
  Motion();
  ~Motion();

  // accel may be null (a source without an accelerometer): then the raw local
  // rates are used, with no calibration or gravity (player/world act as local yaw).
  // Turning autoCalibrate off stops the learning but keeps the drift learned so far.
  void Process(const float gyro[3], const float* accel, float dt, int space, bool autoCalibrate,
               float& pitch, float& yaw);
  // Forget orientation and calibration (a new source, or a debug reset).
  void Reset();
  // Manual calibration: Begin drops the learned drift and averages every sample
  // until End, with auto calibration paused in between; End keeps the average
  // (at full confidence) and lets auto calibration carry on from it.
  void BeginManualCalibration();
  void EndManualCalibration();
  bool ManualCalibrating() const { return mManual; }
  // How sure the drift calibration is, 0..1, and whether the pad was last seen still.
  float Confidence() const;
  bool Steady() const;

private:
  std::unique_ptr< GamepadMotion > mMotion;
  bool mAutoCalibrate = false;
  bool mHasMode = false;
  bool mManual = false;
  int mManualSamples = 0;
};

// Soft tiered smoothing and tightening (as in JoyShockMapper): slow turns,
// below smoothDeg deg/s, are averaged over a short window to hide sensor noise,
// fading to the direct rate by twice that; turns below tightenDeg deg/s are
// scaled down by speed/tightenDeg, so a pad held still stays still. 0 turns
// either off. Rates are the port's pitch/yaw in rad/s.
class Filter {
public:
  void Apply(float& pitch, float& yaw, float dt, float smoothDeg, float tightenDeg);
  void Reset();

private:
  struct Sample {
    float pitch;
    float yaw;
    float dt;
  };
  // Enough for the whole window up to ~2000 fps (uncapped frames).
  std::array< Sample, 256 > mSamples{};
  int mNext = 0;
};

} // namespace PortGyro
