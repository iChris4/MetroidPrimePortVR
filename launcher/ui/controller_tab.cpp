// SPDX-License-Identifier: GPL-3.0-or-later
//
// Controller bindings are configured in Customize controller layout.
// This tab retains handedness, feedback, gesture tuning and movement behaviour.

#include "tabs.h"

#include "settings_model.h"
#include "tab_context.h"
#include "widgets.h"

#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

namespace PrimedGunLauncher {
void ShowControllerLayout(TabContext& ctx);

void BuildControllerTab(TabContext& ctx, QTabWidget* tabs) {
  QVBoxLayout* layout = ctx.MakeScrollTab(tabs, QObject::tr("Controller"));
  auto* customize = new QPushButton(QObject::tr("Customize controller layout"));
  layout->addWidget(customize);
  QObject::connect(customize, &QPushButton::clicked, customize, [&ctx] { ShowControllerLayout(ctx); });
  ctx.Check(layout, QObject::tr("Use custom controller layout"), "vr_custom_layout_enabled",
            QObject::tr("Overrides standard VR bindings and other input remaps. Configure the layout with the button above."));
  layout->addWidget(ctx.Section(QObject::tr("Controller Mapping")));
  auto* resetController = new QPushButton(QObject::tr("Reset Controller"));
  layout->addWidget(resetController);
  ctx.RadioPair(layout, QObject::tr("Right hand"), QObject::tr("Left hand"), "vr_use_right_hand",
                true);
  ctx.Check(layout, QObject::tr("Beam wheel lights the HUD beam box"),
            "vr_beam_wheel_hud_highlight",
            QObject::tr("While the beam selector is held, the beam the cannon points at has its "
                        "box on the HUD lit."));

  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("Rumble")));
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
  ctx.Float(layout, QObject::tr("Rumble intensity"), "vr_rumble_intensity");

  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("Visor selection")));
  ctx.Check(layout, QObject::tr("Off-hand visor wristband"), "vr_wrist_visor_enabled",
            QObject::tr("Move the main controller near a visor icon on your off-hand wrist and press A or right trigger in right-hand mode, or X or left trigger in left-hand mode."));
  ctx.Float(layout, QObject::tr("Head radius"), "vr_xr_dpad_head_radius");
  ctx.Float(layout, QObject::tr("Below head"), "vr_xr_dpad_head_y_below");
  ctx.Float(layout, QObject::tr("Stick deadzone"), "vr_xr_dpad_deadzone");

  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("Directional Movement")));
  ctx.Check(layout, QObject::tr("Left stick strafe movement"), "vr_directional_movement_enabled");
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
    ctx.model.ResetToDefault("vr_wrist_visor_enabled");
    ctx.model.ResetToDefault("vr_wrist_offset_x");
    ctx.model.ResetToDefault("vr_wrist_offset_y");
    ctx.model.ResetToDefault("vr_wrist_offset_z");
    ctx.model.ResetToDefault("vr_wrist_pitch");
    ctx.model.ResetToDefault("vr_wrist_yaw");
    ctx.model.ResetToDefault("vr_wrist_roll");

    ctx.model.ResetToDefault("vr_custom_layout_enabled");
    ctx.model.ResetToDefault("vr_custom_trigger_threshold");
    ctx.model.ResetToDefault("vr_custom_grip_threshold");
    ctx.model.ResetToDefault("vr_custom_touchpad_threshold");
    ctx.model.ResetToDefault("vr_custom_stick_threshold");
    ctx.model.ResetToDefault("vr_custom_label_profile");
    ctx.model.ResetToDefault("vr_custom_move_hand"); ctx.model.ResetToDefault("vr_custom_look_hand");
    ctx.model.ResetToDefault("vr_custom_visor_hand"); ctx.model.ResetToDefault("vr_custom_visor_mode");
#define VR_BIND(name, initial, group, label) ctx.model.ResetToDefault("vr_custom_" #name);
#include "vr/vr_controller_bindings.inc"
#undef VR_BIND
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
