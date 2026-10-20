#include "tab_context.h"
#include "settings_model.h"
#include "style.h"
#include <QApplication>
#include <QCheckBox>
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
    auto* mode = dialog->findChild<QComboBox*>("vr_custom_visor_mode"); mode->setCurrentIndex(mode->findData(1));
    auto* hold = dialog->findChild<QComboBox*>("vr_custom_visor_hold"); hold->setCurrentIndex(hold->findData(4));
    dialog->grab().save(QDir(QApplication::applicationDirPath()).filePath("controller-layout-preview.png"));
    dialog->findChild<QTabWidget*>()->setCurrentIndex(4);
    dialog->grab().save(QDir(QApplication::applicationDirPath()).filePath("controller-layout-visors-preview.png"));
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
  });
  ShowControllerLayout(ctx);
  Check(model.Bool("vr_custom_layout_enabled"), "Apply enables custom layout");
  Check(model.Int("vr_custom_jump") == 24 && model.Int("vr_custom_jump_extra") == 17, "button jump does not replace stick jump");
  Check(model.Int("vr_custom_visor_mode") == 1 && model.Int("vr_custom_visor_hold") == 4, "hold-to-select settings applied");
  QTimer::singleShot(50, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    auto* extra = dialog->findChild<QComboBox*>("vr_custom_jump_extra"); extra->setCurrentIndex(extra->findData(0));
    dialog->reject();
  });
  ShowControllerLayout(ctx);
  Check(model.Int("vr_custom_jump_extra") == 17, "Cancel discards edits");
  QTimer::singleShot(50, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    dialog->findChild<QComboBox*>("controllerFamily")->setCurrentIndex(4);
    dialog->findChild<QPushButton*>("loadControllerDefaults")->click();
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
  });
  ShowControllerLayout(ctx);
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
