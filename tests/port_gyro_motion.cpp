#include "port_gyro_motion.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>

namespace {

constexpr float kDt = 1.f / 60.f;
constexpr float kG = 9.80665f;

// Feeds a constant sample for some seconds and returns the last rates.
void Run(PortGyro::Motion& m, const float gyro[3], const float accel[3], float seconds, int space, bool autoCal,
         float& pitch, float& yaw) {
  for (int i = 0; i < int(seconds / kDt); ++i) {
    m.Process(gyro, accel, kDt, space, autoCal, pitch, yaw);
  }
}
} // namespace

int main() {
  {
    PortGyro::Activation activation;
    activation.Update(4, false);
    CHECK_MSG(!activation.Active(), "toggle starts off");
    activation.Update(4, true);
    CHECK_MSG(activation.Active(), "first press enables gyro");
    activation.Update(4, true);
    CHECK_MSG(activation.Active(), "held button does not repeat toggle");
    activation.Update(4, false);
    CHECK_MSG(activation.Active(), "release keeps gyro enabled");
    activation.Update(4, true);
    CHECK_MSG(!activation.Active(), "second press disables gyro");
    activation.Update(2, true);
    CHECK_MSG(activation.Active(), "always mode");
    activation.Update(4, true);
    CHECK_MSG(!activation.Active(), "mode change resets toggle despite held button");
    activation.Update(4, false);
    activation.Update(4, true);
    CHECK_MSG(activation.Active(), "fresh press after mode change enables gyro");
    activation.Update(0, true);
    CHECK_MSG(!activation.Active(), "off mode");
    activation.Update(1, true);
    CHECK_MSG(activation.Active(), "hold-to-aim held");
    activation.Update(1, false);
    CHECK_MSG(!activation.Active(), "hold-to-aim released");
    activation.Update(3, false);
    CHECK_MSG(activation.Active(), "hold-to-pause released");
    activation.Update(3, true);
    CHECK_MSG(!activation.Active(), "hold-to-pause held");
  }
  float pitch = 0.f;
  float yaw = 0.f;

  // No accelerometer: the raw rates pass through, a right turn (clockwise from
  // above, negative y) is positive yaw.
  {
    PortGyro::Motion m;
    const float gyro[3] = {0.5f, -1.f, 0.f};
    m.Process(gyro, nullptr, kDt, PortGyro::kPlayer, true, pitch, yaw);
    CHECK_MSG(pitch == 0.5f && yaw == 1.f, "no accel passes through");
  }

  // Held flat (the pad's face up) and turned right: the same yaw in every space
  // but local roll. Gravity starts unknown and settles over a few seconds.
  for (int space = 0; space < PortGyro::kSpaceCount; ++space) {
    if (space == PortGyro::kLocalRoll) {
      continue;
    }
    PortGyro::Motion m;
    const float still[3] = {0.f, 0.f, 0.f};
    const float flat[3] = {0.f, kG, 0.f};
    Run(m, still, flat, 10.f, space, false, pitch, yaw);
    const float turn[3] = {0.f, -1.f, 0.f};
    Run(m, turn, flat, 0.05f, space, false, pitch, yaw);
    CHECK_MSG(std::fabs(yaw - 1.f) < 0.05f && std::fabs(pitch) < 0.05f, "flat turn right");
  }

  // Far edge tilted up 90 degrees (the face toward the player, up is the pad's
  // -z): a turn right is about the pad's z, which local yaw ignores and every
  // other space reads as yaw.
  {
    const float still[3] = {0.f, 0.f, 0.f};
    const float upright[3] = {0.f, 0.f, -kG};
    const float turn[3] = {0.f, 0.f, 1.f};
    for (int space = 0; space < PortGyro::kSpaceCount; ++space) {
      PortGyro::Motion m;
      Run(m, still, upright, 10.f, space, false, pitch, yaw);
      Run(m, turn, upright, 0.05f, space, false, pitch, yaw);
      if (space == PortGyro::kLocal) {
        CHECK_MSG(std::fabs(yaw) < 0.05f, "upright local yaw ignores the turn");
      } else {
        CHECK_MSG(std::fabs(yaw - 1.f) < 0.1f, "upright turn right");
      }
    }
  }

  // A still pad with a drift: auto calibration learns it away, off keeps it.
  {
    const float drift[3] = {0.f, -0.03f, 0.f};
    const float flat[3] = {0.f, kG, 0.f};
    PortGyro::Motion cal;
    Run(cal, drift, flat, 8.f, PortGyro::kLocal, true, pitch, yaw);
    CHECK_MSG(std::fabs(yaw) < 0.003f, "auto calibration removes drift");
    CHECK_MSG(cal.Confidence() > 0.5f, "calibration confident");
    PortGyro::Motion raw;
    Run(raw, drift, flat, 8.f, PortGyro::kLocal, false, pitch, yaw);
    CHECK_MSG(std::fabs(yaw - 0.03f) < 0.001f, "no calibration keeps drift");
    // Turning calibration off keeps the learned offset.
    Run(cal, drift, flat, 0.1f, PortGyro::kLocal, false, pitch, yaw);
    CHECK_MSG(std::fabs(yaw) < 0.003f, "calibration off keeps offset");
    cal.Reset();
    Run(cal, drift, flat, 0.1f, PortGyro::kLocal, false, pitch, yaw);
    CHECK_MSG(std::fabs(yaw - 0.03f) < 0.001f, "reset forgets offset");
  }

  // Manual calibration averages the still pad, with auto calibration off or
  // paused, and keeps the result at full confidence.
  {
    const float drift[3] = {0.f, -0.03f, 0.f};
    const float flat[3] = {0.f, kG, 0.f};
    for (const bool autoCalibrate : {false, true}) {
      PortGyro::Motion m;
      m.BeginManualCalibration();
      CHECK_MSG(m.ManualCalibrating(), "manual calibration running");
      Run(m, drift, flat, 1.f, PortGyro::kLocal, autoCalibrate, pitch, yaw);
      CHECK_MSG(m.Confidence() < 0.01f, "auto calibration paused while manual samples");
      m.EndManualCalibration();
      CHECK_MSG(!m.ManualCalibrating() && m.Confidence() > 0.99f, "manual calibration done, confident");
      Run(m, drift, flat, 0.1f, PortGyro::kLocal, autoCalibrate, pitch, yaw);
      CHECK_MSG(std::fabs(yaw) < 0.001f, "manual calibration removes drift");
    }
    PortGyro::Motion idle;
    idle.BeginManualCalibration();
    idle.EndManualCalibration();
    CHECK_MSG(idle.Confidence() < 0.01f, "manual calibration without samples isn't trusted");
  }

  // Without an accelerometer the local spaces still apply, player/world act as
  // local yaw.
  {
    PortGyro::Motion m;
    const float roll[3] = {0.f, 0.f, 1.f};
    m.Process(roll, nullptr, kDt, PortGyro::kLocalRoll, true, pitch, yaw);
    CHECK_MSG(std::fabs(yaw - 1.f) < 1e-5f, "raw local roll");
    m.Process(roll, nullptr, kDt, PortGyro::kPlayer, true, pitch, yaw);
    CHECK_MSG(std::fabs(yaw) < 1e-5f, "raw player acts as local yaw");
    const float both[3] = {0.f, 0.6f, -0.8f};
    m.Process(both, nullptr, kDt, PortGyro::kLocalBoth, true, pitch, yaw);
    CHECK_MSG(std::fabs(yaw + 1.f) < 1e-5f, "yaw and roll: combined size, larger axis's sign");
  }

  // Tightening scales slow turns down and leaves fast ones alone.
  {
    constexpr float kRad = 1.f / 57.29577951308232f;
    PortGyro::Filter f;
    float p = 0.f;
    float y = 2.f * kRad;
    f.Apply(p, y, kDt, 0.f, 4.f);
    CHECK_MSG(std::fabs(y - kRad) < 1e-5f, "tightening halves a turn at half the threshold");
    y = 10.f * kRad;
    f.Apply(p, y, kDt, 0.f, 4.f);
    CHECK_MSG(std::fabs(y - 10.f * kRad) < 1e-6f, "tightening leaves fast turns");
  }

  // Smoothing averages slow turns over the window and passes fast ones through.
  {
    constexpr float kRad = 1.f / 57.29577951308232f;
    PortGyro::Filter f;
    float p = 0.f;
    float y = 0.f;
    // A slow jitter alternating +-2 deg/s averages to about 0.
    for (int i = 0; i < 60; ++i) {
      p = 0.f;
      y = (i % 2 == 0 ? 2.f : -2.f) * kRad;
      f.Apply(p, y, kDt, 5.f, 0.f);
    }
    CHECK_MSG(std::fabs(y) < 0.2f * kRad, "smoothing averages slow jitter");
    // A steady slow turn keeps its speed once the window fills.
    for (int i = 0; i < 60; ++i) {
      p = 0.f;
      y = 3.f * kRad;
      f.Apply(p, y, kDt, 5.f, 0.f);
    }
    CHECK_MSG(std::fabs(y - 3.f * kRad) < 1e-5f, "smoothing keeps a steady turn");
    // Above twice the threshold the turn is direct once the window's slow
    // samples have aged out.
    for (int i = 0; i < 10; ++i) {
      p = 0.f;
      y = 20.f * kRad;
      f.Apply(p, y, kDt, 5.f, 0.f);
    }
    CHECK_MSG(std::fabs(y - 20.f * kRad) < 1e-5f, "fast turn direct");
  }

  if (port_test::Failures() == 0) {
    std::printf("port_gyro_motion: ok\n");
  }
  return PORT_TEST_RESULT();
}
