// SPDX-License-Identifier: GPL-3.0-or-later
#include "tabs.h"
#include "tab_context.h"
#include "settings_model.h"
#include "style.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QVBoxLayout>
#include <array>
#include <map>
#include <string>
#include <vector>

namespace PrimedGunLauncher {
void ShowControllerLayout(TabContext& ctx) {
  QDialog dialog(ctx.window);
  dialog.setWindowTitle(QObject::tr("Customize controller layout"));
  dialog.resize(820, 680);
  if (qEnvironmentVariableIsSet("PRIMEDGUN_LAYOUT_TEST")) dialog.setAttribute(Qt::WA_DontShowOnScreen);
  dialog.setStyleSheet(Style::kPage);
  auto* body = new QVBoxLayout(&dialog);
  auto* enabled = new QCheckBox(QObject::tr("Use custom controller layout"));
  enabled->setChecked(true);
  enabled->setObjectName("customLayoutEnabled");
  body->addWidget(enabled);
  auto* note = new QLabel(QObject::tr("This layout takes priority over PrimedGun's standard bindings and other input remaps. "
      "Movement speed, turning, handedness and rumble settings still apply. Choose only controls your controllers have. "
      "Headset system buttons may be reserved by the runtime."));
  note->setWordWrap(true); body->addWidget(note);
  auto* family = new QComboBox;
  family->setObjectName("controllerFamily");
  family->addItems({"Quest / Touch / Pico", "Valve Index", "HTC Vive wands", "PSVR2", "Steam Frame", "Generic OpenXR", "WMR / Reverb"});
  body->addWidget(new QLabel(QObject::tr("Controller button labels"))); body->addWidget(family);
  auto* tabs = new QTabWidget; body->addWidget(tabs, 1);
  std::map<std::string, QFormLayout*> groups;
  std::map<std::string, QComboBox*> rows;
  std::vector<QComboBox*> inputRows;
  const auto group = [&](const char* title) {
    if (groups.count(title)) return groups.at(title);
    auto* page = new QWidget; auto* form = new QFormLayout(page);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto* scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setWidget(page);
    tabs->addTab(scroll, QString::fromUtf8(title)); groups[title] = form; return form;
  };
  const auto row = [&](const char* key, const char* title, const char* section) {
    auto* combo = new QComboBox;
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->completer()->setFilterMode(Qt::MatchContains);
    combo->completer()->setCompletionMode(QCompleter::PopupCompletion);
    combo->setObjectName(QString::fromUtf8(key));
    combo->setMinimumWidth(280); rows[key] = combo;
    group(section)->addRow(QString::fromUtf8(title), combo); return combo;
  };
#define VR_BIND(name, initial, section, label) inputRows.push_back(row("vr_custom_" #name, label, section));
#include "vr/vr_controller_bindings.inc"
#undef VR_BIND
  auto* move = row("vr_custom_move_hand", "Movement stick / trackpad", "On foot");
  auto* look = row("vr_custom_look_hand", "Look / turn stick / trackpad", "On foot");
  auto* visor = row("vr_custom_visor_hand", "Visor selector stick / trackpad", "Visors");
  for (auto* combo : {move, look, visor}) { combo->addItem("Left", 0); combo->addItem("Right", 1); }
  auto* mode = row("vr_custom_visor_mode", "Visor selection method", "Visors");
  mode->addItem("Controller near head + visor stick", 0);
  mode->addItem("Hold visor selector button + visor stick", 1);
  mode->addItem("Physical D-pad / assigned visor buttons only", 2);
  auto* visorNote = new QLabel(QObject::tr("Direct visor bindings below work in every mode. With Hold selected, moving "
      "the visor stick changes visors only while its selector button is held. Its movement, turn and jump inputs are suppressed meanwhile."));
  visorNote->setWordWrap(true); group("Visors")->addRow(visorNote);
  auto* jumpNote = new QLabel(QObject::tr("Keep Jump - main input on a stick direction and assign Jump - additional input "
      "to a button to use both independently. Beam select is held while swinging the weapon controller."));
  jumpNote->setWordWrap(true); group("On foot")->addRow(jumpNote);
  const auto relabel = [&] {
    for (auto* combo : inputRows) {
      const int selected = combo->currentData().isValid() ? combo->currentData().toInt() : 0;
      combo->clear(); combo->addItem("Not assigned", 0);
      for (int hand = 0; hand < 2; ++hand) {
        const QString side = hand == 0 ? "Left - " : "Right - ";
        QString primary = hand == 0 ? "X / primary button" : "A / primary button";
        QString secondary = hand == 0 ? "Y / secondary button" : "B / secondary button";
        if (family->currentIndex() == 1) { primary = "A / lower button"; secondary = "B / upper button"; }
        if (family->currentIndex() == 3) { primary = hand == 0 ? "Square" : "Cross"; secondary = hand == 0 ? "Triangle" : "Circle"; }
        if (family->currentIndex() == 4 && hand == 0) { primary = "D-pad down (A equivalent)"; secondary = "D-pad up (B equivalent)"; }
        const std::array<QString, 16> labels{primary, secondary, "Trigger", "Grip / squeeze", "Stick / trackpad click",
          "Menu / View (if available)", "Touchpad press", "Stick / trackpad up", "Stick / trackpad down",
          "Stick / trackpad left", "Stick / trackpad right", "Physical D-pad up", "Physical D-pad down",
          "Physical D-pad left", "Physical D-pad right", "Shoulder (if available)"};
        for (int i = 0; i < 16; ++i) combo->addItem(side + labels[i], 1 + 16 * hand + i);
      }
      combo->addItem("Right - X (Steam Frame)", 33); combo->addItem("Right - Y (Steam Frame)", 34);
      combo->addItem("Left - extra face button 3", 35); combo->addItem("Left - extra face button 4", 36);
      if (auto* model = qobject_cast<QStandardItemModel*>(combo->model())) {
        for (int i = 1; i < combo->count(); ++i) {
          const int input = combo->itemData(i).toInt();
          const int part = input <= 32 ? (input - 1) % 16 + 1 : 0;
          const int hand = input <= 16 ? 0 : 1;
          bool available = true;
          switch (family->currentIndex()) {
          case 0: case 3: available = input <= 32 && part != 7 && part < 12; break;
          case 1: available = input <= 32 && part != 6 && part < 12; break;
          case 2: available = input <= 32 && part >= 3 && part != 7 && part < 12; break;
          case 6: available = input <= 32 && part >= 3 && part < 12; break;
          case 4: available = (input == 33 || input == 34) ||
              (input <= 32 && part != 7 && !(hand == 1 && part >= 12 && part <= 15)); break;
          default: break;
          }
          if (model->item(i)) model->item(i)->setEnabled(available);
        }
      }
      combo->setCurrentIndex(combo->findData(selected));
    }
  };
  QObject::connect(family, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&](int) { relabel(); });
  family->setCurrentIndex(ctx.model.Int("vr_custom_label_profile"));
  relabel();
  for (const auto& [key, combo] : rows) combo->setCurrentIndex(combo->findData(ctx.model.Int(key)));
  auto* defaults = new QPushButton(QObject::tr("Load default layout for these controllers")); body->addWidget(defaults);
  defaults->setObjectName("loadControllerDefaults");
  QObject::connect(defaults, &QPushButton::clicked, &dialog, [&] {
#define VR_BIND(name, initial, section, label) rows.at("vr_custom_" #name)->setCurrentIndex(rows.at("vr_custom_" #name)->findData(initial));
#include "vr/vr_controller_bindings.inc"
#undef VR_BIND
    move->setCurrentIndex(0); look->setCurrentIndex(1); visor->setCurrentIndex(0); mode->setCurrentIndex(0);
    const auto set = [&](const char* key, int value) { rows.at(key)->setCurrentIndex(rows.at(key)->findData(value)); };
    if (!ctx.model.Bool("vr_use_right_hand")) {
      for (const char* key : {"vr_custom_shoot", "vr_custom_lock", "vr_custom_missile", "vr_custom_map", "vr_custom_morph", "vr_custom_pause", "vr_custom_beam", "vr_custom_bomb", "vr_custom_unmorph"}) {
        const int value = rows.at(key)->currentData().toInt(); set(key, value <= 16 ? value + 16 : value - 16);
      }
      visor->setCurrentIndex(1);
    }
    if (family->currentIndex() == 2 || family->currentIndex() == 6) {
      // Wands and WMR have no face buttons: use their exposed physical controls.
      const int morphInput = family->currentIndex() == 2 ? 21 : 23;
      set("vr_custom_morph", morphInput); set("vr_custom_unmorph", morphInput);
      set("vr_custom_pause", 6); set("vr_custom_menu_pause", 6); set("vr_custom_beam", 22);
      set("vr_custom_confirm", 19); set("vr_custom_back", 22); set("vr_custom_bomb", 19);
      set("vr_custom_boost", 20); set("vr_custom_power_bomb", 4);
      set("vr_custom_spider", 3); set("vr_custom_ball_camera", 0);
      set("vr_custom_height", family->currentIndex() == 2 ? 0 : 21);
    }
    if (family->currentIndex() == 4) {
      set("vr_custom_morph", 33); set("vr_custom_unmorph", 33); set("vr_custom_pause", 6); set("vr_custom_menu_pause", 6); set("vr_custom_map", 16); set("vr_custom_missile", 32);
      mode->setCurrentIndex(2);
    }
  });
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel); body->addWidget(buttons);
  QObject::connect(buttons->button(QDialogButtonBox::Cancel), &QPushButton::clicked, &dialog, &QDialog::reject);
  QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&] {
    for (const auto& [key, combo] : rows) {
      const int index = combo->findText(combo->currentText());
      if (index < 0) { QMessageBox::warning(&dialog, "Choose an input", "Select an input from the list, or choose Not assigned."); return; }
      combo->setCurrentIndex(index);
    }
    if (enabled->isChecked() && (move->currentData() == look->currentData() ||
        rows.at("vr_custom_menu")->currentData().toInt() == 0 ||
        rows.at("vr_custom_confirm")->currentData().toInt() == 0 || rows.at("vr_custom_back")->currentData().toInt() == 0 ||
        (mode->currentData().toInt() == 1 && rows.at("vr_custom_visor_hold")->currentData().toInt() == 0))) {
      QMessageBox::warning(&dialog, "Check layout", "Use different movement and look sticks, assign VR settings, Confirm and Back, "
          "and assign a visor selector button when using Hold mode."); return;
    }
    for (const auto& [key, combo] : rows) ctx.model.SetInt(key, combo->currentData().toInt());
    ctx.model.SetInt("vr_custom_label_profile", family->currentIndex());
    ctx.model.SetBool("vr_custom_layout_enabled", enabled->isChecked());
    ctx.RefreshAll(); ctx.Edited(); dialog.accept();
  });
  if (!ctx.model.Bool("vr_custom_layout_enabled")) defaults->click();
  dialog.exec();
}
} // namespace PrimedGunLauncher
