// SPDX-License-Identifier: GPL-3.0-or-later
//
// In place of PrimedGun's Dolphin Config tab: the native port's own headset and
// display settings, and the folders the game writes to.

#include "tabs.h"

#include "settings_model.h"
#include "tab_context.h"
#include "widgets.h"

#include <QCheckBox>
#include <QDir>
#include <QComboBox>
#include <QLabel>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace PrimedGunLauncher {

void BuildPortConfigTab(TabContext& ctx, QTabWidget* tabs) {
  QVBoxLayout* layout = ctx.MakeScrollTab(tabs, QObject::tr("Port Config"));
  layout->addWidget(ctx.Section(QObject::tr("Port Config")));
  auto* note = new QLabel(QObject::tr(
      "The native port's own settings. In game, F1 opens the full port overlay."));
  note->setObjectName(QStringLiteral("PrimedGunMuted"));
  note->setWordWrap(true);
  layout->addWidget(note);
  layout->addSpacing(8);
  auto* portOptions = new QPushButton(QObject::tr("Open Full Port Options (F1)..."));
  portOptions->setToolTip(QObject::tr("Open the full F1 settings panel, with or without a running game."));
  layout->addWidget(portOptions);
  QObject::connect(portOptions, &QPushButton::clicked, portOptions, [&ctx] {
    if (ctx.openPortOptions) ctx.openPortOptions();
  });

  ctx.Check(layout, QObject::tr("Quick resume"), "quick_resume_enabled",
            QObject::tr("Save your place when quitting and offer to resume it at the next launch. Off by default."));

  layout->addWidget(ctx.Section(QObject::tr("VR Headset")));
  ctx.Check(layout, QObject::tr("Enable VR"), "vr_enabled",
            QObject::tr("Off starts the game on the desktop only. Takes effect at the next start."));
  ctx.Combo(layout, QObject::tr("Control scheme"), "vr_controller_mode",
            {{QObject::tr("PrimedGun"), QStringLiteral("primedgun")},
             {QObject::tr("Gamepad"), QStringLiteral("gamepad")},
             {QObject::tr("None"), QStringLiteral("none")}});
  ctx.Combo(layout, QObject::tr("Mirror view"), "vr_mirror_view",
            {{QObject::tr("Normal"), QStringLiteral("normal")},
             {QObject::tr("Both eyes"), QStringLiteral("both")},
             {QObject::tr("Left eye"), QStringLiteral("left")},
             {QObject::tr("Right eye"), QStringLiteral("right")},
             {QObject::tr("None"), QStringLiteral("none")}});
  ctx.Float(layout, QObject::tr("Headset render scale"), "vr_render_scale")
      ->setToolTip(QObject::tr("Of the eye size the VR runtime recommends."));
  ctx.Float(layout, QObject::tr("World scale"), "vr_world_scale")
      ->setToolTip(QObject::tr("Game units per metre; a larger value makes the world look smaller."));
  ctx.Check(layout, QObject::tr("Draw the world per eye"), "vr_immersive_replay",
            QObject::tr("Off shows the flat game image on the virtual screen."));
  ctx.Check(layout, QObject::tr("Room Scale"), "vr_room_scale",
            QObject::tr("Physical walking moves Samus with collision checks in first person. "
                        "On by default; morph ball and cinematics retain their camera movement."));
  ctx.Combo(layout, QObject::tr("Foveated rendering (Quest)"), "vr_foveation",
            {{QObject::tr("Off"), QStringLiteral("off")},
             {QObject::tr("Low"), QStringLiteral("low")},
             {QObject::tr("Medium"), QStringLiteral("medium")},
             {QObject::tr("High"), QStringLiteral("high")}})
      ->setToolTip(QObject::tr("Shades the edges of each eye in 2x2 (Low) or 4x4 (Medium, High) "
                               "pixel blocks. From Off to a level takes a restart."));
  ctx.Check(layout, QObject::tr("Remove cinematic bars"), "vr_remove_cinematic_bars",
            QObject::tr("Keeps the morph ball and visor transitions full size in the headset."));
  ctx.Check(layout, QObject::tr("Sky at infinity"), "vr_sky_at_infinity",
            QObject::tr("Draws the sky with the head's rotation only, so planets and stars look far away."));
  ctx.Check(layout, QObject::tr("Space warp"), "vr_space_warp",
            QObject::tr("The distortion around charged shots (also Flickerbats, Chozo Ghosts and Metroid Prime)."));
  ctx.Check(layout, QObject::tr("Scan window zoom"), "vr_scan_zoom",
            QObject::tr("Off, the scan visor's window shows the view behind it unmagnified, keeping its frame."));
  ctx.Float(layout, QObject::tr("Virtual screen distance"), "vr_screen_distance_meters")
      ->setToolTip(QObject::tr("Metres; menus, cutscenes and the morph ball's HUD hang on this screen."));
  ctx.Float(layout, QObject::tr("Virtual screen width"), "vr_screen_width_meters")
      ->setToolTip(QObject::tr("Metres."));
  ctx.Float(layout, QObject::tr("Lean back"), "vr_lean_back_degrees")
      ->setToolTip(QObject::tr("Degrees, for playing seated or lying back."));

  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("Display")));
  ctx.Check(layout, QObject::tr("Fullscreen"), "fullscreen");
  ctx.Check(layout, QObject::tr("VSync"), "vsync",
            QObject::tr("Holds frames to the display's refresh rate. "
                        "Off while the headset runs: the window only mirrors it."));
  ctx.Combo(layout, QObject::tr("MSAA"), "msaa",
            {{QObject::tr("Off"), QStringLiteral("1")}, {QObject::tr("4x"), QStringLiteral("4")}});
  ctx.Combo(layout, QObject::tr("Anisotropic filtering"), "anisotropy",
            {{QObject::tr("1x"), QStringLiteral("1")},
             {QObject::tr("2x"), QStringLiteral("2")},
             {QObject::tr("4x"), QStringLiteral("4")},
             {QObject::tr("8x"), QStringLiteral("8")},
             {QObject::tr("16x"), QStringLiteral("16")}});
  // render_scale 0 is the overlay's "Auto render scale (native)".
  auto* autoScale = new QCheckBox(QObject::tr("Auto render scale (native)"));
  layout->addWidget(autoScale);
  FloatRow* scale = ctx.FloatWith(
      layout, QObject::tr("EFB scale"), 1.0, 2.0, 0.25,
      [&ctx] { return std::max(ctx.model.Float("render_scale"), 1.0f); },
      [&ctx](float v) { ctx.model.SetFloat("render_scale", v); }, "render_scale");
  scale->setToolTip(QObject::tr("Scales the internal EFB; higher values use more GPU memory."));
  QObject::connect(autoScale, &QCheckBox::toggled, autoScale, [&ctx, scale](bool checked) {
    ctx.model.SetFloat("render_scale", checked ? 0.0f : 1.0f);
    scale->SetValue(1.0);
    scale->setEnabled(!checked);
    ctx.Edited();
  });
  ctx.AddRefresher([&ctx, autoScale, scale] {
    const bool isAuto = ctx.model.Float("render_scale") <= 0.0f;
    const QSignalBlocker blocker(autoScale);
    autoScale->setChecked(isAuto);
    scale->setEnabled(!isAuto);
  });

  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("Folders")));
  auto* openUser = new QPushButton(QObject::tr("Open User Folder"));
  auto* openSettings = new QPushButton(QObject::tr("Open Settings File"));
  auto* openLog = new QPushButton(QObject::tr("Open Last Run Log"));
  auto* openTextures = new QPushButton(QObject::tr("Open Texture Pack Folder"));
  auto* textureHelp = new QPushButton(QObject::tr("Texture Pack Help"));
  auto* openDumps = new QPushButton(QObject::tr("Open Crash Dumps"));
  openDumps->setToolTip(QObject::tr("The game writes crash_*.dmp next to itself when it crashes."));
  for (QPushButton* button : {openUser, openSettings, openLog}) {
    layout->addWidget(button);
  }
  auto* textureRow = new QHBoxLayout;
  textureRow->addWidget(openTextures, 1);
  textureRow->addWidget(textureHelp);
  layout->addLayout(textureRow);
  layout->addWidget(openDumps);
  layout->addStretch();

  QObject::connect(openUser, &QPushButton::clicked, openUser,
                   [&ctx] { OpenFolder(ctx.paths.userFolder); });
  QObject::connect(openSettings, &QPushButton::clicked, openSettings,
                   [&ctx] { OpenFile(ctx.paths.settingsFile); });
  QObject::connect(openLog, &QPushButton::clicked, openLog,
                   [&ctx] { OpenFile(NewestLog(ctx.paths)); });
  QObject::connect(openTextures, &QPushButton::clicked, openTextures,
                   [&ctx] { OpenFolder(ctx.paths.userTextures); });
  QObject::connect(textureHelp, &QPushButton::clicked, textureHelp, [&ctx] {
    QMessageBox::information(ctx.window, QObject::tr("Installing HD Texture Packs"),
        QObject::tr("1. Extract your Metroid Prime texture pack ZIP/7z first. Use its PNG/DDS files, "
                    "keeping their original texture filenames (usually tex1_...). No ISO patch is needed.\n\n"
                    "2. Click Open Texture Pack Folder and copy the extracted textures there. "
                    "A subfolder such as My HD Pack is fine; subfolders are scanned automatically.\n\n"
                    "Your texture folder:\n%1\n\n"
                    "3. Restart the game, or use F1 > Mods > Texture pack > Reload texture pack while playing.\n\n"
                    "Use one pack/version at a time to avoid conflicting replacements. "
                    "Larger packs use more memory; choose a lower-resolution version if needed.")
            .arg(QDir::toNativeSeparators(ctx.paths.userTextures)));
  });
  QObject::connect(openDumps, &QPushButton::clicked, openDumps,
                   [&ctx] { OpenFolder(ctx.paths.exeFolder); });
}

} // namespace PrimedGunLauncher
