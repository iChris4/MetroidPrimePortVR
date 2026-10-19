// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrimedGun's Controller tab (DolphinQt/MainWindow.cpp ConnectStack): the
// control scheme's behaviour; the bindings themselves are fixed, as in
// PrimedGun (platform/vr/vr_pad.cpp).

#include "tabs.h"

#include "settings_model.h"
#include "tab_context.h"
#include "widgets.h"

#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

namespace PrimedGunLauncher {

void BuildControllerTab(TabContext& ctx, QTabWidget* tabs) {
  QVBoxLayout* layout = ctx.MakeScrollTab(tabs, QObject::tr("Controller"));
  layout->addWidget(ctx.Section(QObject::tr("Controller Mapping")));
  auto* resetController = new QPushButton(QObject::tr("Reset Controller"));
  layout->addWidget(resetController);
  ctx.RadioPair(layout, QObject::tr("Right hand"), QObject::tr("Left hand"), "vr_use_right_hand",
                true);
  ctx.Check(layout, QObject::tr("Longer held press for VR menu"), "vr_vr_menu_hold_left_stick");
  ctx.Check(layout, QObject::tr("VR menu requires controller near head to activate"),
            "vr_vr_menu_requires_head_zone");
  ctx.Check(layout, QObject::tr("Use A button for jump"), "vr_combat_jump_use_primary_button");
  ctx.Check(layout, QObject::tr("Beam wheel lights the HUD beam box"),
            "vr_beam_wheel_hud_highlight",
            QObject::tr("While B holds the beam wheel open, the beam the cannon points at has its "
                        "box on the HUD lit."));

  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("Rumble / Grip Inputs")));
  ctx.Check(layout, QObject::tr("Rumble"), "vr_rumble_enabled");
  ctx.Combo(layout, QObject::tr("Rumble target"), "vr_rumble_hand",
            {{QObject::tr("Split (cannon on gun hand)"), QStringLiteral("split")},
             {QObject::tr("Both"), QStringLiteral("both")},
             {QObject::tr("Left only"), QStringLiteral("left")},
             {QObject::tr("Right only"), QStringLiteral("right")}});
  auto* rumbleNote =
      new QLabel(QObject::tr("Split: the arm cannon (charge, missiles, recoil) rumbles the gun hand, "
                             "the grapple the off hand, everything else both."));
  rumbleNote->setWordWrap(true);
  rumbleNote->setObjectName(QStringLiteral("PrimedGunMuted"));
  layout->addWidget(rumbleNote);
  ctx.Check(layout, QObject::tr("Use grip input"), "vr_grip_inputs_enabled");
  ctx.Check(layout, QObject::tr("Use touchpad for PrimedGun grip inputs (Index users)"),
            "vr_grip_inputs_use_trackpad",
            QObject::tr("Use Index touchpad pressure for map and missiles. When off, squeeze the "
                        "grips. Other controller types use their normal grips."));
  ctx.Float(layout, QObject::tr("Rumble intensity"), "vr_rumble_intensity");

  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("D-pad")));
  ctx.Check(layout, QObject::tr("Enable visor gesture input"), "vr_xr_dpad_enabled");
  ctx.Float(layout, QObject::tr("Head radius"), "vr_xr_dpad_head_radius");
  ctx.Float(layout, QObject::tr("Below head"), "vr_xr_dpad_head_y_below");
  ctx.Float(layout, QObject::tr("Stick deadzone"), "vr_xr_dpad_deadzone");

  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("Directional Movement")));
  ctx.Check(layout, QObject::tr("Left stick strafe movement"), "vr_directional_movement_enabled");
  ctx.RadioPair(layout, QObject::tr("Left stick"), QObject::tr("Right stick"),
                "vr_directional_movement_use_right_stick", false);
  ctx.RadioPair(layout, QObject::tr("Controller direction"), QObject::tr("HMD direction"),
                "vr_directional_movement_use_hmd_direction", false);
  ctx.Float(layout, QObject::tr("Movement deadzone"), "vr_directional_movement_deadzone");
  ctx.Float(layout, QObject::tr("Movement speed"), "vr_directional_movement_speed");
  ctx.Float(layout, QObject::tr("Movement acceleration"), "vr_directional_movement_accel");
  ctx.Float(layout, QObject::tr("Air acceleration"), "vr_directional_movement_air_accel");
  FloatRow* lookYaw = ctx.Float(layout, QObject::tr("Look yaw sensitivity"), "vr_look_yaw_sensitivity");
  lookYaw->setToolTip(QObject::tr("Adjusts smooth look-stick turning speed. Values above 1.0 turn faster; snap-turn angles are unchanged."));
  ctx.Check(layout, QObject::tr("Snap turn"), "vr_snap_turn_enabled");
  ctx.Combo(layout, QObject::tr("Snap turn angle"), "vr_snap_turn_degrees",
            {{QObject::tr("30 degrees"), QStringLiteral("30")},
             {QObject::tr("45 degrees"), QStringLiteral("45")},
             {QObject::tr("60 degrees"), QStringLiteral("60")},
             {QObject::tr("90 degrees"), QStringLiteral("90")}});
  layout->addStretch();

  QObject::connect(resetController, &QPushButton::clicked, resetController, [&ctx] {
    // PrimedGun's Reset Controller: every one of these back to its default.
    for (const char* key :
         {"vr_use_right_hand", "vr_vr_overlays_enabled", "vr_vr_menu_hold_left_stick",
          "vr_vr_menu_requires_head_zone", "vr_vr_menu_floating", "vr_cinematic_screen_enabled",
          "vr_game_menu_screen_enabled", "vr_message_box_immersive", "vr_floating_hud",
          "vr_rumble_enabled",
          "vr_rumble_intensity",
          "vr_rumble_hand", "vr_xr_dpad_enabled", "vr_combat_jump_use_primary_button",
          "vr_beam_wheel_hud_highlight",
          "vr_grip_inputs_enabled", "vr_grip_inputs_use_trackpad", "vr_trackpad_press_threshold",
          "vr_index_grip_press_threshold", "vr_directional_movement_enabled",
          "vr_directional_movement_use_right_stick", "vr_directional_movement_use_hmd_direction",
          "vr_xr_dpad_head_radius", "vr_xr_dpad_head_y_below", "vr_xr_dpad_deadzone",
          "vr_directional_movement_deadzone", "vr_directional_movement_speed",
          "vr_directional_movement_accel", "vr_directional_movement_air_accel",
          "vr_look_yaw_sensitivity", "vr_snap_turn_enabled", "vr_snap_turn_degrees"}) {
      ctx.model.ResetToDefault(key);
    }
    ctx.RefreshAll();
    ctx.Edited();
  });
}

} // namespace PrimedGunLauncher
