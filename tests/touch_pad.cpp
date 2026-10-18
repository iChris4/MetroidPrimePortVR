// Drives a real SDL virtual gamepad through the port's own touch-pad code and
// asserts what the GAME would read back, not what the code wrote.
//
// The point is the mapping. The overlay writes raw SDL_Gamepad* indices into
// SDL_SetJoystickVirtualButton/Axis, and SDL turns those into logical controls
// using a mapping string. If the mapping is wrong the write still succeeds and
// the game still reads *something* - the right stick moves when A is pressed,
// Start does nothing - so nothing upstream can notice. Driving a real pad and
// reading it back through SDL_GetGamepadButton/Axis is the only thing that
// actually tests it.
//
// This code ran on Android only, inside #if defined(__ANDROID__), and had no
// test. That is why it is here.

#include "touch_pad.h"
#include "port_test.h"

#include <cstdio>
#include <string>

namespace {

const char* ButtonName(SDL_GamepadButton button) {
  switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "SOUTH(A)";
    case SDL_GAMEPAD_BUTTON_EAST: return "EAST(B)";
    case SDL_GAMEPAD_BUTTON_WEST: return "WEST(X)";
    case SDL_GAMEPAD_BUTTON_NORTH: return "NORTH(Y)";
    case SDL_GAMEPAD_BUTTON_BACK: return "BACK";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "GUIDE";
    case SDL_GAMEPAD_BUTTON_START: return "START";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "LSTICK";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "RSTICK";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LSHOULDER";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RSHOULDER";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "DUP";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "DDOWN";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "DLEFT";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "DRIGHT";
    default: return "?";
  }
}

// A stick rests at 0 and a trigger rests at the minimum, so "all axes at rest"
// is not one value. Getting this wrong is what made an earlier version of this
// test report cross-axis contamination that was not there: it left the previous
// axis at the minimum instead of at rest, then asserted the others read 0.
void RestAxes(SDL_Joystick* pad) {
  for (int raw = 0; raw < SDL_GAMEPAD_AXIS_COUNT; ++raw) {
    const SDL_GamepadAxis axis = static_cast< SDL_GamepadAxis >(raw);
    const bool trigger = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                         axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    SDL_SetJoystickVirtualAxis(pad, raw, PortTouchPad::AxisValue(trigger ? -1.f : 0.f));
  }
  SDL_UpdateJoysticks();
}

// SDL3 replaced SDL_GetNumJoysticks with a function that fills an id array and
// returns the count.
int AttachedCount() {
  int count = 0;
  SDL_GetJoysticks(&count);
  return count;
}

const char* AxisName(SDL_GamepadAxis axis) {
  switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX: return "LEFTX";
    case SDL_GAMEPAD_AXIS_LEFTY: return "LEFTY";
    case SDL_GAMEPAD_AXIS_RIGHTX: return "RIGHTX";
    case SDL_GAMEPAD_AXIS_RIGHTY: return "RIGHTY";
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "LTRIGGER";
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "RTRIGGER";
    default: return "?";
  }
}

} // namespace

int main() {
  // Joystick only: the virtual pad needs no window, and asking for video would
  // make this test depend on a display.
  if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK)) {
    std::printf("FAIL: SDL_InitSubSystem(JOYSTICK): %s\n", SDL_GetError());
    return 1;
  }
  PortTouchPad::Pad pad = PortTouchPad::Attach();
  if (!pad.ok()) {
    // Not a skip. SDL_AttachVirtualJoystick is compiled into the SDL this
    // project links on every platform, so a failure here means the feature is
    // gone, and a test that quietly skips is worse than no test.
    std::printf("FAIL: could not attach a virtual pad: %s\n", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    return 1;
  }
  CHECK_MSG(SDL_IsGamepad(pad.id), "the attached pad reports as a gamepad");

  SDL_Gamepad* gamepad = SDL_OpenGamepad(pad.id);
  CHECK_MSG(gamepad != nullptr, "the pad opens as a gamepad");
  if (gamepad == nullptr) {
    PortTouchPad::Detach(pad);
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    return 1;
  }

  // --- every button, by raw index, must come back as the same logical control
  // This is the mapping test. A wrong mapping shows up as some OTHER button
  // being down, and the message names both so a failure says what went where.
  for (int raw = 0; raw < SDL_GAMEPAD_BUTTON_COUNT; ++raw) {
    SDL_SetJoystickVirtualButton(pad.handle, raw, true);
    SDL_UpdateJoysticks();
    for (int logical = 0; logical < SDL_GAMEPAD_BUTTON_COUNT; ++logical) {
      const bool down = SDL_GetGamepadButton(gamepad, static_cast< SDL_GamepadButton >(logical));
      if (raw == logical) {
        CHECK_MSG(down, "every raw button index must reach the same logical control");
        if (!down)
          std::printf("      raw %d did not reach %s\n", raw, ButtonName(static_cast< SDL_GamepadButton >(logical)));
      } else if (down) {
        std::printf("FAIL: raw %d also brought up %s\n", raw, ButtonName(static_cast< SDL_GamepadButton >(logical)));
        port_test::CountFailure();
      }
    }
    SDL_SetJoystickVirtualButton(pad.handle, raw, false);
    SDL_UpdateJoysticks();
  }
  for (int logical = 0; logical < SDL_GAMEPAD_BUTTON_COUNT; ++logical)
    CHECK_MSG(!SDL_GetGamepadButton(gamepad, static_cast< SDL_GamepadButton >(logical)),
              "every button reads clear once released");

  // --- the axes, through the port's own conversion
  struct AxisCase {
    SDL_GamepadAxis axis;
    float left;
    float right;
    Sint16 expectedLeft;
    Sint16 expectedRight;
  };
  // A stick is centred at 0 and runs both ways. A trigger rests at the minimum
  // and rises to the maximum, which is why the conversion scales asymmetrically
  // and why a trigger release must send -1 rather than 0.
  const AxisCase cases[] = {
      {SDL_GAMEPAD_AXIS_LEFTX, -1.f, 1.f, -32768, 32767},
      {SDL_GAMEPAD_AXIS_LEFTY, -1.f, 1.f, -32768, 32767},
      {SDL_GAMEPAD_AXIS_RIGHTX, -1.f, 1.f, -32768, 32767},
      {SDL_GAMEPAD_AXIS_RIGHTY, -1.f, 1.f, -32768, 32767},
      {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -1.f, 1.f, -32768, 32767},
      {SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -1.f, 1.f, -32768, 32767},
  };
  for (const AxisCase& c : cases) {
    const int raw = static_cast< int >(c.axis);
    RestAxes(pad.handle);
    for (int logical = 0; logical < SDL_GAMEPAD_AXIS_COUNT; ++logical) {
      SDL_SetJoystickVirtualAxis(pad.handle, raw, PortTouchPad::AxisValue(c.left));
      SDL_UpdateJoysticks();
      const Sint16 value = SDL_GetGamepadAxis(gamepad, static_cast< SDL_GamepadAxis >(logical));
      if (raw == logical) {
        // SDL reports a stick axis as -32768..32767 and a trigger as 0..32767,
        // so only the stick's negative end is symmetric.
        const bool trigger = c.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                             c.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        const Sint16 want = c.expectedLeft < 0 && trigger ? 0 : c.expectedLeft;
        CHECK_MSG(value == want, "a full negative deflection must reach the axis minimum");
        if (value != want)
          std::printf("      %s at full negative: got %d, want %d\n", AxisName(c.axis), value, want);
      } else if (value != 0) {
        std::printf("FAIL: moving %s also moved %s (%d)\n", AxisName(c.axis),
                    AxisName(static_cast< SDL_GamepadAxis >(logical)), value);
        port_test::CountFailure();
      }
    }
    SDL_SetJoystickVirtualAxis(pad.handle, raw, PortTouchPad::AxisValue(c.right));
    SDL_UpdateJoysticks();
    CHECK_MSG(SDL_GetGamepadAxis(gamepad, c.axis) == 32767,
              "a full positive deflection must reach the axis maximum");
    RestAxes(pad.handle);
  }

  // --- partial deflection must not be truncated away, which is the bug the
  // conversion exists to fix: passing the float straight through made every
  // partial deflection 0, so the sticks read centred and the player could
  // neither move nor aim.
  RestAxes(pad.handle);
  SDL_SetJoystickVirtualAxis(pad.handle, static_cast< int >(SDL_GAMEPAD_AXIS_LEFTX),
                             PortTouchPad::AxisValue(0.5f));
  SDL_UpdateJoysticks();
  CHECK_MSG(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) == 16384,
            "half deflection must be a real half, not zero");
  SDL_SetJoystickVirtualAxis(pad.handle, static_cast< int >(SDL_GAMEPAD_AXIS_LEFTX),
                             PortTouchPad::AxisValue(-0.5f));
  SDL_UpdateJoysticks();
  CHECK_MSG(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) == -16384,
            "half deflection must be a real half in the negative direction too");
  SDL_SetJoystickVirtualAxis(pad.handle, static_cast< int >(SDL_GAMEPAD_AXIS_LEFTX),
                             PortTouchPad::AxisValue(0.f));
  SDL_UpdateJoysticks();
  CHECK_MSG(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) == 0,
            "a centred stick reads exactly zero, with no jitter floor");

  // --- out-of-range input is clamped, not wrapped
  RestAxes(pad.handle);
  SDL_SetJoystickVirtualAxis(pad.handle, static_cast< int >(SDL_GAMEPAD_AXIS_LEFTX),
                             PortTouchPad::AxisValue(4.f));
  SDL_UpdateJoysticks();
  CHECK_MSG(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) == 32767,
            "an over-range deflection clamps to the maximum");
  SDL_SetJoystickVirtualAxis(pad.handle, static_cast< int >(SDL_GAMEPAD_AXIS_LEFTX),
                             PortTouchPad::AxisValue(-4.f));
  SDL_UpdateJoysticks();
  CHECK_MSG(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) == -32768,
            "an under-range deflection clamps to the minimum");

  // --- detach must actually remove the device, or a second Attach would double up
  const int before = AttachedCount();
  PortTouchPad::Detach(pad);
  CHECK_MSG(!pad.ok(), "a detached pad is cleared");
  SDL_UpdateJoysticks();
  CHECK_MSG(AttachedCount() == before - 1, "detaching removes the device");

  PortTouchPad::Pad second = PortTouchPad::Attach();
  CHECK_MSG(second.ok(), "a pad can be attached again after detaching");
  SDL_UpdateJoysticks();
  CHECK_MSG(AttachedCount() == before, "reattaching restores the device count");
  PortTouchPad::Detach(second);
  // Detaching twice, or detaching a pad that never opened, must be harmless.
  PortTouchPad::Detach(second);
  PortTouchPad::Pad never;
  PortTouchPad::Detach(never);

  SDL_CloseGamepad(gamepad);
  SDL_QuitSubSystem(SDL_INIT_JOYSTICK);

  if (port_test::Failures() == 0) {
    std::printf("touch pad: mapping, conversion and cleanup all behave\n");
  }
  return PORT_TEST_RESULT();
}
