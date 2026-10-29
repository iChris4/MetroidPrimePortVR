#include "launcher_window.h"
#include "port_settings_file.h"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileSystemWatcher>
#include <QTemporaryDir>
#include <QTimer>
#include <QPushButton>
#include <QDir>

#include <cstdio>
#include <cstdlib>

namespace PrimedGunLauncher {
namespace {
void Check(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "Launcher settings sync: %s\n", message);
    std::abort();
  }
}

template <typename Predicate>
void Await(Predicate ready, const char* message) {
  QElapsedTimer timer;
  timer.start();
  while (!ready() && timer.elapsed() < 3000) {
    QEventLoop loop;
    QTimer::singleShot(10, &loop, &QEventLoop::quit);
    loop.exec();
  }
  Check(ready(), message);
}
} // namespace

struct LauncherSettingsSyncTest {
  static void Run() {
    QTemporaryDir user;
    Check(user.isValid(), "isolated user folder created");
    qputenv("MP_USER_PATH", user.path().toUtf8());
    LauncherWindow window;
    QCheckBox* roomScale = nullptr;
    for (auto* check : window.findChildren<QCheckBox*>()) {
      if (check->text() == QStringLiteral("Room Scale")) roomScale = check;
    }
    Check(roomScale != nullptr, "roomscale toggle exists");
    window.m_model.SetBool("vr_room_scale", false);
    Check(window.SaveSettings(), "launcher saves the initial toggle");
    window.m_ctx.RefreshAll();
    Check(!roomScale->isChecked(), "initial desktop toggle matches saved value");

    QPushButton* updateButton = nullptr;
    for (auto* button : window.findChildren<QPushButton*>()) {
      if (button->text() == QStringLiteral("Check for updates...")) updateButton = button;
    }
    Check(updateButton && updateButton->isEnabled(), "updates available without a selected disc");
    Check(QFile::exists(QStringLiteral(":/update-windows.ps1")), "self-update helper is embedded in launcher");
    QPushButton* portOptions = nullptr;
    for (auto* button : window.findChildren<QPushButton*>()) {
      if (button->text() == QStringLiteral("Open Full Port Options (F1)...")) portOptions = button;
    }
    Check(portOptions && portOptions->isEnabled(), "full options available without a selected disc");

    // Route file notifications through the same live-refresh path as a running game.
    QProcess game;
    window.m_game = &game;
    portOptions->click();
    QFile optionsRequest(QDir(window.m_paths.userFolder).filePath(QStringLiteral("open_options.request")));
    Check(optionsRequest.open(QIODevice::ReadOnly) && optionsRequest.readAll() == "open\n",
          "options button requests the existing game panel rather than starting another game");
    optionsRequest.close();
    Check(optionsRequest.remove(), "consume test options request");
    const auto saveFromGame = [&](bool enabled) {
      PortSettingsFile file;
      Check(file.Load(ToPath(window.m_paths.settingsFile)), "load game settings");
      file.Set("vr_room_scale", enabled ? "1" : "0");
      std::string error;
      Check(file.Save(ToPath(window.m_paths.settingsFile), error), "replace game settings atomically");
    };
    for (int cycle = 0; cycle < 3; ++cycle) {
      saveFromGame(true);
      Await([&] { return roomScale->isChecked(); }, "VR change updates desktop toggle");
      saveFromGame(false); // Exactly the launcher's earlier file contents.
      Await([&] { return !roomScale->isChecked(); }, "VR reset updates desktop toggle");
      Check(!window.m_model.Dirty(), "external refresh does not create desktop edits");
    }

    roomScale->click();
    Await([&] {
      PortSettingsFile file;
      return file.Load(ToPath(window.m_paths.settingsFile)) && file.Get("vr_room_scale") == "1";
    }, "desktop toggle publishes live without Save Settings");
    Await([&] { return !window.m_model.Dirty(); }, "desktop edit finishes saving");
    saveFromGame(false);
    Await([&] { return !roomScale->isChecked(); }, "VR edit after desktop edit updates the toggle");

    window.m_model.SetBool("vr_use_right_hand", false);
    saveFromGame(true);
    Await([&] { return roomScale->isChecked(); }, "refresh while another desktop edit is pending");
    Check(!window.m_model.Bool("vr_use_right_hand"), "unrelated pending desktop edit preserved");
    Check(window.m_watcher->directories().contains(window.m_paths.userFolder), "folder watch survives replacements");

    Check(QFile::remove(window.m_paths.settingsFile), "remove settings for configuration reset");
    Await([&] { return window.m_lastSettingsText.empty(); }, "missing settings reload defaults");
    saveFromGame(false);
    Await([&] { return !roomScale->isChecked(); }, "watch reconnects after settings recreation");
    window.m_game = nullptr;
    std::puts("Launcher live settings and reset synchronization passed");
  }
};
} // namespace PrimedGunLauncher

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  PrimedGunLauncher::LauncherSettingsSyncTest::Run();
}
