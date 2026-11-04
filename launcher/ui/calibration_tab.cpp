// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrimedGun's Calibration tab (DolphinQt/MainWindow.cpp ConnectStack):
// in-headset display, culling, HUD, targeting and the arm cannon's offsets,
// with PrimedGun's two arm presets.

#include "tabs.h"

#include "settings_model.h"
#include "tab_context.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace PrimedGunLauncher {
namespace {

void ResetKeys(TabContext& ctx, std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    ctx.model.ResetToDefault(key);
  }
  ctx.RefreshAll();
  ctx.Edited();
}

// The HUD offsets are four non-negative keys; PrimedGun shows each axis as one
// signed slider.
void AddHudAxis(TabContext& ctx, QVBoxLayout* layout, const QString& label, const char* positive,
                const char* negative) {
  ctx.FloatWith(
      layout, label, -1.0, 1.0, 0.01,
      [&ctx, positive, negative] { return ctx.model.Float(positive) - ctx.model.Float(negative); },
      [&ctx, positive, negative](float v) {
        ctx.model.SetFloat(positive, std::max(v, 0.0f));
        ctx.model.SetFloat(negative, std::max(-v, 0.0f));
      },
      positive);
}

} // namespace

void BuildCalibrationTab(TabContext& ctx, QTabWidget* tabs) {
  QVBoxLayout* layout = ctx.MakeScrollTab(tabs, QObject::tr("Calibration"));
  layout->addWidget(ctx.Section(QObject::tr("In-headset Display")));
  ctx.Check(layout, QObject::tr("In-headset overlays"), "vr_vr_overlays_enabled");
  ctx.Check(layout, QObject::tr("Show height prompt"), "vr_height_prompt_enabled");
  ctx.Check(layout, QObject::tr("Show cutscenes on cinema screen"), "vr_cinematic_screen_enabled");
  ctx.Check(layout, QObject::tr("X-ray visor blur"), "vr_xray_blur_enabled", {},
            QObject::tr("Turn off for a sharp X-ray image in VR. Keeps stereo depth and X-ray colours."));
  ctx.Check(layout, QObject::tr("Detach VR menu from hand"), "vr_vr_menu_floating");
  ctx.Check(layout, QObject::tr("Detach game menu and map from view"),
            "vr_game_menu_screen_enabled");
  ctx.Check(layout, QObject::tr("Keep message boxes in view"), "vr_message_box_immersive", {},
            QObject::tr("Item pickup and save messages stay over the blurred world in the "
                        "headset; off, they show on the cinema screen"));
  ctx.Check(layout, QObject::tr("Floating HUD"), "vr_floating_hud", {},
            QObject::tr("The morph ball HUD and message boxes ease toward the headset; off, they "
                        "stay fixed"));
  ctx.Check(layout, QObject::tr("Enable visor helmet"), "vr_visor_helmet_enabled", {},
            QObject::tr("Allows user to turn helmet back on in the pause menu"));
  ctx.Check(layout, QObject::tr("Show floor position marker"), "vr_position_marker_enabled");

  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("Culling")));
  ctx.Check(layout, QObject::tr("Enable frustum culling"), "vr_frustum_culling_enabled");
  ctx.Float(layout, QObject::tr("Culling cone"), "vr_frustum_culling_degrees");

  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("HUD")));
  auto* resetHud = new QPushButton(QObject::tr("Reset HUD"));
  layout->addWidget(resetHud);
  ctx.Float(layout, QObject::tr("HUD distance"), "vr_metroid_hud_distance");
  ctx.Float(layout, QObject::tr("HUD size"), "vr_metroid_hud_size");
  AddHudAxis(ctx, layout, QObject::tr("HUD vertical"), "vr_metroid_hud_offset_up",
             "vr_metroid_hud_offset_down");
  AddHudAxis(ctx, layout, QObject::tr("HUD horizontal"), "vr_metroid_hud_offset_right",
             "vr_metroid_hud_offset_left");

  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("Targeting")));
  auto* resetTargeting = new QPushButton(QObject::tr("Reset Targeting"));
  layout->addWidget(resetTargeting);
  ctx.Float(layout, QObject::tr("Target distance"), "vr_gun_targeting_distance");
  ctx.Float(layout, QObject::tr("Target radius"), "vr_gun_targeting_radius");

  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("Offset Tuning")));
  auto* resetCalibration = new QPushButton(QObject::tr("Reset Calibration"));
  layout->addWidget(resetCalibration);
  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("Position")));
  ctx.Float(layout, QObject::tr("Left / right"), "vr_model_offset_x");
  ctx.Float(layout, QObject::tr("Forward / back"), "vr_model_offset_y");
  ctx.Float(layout, QObject::tr("Up / down"), "vr_model_offset_z");
  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("Rotation")));
  ctx.Float(layout, QObject::tr("Pitch offset"), "vr_rot_offset_x");
  ctx.Float(layout, QObject::tr("Yaw offset"), "vr_rot_offset_y");
  ctx.Float(layout, QObject::tr("Roll offset"), "vr_rot_offset_z");

  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("Presets")));
  auto* presetRow = new QHBoxLayout;
  auto* defaultPreset = new QPushButton;
  auto* samusPreset = new QPushButton;
  defaultPreset->setIcon(QIcon(QStringLiteral(":/default_arm.png")));
  defaultPreset->setIconSize(QSize(120, 90));
  defaultPreset->setToolTip(QObject::tr("Default arm preset: zero offsets"));
  samusPreset->setIcon(QIcon(QStringLiteral(":/samus_arm.png")));
  samusPreset->setIconSize(QSize(120, 90));
  samusPreset->setToolTip(QObject::tr("Samus arm preset"));
  presetRow->addWidget(defaultPreset);
  presetRow->addWidget(samusPreset);
  presetRow->addStretch();
  layout->addLayout(presetRow);
  layout->addStretch();

  QObject::connect(resetHud, &QPushButton::clicked, resetHud, [&ctx] {
    ResetKeys(ctx, {"vr_metroid_hud_distance", "vr_metroid_hud_size", "vr_metroid_hud_offset_up",
                    "vr_metroid_hud_offset_down", "vr_metroid_hud_offset_left",
                    "vr_metroid_hud_offset_right"});
  });
  QObject::connect(resetTargeting, &QPushButton::clicked, resetTargeting, [&ctx] {
    ResetKeys(ctx, {"vr_gun_targeting_enabled", "vr_gun_targeting_distance",
                    "vr_gun_targeting_radius", "vr_visor_helmet_enabled"});
  });
  const auto resetOffsets = [&ctx] {
    ResetKeys(ctx, {"vr_model_offset_x", "vr_model_offset_y", "vr_model_offset_z",
                    "vr_rot_offset_x", "vr_rot_offset_y", "vr_rot_offset_z"});
  };
  QObject::connect(resetCalibration, &QPushButton::clicked, resetCalibration, resetOffsets);
  QObject::connect(defaultPreset, &QPushButton::clicked, defaultPreset, resetOffsets);
  QObject::connect(samusPreset, &QPushButton::clicked, samusPreset, [&ctx] {
    // PrimedGun's Samus arm (platform/vr/vr_settings.cpp ApplyVrSamusArmPreset).
    ctx.model.SetFloat("vr_model_offset_x", 0.0f);
    ctx.model.SetFloat("vr_model_offset_y", -0.30f);
    ctx.model.SetFloat("vr_model_offset_z", 0.0f);
    ctx.model.SetFloat("vr_rot_offset_x", 0.0f);
    ctx.model.SetFloat("vr_rot_offset_y", 20.0f);
    ctx.model.SetFloat("vr_rot_offset_z", -90.0f);
    ctx.RefreshAll();
    ctx.Edited();
  });
}

} // namespace PrimedGunLauncher
