#include "port_input_settings.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace {

std::string Text() {
  std::ostringstream out;
  PortInput::WriteSettings(out);
  return out.str();
}
} // namespace

int main() {
  using namespace PortInput;

  // Defaults, and the file they write.
  const std::string defaults = Text();
  REQUIRE_MSG(!Get().mouseAim && !Get().twinStick && Get().remasteredMovement, "defaults");
  REQUIRE_MSG(Get().springBall && defaults.find("spring_ball=1\n") != std::string::npos,
        "PrimedGun: Spring Ball on by default");
  REQUIRE_MSG(Get().shiftBindings[0] == kScancodeLShift && Get().shiftBindings[1] == -1, "default shift");
  REQUIRE_MSG(defaults.rfind("mouse_aim=0\ntwin_stick=0\n", 0) == 0, "first lines");
  REQUIRE_MSG(defaults.find("pad_alt=-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1\n") != std::string::npos, "pad_alt");
  REQUIRE_MSG(defaults.find("mouse_left=") != std::string::npos && defaults.find("mouse_x2=") != std::string::npos,
        "mouse action keys");

  // Keys that aren't input keys are left to the caller.
  REQUIRE_MSG(!ApplySetting("fov", "90") && !ApplySetting("game_options", "00") && !ApplySetting("nonsense", "1"),
              "foreign keys");

  // Toggles, ranges and rejects.
  REQUIRE_MSG(ApplySetting("mouse_aim", "1") && Get().mouseAim, "mouse_aim");
  REQUIRE_MSG(ApplySetting("stick_aim_rate", "2000") && Get().stickAimRate == 2000.f, "stick_aim_rate");
  ApplySetting("stick_aim_rate", "10");
  REQUIRE_MSG(Get().stickAimRate == 2000.f, "stick_aim_rate below range");
  ApplySetting("stick_aim_rate", "20000");
  REQUIRE_MSG(Get().stickAimRate == 2000.f, "stick_aim_rate above range");
  REQUIRE_MSG(ApplySetting("stick_aim_split", "1") && Get().stickAimSplit, "stick_aim_split");
  REQUIRE_MSG(ApplySetting("stick_aim_rate_y", "450") && Get().stickAimRateY == 450.f, "stick_aim_rate_y");
  ApplySetting("stick_aim_rate_y", "20000");
  REQUIRE_MSG(Get().stickAimRateY == 450.f, "stick_aim_rate_y above range");
  ApplySetting("stick_aim_split", "0");
  ApplySetting("crosshair_size", "80");
  REQUIRE_MSG(Get().crosshairSize == 80, "crosshair_size");
  ApplySetting("crosshair_size", "5");
  REQUIRE_MSG(Get().crosshairSize == 80, "crosshair_size range");
  ApplySetting("touch_side_margin", "40");
  REQUIRE_MSG(Get().touchSideMargin.load() == 40.f, "touch_side_margin");
  ApplySetting("touch_side_margin", "400");
  REQUIRE_MSG(Get().touchSideMargin.load() == 40.f, "touch_side_margin range");
  REQUIRE_MSG(Get().touchControls.load(), "touch_controls defaults on");
  ApplySetting("touch_controls", "0");
  REQUIRE_MSG(!Get().touchControls.load(), "touch_controls off");
  ApplySetting("touch_controls", "1");
  REQUIRE_MSG(Get().touchControls.load(), "touch_controls on");
  ApplySetting("touch_opacity", "50");
  REQUIRE_MSG(Get().touchOpacity.load() == 50.f, "touch_opacity");
  ApplySetting("touch_opacity", "400");
  REQUIRE_MSG(Get().touchOpacity.load() == 50.f, "touch_opacity above range");
  ApplySetting("touch_opacity", "0");
  REQUIRE_MSG(Get().touchOpacity.load() == 50.f, "touch_opacity below range");
  ApplySetting("gyro_mode", "2");
  ApplySetting("gyro_mode", "3");
  ApplySetting("gyro_mode", "4");
  REQUIRE_MSG(Get().gyroMode == 4, "gyro toggle mode");
  ApplySetting("gyro_mode", "5");
  REQUIRE_MSG(Get().gyroMode == 4, "gyro_mode range");
  REQUIRE_MSG(Get().gyroSensitivity == 2.f, "gyro_sensitivity default");
  ApplySetting("gyro_sensitivity", "1.5");
  ApplySetting("gyro_sensitivity", "50");
  REQUIRE_MSG(Get().gyroSensitivity == 1.5f, "gyro_sensitivity range");
  PostLoad();
  // An old gyro_rate (pixels per rad/s at mouse_sensitivity rad per pixel) becomes
  // the same turn on the natural scale, unless gyro_sensitivity is also there.
  ApplySetting("mouse_sensitivity", "0.0035");
  ApplySetting("gyro_rate", "600");
  PostLoad();
  REQUIRE_MSG(std::fabs(Get().gyroSensitivity - 2.1f) < 1e-4f, "gyro_rate migrates");
  ApplySetting("gyro_rate", "600");
  ApplySetting("gyro_sensitivity", "1.5");
  PostLoad();
  REQUIRE_MSG(Get().gyroSensitivity == 1.5f, "gyro_sensitivity wins over gyro_rate");
  REQUIRE_MSG(Text().find("gyro_rate") == std::string::npos, "gyro_rate not written");
  REQUIRE_MSG(Get().gyroSpace == 1 && Get().gyroAutoCalibrate, "gyro space/calibration defaults");
  ApplySetting("gyro_space", "2");
  ApplySetting("gyro_space", "5");
  ApplySetting("gyro_auto_calibrate", "0");
  REQUIRE_MSG(Get().gyroSpace == 2 && !Get().gyroAutoCalibrate, "gyro space/calibration");
  REQUIRE_MSG(Text().find("gyro_space=2\n") != std::string::npos &&
                  Text().find("gyro_auto_calibrate=0\n") != std::string::npos,
              "gyro space/calibration written");
  REQUIRE_MSG(Get().gyroSmoothing == 0.f && Get().gyroTightening == 0.f, "gyro filter defaults off");
  ApplySetting("gyro_smoothing", "5");
  ApplySetting("gyro_tightening", "3");
  ApplySetting("gyro_tightening", "51");
  REQUIRE_MSG(Get().gyroSmoothing == 5.f && Get().gyroTightening == 3.f, "gyro filter");
  REQUIRE_MSG(Text().find("gyro_smoothing=5\n") != std::string::npos &&
                  Text().find("gyro_tightening=3\n") != std::string::npos,
              "gyro filter written");
  REQUIRE_MSG(Get().gyroCursorAngle == PortInput::kGyroCursorAngleDefault && !Get().gyroCursorCamera,
              "gyro cursor defaults");
  ApplySetting("gyro_cursor_angle", "60");
  ApplySetting("gyro_cursor_angle", "200");
  ApplySetting("gyro_cursor_camera", "1");
  REQUIRE_MSG(Get().gyroCursorAngle == 60.f && Get().gyroCursorCamera, "gyro cursor");
  REQUIRE_MSG(Text().find("gyro_cursor_angle=60\n") != std::string::npos &&
                  Text().find("gyro_cursor_camera=1\n") != std::string::npos,
              "gyro cursor written");
  ApplySetting("shift_key_alt", "44");
  ApplySetting("turbo_pad", "3");
  REQUIRE_MSG(Get().shiftBindings[1] == 44 && Get().turboBindings[2] == 3, "bindings");
  ApplySetting("mouse_right", "lock_on");
  const std::string before = Text();
  ApplySetting("mouse_right", "not_an_action");
  REQUIRE_MSG(Text() == before, "unknown mouse action ignored");

  // Touch layout.
  REQUIRE_MSG(ApplySetting("touch_layout", "1:0.5,0.5,1") && TouchLayout() == "1:0.5,0.5,1", "touch_layout");
  REQUIRE_MSG(!SetTouchLayout(std::string(kTouchLayoutMaxLen + 1, 'x')), "touch layout length");
  REQUIRE_MSG(TouchLayout() == "1:0.5,0.5,1", "touch layout kept");

  // Classic GameCube wins over twin stick on load.
  ApplySetting("touch_twin_stick", "1");
  ApplySetting("touch_classic_gc", "1");
  PostLoad();
  REQUIRE_MSG(Get().touchClassic && !Get().touchTwinStick, "classic wins");

  // Write then read back reproduces the same text.
  const std::string saved = Text();
  ApplySetting("mouse_aim", "0");
  ApplySetting("stick_aim_rate", "300");
  ApplySetting("touch_side_margin", "10");
  ApplySetting("shift_key", "5");
  ApplySetting("touch_classic_gc", "0");
  SetTouchLayout("");
  REQUIRE_MSG(Text() != saved, "state changed before the read-back");
  std::istringstream in(saved);
  for (std::string line; std::getline(in, line);) {
    const size_t eq = line.find('=');
    REQUIRE_MSG(eq != std::string::npos, "line has =");
    REQUIRE_MSG(ApplySetting(line.substr(0, eq), line.substr(eq + 1)) || line.compare(0, 12, "touch_layout") == 0,
                "every written key is read");
  }
  PostLoad();
  REQUIRE_MSG(Text() == saved, "round trip");
  return 0;
}
