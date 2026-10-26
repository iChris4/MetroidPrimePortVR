#include "tab_context.h"
#include "settings_model.h"
#include "style.h"
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QScrollArea>
#include <QScrollBar>
#include <QWheelEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QPixmap>
#include <QTimer>
#include <QTabWidget>
#include <QDir>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace PrimedGunLauncher { void ShowControllerLayout(TabContext&); }
static void Check(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "Controller editor: %s\n", what); std::abort(); }
}
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  using namespace PrimedGunLauncher;
  QApplication::setPalette(Style::DarkPalette(QApplication::palette()));
  QWidget window;
  SettingsModel model; LauncherPaths paths; TabContext ctx(model, paths, &window);
  QTimer::singleShot(150, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()); Check(dialog != nullptr, "editor opens");
    auto* extra = dialog->findChild<QComboBox*>("vr_custom_jump_extra"); Check(extra != nullptr, "additional jump picker exists");
    extra->setCurrentIndex(extra->findData(17));
    auto* menuLong = dialog->findChild<QCheckBox*>("vr_custom_menu_long_press");
    auto* menuTime = dialog->findChild<QWidget*>("vr_custom_menu_hold_seconds");
    Check(menuLong && menuTime && !menuTime->isEnabled(), "menu hold is optional by default");
    menuLong->setChecked(true);
    auto* menuSpin = menuTime->findChild<QDoubleSpinBox*>();
    Check(menuTime->isEnabled() && menuSpin && menuSpin->singleStep() == 0.01,
          "menu duration enables hundredth-second steps");
    menuSpin->setValue(0.25);
    dialog->resize(650, 480); QApplication::processEvents();
    QScrollArea* page = nullptr;
    for (auto* parent = extra->parentWidget(); parent; parent = parent->parentWidget()) {
      if ((page = qobject_cast<QScrollArea*>(parent))) break;
    }
    Check(page && page->verticalScrollBar()->maximum() > 0, "small editor page can scroll");
    page->verticalScrollBar()->setValue(0); extra->setFocus();
    const int selected = extra->currentIndex();
    QWheelEvent wheel(QPointF(5, 5), extra->mapToGlobal(QPoint(5, 5)), QPoint(), QPoint(0, -120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(extra, &wheel);
    Check(extra->currentIndex() == selected, "wheel cannot change focused binding");
    Check(page->verticalScrollBar()->value() > 0, "wheel over binding scrolls page");
    dialog->resize(820, 680); QApplication::processEvents();
    auto* threshold = dialog->findChild<QSlider*>("vr_custom_trigger_threshold");
    Check(threshold != nullptr, "trigger activation slider exists"); threshold->setValue(78);
    auto* mode = dialog->findChild<QComboBox*>("vr_custom_visor_mode"); mode->setCurrentIndex(mode->findData(1));
    auto* hold = dialog->findChild<QComboBox*>("vr_custom_visor_hold"); hold->setCurrentIndex(hold->findData(4));
    dialog->grab().save(QDir(QApplication::applicationDirPath()).filePath("controller-layout-preview.png"));
    dialog->findChild<QTabWidget*>()->setCurrentIndex(5);
    QWheelEvent sliderWheel(QPointF(5, 5), threshold->mapToGlobal(QPoint(5, 5)), QPoint(), QPoint(0, -120),
                            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(threshold, &sliderWheel);
    Check(threshold->value() == 78, "wheel cannot change activation threshold");
    dialog->grab().save(QDir(QApplication::applicationDirPath()).filePath("controller-layout-sensitivity-preview.png"));
    dialog->findChild<QTabWidget*>()->setCurrentIndex(4);
    dialog->grab().save(QDir(QApplication::applicationDirPath()).filePath("controller-layout-visors-preview.png"));
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
  });
  ShowControllerLayout(ctx);
  Check(model.Bool("vr_custom_menu_long_press") && model.Float("vr_custom_menu_hold_seconds") == 0.25f, "Apply persists menu hold settings");
  Check(model.Float("vr_custom_trigger_threshold") > 0.77f, "Apply persists threshold");
  Check(model.Bool("vr_custom_layout_enabled"), "Apply enables custom layout");
  Check(model.Int("vr_custom_jump") == 24 && model.Int("vr_custom_jump_extra") == 17, "button jump does not replace stick jump");
  Check(model.Int("vr_custom_visor_mode") == 1 && model.Int("vr_custom_visor_hold") == 4, "hold-to-select settings applied");
  QTimer::singleShot(50, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    auto* extra = dialog->findChild<QComboBox*>("vr_custom_jump_extra"); extra->setCurrentIndex(extra->findData(0));
    dialog->findChild<QSlider*>("vr_custom_trigger_threshold")->setValue(20);
    dialog->findChild<QCheckBox*>("vr_custom_menu_long_press")->setChecked(false);
    dialog->findChild<QWidget*>("vr_custom_menu_hold_seconds")->findChild<QDoubleSpinBox*>()->setValue(0.75);
    dialog->reject();
  });
  ShowControllerLayout(ctx);
  Check(model.Int("vr_custom_jump_extra") == 17, "Cancel discards edits");
  Check(model.Bool("vr_custom_menu_long_press") && model.Float("vr_custom_menu_hold_seconds") == 0.25f,
        "Cancel discards menu hold edits");
  Check(model.Float("vr_custom_trigger_threshold") > 0.77f, "Cancel discards threshold edits");
  QTimer::singleShot(50, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    dialog->findChild<QComboBox*>("controllerFamily")->setCurrentIndex(4);
    auto* morph = dialog->findChild<QComboBox*>("vr_custom_morph");
    Check(morph->currentData().toInt() == 33, "Quest Morph Ball translates directly to Frame X");
    Check(dialog->findChild<QComboBox*>("vr_custom_jump_extra")->currentData().toInt() == 17,
          "changing controller type preserves custom assignments");
    Check(dialog->findChild<QComboBox*>("vr_custom_missile")->currentData().toInt() == 32,
          "Steam Frame missiles use the right shoulder immediately");
    Check(dialog->findChild<QSlider*>("vr_custom_trigger_threshold")->value() == 78,
          "changing controller type preserves activation thresholds");
    std::map<QString, int> frameDefaults;
    for (auto* combo : dialog->findChildren<QComboBox*>()) {
      if (combo->objectName().startsWith("vr_custom_") && combo->objectName() != "vr_custom_jump_extra")
        frameDefaults[combo->objectName()] = combo->currentData().toInt();
    }
    dialog->findChild<QPushButton*>("loadControllerDefaults")->click();
    Check(morph->currentData().toInt() == 33, "loading Frame defaults does not change Morph Ball a second time");
    for (const auto& [key, value] : frameDefaults) {
      if (key != "vr_custom_visor_hold" && key != "vr_custom_visor_mode")
        Check(dialog->findChild<QComboBox*>(key)->currentData().toInt() == value,
              "translated standard bindings match the selected controller defaults");
    }
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
  });
  ShowControllerLayout(ctx);
  Check(model.Float("vr_custom_trigger_threshold") > 0.54f && model.Float("vr_custom_trigger_threshold") < 0.56f, "Load defaults resets thresholds");
  Check(!model.Bool("vr_custom_menu_long_press") && model.Float("vr_custom_menu_hold_seconds") == 1.f, "Load defaults resets menu hold");
  Check(model.Int("vr_custom_label_profile") == 4, "controller label choice persists");
  Check(model.Int("vr_custom_visor_mode") == 2 && model.Int("vr_custom_visor_left") == 14, "Frame preset selects real D-pad visors");
  Check(model.Int("vr_custom_morph") == 33 && model.Int("vr_custom_unmorph") == 33, "Frame D-pad is not also used to morph");
  Check(model.Int("vr_custom_menu_pause") == 6, "Frame D-pad does not also pause menus");
  QTimer::singleShot(50, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    dialog->findChild<QComboBox*>("controllerFamily")->setCurrentIndex(2);
    dialog->findChild<QPushButton*>("loadControllerDefaults")->click();
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
  });
  ShowControllerLayout(ctx);
  Check(model.Int("vr_custom_morph") == 21 && model.Int("vr_custom_beam") == 22 &&
        model.Int("vr_custom_confirm") == 19, "Vive preset uses pad click, menu and trigger instead of absent face buttons");
}
