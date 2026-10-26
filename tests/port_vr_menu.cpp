// PrimedGun's VR menu (platform/include/vr/vr_menu.h and the controls in
// openxr_settings_panel.h): the image keeps PrimedGun's pixels and byte order,
// clicks land where PrimedGun's hit boxes put them, the two-press actions
// expire, and the panel, laser and hit dot hang where PrimedGun put them. The
// port's own CONFIG and DEBUG tabs fit the strip and change what they show.

#include "vr/openxr_settings_panel.h"
#include "vr/vr_menu.h"
#include "vr/virtual_screen_anchor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using namespace PortVr;
using namespace PortVr::VrMenu;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "vr menu regression failed: %s\n", what);
    std::abort();
  }
}

bool Near(float a, float b, float tolerance = 1.0e-3f) { return std::fabs(a - b) <= tolerance; }

bool Near(const Vec3& a, const Vec3& b, float tolerance = 1.0e-3f) {
  return Near(a[0], b[0], tolerance) && Near(a[1], b[1], tolerance) && Near(a[2], b[2], tolerance);
}

Quat AxisAngle(const Vec3& axis, float degrees) {
  const float half = degrees * 3.14159265f / 360.0f;
  const float s = std::sin(half);
  return {axis[0] * s, axis[1] * s, axis[2] * s, std::cos(half)};
}

uint32_t At(const Pixels& pixels, int x, int y) { return pixels[static_cast<size_t>(y) * kWidth + x]; }

class RecordingActions final : public Actions {
public:
  void SaveSettings() override { ++saves; }
  void ExitGame() override { ++exits; }
  int StateSlot() const override { return slot; }
  void SelectStateSlot(int s) override { slot = s; }
  void LoadState(int s) override { loaded = s; }
  void SaveState(int s) override { saved = s; }
  void LoadNewestState() override { ++newest; }
  void SaveOldestState() override { ++oldest; }
  bool ApplyCannonSlot(int s) override {
    cannon = s;
    return true;
  }
  void FullHealth() override { ++healed; }
  void GrantEverything() override { ++granted; }
  void SetInvulnerable(bool on) override { invulnerable = on ? 1 : 0; }
  void SetStreamedAudio(bool on) override { streamed = on ? 1 : 0; }
  void SetMusyxAudio(bool on) override { musyx = on ? 1 : 0; }
  void SetLogFile(bool on) override { log = on ? 1 : 0; }
  void SetQuickResume(bool on) override { quickResume = on ? 1 : 0; }
  void SetDrawTags(bool on) override { tags = on ? 1 : 0; }

  int saves = 0;
  int exits = 0;
  int slot = 1;
  int loaded = 0;
  int saved = 0;
  int newest = 0;
  int oldest = 0;
  int cannon = -1;
  int healed = 0;
  int granted = 0;
  int invulnerable = -1;
  int streamed = -1;
  int musyx = -1;
  int log = -1;
  int tags = -1;
  int quickResume = -1;
};

// The middle of row `index`'s hit band, and of its value box's halves.
float RowY(uint32_t tab, uint32_t index) { return static_cast<float>(RowTextY(tab, index)) + 4.0f; }
constexpr float kLabelX = 200.0f;
constexpr float kMinusX = 800.0f;
constexpr float kPlusX = 920.0f;
// Inside a tab, and its label's middle.
float TabX(uint32_t tab) { return static_cast<float>(TabRect(tab).x) + 10.0f; }
constexpr float kTabY = 80.0f;
const float kPageY = static_cast<float>(kPageButtonY) + 14.0f;
const float kPreviousX = static_cast<float>(kPreviousButtonX) + 100.0f;
const float kNextX = static_cast<float>(kNextButtonX) + 100.0f;

void TestImage() {
  const State state{};
  const PortVrSettings settings{};
  const Pixels pixels = BuildPixels(state, settings, View{}, false);
  Check(pixels.size() == static_cast<size_t>(kWidth) * kImageHeight, "image size");
  Check(At(pixels, 2, 250) == 0xD0100804u, "panel background");
  Check(At(pixels, 500, 5) == 0xE0FFB030u && At(pixels, 500, 507) == 0xE0FFB030u, "top and bottom bars");
  // The bytes are R, G, B, A: PrimedGun's accent is #30B0FF at 224.
  uint8_t bytes[4];
  const uint32_t bar = At(pixels, 500, 5);
  std::memcpy(bytes, &bar, sizeof(bytes));
  Check(bytes[0] == 0x30 && bytes[1] == 0xB0 && bytes[2] == 0xFF && bytes[3] == 0xE0, "RGBA byte order");
  // 'P' of the title, scale 4, top-left cell lit.
  Check(At(pixels, 49, 29) == 0xFFFFD8A0u, "title text");
  // LAYOUT is the active tab: bright underline; the next one's is dim.
  Check(At(pixels, TabRect(0).x + 5, 100) == 0xFFFFB030u && At(pixels, TabRect(1).x + 5, 100) == 0x604A2C12u,
        "tab underlines");
  // The layout page's help line in PrimedGun's lime.
  bool lime = false;
  for (int x = 80; x < 950 && !lime; ++x) {
    lime = At(pixels, x, 175) == 0xFF40F0E0u;
  }
  Check(lime, "layout help line");
  // The sprite strip: nothing between the canvas and it, the laser block and the dot.
  Check(At(pixels, 1, 515) == 0u, "gap below the canvas");
  Check(At(pixels, kLaserRect.x, kLaserRect.y) == kPointerColor &&
            At(pixels, kLaserRect.x + kLaserRect.w - 1, kLaserRect.y + kLaserRect.h - 1) == kPointerColor,
        "laser block");
  Check(At(pixels, kDotRect.x + 12, kDotRect.y + 12) == kPointerColor && At(pixels, kDotRect.x, kDotRect.y) == 0u,
        "hit dot");

  // A settings tab: the selected row's accent is bright, the others dim, and
  // a numeric row has its -/+.
  State calibration{};
  calibration.tab = kCalibrationTab;
  calibration.selected = 2;
  const Pixels rows = BuildPixels(calibration, settings, View{}, false);
  Check(At(rows, 54, RowTextY(kCalibrationTab, 2)) == 0xFFFFB030u, "selected row accent");
  Check(At(rows, 54, RowTextY(kCalibrationTab, 1) - 4) == 0x80FFB030u, "unselected row accent");
  Check(At(rows, 790 + 2, RowTextY(kCalibrationTab, 2) + 6) == 0xFFFFB030u, "minus sign");
  Check(ViewKey(calibration, settings, View{}, false) != ViewKey(calibration, settings, View{}, true), "notice in the key");
  // Page 1 has NEXT and no PREVIOUS; page 2 the other way round.
  const auto accent = [](const Pixels& p, int button_x) { return At(p, button_x + 1, kPageButtonY + 10); };
  Check(accent(rows, kNextButtonX) == 0x80FFB030u && accent(rows, kPreviousButtonX) == 0xD0100804u,
        "page 1 buttons");
  calibration.calibration_page = 1;
  const Pixels second = BuildPixels(calibration, settings, View{}, false);
  Check(accent(second, kPreviousButtonX) == 0x80FFB030u && accent(second, kNextButtonX) == 0xD0100804u,
        "page 2 buttons");
  Check(ViewKey(calibration, settings, View{}, false) != ViewKey(State{calibration.tab}, settings, View{}, false),
        "page in the key");
  // Tabs without pages have no buttons.
  State movement{};
  movement.tab = kMovementTab;
  Check(accent(BuildPixels(movement, settings, View{}, false), kNextButtonX) == 0xD0100804u, "no buttons without pages");
}

void TestRows() {
  State s{};
  Check(ItemCount(s, View{}) == 0, "layout has no rows");
  s.tab = kCalibrationTab;
  Check(ItemCount(s, View{}) == 12, "calibration page 1");
  s.calibration_page = 1;
  Check(ItemCount(s, View{}) == 12, "calibration page 2");
  s.tab = kControlTab;
  Check(ItemCount(s, View{}) == 8, "control page 1");
  s.control_page = 1;
  Check(ItemCount(s, View{}) == 8, "control page 2");
  s.tab = kMovementTab;
  Check(ItemCount(s, View{}) == 11, "movement");
  s.tab = kTexturesTab;
  Check(ItemCount(s, View{}) == 6, "textures");
  s.tab = kStatesTab;
  Check(ItemCount(s, View{}) == 12, "states");
  Check(RowTextY(kStatesTab, 4) == 146 + 4 * 22 + 18, "state slots sit lower");
  Check(RowFromTextureY(kCalibrationTab, 160.0f, 13) == 1 && RowFromTextureY(kCalibrationTab, 100.0f, 13) == -1,
        "row hit band");
  const PortVrSettings settings{};
  State calibration{};
  calibration.tab = kCalibrationTab;
  const auto rows = BuildRows(calibration, settings, View{});
  Check(rows.size() == 12 && std::strcmp(rows[0].label, "CUTSCENE CINEMA SCREEN") == 0 &&
            std::strcmp(rows[10].label, "CULLING CONE") == 0 && rows[10].value == "115.00",
        "PrimedGun's rows without the PAGE row, and its culling cone text");
  Check(SnapTurnStep(45, 1) == 60 && SnapTurnStep(90, 1) == 90 && SnapTurnStep(30, -1) == 30, "snap turn steps");
}

void TestClicks() {
  RecordingActions actions;
  PortVrSettings v{};
  const View view{};
  State s{};
  Open(s);
  double now = 100.0;

  // Tabs: inside a tab switches; the gap between two does not.
  Click(s, v, view, TabX(kControlTab), kTabY, now, actions);
  Check(s.tab == kControlTab && s.selected == 0, "tab click");
  Click(s, v, view, static_cast<float>(TabRect(0).x + TabRect(0).w) + 4.0f, kTabY, now, actions);
  Check(s.tab == kControlTab, "tab gap ignored");
  Click(s, v, view, TabX(kCalibrationTab), kTabY, now, actions);
  Check(s.tab == kCalibrationTab, "calibration tab");

  // Numeric: right half up, left half down.
  Click(s, v, view, kPlusX, RowY(kCalibrationTab, 1), now, actions);
  Check(s.selected == 1 && Near(v.metroid_hud_distance, 0.15f), "HUD distance up");
  Click(s, v, view, kMinusX, RowY(kCalibrationTab, 1), now, actions);
  Check(Near(v.metroid_hud_distance, 0.10f), "HUD distance down");
  // Toggle anywhere else on the row.
  Click(s, v, view, kLabelX, RowY(kCalibrationTab, 0), now, actions);
  Check(!v.cinematic_screen_enabled, "cinema screen toggle");
  // HUD VERTICAL keeps up and down apart.
  Click(s, v, view, kMinusX, RowY(kCalibrationTab, 3), now, actions);
  Check(Near(v.metroid_hud_offset_down, 0.01f) && Near(v.metroid_hud_offset_up, 0.0f), "HUD vertical split");

  // Pages: PREVIOUS does nothing on page 1, NEXT turns to page 2 (and lets go
  // of a pending reset), NEXT does nothing there.
  Click(s, v, view, kPreviousX, kPageY, now, actions);
  Check(s.calibration_page == 0, "no page before the first");
  Click(s, v, view, kLabelX, RowY(kCalibrationTab, 11), now, actions);
  Check(s.reset_confirm == kResetTargeting, "reset targeting armed");
  Click(s, v, view, kNextX, kPageY, now, actions);
  Check(s.calibration_page == 1 && s.reset_confirm == kNoReset, "next page");
  Click(s, v, view, kNextX, kPageY, now, actions);
  Check(s.calibration_page == 1, "no page after the last");
  Click(s, v, view, 512.0f, kPageY, now, actions);
  Check(s.calibration_page == 1, "the page number is not a button");
  // SAMUS ARM PRESET is on page 2.
  Click(s, v, view, kLabelX, RowY(kCalibrationTab, 9), now, actions);
  Check(Near(v.model_offset_y, -0.3f) && Near(v.rot_offset_y, 20.0f) && Near(v.rot_offset_z, -90.0f),
        "Samus arm preset");

  // RESET CALIBRATION takes two clicks within six seconds.
  Click(s, v, view, kLabelX, RowY(kCalibrationTab, 7), now, actions);
  Check(s.reset_confirm == kResetCalibration && Near(v.rot_offset_z, -90.0f), "reset armed");
  now += 7.0;
  Refresh(s, now);
  Check(s.reset_confirm == kNoReset, "confirmation expires");
  Click(s, v, view, kLabelX, RowY(kCalibrationTab, 7), now, actions);
  Click(s, v, view, kLabelX, RowY(kCalibrationTab, 7), now + 1.0, actions);
  Check(s.reset_confirm == kNoReset && Near(v.rot_offset_z, 0.0f) && Near(v.model_offset_y, 0.0f),
        "reset confirmed");
  // DETACH VR MENU FROM HAND.
  Click(s, v, view, kLabelX, RowY(kCalibrationTab, 10), now, actions);
  Check(v.vr_menu_floating, "detach toggle");
  // PREVIOUS goes back to page 1.
  Click(s, v, view, kPreviousX, kPageY, now, actions);
  Check(s.calibration_page == 0, "previous page");

  // SAVE SETTINGS shows the notice; RESET ALL keeps the renderer's settings.
  Click(s, v, view, 150.0f, 120.0f, now, actions);
  Check(actions.saves == 1 && s.saved_notice_until > now, "save settings");
  v.render_scale = 1.5f;
  v.metroid_hud_size = 2.0f;
  v.patch_cannon_rotation = false;
  Click(s, v, view, 400.0f, 120.0f, now, actions);
  Check(Near(v.metroid_hud_size, 2.0f), "reset all armed");
  Click(s, v, view, 400.0f, 120.0f, now, actions);
  Check(Near(v.metroid_hud_size, 0.75f) && Near(v.render_scale, 1.5f) && !v.patch_cannon_rotation &&
            !v.vr_menu_floating,
        "reset all");

  // Control: RUMBLE TARGET cycles SPLIT -> BOTH -> LEFT -> RIGHT.
  Click(s, v, view, TabX(kControlTab), kTabY, now, actions);
  Check(v.rumble_hand == RumbleHand::Split, "rumble default");
  Click(s, v, view, kLabelX, RowY(kControlTab, 2), now, actions);
  Check(v.rumble_hand == RumbleHand::Both, "rumble cycles");
  v.rumble_hand = RumbleHand::Right;
  Click(s, v, view, kLabelX, RowY(kControlTab, 2), now, actions);
  Check(v.rumble_hand == RumbleHand::Split, "rumble cycles back to split");
  v.rumble_hand = RumbleHand::Both;
  // The control tab's second page: VISOR GESTURE, then back.
  Click(s, v, view, kNextX, kPageY, now, actions);
  Click(s, v, view, kLabelX, RowY(kControlTab, 2), now, actions);
  Check(s.control_page == 1 && !v.xr_dpad_enabled, "visor gesture on control page 2");
  Click(s, v, view, kPreviousX, kPageY, now, actions);
  Check(s.control_page == 0, "control page 1 again");
  // Movement: SNAP TURN ANGLE steps through PrimedGun's choices.
  Click(s, v, view, TabX(kMovementTab), kTabY, now, actions);
  Click(s, v, view, kPlusX, RowY(kMovementTab, 9), now, actions);
  Check(v.snap_turn_degrees == 60, "snap turn angle");

  // Textures: a slot is applied, then marked.
  Click(s, v, view, TabX(kTexturesTab), kTabY, now, actions);
  Click(s, v, view, kLabelX, RowY(kTexturesTab, 2), now, actions);
  Check(actions.cannon == 2 && v.cannon_texture_slot == 2, "cannon slot");

  // States: a slot row selects; LOAD STATE takes two clicks.
  Click(s, v, view, TabX(kStatesTab), kTabY, now, actions);
  Click(s, v, view, kLabelX, RowY(kStatesTab, 4 + 2), now, actions);
  Check(actions.slot == 3 && v.vr_state_slot == 3, "state slot");
  Click(s, v, view, kLabelX, RowY(kStatesTab, 0), now, actions);
  Check(actions.loaded == 0 && s.state_confirm == kLoadState, "load armed");
  Click(s, v, view, kLabelX, RowY(kStatesTab, 0), now, actions);
  Check(actions.loaded == 3 && s.state_confirm == kNoStateAction, "load confirmed");

  // EXIT GAME, from any tab: two clicks.
  Click(s, v, view, 860.0f, 40.0f, now, actions);
  Check(actions.exits == 0 && s.reset_confirm == kExitGame, "exit armed");
  Click(s, v, view, 860.0f, 40.0f, now, actions);
  Check(actions.exits == 1, "exit confirmed");

  // Hover selects; opening goes back to LAYOUT.
  s.tab = kCalibrationTab;
  Hover(s, view, RowY(kCalibrationTab, 5));
  Check(s.selected == 5, "hover selects");
  Open(s);
  Check(s.tab == kLayoutTab && s.selected == 0, "opens on layout");
}

void TestTabs() {
  // Eight tabs in PrimedGun's strip, in order, apart, each holding its label.
  for (uint32_t tab = 0; tab < kTabCount; ++tab) {
    const Rect rect = TabRect(tab);
    Check(rect.y == 64 && rect.h == 38, "tab row");
    Check(rect.w >= TextWidth(kTabLabels[tab], 2) + 16, "label fits its tab");
    if (tab > 0) {
      const Rect before = TabRect(tab - 1);
      Check(rect.x >= before.x + before.w + kTabGap, "tabs apart");
    }
  }
  Check(TabRect(0).x >= 22 && TabRect(kTabCount - 1).x + TabRect(kTabCount - 1).w <= 1002, "inside the strip");
  Check(std::strcmp(kTabLabels[kPortConfigTab], "CONFIG") == 0 && kPortConfigTab == kLayoutTab + 1 &&
            std::strcmp(kTabLabels[kDebugTab], "DEBUG") == 0 && kDebugTab == kTabCount - 1,
        "CONFIG next to LAYOUT, DEBUG last");
}

void TestConfig() {
  RecordingActions actions;
  PortVrSettings v{};
  View pc{};
  View quest{};
  quest.standalone = true;
  double now = 50.0;

  // Each platform its rows: the PC's mirror, the Quest's own switches.
  const auto has = [](const View& view, PortItem item) {
    const PortList list = PortItems(view);
    for (uint32_t i = 0; i < list.count; ++i) {
      if (list.items[i] == item) {
        return true;
      }
    }
    return false;
  };
  Check(has(pc, PortItem::MirrorView) && !has(pc, PortItem::Foveation) && !has(pc, PortItem::Passthrough),
        "PC rows");
  Check(!has(quest, PortItem::MirrorView) && has(quest, PortItem::Foveation) &&
            has(quest, PortItem::PerformanceLevel) && has(quest, PortItem::DirectPresent),
        "Quest rows");
  Check(PageCount(kPortConfigTab, pc) == 3 && PageCount(kPortConfigTab, quest) == 3, "three pages");

  State s{};
  Click(s, v, pc, TabX(kPortConfigTab), kTabY, now, actions);
  Check(s.tab == kPortConfigTab && ItemCount(s, pc) == kRowsPerPage, "config tab");
  // RENDER SCALE: -/+ by 0.05 within the settings' bounds.
  Click(s, v, pc, kPlusX, RowY(kPortConfigTab, 0), now, actions);
  Check(Near(v.render_scale, 1.05f), "render scale up");
  Click(s, v, pc, kMinusX, RowY(kPortConfigTab, 0), now, actions);
  Click(s, v, pc, kMinusX, RowY(kPortConfigTab, 0), now, actions);
  Check(Near(v.render_scale, 0.95f), "render scale down");
  v.render_scale = kVrRenderScaleMax;
  Click(s, v, pc, kPlusX, RowY(kPortConfigTab, 0), now, actions);
  Check(Near(v.render_scale, kVrRenderScaleMax), "render scale bound");
  // EYE RESOLUTION reads out the chosen scale's eye size and takes no click.
  View sized = pc;
  sized.eye_width = 1428;
  sized.eye_height = 1496;
  State shown = s;
  const auto rows = BuildRows(shown, v, sized);
  Check(rows[1].value == "1428X1496" && BuildRows(shown, v, pc)[1].value == "UNKNOWN", "eye resolution");
  const PortVrSettings before = v;
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 1), now, actions);
  Check(v.render_scale == before.render_scale, "readout takes no click");
  // REFRESH RATE: the value box steps and stops at the ends, the row cycles on.
  Click(s, v, pc, kMinusX, RowY(kPortConfigTab, 2), now, actions);
  Check(v.display_refresh_rate == 0.0f, "refresh rate stops at DEFAULT");
  Click(s, v, pc, kPlusX, RowY(kPortConfigTab, 2), now, actions);
  Check(v.display_refresh_rate == 72.0f, "refresh rate up");
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 2), now, actions);
  Check(v.display_refresh_rate == 80.0f && BuildRows(s, v, pc)[2].value == "80 HZ", "refresh rate cycles");
  // A switch.
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 5), now, actions);
  Check(!v.remove_cinematic_bars, "cinematic bars switch");

  // Page 2 includes room scale and the message box switch before reset.
  Click(s, v, pc, kNextX, kPageY, now, actions);
  Check(s.port_page == 1 && ItemCount(s, pc) == 12, "config page 2");
  // LOOK TO LOCK-ON, LOOK TO GRAPPLE and NO CAMERA TURN ON LOCK follow LOOK TO SCAN.
  Check(BuildRows(s, v, pc)[4].label == std::string("CANNON TARGETING") &&
            BuildRows(s, v, pc)[5].label == std::string("LOOK TO LOCK-ON") &&
            BuildRows(s, v, pc)[6].label == std::string("LOOK TO GRAPPLE") &&
            BuildRows(s, v, pc)[7].label == std::string("NO CAMERA TURN ON LOCK"),
        "look to lock-on and grapple rows");
  Check(!v.look_lock_no_camera_turn, "the camera turns on a lock by default");
  Check(v.cannon_targeting, "cannon targeting defaults on");
  Check(v.CannonTargetingForVisor(false) && !v.CannonTargetingForVisor(true),
        "enabled cannon targeting applies to combat but never the scan visor");
  v.cannon_targeting = false;
  Check(!v.CannonTargetingForVisor(false) && !v.CannonTargetingForVisor(true),
        "disabled cannon targeting changes neither visor");
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 4), now, actions);
  Check(v.cannon_targeting && BuildRows(s, v, pc)[4].value == "ON", "cannon targeting toggles on");
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 4), now, actions);
  Check(!v.cannon_targeting, "cannon targeting toggles off");
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 4), now, actions);
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 5), now, actions);
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 6), now, actions);
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 7), now, actions);
  Check(!v.look_to_lock_on && !v.look_to_grapple && v.look_lock_no_camera_turn,
        "look to lock-on, grapple and camera turn switches");
  v.metroid_hud_size = 2.0f;
  Check(v.room_scale, "room scale defaults on");
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 9), now, actions);
  Check(!v.room_scale && BuildRows(s, v, pc)[9].label == std::string("ROOM SCALE"),
        "room scale toggles off");
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 9), now, actions);
  Check(v.room_scale, "room scale toggles on");
  v.room_scale = false;
  Check(v.message_box_immersive, "message boxes stay in view by default");
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 10), now, actions);
  Check(!v.message_box_immersive && BuildRows(s, v, pc)[10].label == std::string("MESSAGE BOX IN VIEW") &&
            BuildRows(s, v, pc)[10].value == "OFF",
        "message box switch");
  Check(!v.floating_hud, "the morph ball HUD and the message boxes are fixed by default");
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 11), now, actions);
  Check(v.floating_hud && BuildRows(s, v, pc)[11].label == std::string("FLOATING HUD") &&
            BuildRows(s, v, pc)[11].value == "ON",
        "floating HUD switch");
  // Page 3: wristband and reset.
  Click(s, v, pc, kNextX, kPageY, now, actions);
  Check(s.port_page == 2 && ItemCount(s, pc) == 2, "config page 3");
  Check(!v.wrist_visor_enabled, "wristband is opt-in");
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 0), now, actions);
  Check(v.wrist_visor_enabled, "headset wristband toggle works");
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 1), now, actions);
  Check(s.reset_confirm == kResetPortConfig && Near(v.render_scale, kVrRenderScaleMax), "reset config armed");
  Click(s, v, pc, kLabelX, RowY(kPortConfigTab, 1), now, actions);
  Check(Near(v.render_scale, PortVrSettings{}.render_scale) && v.display_refresh_rate == 0.0f &&
            v.remove_cinematic_bars && v.look_to_lock_on && v.look_to_grapple && !v.look_lock_no_camera_turn &&
            !v.wrist_visor_enabled && Near(v.wrist_pitch, PortVrSettings{}.wrist_pitch) && v.cannon_targeting && v.room_scale && v.message_box_immersive && !v.floating_hud &&
            Near(v.metroid_hud_size, 2.0f),
        "reset config");

  // The Quest: foveation waits for the next start without density maps.
  State q{};
  q.tab = kPortConfigTab;
  Check(BuildRows(q, v, quest)[3].label == std::string("FOVEATION - NEXT START"), "foveation next start");
  View live = quest;
  live.foveation_live = true;
  Check(BuildRows(q, v, live)[3].label == std::string("FOVEATION"), "foveation live");
  Click(q, v, quest, kPlusX, RowY(kPortConfigTab, 3), now, actions);
  Check(v.foveation == FoveationLevel::Low, "foveation up");
  Click(q, v, quest, kLabelX, RowY(kPortConfigTab, 4), now, actions);
  Check(v.performance_level == "sustained_high" && BuildRows(q, v, quest)[4].value == "SUSTAINED HIGH",
        "performance level cycles");
  Click(q, v, quest, kLabelX, RowY(kPortConfigTab, 5), now, actions);
  Check(!v.passthrough, "passthrough switch");
}

void TestDebug() {
  RecordingActions actions;
  PortVrSettings v{};
  View view{};
  double now = 10.0;
  State s{};
  Click(s, v, view, TabX(kDebugTab), kTabY, now, actions);
  Check(s.tab == kDebugTab && ItemCount(s, view) == kDebugItems, "debug tab");
  // No game: the cheats say so and do nothing.
  Check(BuildRows(s, v, view)[0].value == "NO GAME", "no game");
  Click(s, v, view, kLabelX, RowY(kDebugTab, 0), now, actions);
  Check(actions.healed == 0, "no cheat without a game");
  view.in_game = true;
  Click(s, v, view, kLabelX, RowY(kDebugTab, 0), now, actions);
  Check(actions.healed == 1, "full health");
  // GRANT EVERYTHING takes two clicks.
  Click(s, v, view, kLabelX, RowY(kDebugTab, 1), now, actions);
  Check(actions.granted == 0 && s.reset_confirm == kGrantEverything, "grant armed");
  Click(s, v, view, kLabelX, RowY(kDebugTab, 1), now, actions);
  Check(actions.granted == 1, "grant confirmed");
  // The switches flip what the view shows.
  view.invulnerable = true;
  Click(s, v, view, kLabelX, RowY(kDebugTab, 2), now, actions);
  Check(actions.invulnerable == 0, "invulnerable off");
  Click(s, v, view, kLabelX, RowY(kDebugTab, 3), now, actions);
  Click(s, v, view, kLabelX, RowY(kDebugTab, 4), now, actions);
  Click(s, v, view, kLabelX, RowY(kDebugTab, 5), now, actions);
  Check(actions.streamed == 0 && actions.musyx == 0 && actions.log == 1, "audio and log switches");
  Click(s, v, view, kLabelX, RowY(kDebugTab, 6), now, actions);
  Check(v.diagnostics_logging, "xr diagnostics switch");
  Check(BuildRows(s, v, view)[7].value == "ON", "draw tags on by default");
  Click(s, v, view, kLabelX, RowY(kDebugTab, 7), now, actions);
  Check(actions.tags == 0, "draw tags switch");
  // Readouts.
  view.headset_hz = 90.0f;
  view.headset_fps = 89.94f;
  view.draws = 1234;
  const auto rows = BuildRows(s, v, view);
  Check(rows[8].value == "90 HZ" && rows[9].value == "89.9" && rows[10].value == "UNKNOWN" && rows[11].value == "1234",
        "readouts");
  Check(PageCount(kDebugTab, view) == 1, "one page");
  Check(BuildRows(s, v, view)[12].value == "OFF", "quick resume defaults off");
  Click(s, v, view, kLabelX, RowY(kDebugTab, 12), now, actions);
  Check(actions.quickResume == 1, "quick resume enables");
  view.quick_resume = true;
  Check(BuildRows(s, v, view)[12].value == "ON", "quick resume reflects live state");
  Click(s, v, view, kLabelX, RowY(kDebugTab, 12), now, actions);
  Check(actions.quickResume == 0, "quick resume disables");

  // The frame rate beside the title, on every tab; nothing while it is unknown.
  State layout{};
  const PortVrSettings settings{};
  const auto title_row_lit = [](const Pixels& p) {
    for (int y = 34; y < 48; ++y) {
      for (int x = kFpsX; x < 570; ++x) {
        if (At(p, x, y) == 0xFFD8C0A0u) {
          return true;
        }
      }
    }
    return false;
  };
  View unknown{};
  Check(!title_row_lit(BuildPixels(layout, settings, unknown, false)) && FpsText(unknown).empty(), "no rate yet");
  View measured{};
  measured.game_fps = 119.6f;
  Check(FpsText(measured) == "120 FPS" && kFpsX + TextWidth("120 FPS", 2) <= 570, "rate text fits");
  Check(title_row_lit(BuildPixels(layout, settings, measured, false)), "rate drawn on LAYOUT");
  Check(!title_row_lit(BuildPixels(layout, settings, measured, true)), "SETTINGS SAVED takes its place");
  Check(ViewKey(layout, settings, measured, false) != ViewKey(layout, settings, unknown, false), "rate in the key");
}

void TestPlacement() {
  // On the hand: the panel faces the grip's +Y, 0.18 below and 0.10 ahead of it.
  Pose grip{};
  const Pose panel = PanelPoseFromGrip(grip);
  Check(Near(screen_math::Rotate(panel.orientation, {0.0f, 0.0f, 1.0f}), {0.0f, 1.0f, 0.0f}), "wrist panel normal");
  Check(Near(panel.position, {0.0f, -0.18f, -0.10f}), "wrist panel offset");

  // Floating: 2.7 m ahead of a head looking left, facing it, upright.
  Pose head{};
  head.position = {0.0f, 1.6f, 0.0f};
  head.orientation = AxisAngle({0.0f, 1.0f, 0.0f}, 90.0f);
  Pose floating{};
  Check(FloatingPoseFromHead(head, floating), "floating pose");
  Check(Near(floating.position, {-2.7f, 1.6f, 0.0f}), "floating position");
  Check(Near(screen_math::Rotate(floating.orientation, {0.0f, 0.0f, 1.0f}), {1.0f, 0.0f, 0.0f}),
        "floating panel faces the head");

  // The pointer: centre, an offset point, from behind, and too far.
  Pose screen{};
  screen.position = {0.0f, 0.0f, -2.0f};
  Pose aim{};
  PointerHit hit = HitPanel(screen, 4.0f, 2.0f, aim);
  Check(hit.valid && Near(hit.x, 0.5f) && Near(hit.y, 0.5f) && Near(hit.distance, 2.0f), "centre hit");
  aim.orientation = AxisAngle({0.0f, 1.0f, 0.0f}, -std::atan2(1.0f, 2.0f) * 180.0f / 3.14159265f);
  hit = HitPanel(screen, 4.0f, 2.0f, aim);
  Check(hit.valid && Near(hit.x, 0.75f) && Near(hit.y, 0.5f), "offset hit");
  Pose behind{};
  behind.position = {0.0f, 0.0f, -4.0f};
  behind.orientation = AxisAngle({0.0f, 1.0f, 0.0f}, 180.0f);
  Check(HitPanel(screen, 4.0f, 2.0f, behind).valid, "hit from behind, as in PrimedGun");
  screen.position = {0.0f, 0.0f, -9.0f};
  Check(!HitPanel(screen, 4.0f, 2.0f, Pose{}).valid, "beyond the laser's reach");

  // The laser runs along the ray and faces the eye; the dot sits just in front.
  const Pose laser = LaserPose(Pose{}, 2.0f, {0.0f, 0.3f, 0.2f});
  Check(Near(laser.position, {0.0f, 0.0f, -1.0f}), "laser centre");
  Check(Near(screen_math::Rotate(laser.orientation, {0.0f, 1.0f, 0.0f}), {0.0f, 0.0f, -1.0f}), "laser along ray");
  Check(Near(screen_math::Rotate(laser.orientation, {0.0f, 0.0f, 1.0f}), {0.0f, 1.0f, 0.0f}), "laser faces eye");
  screen.position = {0.0f, 0.0f, -2.0f};
  const Pose dot = DotPose(screen, Pose{}, 2.0f);
  Check(Near(dot.position, {0.0f, 0.0f, -2.0f + kDotLift}), "dot in front of the panel");
}

void TestScreenAnchor() {
  VirtualScreenAnchor anchor;
  screen_math::Pose head{{1.f, 1.7f, 2.f}, {0.f, 0.f, 0.f, 1.f}};
  anchor.Update(VRPresentationMode::VirtualScreen, VRGameMode::FrontEnd, nullptr, 2.f, 0);
  Check(!anchor.Valid(), "screen waits for the first tracked head pose");
  anchor.Update(VRPresentationMode::VirtualScreen, VRGameMode::FrontEnd, &head, 2.f, 0);
  Check(anchor.Valid() && Near(anchor.Pose().position, {1.f, 1.7f, 0.f}),
        "first screen is ahead of the head at head height");

  const float halfSqrt = std::sqrt(0.5f);
  head.orientation = {0.f, halfSqrt, 0.f, halfSqrt};
  head.position = {3.f, 1.8f, 4.f};
  anchor.Update(VRPresentationMode::VirtualScreen, VRGameMode::FrontEnd, &head, 2.f, 0);
  Check(Near(anchor.Pose().position, {1.f, 1.7f, 0.f}),
        "turning or moving the head does not drag an open screen");

  anchor.Update(VRPresentationMode::Immersive, VRGameMode::InGame, &head, 2.f, 0);
  Check(!anchor.Valid(), "gameplay releases the previous screen anchor");
  anchor.Update(VRPresentationMode::VirtualScreen, VRGameMode::Paused, nullptr, 2.f, 0);
  Check(anchor.Valid() && Near(anchor.Pose().position, {1.f, 1.8f, 4.f}),
        "menu opening during a tracking blip uses the last headset facing");
  Check(Near(screen_math::Rotate(anchor.Pose().orientation, {0.f, 0.f, -1.f}), {-1.f, 0.f, 0.f}),
        "reopened menu faces the new headset yaw");

  head.orientation = {0.f, -halfSqrt, 0.f, halfSqrt};
  anchor.Update(VRPresentationMode::VirtualScreen, VRGameMode::Cinematic, &head, 2.f, 0);
  Check(Near(anchor.Pose().position, {5.f, 1.8f, 4.f}),
        "cinema opening after a menu captures a new facing without gameplay between");
  head.orientation = {halfSqrt, 0.f, 0.f, halfSqrt};
  anchor.Update(VRPresentationMode::VirtualScreen, VRGameMode::Cinematic, &head, 2.f, 0);
  Check(Near(anchor.Pose().position, {5.f, 1.8f, 4.f}), "cinema stays anchored while looking around");

  anchor.Reset();
  anchor.Update(VRPresentationMode::VirtualScreen, VRGameMode::Cinematic, nullptr, 2.f, 0);
  Check(!anchor.Valid(), "recenter discards poses from the old reference space");
  head.orientation = {0.5f, 0.f, 0.f, std::sqrt(0.75f)};
  anchor.Update(VRPresentationMode::VirtualScreen, VRGameMode::Cinematic, &head, 2.f, 0);
  Check(Near(anchor.Pose().position, {3.f, 1.8f, 2.f}) &&
            Near(screen_math::Rotate(anchor.Pose().orientation, {0.f, 1.f, 0.f}), {0.f, 1.f, 0.f}),
        "screen remains upright when captured with head pitch");

  // A message box: in the immersive view there is no screen; shown on the
  // screen, it captures its own facing (it is no cinema continuation).
  anchor.Update(VRPresentationMode::Immersive, VRGameMode::Message, &head, 2.f, 0);
  Check(!anchor.Valid(), "a message box in the immersive view keeps no screen anchor");
  head.orientation = {0.f, 0.f, 0.f, 1.f};
  head.position = {0.f, 1.6f, 0.f};
  anchor.Update(VRPresentationMode::VirtualScreen, VRGameMode::Message, &head, 2.f, 0);
  Check(anchor.Valid() && Near(anchor.Pose().position, {0.f, 1.6f, -2.f}),
        "a message box on the screen captures its own facing");
}

void TestCinemaCooldown() {
  VirtualScreenAnchor anchor;
  screen_math::Pose head{{0.f, 1.7f, 0.f}, {0.f, 0.f, 0.f, 1.f}};
  anchor.Update(VRPresentationMode::VirtualScreen, VRGameMode::Cinematic, &head, 2.f, 100'000'000);
  const auto original = anchor.Pose().position;
  const float halfSqrt = std::sqrt(0.5f);
  head.orientation = {0.f, halfSqrt, 0.f, halfSqrt};
  anchor.Update(VRPresentationMode::VirtualScreen, VRGameMode::Transition, &head, 2.f, 600'000'000);
  Check(anchor.Valid() && Near(anchor.Pose().position, original),
        "scene loading keeps the cinema screen's anchor during cooldown");
  anchor.Update(VRPresentationMode::Immersive, VRGameMode::InGame, &head, 2.f, 900'000'000);
  Check(!anchor.Valid(), "retained cinema anchor is not presented during immersive gameplay");
  anchor.Update(VRPresentationMode::VirtualScreen, VRGameMode::Cinematic, &head, 2.f, 1'099'000'000);
  Check(anchor.Valid() && Near(anchor.Pose().position, original),
        "a cinema scene returning at 999 milliseconds retains its facing");

  anchor.Update(VRPresentationMode::Immersive, VRGameMode::InGame, &head, 2.f, 1'200'000'000);
  anchor.Update(VRPresentationMode::VirtualScreen, VRGameMode::Cinematic, &head, 2.f, 2'099'000'000);
  Check(Near(anchor.Pose().position, {-2.f, 1.7f, 0.f}),
        "after one full second outside cinema a new cutscene captures current facing");
  head.orientation = {0.f, -halfSqrt, 0.f, halfSqrt};
  anchor.Update(VRPresentationMode::VirtualScreen, VRGameMode::Cinematic, &head, 2.f, 10'000'000'000);
  Check(Near(anchor.Pose().position, {-2.f, 1.7f, 0.f}),
        "cooldown never relocates a continuously visible cutscene");
  anchor.Update(VRPresentationMode::VirtualScreen, VRGameMode::Paused, &head, 2.f, 10'100'000'000);
  Check(Near(anchor.Pose().position, {2.f, 1.7f, 0.f}),
        "an intentional menu still opens in the latest facing during cinema cooldown");
}

void TestControls() {
  using settings_panel::Controls;
  using settings_panel::ToggleOptions;
  std::array<HandInputs, 2> hands{};
  ToggleOptions options;
  options.pointing_hand = 1;
  Controls controls;
  bool open = false;
  const auto step = [&](float dt = 1.0f / 90.0f) {
    return controls.Update(hands, open, dt, OpenXRControllerMode::PrimedGun, options);
  };

  // The left stick click opens it; the game sees nothing until it is released.
  hands[0].thumbstick_click = true;
  auto frame = step();
  Check(open && frame.withheld && frame.pointing_hand == 1, "left click opens");
  hands[0].thumbstick_click = false;
  frame = step();
  Check(open && frame.withheld && !frame.select, "open after release");
  // The right trigger clicks once per pull; the left one does nothing.
  hands[0].trigger = 1.0f;
  frame = step();
  Check(!frame.select, "off hand trigger ignored");
  hands[0].trigger = 0.0f;
  hands[1].trigger = 1.0f;
  frame = step();
  Check(frame.select && frame.clicked, "right trigger clicks");
  frame = step();
  Check(frame.select && !frame.clicked, "held trigger clicks once");
  hands[1].trigger = 0.0f;
  // The right stick click is SET HEIGHT, not the menu.
  hands[1].thumbstick_click = true;
  step();
  Check(open, "right stick leaves it open");
  hands[1].thumbstick_click = false;
  step();
  // The left menu button closes it; the controllers come back once released.
  hands[0].menu = true;
  frame = step();
  Check(!open && frame.withheld, "menu button closes");
  hands[0].menu = false;
  frame = step();
  Check(!frame.withheld, "controllers back");

  // Left-handed: stick clicks stay fixed, while the left hand points.
  options.pointing_hand = 0;
  hands[1].thumbstick_click = true;
  frame = step();
  Check(!open, "left-handed right click remains height calibration, not the menu");
  hands[1].thumbstick_click = false;
  step();
  hands[0].thumbstick_click = true;
  frame = step();
  Check(open && frame.pointing_hand == 0, "left-handed opens with the left stick and points with the left hand");
  hands[0].thumbstick_click = false;
  step();
  hands[1].thumbstick_click = true;
  step();
  Check(open, "left-handed right click cannot close the menu");
  hands[1].thumbstick_click = false;
  step();
  hands[0].thumbstick_click = true;
  step();
  hands[0].thumbstick_click = false;
  step();
  Check(!open, "left-handed closes");
  options.pointing_hand = 1;

  // A one-second press opens it; one press closes it.
  options.long_press = true;
  hands[0].thumbstick_click = true;
  step(0.5f);
  Check(!open, "half a second is not enough");
  step(0.6f);
  Check(open, "a second opens");
  step(0.5f);
  Check(open, "still held stays open");
  hands[0].thumbstick_click = false;
  step();
  hands[0].thumbstick_click = true;
  step();
  Check(!open, "a press closes");
  step(2.0f);
  Check(!open, "the closing press does not reopen");
  hands[0].thumbstick_click = false;
  step();
  options.long_press = false;

  // Custom duration affects opening only; partial holds cannot accumulate.
  options.long_press = true;
  options.long_press_seconds = 0.25f;
  hands[0].thumbstick_click = true;
  step(0.20f);
  Check(!open, "custom hold does not open before its duration");
  hands[0].thumbstick_click = false;
  step();
  hands[0].thumbstick_click = true;
  step(0.20f);
  Check(!open, "releasing resets a partial custom hold");
  step(0.06f);
  Check(open, "custom 0.25-second hold opens the menu");
  step(1.f);
  Check(open, "continued hold does not close the menu");
  hands[0].thumbstick_click = false;
  step();
  hands[0].thumbstick_click = true;
  step(0.01f);
  Check(!open, "custom long press still closes immediately");
  hands[0].thumbstick_click = false;
  step();
  options.long_press = false;

  // Near the head only, when asked.
  options.needs_head_zone = true;
  hands[0].thumbstick_click = true;
  step();
  Check(!open, "away from the head");
  hands[0].thumbstick_click = false;
  step();
  options.hand_near_head = true;
  hands[0].thumbstick_click = true;
  step();
  Check(open, "next to the head");
  hands[0].thumbstick_click = false;
  step();
  options.needs_head_zone = false;

  // Not allowed (overlays off, no layer): it closes.
  options.allowed = false;
  step();
  Check(!open, "closes when not allowed");
  hands[0].thumbstick_click = true;
  step();
  Check(!open, "stays closed when not allowed");
}

void TestHeldGripsMenuRelease() {
  settings_panel::Controls controls;
  settings_panel::ToggleOptions options;
  std::array<HandInputs, 2> hands{};
  bool open = false;
  const auto step = [&] {
    return controls.Update(hands, open, 1.f / 90.f, OpenXRControllerMode::PrimedGun, options);
  };
  // Keep both grips fully pressed throughout opening, selecting and closing.
  for (auto& hand : hands) {
    hand.squeeze = 1.f;
  }
  hands[0].thumbstick_click = true;
  step();
  hands[0].thumbstick_click = false;
  step();
  hands[1].trigger = 1.f;
  Check(step().clicked, "Index finger closure cannot block menu selection");
  hands[0].thumbstick_click = true;
  Check(step().withheld && !open, "closing still captures its button press");
  hands[0].thumbstick_click = false;
  Check(step().withheld, "a held select trigger still gates gameplay release");
  hands[1].trigger = 0.f;
  Check(!step().withheld, "Index controls return without letting go of the handles");
  // A runtime focus loss resets Controls but preserves the open menu.
  controls.Reset();
  open = true;
  step();
  hands[1].primary = true;
  Check(step().clicked, "Index menu accepts input after focus returns");

}

void TestCompileToast() {
  // PrimedGun's panel in the image's top strip, nothing below it; the bar fills with the count.
  const Pixels half = BuildCompileToastPixels(50, 100);
  Check(half.size() == static_cast<size_t>(kWidth) * kImageHeight, "toast image size");
  Check(At(half, 0, 0) == 0xE0FFB030u && At(half, 0, kToastHeight - 1) == 0xE0FFB030u, "toast frame");
  Check(At(half, 20, 100) == 0xD0100804u, "toast background");
  Check(At(half, 20, kToastHeight) == 0u && At(half, kWidth - 1, kImageHeight - 1) == 0u, "transparent below");
  Check(At(half, 112 + 399, 190) == 0xFFFFB030u && At(half, 112 + 401, 190) == 0xF0201810u, "half a bar");
  const Pixels over = BuildCompileToastPixels(7, 3);
  Check(At(over, 112 + 799, 190) == 0xFFFFB030u, "done is capped at the total");
  Check(At(BuildCompileToastPixels(0, 0), 112, 190) == 0xF0201810u, "nothing to compile, empty bar");
  Check(CompileToastKey(0, 0) == 0 && CompileToastKey(0, 10) == 1 && CompileToastKey(10, 10) == 101 &&
            CompileToastKey(4, 1000) == CompileToastKey(9, 1000) && CompileToastKey(9, 1000) != CompileToastKey(10, 1000),
        "toast redrawn per percent");

  // Across the top of a 2 x 1.5 m screen 2 m ahead, turned to face +X: centred, 55% wide,
  // its top edge 5% of the height below the screen's, just in front of it.
  Pose screen{};
  screen.position = {-2.0f, 1.5f, 0.0f};
  screen.orientation = AxisAngle({0.0f, 1.0f, 0.0f}, 90.0f);
  const ToastPlacement top = CompileToastOnScreen(screen, 1.0f, 0.75f);
  Check(Near(top.width_meters, 1.1f) && Near(top.height_meters, 1.1f * kToastHeight / kWidth), "toast size on screen");
  const float topEdge = top.pose.position[1] + 0.5f * top.height_meters;
  Check(Near(topEdge, 1.5f + 0.75f - 0.075f), "toast below the screen's top edge");
  Check(Near(top.pose.position[0], -2.0f + kToastLift) && Near(top.pose.position[2], 0.0f), "toast on the screen");
  Check(Near(screen_math::Rotate(top.pose.orientation, {0.0f, 0.0f, 1.0f}), {1.0f, 0.0f, 0.0f}), "toast faces the head");

  // No screen: 1.35 m ahead of a head looking left and down, upright and raised.
  Pose head{};
  head.position = {0.0f, 1.6f, 0.0f};
  head.orientation = Multiply(AxisAngle({0.0f, 1.0f, 0.0f}, 90.0f), AxisAngle({1.0f, 0.0f, 0.0f}, -30.0f));
  ToastPlacement floating;
  Check(CompileToastFloating(head, floating), "floating toast");
  Check(Near(floating.pose.position, {-kToastFloatingDistance, 1.6f + kToastFloatingRaise, 0.0f}), "floating toast position");
  Check(Near(screen_math::Rotate(floating.pose.orientation, {0.0f, 1.0f, 0.0f}), {0.0f, 1.0f, 0.0f}), "floating toast upright");
  Check(Near(floating.width_meters, kToastFloatingWidth), "floating toast width");
  head.orientation = AxisAngle({1.0f, 0.0f, 0.0f}, 90.0f);
  Check(!CompileToastFloating(head, floating), "no floating toast looking straight up");
}

void TestLiveSettings() {
  RecordingActions actions;
  PortVrSettings settings{};
  State state{};
  View view{};
  int published = 0;
  PortVrSettings runtime = settings;
  const auto click = [&](float x, float y) {
    ClickAndSave(state, settings, view, x, y, 100.0, actions, [&](const PortVrSettings& changed) {
      Check(actions.saves == published, "settings publish before save request");
      runtime = changed;
      ++published;
    });
  };
  click(TabX(kCalibrationTab), kTabY);
  Check(actions.saves == 0 && published == 0, "navigation does not save settings");
  click(kLabelX, RowY(kCalibrationTab, 0));
  Check(!runtime.cinematic_screen_enabled && actions.saves == 1, "VR toggle publishes and saves live");
  click(kLabelX, RowY(kCalibrationTab, 0));
  Check(runtime.cinematic_screen_enabled && actions.saves == 2, "VR toggle back publishes and saves live");
  settings.metroid_hud_size = 2.f;
  click(400.f, 120.f);
  Check(actions.saves == 2, "reset confirmation does not save");
  click(400.f, 120.f);
  Check(runtime.metroid_hud_size != 2.f && actions.saves == 3, "confirmed reset publishes and saves live");
}

} // namespace

int main() {
  TestLiveSettings();
  TestImage();
  TestRows();
  TestClicks();
  TestTabs();
  TestConfig();
  TestDebug();
  TestPlacement();
  TestScreenAnchor();
  TestCinemaCooldown();
  TestControls();
  TestHeldGripsMenuRelease();
  TestCompileToast();
  std::puts("vr menu tests passed");
  return 0;
}
