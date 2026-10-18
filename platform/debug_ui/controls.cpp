// F1 overlay: The Controls page. State and helpers shared with the other parts are in
// debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_debug.h"
#include "port_gyro_motion.h"
#include "port_pointer.h"
#include "port_apclient.h"
#include "port_controls.h"
#include "port_input_devices.h"
#include "port_mouse.h"

#include "MetroidPrime/Player/CGameOptions.hpp"
#include "MetroidPrime/Player/CGameState.hpp"

#include <aurora/imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL_gamepad.h>

#if defined(__ANDROID__)
#include "touch_pad.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>

namespace PortDebug {

void DrawControlsOptions() {
  const bool locked = BeginOriginalLocked();
  ImGui::SeparatorText("On-screen prompts");
  int glyphs = sPromptGlyphs.load(std::memory_order_relaxed);
  if (ImGui::Combo("Button glyphs", &glyphs,
                   "Follow the input in use\0" "Xbox\0" "PlayStation\0" "Nintendo Switch\0" "GameCube\0"
                   "Keyboard & mouse\0")) {
    sPromptGlyphs.store(glyphs, std::memory_order_relaxed);
    MarkDirty();
  }
  ItemHelp("Which buttons the game's prompts and tutorials show. Follow: the keyboard, controller or "
           "touch layout you last used. GameCube shows the game's own art.");

  ImGui::SeparatorText("Buttons");
  bool lockOnToggle = sInput.lockOnToggle;
  if (ImGui::Checkbox("Toggle Lock-On", &lockOnToggle)) {
    SetLockOnToggle(lockOnToggle);
  }
  ItemHelp("Press L once to lock on, scan or grapple, and again to let go. With nothing to lock "
           "on, L works as usual (strafe while held). The lock also lets go by itself when its "
           "target is gone.");
  bool stickyCharge = sInput.stickyCharge;
  if (ImGui::Checkbox("Sticky Charge", &stickyCharge)) {
    SetStickyCharge(stickyCharge);
  }
  ItemHelp("Taps fire as usual. Hold fire for a moment and let go, and the beam keeps charging; press "
           "fire again to shoot. Needs the Charge Beam.");
  bool rapidCharge = sInput.rapidCharge;
  if (ImGui::Checkbox("Remastered charge (rapid fire)", &rapidCharge)) {
    SetRapidCharge(rapidCharge);
  }
  ItemHelp("As in Metroid Prime Remastered: holding fire fires a few shots before charging (3 in all "
           "with Power, 2 with Wave or Plasma, 1 with Ice), then charges faster, so a full charge takes about as "
           "long as before. Without the Charge Beam it fires the same shots, then waits for you to let go.");
  bool swapScanXray = sInput.swapScanXray;
  if (ImGui::Checkbox("Swap the Scan and X-Ray visor buttons", &swapScanXray)) {
    SetSwapScanXray(swapScanXray);
  }
  ImGui::SetItemTooltip("Each takes the other's D-pad direction, as in Metroid Prime\n"
                        "Remastered's Dual Sticks layout. The Remastered controller preset\n"
                        "turns it on and the other presets off.");

  ImGui::SeparatorText("Aiming");
  if (ImGui::Checkbox("Auto-aim", &sAutoAim)) {
    MarkDirty();
  }
  ItemHelp("On, as the original: without Lock-On, the arm cannon turns toward the enemy nearest the "
           "middle of the screen and shots home in on it. Off, shots go where you aim; Lock-On still "
           "aims at its target.");
  bool pointerAim = sInput.pointerAim;
  if (ImGui::Checkbox("Pointer aim (Trilogy style)", &pointerAim)) {
    SetPointerAim(pointerAim);
  }
  ItemHelp("The mouse, the right stick (twin stick), gyro or a touch drag moves the crosshair over "
           "the screen and the shots follow it; the view turns only when the crosshair passes the "
           "dead box, faster the closer it gets to the edge. Lock-on, scanning and grappling still "
           "aim through the centre. Needs Mouse aim, Twin stick or the modern touch layout; Invert "
           "X/Y do not apply.");
  ImGui::BeginDisabled(!sInput.pointerAim);
  if (ImGui::SliderInt("Dead box", &sInput.pointerAimBox, PortInput::kPointerAimBoxMin, PortInput::kPointerAimBoxMax,
                       "%d%%")) {
    sInput.pointerAimBox = std::clamp(sInput.pointerAimBox, PortInput::kPointerAimBoxMin, PortInput::kPointerAimBoxMax);
    MarkDirty();
  }
  ImGui::SetItemTooltip("How far from the centre (percent of the half screen) the crosshair moves "
                        "before the view turns.");
  if (ImGui::SliderInt("Edge turn speed", &sInput.pointerAimTurn, PortInput::kPointerAimTurnMin,
                       PortInput::kPointerAimTurnMax, "%d deg/s")) {
    sInput.pointerAimTurn =
        std::clamp(sInput.pointerAimTurn, PortInput::kPointerAimTurnMin, PortInput::kPointerAimTurnMax);
    MarkDirty();
  }
  ImGui::SetItemTooltip("The turn speed with the crosshair at the screen edge.");
  ImGui::EndDisabled();

  ImGui::SeparatorText("Morph ball");
  bool fastMorph = sInput.fastMorph;
  if (ImGui::Checkbox("Fast Morph", &fastMorph)) {
    SetFastMorph(fastMorph);
  }
  ItemHelp("Morphing and unmorphing take a fraction of a second and keep your momentum, as in "
           "Metroid Prime 4. Unmorphing on the ground caps speed at walking speed; in the air the "
           "whole jump arc carries over.");
  const int springRule = PortAp::SpringBallRule();
  ImGui::BeginDisabled(springRule >= 0);
  bool springBall = sInput.springBall;
  if (ImGui::Checkbox("Spring Ball (C-stick up)", &springBall)) {
    SetSpringBall(springBall);
  }
  ImGui::EndDisabled();
  if (springRule >= 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("(set by the Archipelago seed: %s)", springRule == 0   ? "not received yet"
                                                             : springRule == 1 ? "with the Bombs"
                                                                               : "unlocked");
  } else {
    ItemHelp("A small jump in morph ball, as in Metroid Prime Trilogy, once the Morph Ball Bombs are "
             "held. Twin stick still passes the right stick up to it, and the beam shift (X in the "
             "Remastered preset) springs too.");
  }
  EndOriginalLocked(locked);

  // The game's own option, so it is the same switch as the pause menu's and is
  // saved with the other game options; not locked by Original experience.
  ImGui::SeparatorText("Rumble");
  ImGui::BeginDisabled(gpGameState == nullptr);
  bool rumble = gpGameState != nullptr && gpGameState->GameOptions().GetIsRumbleEnabled();
  if (ImGui::Checkbox("Rumble", &rumble) && gpGameState != nullptr) {
    gpGameState->GameOptions().SetIsRumbleEnabled(rumble);
  }
  ImGui::EndDisabled();
  ItemHelp("Controller vibration, the same switch as Rumble in the pause menu's options. A controller "
           "never rumbles while the keyboard or mouse was the last input used.");
}

void DrawControlsKeyboardMouse() {
  ImGui::SeparatorText("Mouse aim");
  bool mouseAim = sInput.mouseAim;
  if (ImGui::Checkbox("Mouse aim", &mouseAim)) {
    SetMouseAim(mouseAim);
    // Mouse aim replaces the R-button free look, so without twin-stick a pad
    // would have no aim at all. Both add into the same aim; twin-stick can
    // still be switched off on its own.
    if (mouseAim) SetTwinStick(true);
    MarkDirty();
  }
  ImGui::TextDisabled("Also switches on Twin stick, so a controller's right stick aims too.");
  // Shown on the Quake/Source/Apex scale (0.022 degrees a count per unit), stored in radians.
  float sourceSensitivity = PortMouse::SourceFromRadians(sInput.mouseSensitivity);
  if (ImGui::SliderFloat("Sensitivity", &sourceSensitivity, 0.05f, 30.f, "%.3f",
                         ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat)) {
    SetMouseSensitivity(PortMouse::RadiansFromSource(sourceSensitivity));
    MarkDirty();
  }
  ImGui::SetItemTooltip("The scale of Quake, Source and Apex Legends games (0.022 degrees a count "
                        "per unit): the same number turns the same. Ctrl+click to type a value.");
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.f);
  if (ImGui::InputInt("Mouse DPI", &sInput.mouseDpi, 0, 0)) {
    sInput.mouseDpi = std::clamp(sInput.mouseDpi, 100, 64000);
    MarkDirty();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%.1f cm (%.1f in) per 360 degrees",
                      PortMouse::CentimetresPer360(sInput.mouseSensitivity, float(sInput.mouseDpi)),
                      PortMouse::CentimetresPer360(sInput.mouseSensitivity, float(sInput.mouseDpi)) / 2.54f);
  ImGui::SetItemTooltip("Only for this readout; the game reads mouse counts.");
  if (ImGui::Checkbox("Invert X", &sInput.mouseInvertX)) {
    MarkDirty();
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Invert Y", &sInput.mouseInvertY)) {
    MarkDirty();
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Weapon buttons", &sInput.mouseButtons)) {
    sMouseButtonGate.Reset();
    MarkDirty();
  }
  ImGui::SetItemTooltip("Mouse buttons are set below and in Controls > Remap. Existing "
                        "keyboard/controller weapon bindings also work.");
  if (ImGui::Checkbox("Crosshair", &sInput.mouseCrosshair)) {
    MarkDirty();
  }
  ImGui::SetItemTooltip("A crosshair at the aim point while mouse aiming.");
  ImGui::SameLine();
  int crosshairSize = sInput.crosshairSize;
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.f);
  if (ImGui::SliderInt("Size", &crosshairSize, kCrosshairSizeMin, kCrosshairSizeMax, "%d%%")) {
    SetCrosshairSize(crosshairSize);
  }
  ImGui::SetItemTooltip("Applies under mouse aim and twin stick.");

  ImGui::SeparatorText("Mouse buttons");
  ImGui::TextDisabled("Mouse buttons are bound in Controls > Remap (Keyboard & mouse).");
}

void DrawControlsController() {
  ImGui::SeparatorText("Stick aim");
  bool twinStick = sInput.twinStick;
  if (ImGui::Checkbox("Twin stick (right stick aims)", &twinStick)) {
    SetTwinStick(twinStick);
    MarkDirty();
  }
  ItemHelp("Twin stick uses the right stick as a direct camera aim (the same path as the mouse) and "
           "consumes it, so it no longer free-looks. Fire stays on whatever is bound to A; remap it "
           "in Controls > Remap.");
  bool remasteredMovement = sInput.remasteredMovement;
  if (ImGui::Checkbox("Remastered movement (twin stick and mouse aim)", &remasteredMovement)) {
    SetRemasteredMovement(remasteredMovement);
  }
  ItemHelp("Moves as Metroid Prime Remastered and PrimeHack do: diagonals as fast as straight "
           "ahead, and a steady air drag, so a jump keeps a smooth arc. Off, jumps move as on the "
           "GameCube, whose speed dips at takeoff and picks up again near the top. Classic "
           "controls always move as on the GameCube.");
  // Shown only with twin stick on, as the pause menu's Stick Aim Speed row.
  if (sInput.twinStick) {
    float stickRate = sInput.stickAimRate;
    if (ImGui::SliderFloat("Stick aim speed", &stickRate, 100.f, 8100.f, "%.0f px/s",
                           ImGuiSliderFlags_Logarithmic)) {
      SetStickAimRate(stickRate);
    }
    ItemHelp(sInput.stickAimSplit
                 ? "How fast the right stick turns the camera left and right at full tilt. 900 px/s "
                   "(the default) is about 180 degrees a second at the default mouse sensitivity."
                 : "How fast the right stick turns the camera at full tilt. 900 px/s (the default) is "
                   "about 180 degrees a second at the default mouse sensitivity.");
    bool split = sInput.stickAimSplit;
    if (ImGui::Checkbox("Separate vertical speed", &split)) {
      sInput.stickAimSplit = split;
      MarkDirty();
    }
    ItemHelp("Gives looking up and down its own speed; Stick aim speed then sets only left and "
             "right.");
    if (sInput.stickAimSplit) {
      float stickRateY = sInput.stickAimRateY;
      if (ImGui::SliderFloat("Vertical stick aim speed", &stickRateY, 100.f, 8100.f, "%.0f px/s",
                             ImGuiSliderFlags_Logarithmic) &&
          std::isfinite(stickRateY) && stickRateY >= 50.f && stickRateY <= 10000.f) {
        sInput.stickAimRateY = stickRateY;
        MarkDirty();
      }
      ItemHelp("How fast the right stick turns the camera up and down at full tilt.");
    }
    // The game's own option, saved with its settings; free look uses it too.
    ImGui::BeginDisabled(gpGameState == nullptr);
    bool invertY = gpGameState != nullptr && gpGameState->GameOptions().GetInvertYAxis();
    if (ImGui::Checkbox("Invert stick aim Y (the game's Reverse Y Axis)", &invertY)) {
      gpGameState->GameOptions().SetInvertYAxis(invertY);
    }
    ImGui::EndDisabled();
  }
  const s32 controllerIndex = PADGetIndexForPort(PAD_CHAN0);
  if (SDL_Gamepad* pad = controllerIndex >= 0 ? PADGetSDLGamepadForIndex(static_cast< u32 >(controllerIndex)) : nullptr;
      pad != nullptr && SDL_IsJoystickVirtual(SDL_GetGamepadID(pad)) && !PortVr::OpenXRIsRunning()) {
    // Aurora ignores mappings on the virtual touch pad. PortVr: Gamepad mode's
    // "OpenXR Controllers" is virtual too, but Remap and the dead zones apply to it
    // while the game runs (Aurora saves no mapping for a virtual pad).
    ImGui::TextWrapped("The touch controls always use the GameCube layout; presets and remaps apply to "
                       "real controllers only.");
  } else {
    PortControls::DrawDeadZones();
    if (pad != nullptr && SDL_IsJoystickVirtual(SDL_GetGamepadID(pad))) {
      ImGui::TextDisabled("OpenXR Controllers: dead zones last until the game closes.");
    }
  }
}

void DrawControlsTouchGyro() {
#if defined(__ANDROID__)
  ImGui::SeparatorText("Touch controls");
  bool touchControls = sInput.touchControls.load();
  if (ImGui::Checkbox("On-screen controls", &touchControls)) {
    sInput.touchControls.store(touchControls);
    MarkDirty();
  }
  ItemHelp("Off, only the MENU and START buttons stay on screen, so a pad player's stray touches "
           "don't bring the controls back. The F1 menu is always reachable from MENU.");
  ImGui::BeginDisabled(!touchControls);
  if (ImGui::Checkbox("Coloured buttons", &sInput.touchColors)) {
    MarkDirty();
  }
  ItemHelp("Draws the on-screen buttons in the GameCube pad's colours: green A, red B, yellow "
           "C-stick, purple Z. Off, they are plain and see-through.");
  if (ImGui::Checkbox("Button descriptions", &sInput.touchLabels)) {
    MarkDirty();
  }
  ItemHelp("Writes what each on-screen button does next to its letter (Fire, Jump, Lock...). "
           "Off, only the letters are shown.");
  ImGui::BeginDisabled(sOriginalExperience);
  if (ImGui::Checkbox("Turbo fire button", &sInput.touchTurbo)) {
    if (!sInput.touchTurbo) {
      sTouchTurboFire.store(false, std::memory_order_release);
    }
    MarkDirty();
  }
  ImGui::EndDisabled();
  ItemHelp("Adds a Turbo button next to Fire: holding it fires as if Fire were tapped as fast as the "
           "game accepts. It can be moved and resized in Edit layout.");
  if (ImGui::Checkbox("Floating left stick", &sInput.touchFloatingStick)) {
    MarkDirty();
  }
  ItemHelp("Hides the left stick until a finger touches a free spot on the left half of the screen, "
           "then centres it under that finger. Another finger on the left half aims, like the rest "
           "of the free area. The map screen keeps the fixed stick.");
  int touchLayout = sInput.touchClassic ? 1 : sInput.touchTwinStick ? 2 : 0;
  static const char* const kTouchLayouts[] = {"Default", "Classic GameCube", "Twin stick (Remastered)"};
  if (ImGui::Combo("Layout", &touchLayout, kTouchLayouts, 3)) {
    SetTouchClassic(touchLayout == 1);
    SetTouchTwinStick(touchLayout == 2);
    if (touchLayout == 2) {
      // Remastered picks beams with Y + the D-pad, so twin starts with the D-pad, not the wheels.
      SetTouchWheels(false);
    }
  }
  ItemHelp("Default: the GameCube pad without a C-stick; drag the free screen area to aim like a "
           "mouse (the left stick strafes), and beams and visors come from the Visor and Beam "
           "wheels. Classic GameCube: brings back the C-stick and the D-pad. Twin stick "
           "(Remastered): a right stick that aims, with Remastered's Dual Sticks buttons (Jump, "
           "Fire, Morph, Missile, LT Lock) and a D-pad for visors; hold Y (Beam) and press the D-pad to pick "
           "a beam. The free-area drag stays on; the wheels can replace the D-pad.");
  ImGui::BeginDisabled(!sInput.touchClassic);
  bool touchAim = sInput.touchAim;
  if (ImGui::Checkbox("Touch aim", &touchAim)) {
    SetTouchAim(touchAim);
  }
  ItemHelp("Drag a finger on the free screen area. Classic layout with twin stick off: dragging "
           "sideways turns Samus, dragging up or down looks up or down like R free look and "
           "levels out when you let go. Otherwise the view aims like a mouse, by the distance "
           "dragged. Not while locked on or in the ball.");
  ImGui::EndDisabled();
  bool touchMapTap = sInput.touchMapTap;
  if (ImGui::Checkbox("Tap minimap for map", &touchMapTap)) {
    SetTouchMapTap(touchMapTap);
  }
  ItemHelp("Tapping the minimap opens the map; hides the GameCube layout's Z button.");
  ImGui::BeginDisabled(!sInput.touchClassic && !sInput.touchTwinStick);
  bool touchWheels = sInput.touchWheels;
  if (ImGui::Checkbox("Beam and visor wheels", &touchWheels)) {
    SetTouchWheels(touchWheels);
  }
  ItemHelp("Replaces the D-pad with a Visor and a Beam button. Hold one, slide to a sector, "
           "let go to pick. Letting go in the middle cancels. Off, the D-pad is back.");
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!((!sInput.touchClassic && !sInput.touchTwinStick) || sInput.touchWheels));
  bool touchVisorTapScan = sInput.touchVisorTapScan;
  if (ImGui::Checkbox("Tap Visor for Scan Visor", &touchVisorTapScan)) {
    SetTouchVisorTapScan(touchVisorTapScan);
  }
  ItemHelp("A quick tap on the Visor button (no slide) selects the Scan Visor.");
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!(!sInput.touchClassic || sInput.touchAim));
  float touchAimSpeed = sInput.touchAimSpeed;
  if (ImGui::SliderFloat("Touch aim speed", &touchAimSpeed, 0.5f, 6.f, "%.2f px/dp",
                         ImGuiSliderFlags_Logarithmic)) {
    SetTouchAimSpeed(touchAimSpeed);
  }
  ItemHelp("How far the view turns per dp of finger travel. The default turns about 180 degrees "
           "over a 400 dp drag at the default mouse sensitivity.");
  ImGui::EndDisabled();
  bool springSwipe = sInput.springSwipe;
  if (ImGui::Checkbox("Spring Ball on swipe up", &springSwipe)) {
    SetSpringBallSwipe(springSwipe);
  }
  ItemHelp("In morph ball, a quick swipe up on the free screen area (or a fast slide up from "
           "Fire or Jump) springs, without a gyro flick. Needs the Spring Ball option.");
  float touchOpacity = sInput.touchOpacity.load();
  if (ImGui::SliderFloat("Opacity", &touchOpacity, kTouchOpacityMin, kTouchOpacityMax, "%.0f%%")) {
    sInput.touchOpacity.store(touchOpacity);
    MarkDirty();
  }
  ItemHelp("How visible the on-screen controls are. Lower lets more of the game show through; "
           "the layout editor and the menu buttons stay fully visible.");
  float sideMargin = sInput.touchSideMargin.load();
  if (ImGui::SliderFloat("Side margin", &sideMargin, 0.f, kTouchMarginMaxDp, "%.0f dp")) {
    sInput.touchSideMargin.store(sideMargin);
    MarkDirty();
  }
  ItemHelp("Moves every on-screen control in from the left and right edges, for curved screen "
           "edges or a case.");
  float stickInset = sInput.touchStickInset.load();
  if (ImGui::SliderFloat("Stick inset", &stickInset, 0.f, kTouchMarginMaxDp, "%.0f dp")) {
    sInput.touchStickInset.store(stickInset);
    MarkDirty();
  }
  ItemHelp("Extra room between the left stick and the screen's left edge, on top of the side "
           "margin.");
  float buttonInset = sInput.touchButtonInset.load();
  if (ImGui::SliderFloat("Button inset", &buttonInset, 0.f, kTouchMarginMaxDp, "%.0f dp")) {
    sInput.touchButtonInset.store(buttonInset);
    MarkDirty();
  }
  ItemHelp("Extra room between the face buttons (and the classic C-stick) and the screen's right "
           "edge, on top of the side margin.");
  if (ImGui::Button("Reset margins")) {
    sInput.touchSideMargin.store(kTouchSideMarginDefault);
    sInput.touchStickInset.store(kTouchStickInsetDefault);
    sInput.touchButtonInset.store(kTouchButtonInsetDefault);
    MarkDirty();
  }
  ImGui::EndDisabled();
  if (ImGui::Button("Edit layout")) {
    // The touch view takes the request and opens its editor over the game.
    sTouchEditRequested.store(true, std::memory_order_release);
    RequestToggle();
  }
  ItemHelp("Closes this menu and lets you drag each on-screen control to where you want it and "
           "pinch it to resize. Done saves; Reset all puts everything back.");
#endif

  ImGui::SeparatorText("Gyro aim");
  const char* gyroModes[] = {"Off", "Hold to aim", "Always aim", "Hold to pause", "Toggle to aim"};
  int gyroMode = sInput.gyroMode;
  if (ImGui::Combo("Mode", &gyroMode, gyroModes, IM_ARRAYSIZE(gyroModes))) {
    SetGyroMode(gyroMode);
  }
  ItemHelp("Tilt the pad or the phone to aim. Hold to aim aims only while the Gyro aim button is "
           "held; hold to pause aims all the time except while it is held, so you can recentre the "
           "pad (ratcheting). Toggle to aim starts off: press once to aim, again to stop. "
           "The button is right stick click or left ctrl by default; change it on the Remap page. "
           "With toggle mode, leave the binding itself as a normal press. Needs mouse aim, twin stick "
           "or the touch controls "
           "(not the classic layout), since the gyro feeds that same aim.");
  ImGui::BeginDisabled(sInput.gyroMode == 0 && !sInput.springFlick);
  const char* gyroSources[] = {"Auto", "Controller", "Phone"};
  int gyroSource = sInput.gyroSource;
  if (ImGui::Combo("Source", &gyroSource, gyroSources, 3)) {
    SetGyroSource(gyroSource);
  }
  ImGui::Text("Gyro: %s", GyroStatus());
  ImGui::EndDisabled();
  ImGui::BeginDisabled(sInput.gyroMode == 0);
  // With pointer aim the gyro moves the pointer at its own speed; the camera
  // sensitivity then only matters if the gyro also turns the camera.
  const bool gyroPointer = PointerAim();
  if (gyroPointer) {
    float cursorAngle = sInput.gyroCursorAngle;
    if (ImGui::SliderFloat("Pointer speed##gyro", &cursorAngle, PortInput::kGyroCursorAngleMin,
                           PortInput::kGyroCursorAngleMax, "%.0f deg across the screen")) {
      sInput.gyroCursorAngle =
          std::clamp(cursorAngle, PortInput::kGyroCursorAngleMin, PortInput::kGyroCursorAngleMax);
      MarkDirty();
    }
    ItemHelp("Pointer aim is on, so the gyro moves the pointer like a mouse cursor: this is how far "
             "the pad turns to sweep the pointer from one side of the screen to the other. Smaller "
             "is faster. Independent of the mouse sensitivity.");
    bool cursorCamera = sInput.gyroCursorCamera;
    if (ImGui::Checkbox("Gyro also turns the camera", &cursorCamera)) {
      sInput.gyroCursorCamera = cursorCamera;
      MarkDirty();
    }
    ItemHelp("Turns the camera with the gyro as without pointer aim (at the Sensitivity below) while "
             "it also moves the pointer, so the view follows the pad and the pointer fine-tunes "
             "the aim.");
  }
  ImGui::BeginDisabled(gyroPointer && !sInput.gyroCursorCamera);
  float gyroSensitivity = sInput.gyroSensitivity;
  // The ## suffix keeps its ImGui id apart from the mouse Sensitivity slider.
  if (ImGui::SliderFloat("Sensitivity##gyro", &gyroSensitivity, PortInput::kGyroSensitivityMin,
                         PortInput::kGyroSensitivityMax, "%.2fx", ImGuiSliderFlags_Logarithmic)) {
    SetGyroSensitivity(gyroSensitivity);
  }
  ItemHelp("How far the camera turns per turn of the pad: at 1.00x turning the pad 90 degrees turns "
           "the camera 90 degrees, at 2.00x 180. Independent of the mouse sensitivity and invert. "
           "With pointer aim it applies only if the gyro also turns the camera.");
  ImGui::EndDisabled();
  // Listed by how common they are; the setting keeps PortGyro::Space's numbers.
  static constexpr int kSpaceOrder[] = {PortGyro::kPlayer, PortGyro::kWorld, PortGyro::kLocal, PortGyro::kLocalRoll,
                                        PortGyro::kLocalBoth};
  static constexpr const char* kSpaceNames[] = {"Player (turn)", "World (turn)", "Local yaw", "Local roll",
                                                "Local yaw and roll"};
  int spaceRow = 0;
  for (int i = 0; i < PortGyro::kSpaceCount; ++i) {
    if (kSpaceOrder[i] == sInput.gyroSpace) {
      spaceRow = i;
    }
  }
  if (ImGui::Combo("Space", &spaceRow, kSpaceNames, PortGyro::kSpaceCount)) {
    SetGyroSpace(kSpaceOrder[spaceRow]);
  }
  ItemHelp("Which turn of the pad turns the camera left and right. Player: turning the pad left "
           "and right, whether it is held flat or upright (the usual choice). World: turning about "
           "the real vertical only. Local yaw: the pad's own flat turn, best held flat. Local roll: "
           "the turn of a pad held upright (as a handheld). Local yaw and roll: both at once. Player "
           "and world need an accelerometer; without one they act as local yaw.");
  bool autoCalibrate = sInput.gyroAutoCalibrate;
  if (ImGui::Checkbox("Auto calibrate", &autoCalibrate)) {
    SetGyroAutoCalibrate(autoCalibrate);
  }
  ItemHelp("Measures the gyro's drift whenever the pad is still and removes it, so the camera "
           "doesn't creep. Put the pad down for a couple of seconds to calibrate. Needs an "
           "accelerometer.");
  ImGui::SameLine();
  if (ImGui::Button("Recalibrate")) {
    ResetGyroCalibration();
  }
  const float confidence = GyroCalibrationConfidence();
  if (confidence >= 0.f) {
    ImGui::Text("Calibration: %d%%", int(confidence * 100.f + 0.5f));
  } else {
    ImGui::TextDisabled("Calibration: no accelerometer");
  }
  float gyroSmoothing = sInput.gyroSmoothing;
  if (ImGui::SliderFloat("Smoothing", &gyroSmoothing, 0.f, PortInput::kGyroThresholdMax,
                         gyroSmoothing > 0.f ? "below %.0f deg/s" : "off")) {
    sInput.gyroSmoothing = std::clamp(gyroSmoothing, 0.f, PortInput::kGyroThresholdMax);
    MarkDirty();
  }
  ItemHelp("Averages slow turns of the pad over a few frames to hide sensor jitter; faster turns, "
           "above twice this speed, stay direct. Adds a little lag to fine aim. Off by default.");
  float gyroTightening = sInput.gyroTightening;
  if (ImGui::SliderFloat("Tightening", &gyroTightening, 0.f, PortInput::kGyroThresholdMax,
                         gyroTightening > 0.f ? "below %.0f deg/s" : "off")) {
    sInput.gyroTightening = std::clamp(gyroTightening, 0.f, PortInput::kGyroThresholdMax);
    MarkDirty();
  }
  ItemHelp("Turns slower than this are scaled down (the slower, the more), so a pad held still "
           "doesn't wobble the aim. Keep it low: high values make fine aim sluggish. Off by "
           "default.");
  ImGui::EndDisabled();

  bool springFlick = sInput.springFlick;
  if (ImGui::Checkbox("Spring Ball on gyro flick", &springFlick)) {
    SetSpringBallFlick(springFlick);
  }
  ItemHelp("Tilt the pad or phone up sharply to spring, like Trilogy's nunchuk flick. Uses the gyro "
           "source below; gyro aim can stay off. Raise the strength if it springs by accident.");
  ImGui::BeginDisabled(!sInput.springFlick);
  float flickRate = sInput.springFlickRate;
  if (ImGui::SliderFloat("Flick strength", &flickRate, 2.f, 20.f, "%.1f rad/s")) {
    SetSpringBallFlickRate(flickRate);
  }
  ImGui::EndDisabled();
}

} // namespace PortDebug
