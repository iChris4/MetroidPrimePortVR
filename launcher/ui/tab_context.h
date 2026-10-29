// SPDX-License-Identifier: GPL-3.0-or-later
//
// What every tab shares: the folders the game uses, the settings model, and
// helpers that build PrimedGun's rows already bound to a port_settings.ini key.
// A row for a key the game does not read yet (launcher_keys `active`) carries
// a "not active yet" tag.

#pragma once

#include "cannon_textures.h"

#include <QString>

#include <filesystem>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QTabWidget;
class QVBoxLayout;
class QWidget;

namespace PrimedGunLauncher {

class FloatRow;
class SettingsModel;

struct LauncherPaths {
  QString exeFolder;     // the launcher's folder, which is the game's
  QString gameExe;       // <exe>/metroid_prime_port.exe
  QString userFolder;    // PortPaths::UserFolder(): settings, logs, texture pack
  QString settingsFile;  // <user>/port_settings.ini
  QString launcherIni;   // <user>/primedgun_launcher.ini
  QString cardBase;      // PortPaths::CardFolder(); the card is USA/Card A in it (EUR for PAL)
  QString userTextures;  // MP_USER_TEXTURES, else <user>/user_textures
  QString lastRunLog;    // <user>/primedgun_last_run.log
  QString gameLog;       // <user>/metroid_prime_port.log (log_file=1)
  Cannon::Folders cannon;
};
LauncherPaths ResolvePaths();

std::filesystem::path ToPath(const QString& path);
QString FromPath(const std::filesystem::path& path);
void OpenFolder(const QString& folder);
// Opens a file, or the folder it would be in when it does not exist yet.
void OpenFile(const QString& path);
// The log of the last run: the launcher's capture, or the game's own
// metroid_prime_port.log when log_file=1 made that one newer.
QString NewestLog(const LauncherPaths& paths);

class TabContext {
public:
  TabContext(SettingsModel& model, const LauncherPaths& paths, QWidget* window)
      : model(model), paths(paths), window(window) {}

  SettingsModel& model;
  const LauncherPaths& paths;
  QWidget* window;
  // Set by the window: after an edit (to show unsaved changes), and to write
  // one key at once (the cannon slot, whose files are already applied).
  std::function<void()> onEdited;
  std::function<void()> openPortOptions;
  std::function<bool()> prepareUpdate;
  std::function<void(std::string_view)> saveKeyNow;

  void AddRefresher(std::function<void()> refresh) { m_refreshers.push_back(std::move(refresh)); }
  // Brings every widget in line with the model, without reporting edits.
  void RefreshAll() const;
  void Edited() const;

  QVBoxLayout* MakeScrollTab(QTabWidget* tabs, const QString& name) const;
  QLabel* Section(const QString& text) const;
  void Separator(QVBoxLayout* layout) const;
  // The muted tag for a key the game does not read yet, or nullptr.
  QLabel* TagFor(std::string_view key) const;

  // `note` is a muted remark beside the box.
  QCheckBox* Check(QVBoxLayout* layout, const QString& text, std::string_view key,
                   const QString& tooltip = {}, const QString& note = {});
  // A slider over the key's range from launcher_keys.
  FloatRow* Float(QVBoxLayout* layout, const QString& label, std::string_view key);
  // A slider over something other than one key (HUD vertical / horizontal).
  FloatRow* FloatWith(QVBoxLayout* layout, const QString& label, double min, double max,
                      double step, std::function<float()> get, std::function<void(float)> set,
                      std::string_view tagKey);
  // Two exclusive choices for a boolean key; the first means `firstValue`.
  void RadioPair(QVBoxLayout* layout, const QString& first, const QString& second,
                 std::string_view key, bool firstValue);
  // A fixed-width label and a combo box of (text, value as the game writes it).
  QComboBox* Combo(QVBoxLayout* layout, const QString& label, std::string_view key,
                   const std::vector<std::pair<QString, QString>>& items);

private:
  std::vector<std::function<void()>> m_refreshers;
};

} // namespace PrimedGunLauncher
