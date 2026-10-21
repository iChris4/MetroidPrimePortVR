// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrimedGun's launcher window (DolphinQt/MainWindow.cpp ConnectStack) for the
// native port: the tabs, the footer, and the game as a child process. The
// settings are edited in memory and written to port_settings.ini by Save
// Settings or Play. While the game runs, every edit is written as it is made
// and the game applies it in play (platform/debug_ui.cpp PollSettingsFile);
// what the game saves (F1, the headset's menu) shows here in turn.

#pragma once

#include "settings_model.h"
#include "tab_context.h"

#include <QMainWindow>
#include <QProcess>
#include <QSettings>

#include <string>
#include <string_view>

class QFileSystemWatcher;
class QLabel;
class QPushButton;
class QTabWidget;
class QTimer;
class QVBoxLayout;

namespace PrimedGunLauncher {

class LauncherWindow final : public QMainWindow {
public:
  LauncherWindow();

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  friend struct LauncherSettingsSyncTest;
  void BuildSetupTab(QTabWidget* tabs);
  void BuildFooter(QVBoxLayout* layout);

  void LoadSettings();
  bool SaveSettings();
  void SaveKeyNow(std::string_view key);
  void WatchSettingsFile();
  void OnSettingsFileChanged();
  void RefreshFromFile();
  void OnEdited();
  void UpdateFooter();

  QString SelectedDisc() const;
  void SetSelectedDisc(const QString& path);
  void UpdateSelectedGame();
  void SelectGame();
  void ShowGameOptions();
  void ShowSetupNotes();
  void TransferOldSave();

  void Play();
  void Stop();
  void OnGameFinished(int exitCode, QProcess::ExitStatus status);
  void SetRunning(bool running);

  LauncherPaths m_paths;
  SettingsModel m_model;
  TabContext m_ctx;
  QSettings m_ini;

  QTabWidget* m_tabs = nullptr;
  QLabel* m_selectedGame = nullptr;
  QLabel* m_discWarning = nullptr;
  QLabel* m_runStatus = nullptr;
  QPushButton* m_selectButton = nullptr;
  QPushButton* m_playButton = nullptr;
  QPushButton* m_stopButton = nullptr;
  QPushButton* m_optionsButton = nullptr;
  QPushButton* m_transferButton = nullptr;
  QPushButton* m_resetAllButton = nullptr;
  QPushButton* m_saveButton = nullptr;
  QLabel* m_footerStatus = nullptr;

  QProcess* m_game = nullptr;
  QTimer* m_forceStopTimer = nullptr;
  QTimer* m_liveSaveTimer = nullptr;
  bool m_stopRequested = false;
  bool m_forceStopOffered = false;
  bool m_closeAfterStop = false;

  QFileSystemWatcher* m_watcher = nullptr;
  std::string m_lastSettingsText; // most recently loaded or saved contents, for duplicate notifications
  bool m_changedOnDisk = false;
};

} // namespace PrimedGunLauncher
