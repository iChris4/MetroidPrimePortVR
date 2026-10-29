// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from PrimedGun's DolphinQt/MainWindow.cpp (GPL-2.0-or-later).

#include "launcher_window.h"
#include "update_dialog.h"

#include "cannon_textures.h"
#include "disc_probe.h"
#include "port_settings_file.h"
#include "primedgun_import.h"
#include "style.h"
#include "tabs.h"

#include "port_gci.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSaveFile>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdlib>

namespace PrimedGunLauncher {
namespace {

constexpr int kForceStopDelayMs = 10000;
// While the game runs, edits are written at most this often, so a dragged
// slider reaches the game as it moves.
constexpr int kLiveSaveMs = 150;

QString LauncherVersion() { return QStringLiteral(PRIMEDGUN_LAUNCHER_VERSION); }

void StyleGameButton(QPushButton* button, bool primary = false) {
  button->setFlat(true);
  button->setStyleSheet(primary ? Style::kPrimaryGameButton : Style::kGameButton);
}

QString DiscWarning(const QString& path, const DiscInfo& info) {
  if (path.isEmpty()) {
    return {};
  }
  switch (info.check) {
  case DiscCheck::WrongGame:
    return QObject::tr("Wrong game");
  case DiscCheck::WrongRevision:
    return QObject::tr("Wrong revision");
  case DiscCheck::UnsupportedFormat:
    return QObject::tr("Format not supported");
  case DiscCheck::Unreadable:
    return QFileInfo::exists(path) ? QObject::tr("Cannot read file") : QObject::tr("File not found");
  case DiscCheck::Ok:
  case DiscCheck::Unverified:
    break;
  }
  return {};
}

// Whether a disc image is PAL: its game keeps its saves on the EUR card and
// reads only GM8P saves (PortGci::CardRegion, IsGameFile). False for anything
// else, also when no disc is selected or its header is not read (WBFS).
bool IsPalDisc(const QString& path) { return !path.isEmpty() && ProbeDisc(ToPath(path)).Pal(); }

// The card the game mounts for that disc: <card base>/<region>/Card A.
QString CardFolderFor(const LauncherPaths& paths, bool pal) {
  return QDir::cleanPath(paths.cardBase +
                         (pal ? QStringLiteral("/EUR/Card A") : QStringLiteral("/USA/Card A")));
}

} // namespace

LauncherWindow::LauncherWindow()
    : m_paths(ResolvePaths()), m_ctx(m_model, m_paths, this),
      m_ini(m_paths.launcherIni, QSettings::IniFormat) {
  setWindowTitle(QStringLiteral("PrimedGun v%1").arg(LauncherVersion()));
  setWindowIcon(QIcon(QStringLiteral(":/PrimedGun.png")));
  setMinimumSize(700, 720);
  resize(700, 720);

  auto* page = new QWidget(this);
  page->setStyleSheet(Style::kPage);
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(14, 12, 14, 10);
  layout->setSpacing(8);

  m_ctx.onEdited = [this] { OnEdited(); };
  m_ctx.openPortOptions = [this] { OpenPortOptions(); };
  m_ctx.prepareUpdate = [this] {
    if (m_game && m_game->state() != QProcess::NotRunning) {
      QMessageBox::information(this, tr("Close the game"), tr("Close the game and Port Options before installing the update."));
      return false;
    }
    if (!SaveSettings()) return false;
    m_ini.setValue(QStringLiteral("geometry"), saveGeometry());
    m_ini.sync();
    return true;
  };
  m_ctx.saveKeyNow = [this](std::string_view key) { SaveKeyNow(key); };

  m_tabs = new QTabWidget(page);
  m_tabs->setDocumentMode(true);
  BuildSetupTab(m_tabs);
  BuildControllerTab(m_ctx, m_tabs);
  BuildCalibrationTab(m_ctx, m_tabs);
  BuildCannonTab(m_ctx, m_tabs);
  BuildLayoutTab(m_ctx, m_tabs);
  BuildPortConfigTab(m_ctx, m_tabs);
  BuildAboutTab(m_ctx, m_tabs);
  layout->addWidget(m_tabs, 1);
  BuildFooter(layout);
  setCentralWidget(page);

  restoreGeometry(m_ini.value(QStringLiteral("geometry")).toByteArray());

  m_forceStopTimer = new QTimer(this);
  m_forceStopTimer->setSingleShot(true);
  connect(m_forceStopTimer, &QTimer::timeout, this, [this] {
    if (m_game != nullptr) {
      m_forceStopOffered = true;
      m_stopButton->setText(tr("Force Stop"));
      m_runStatus->setText(tr("The game has not closed yet. Force Stop ends it at once."));
    }
  });

  m_liveSaveTimer = new QTimer(this);
  m_liveSaveTimer->setSingleShot(true);
  m_liveSaveTimer->setInterval(kLiveSaveMs);
  connect(m_liveSaveTimer, &QTimer::timeout, this, [this] {
    if (m_game != nullptr) {
      SaveSettings();
    }
  });

  m_watcher = new QFileSystemWatcher(this);
  connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { OnSettingsFileChanged(); });
  connect(m_watcher, &QFileSystemWatcher::directoryChanged, this,
          [this] { OnSettingsFileChanged(); });

  LoadSettings();
  if (SelectedDisc().isEmpty()) {
    // First start: the disc the game itself last used, if any.
    PortSettingsFile file;
    if (file.Load(ToPath(m_paths.settingsFile))) {
      if (const auto disc = file.Get("disc_path"); disc && !disc->empty()) {
        SetSelectedDisc(QString::fromStdString(*disc));
      }
    }
  }
  WatchSettingsFile();
  // The disc kept from the last session gets its name, PAL tag and warning too
  // (SetSelectedDisc does this only for a new pick); it also sets the buttons.
  UpdateSelectedGame();
}

void LauncherWindow::BuildSetupTab(QTabWidget* tabs) {
  QVBoxLayout* layout = m_ctx.MakeScrollTab(tabs, tr("Setup"));
  layout->setContentsMargins(12, 10, 12, 0);
  layout->addWidget(m_ctx.Section(tr("Setup")));
  auto* notesButton = new QPushButton(tr("Setup Notes"));
  StyleGameButton(notesButton);
  layout->addWidget(notesButton);
  auto* updates = new QPushButton(tr("Check for updates..."));
  StyleGameButton(updates);
  layout->addWidget(updates);
  connect(updates, &QPushButton::clicked, this, [this] { CheckForUpdates(m_ctx); });
  m_ctx.Separator(layout);

  m_selectedGame = new QLabel;
  layout->addWidget(m_selectedGame);
  auto* selectRow = new QHBoxLayout;
  auto* selectNote = new QLabel(tr("Select Metroid Prime USA 1.00/1.01/1.02, or PAL (experimental)."));
  selectNote->setObjectName(QStringLiteral("PrimedGunMuted"));
  m_discWarning = new QLabel;
  m_discWarning->setObjectName(QStringLiteral("PrimedGunBad"));
  m_discWarning->setVisible(false);
  selectRow->addWidget(selectNote);
  selectRow->addStretch();
  selectRow->addWidget(m_discWarning);
  layout->addLayout(selectRow);

  m_selectButton = new QPushButton(tr("Select Game..."));
  m_playButton = new QPushButton(tr("Play"));
  m_stopButton = new QPushButton(tr("Stop"));
  m_optionsButton = new QPushButton(tr("Game Options..."));
  StyleGameButton(m_selectButton);
  StyleGameButton(m_playButton, true);
  StyleGameButton(m_stopButton);
  StyleGameButton(m_optionsButton);
  layout->addWidget(m_selectButton);
  layout->addWidget(m_playButton);
  layout->addWidget(m_stopButton);
  m_runStatus = new QLabel;
  m_runStatus->setObjectName(QStringLiteral("PrimedGunMuted"));
  m_runStatus->setWordWrap(true);
  layout->addWidget(m_runStatus);
  layout->addWidget(m_optionsButton);

  layout->addSpacing(10);
  layout->addWidget(m_ctx.Section(tr("Memory Card")));
  m_transferButton = new QPushButton(tr("Transfer PrimedGun Memory Card / Settings"));
  StyleGameButton(m_transferButton);
  layout->addWidget(m_transferButton);
  layout->addSpacing(12);
  layout->addStretch();

  auto* art = new QLabel;
  art->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
  art->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  const QPixmap samus(QStringLiteral(":/samus.png"));
  if (!samus.isNull()) {
    const QPixmap scaled = samus.scaled(208, 126, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    art->setPixmap(scaled);
    art->setFixedHeight(scaled.height());
  }
  layout->addWidget(art, 0, Qt::AlignLeft | Qt::AlignBottom);

  connect(notesButton, &QPushButton::clicked, this, [this] { ShowSetupNotes(); });
  connect(m_selectButton, &QPushButton::clicked, this, [this] { SelectGame(); });
  connect(m_playButton, &QPushButton::clicked, this, [this] { Play(); });
  connect(m_stopButton, &QPushButton::clicked, this, [this] { Stop(); });
  connect(m_optionsButton, &QPushButton::clicked, this, [this] { ShowGameOptions(); });
  connect(m_transferButton, &QPushButton::clicked, this, [this] { TransferOldSave(); });
}

void LauncherWindow::BuildFooter(QVBoxLayout* layout) {
  auto* line = new QFrame;
  line->setFrameShape(QFrame::HLine);
  layout->addWidget(line);
  auto* footer = new QHBoxLayout;
  m_resetAllButton = new QPushButton(tr("Reset All"));
  m_saveButton = new QPushButton(tr("Save Settings"));
  m_footerStatus = new QLabel;
  m_footerStatus->setObjectName(QStringLiteral("PrimedGunMuted"));
  // Never wider than the room the credit leaves it.
  m_footerStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  auto* credit = new QLabel(tr("By Nobbie and iChris4   v%1").arg(LauncherVersion()));
  credit->setObjectName(QStringLiteral("PrimedGunMuted"));
  footer->addWidget(m_resetAllButton);
  footer->addWidget(m_saveButton);
  footer->addWidget(m_footerStatus, 1);
  footer->addWidget(credit);
  layout->addLayout(footer);

  connect(m_resetAllButton, &QPushButton::clicked, this, [this] {
    if (QMessageBox::question(this, tr("Reset All"),
                              tr("Reset every PrimedGun setting to its default? Use Save Settings "
                                 "afterwards to keep the reset.")) != QMessageBox::Yes) {
      return;
    }
    m_model.ResetAll();
    m_ctx.RefreshAll();
    OnEdited();
  });
  connect(m_saveButton, &QPushButton::clicked, this, [this] { SaveSettings(); });
}

// --- settings file -------------------------------------------------------

void LauncherWindow::LoadSettings() {
  PortSettingsFile file;
  if (!file.Load(ToPath(m_paths.settingsFile))) {
    QMessageBox::warning(this, tr("PrimedGun"),
                         tr("Could not read the settings file:\n%1")
                             .arg(QDir::toNativeSeparators(m_paths.settingsFile)));
  }
  m_model.Load(file);
  m_lastSettingsText = file.Text();
  m_changedOnDisk = false;
  m_ctx.RefreshAll();
  UpdateFooter();
}

bool LauncherWindow::SaveSettings() {
  if (!m_model.Dirty()) {
    return true;
  }
  // Read again: only the launcher's changed keys are written, over whatever the
  // game left in the file.
  PortSettingsFile file;
  std::string error;
  if (!file.Load(ToPath(m_paths.settingsFile))) {
    error = "cannot read the settings file";
  } else {
    m_model.ApplyChanges(file);
    if (file.Save(ToPath(m_paths.settingsFile), error)) {
      m_lastSettingsText = file.Text();
      m_model.MarkSaved();
      m_changedOnDisk = false;
      WatchSettingsFile();
      UpdateFooter();
      return true;
    }
  }
  QMessageBox::critical(this, tr("Save Settings"),
                        tr("Could not write %1:\n%2")
                            .arg(QDir::toNativeSeparators(m_paths.settingsFile),
                                 QString::fromStdString(error)));
  return false;
}

void LauncherWindow::SaveKeyNow(std::string_view key) {
  PortSettingsFile file;
  std::string error;
  if (!file.Load(ToPath(m_paths.settingsFile))) {
    error = "cannot read the settings file";
  } else {
    file.Set(std::string(key), m_model.Value(key));
    if (key == "vr_cannon_texture_slot" && m_game != nullptr) {
      // The running game reloads the user pack when this line changes
      // (platform/debug_ui.cpp ApplyChangedSetting), also for the same slot.
      file.Set("cannon_textures_applied", std::to_string(QDateTime::currentMSecsSinceEpoch()));
    }
    if (file.Save(ToPath(m_paths.settingsFile), error)) {
      m_lastSettingsText = file.Text();
      m_model.MarkSaved(key);
      WatchSettingsFile();
      UpdateFooter();
      return;
    }
  }
  QMessageBox::critical(this, tr("Save Settings"),
                        tr("Could not write %1:\n%2")
                            .arg(QDir::toNativeSeparators(m_paths.settingsFile),
                                 QString::fromStdString(error)));
}

void LauncherWindow::WatchSettingsFile() {
  // A save replaces the file (temporary file + rename), which ends a watch on
  // it, so the watch is renewed after every change. Keep watching the folder
  // too, so subsequent replacements cannot fall between file watches.
  if (!m_watcher->files().isEmpty()) {
    m_watcher->removePaths(m_watcher->files());
  }
  if (QFileInfo::exists(m_paths.settingsFile)) {
    m_watcher->addPath(m_paths.settingsFile);
  }
  if (m_watcher->directories().isEmpty() && QFileInfo::exists(m_paths.userFolder)) {
    m_watcher->addPath(m_paths.userFolder);
  }
}

void LauncherWindow::OnSettingsFileChanged() {
  WatchSettingsFile();
  PortSettingsFile file;
  if (!file.Load(ToPath(m_paths.settingsFile)) || file.Text() == m_lastSettingsText) {
    return;
  }
  if (m_game != nullptr) {
    // The game saved what was changed in play (F1, the headset's menu).
    RefreshFromFile();
    return;
  }
  if (m_model.Dirty()) {
    m_changedOnDisk = true;
    UpdateFooter();
    return;
  }
  LoadSettings();
}

void LauncherWindow::RefreshFromFile() {
  PortSettingsFile file;
  if (!file.Load(ToPath(m_paths.settingsFile))) {
    return;
  }
  m_model.Refresh(file);
  m_lastSettingsText = file.Text();
  m_changedOnDisk = false;
  m_ctx.RefreshAll();
  UpdateFooter();
}

void LauncherWindow::OnEdited() {
  UpdateFooter();
  if (m_game != nullptr && !m_liveSaveTimer->isActive()) {
    m_liveSaveTimer->start();
  }
}

void LauncherWindow::UpdateFooter() {
  // While the game runs, edits are written within kLiveSaveMs, so there is
  // nothing to report.
  QString status;
  QString tooltip;
  if (m_game == nullptr && m_model.Dirty()) {
    status = m_changedOnDisk ? tr("Unsaved; changed on disk") : tr("Unsaved changes");
    if (m_changedOnDisk) {
      tooltip = tr("port_settings.ini changed on disk since it was read. Save Settings writes only "
                   "the settings you changed and keeps the rest.");
    }
  }
  m_footerStatus->setText(status);
  m_footerStatus->setToolTip(tooltip);
}

// --- Setup tab -------------------------------------------------------------

QString LauncherWindow::SelectedDisc() const {
  return m_ini.value(QStringLiteral("selected_disc")).toString();
}

void LauncherWindow::SetSelectedDisc(const QString& path) {
  if (path.isEmpty()) {
    m_ini.remove(QStringLiteral("selected_disc"));
  } else {
    m_ini.setValue(QStringLiteral("selected_disc"), path);
  }
  m_ini.sync();
  UpdateSelectedGame();
}

void LauncherWindow::UpdateSelectedGame() {
  const QString path = SelectedDisc();
  const DiscInfo info = path.isEmpty() ? DiscInfo{} : ProbeDisc(ToPath(path));
  const QString name = QFileInfo(path).fileName();
  QString selected;
  if (!path.isEmpty()) {
    // The game's PAL support is experimental, and its log says so too.
    selected = info.check == DiscCheck::Ok && info.Pal()
                   ? tr("Selected: %1 (PAL, experimental)").arg(name)
                   : tr("Selected: %1").arg(name);
  }
  m_selectedGame->setVisible(!path.isEmpty());
  m_selectedGame->setText(selected);
  m_selectedGame->setToolTip(QDir::toNativeSeparators(path));
  const QString warning = DiscWarning(path, info);
  m_discWarning->setText(warning);
  m_discWarning->setVisible(!warning.isEmpty());
  SetRunning(m_game != nullptr);
}

void LauncherWindow::SelectGame() {
  const QString start = m_ini.value(QStringLiteral("last_dir"), QFileInfo(SelectedDisc()).absolutePath()).toString();
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Select a File"), start,
      tr("Metroid Prime disc images (*.iso *.gcm *.nkit *.rvz *.ciso *.wbfs);;All Files (*)"));
  if (path.isEmpty()) {
    return;
  }
  m_ini.setValue(QStringLiteral("last_dir"), QFileInfo(path).absolutePath());
  SetSelectedDisc(path);
}

void LauncherWindow::ShowGameOptions() {
  const QString path = SelectedDisc();
  QMenu menu(this);
  menu.addAction(tr("Open Containing Folder"), this,
                 [path] { OpenFolder(QFileInfo(path).absolutePath()); });
  menu.addAction(tr("Open User Folder"), this, [this] { OpenFolder(m_paths.userFolder); });
  menu.addAction(tr("Open Memory Card Folder"), this,
                 [this] { OpenFolder(CardFolderFor(m_paths, IsPalDisc(SelectedDisc()))); });
  menu.addSeparator();
  menu.addAction(tr("Forget Selected Game"), this, [this] { SetSelectedDisc({}); });
  menu.exec(m_optionsButton->mapToGlobal(QPoint(0, m_optionsButton->height())));
}

void LauncherWindow::ShowSetupNotes() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Setup Notes"));
  dialog.setModal(true);
  dialog.resize(680, 500);
  dialog.setMinimumSize(520, 360);
  dialog.setStyleSheet(Style::kPage);

  auto* layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(16, 14, 16, 14);
  layout->setSpacing(12);
  auto* title = new QLabel(tr("Setup Notes"), &dialog);
  title->setObjectName(QStringLiteral("PrimedGunSection"));
  layout->addWidget(title);

  auto* scroll = new QScrollArea(&dialog);
  scroll->setWidgetResizable(true);
  auto* content = new QWidget(scroll);
  auto* contentLayout = new QVBoxLayout(content);
  contentLayout->setContentsMargins(14, 12, 14, 12);
  auto* notes = new QLabel(
      tr(R"(<ul style="margin: 0; padding-left: 22px;">
<li>Select your Metroid Prime USA 1.00/1.01/1.02 or PAL (experimental) disc image (ISO, NKit ISO, RVZ or CISO), then press Play.</li>
<li>Check the Layout tab for controller bindings.</li>
<li>To bring over your PrimedGun saves and settings, use Transfer PrimedGun Memory Card / Settings on this tab. Select your game first: a PAL disc uses the EUR card.</li>
<li>Save states do not carry over from PrimedGun on Dolphin. Make sure to save normally before you transfer.</li>
<li>Once in game, click the right stick to set your height.</li>
<li>Settings changed here while the game runs apply in game at once (Enable VR waits for the next start). Changes made in game with F1 or the headset's menu show here too.</li>
<li>Use Save Settings after changing PrimedGun options; Play saves them too.</li>
</ul>)"),
      content);
  notes->setTextFormat(Qt::RichText);
  notes->setTextInteractionFlags(Qt::TextSelectableByMouse);
  notes->setWordWrap(true);
  notes->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  contentLayout->addWidget(notes);
  contentLayout->addStretch();
  scroll->setWidget(content);
  layout->addWidget(scroll, 1);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  dialog.exec();
}

void LauncherWindow::TransferOldSave() {
  const QString title = tr("Transfer Old Memory Card");
  if (m_game != nullptr) {
    QMessageBox::warning(this, title, tr("Stop the game before transferring a memory card save."));
    return;
  }

  // The card of the selected disc's region, the one the game mounts for it. A
  // PAL disc's game reads only GM8P saves on the EUR card, so they come from
  // Dolphin's EUR card (GC/EUR/Card A, MemoryCardA.EUR.raw). PortGci copies
  // only the game code set here, and FindDolphinCard looks in that region's
  // folders. USA when no disc is selected.
  const bool pal = IsPalDisc(SelectedDisc());
  const char* const region = pal ? "EUR" : "USA";
  const QString discRegion = pal ? QStringLiteral("PAL") : QStringLiteral("USA");
  PortGci::SetGameCode(pal ? "GM8P" : "GM8E");

  QMessageBox prompt(this);
  prompt.setWindowTitle(title);
  prompt.setText(tr("Transfer old save game."));
  prompt.setInformativeText(
      tr("PrimedGun will search nearby old PrimedGun folders and transfer your save and PrimedGun "
         "settings to this install. Choose Folder picks the old PrimedGun folder yourself. Save "
         "states are not transferred."));
  QPushButton* transfer = prompt.addButton(tr("Transfer"), QMessageBox::AcceptRole);
  QPushButton* choose = prompt.addButton(tr("Choose Folder..."), QMessageBox::ActionRole);
  prompt.addButton(QMessageBox::Cancel);
  prompt.exec();

  OldCard card;
  if (prompt.clickedButton() == choose) {
    const QString folder = QFileDialog::getExistingDirectory(
        this, tr("Select your old PrimedGun folder"), m_paths.exeFolder);
    if (folder.isEmpty()) {
      return;
    }
    card = FindOldCardUnder(ToPath(folder), region);
    if (!card.Found()) {
      QMessageBox::critical(
          this, title, tr("No memory card for a %1 disc was found in that folder.").arg(discRegion));
      return;
    }
  } else if (prompt.clickedButton() == transfer) {
    card = FindNearbyOldCard(ToPath(m_paths.exeFolder), region);
    if (!card.Found()) {
      const PortGci::DolphinCard dolphin = PortGci::FindDolphinCard();
      if (dolphin.Found()) {
        const std::filesystem::path found =
            !dolphin.rawImage.empty() ? dolphin.rawImage : dolphin.gciFolder;
        const auto answer = QMessageBox::question(
            this, title,
            tr("No old PrimedGun folder was found near this install. Dolphin's own memory card "
               "was found at:\n%1\n\nTransfer that one?")
                .arg(QDir::toNativeSeparators(FromPath(found))));
        if (answer != QMessageBox::Yes) {
          return;
        }
        card.path = found;
        card.isFolder = dolphin.rawImage.empty();
      } else {
        QMessageBox::critical(
            this, title,
            tr("Could not find an old memory card for a %1 disc near this PrimedGun install.\n\n"
               "Place the new version near your old PrimedGun folder, or use Choose Folder to "
               "pick it.")
                .arg(discRegion));
        return;
      }
    }
  } else {
    return;
  }

  const QString cardFolder = CardFolderFor(m_paths, pal);
  const std::filesystem::path destination = ToPath(cardFolder);
  PortGci::Report report;
  if (card.isFolder) {
    report = PortGci::ImportFolder(card.path, destination);
  } else {
    const QString scratch = QDir::tempPath() + QStringLiteral("/primedgun_card_import");
    QDir().mkpath(scratch);
    report = PortGci::ImportFile(card.path, destination, ToPath(scratch));
  }
  const QString summary = QString::fromStdString(report.Summary("Transferred"));
  if (report.copied == 0) {
    QMessageBox::critical(this, title,
                          tr("No Metroid Prime save was transferred from:\n%1\n\n%2")
                              .arg(QDir::toNativeSeparators(FromPath(card.path)), summary));
    return;
  }

  const OldSettings old = ReadOldSettings(card.UserFolder());
  for (const auto& [key, value] : old.values) {
    if (key != "vr_cannon_texture_slot") {
      m_model.Set(key, value);
    }
  }
  for (const auto& [key, value] : old.values) {
    if (key == "vr_cannon_texture_slot") {
      // The slot is applied as files, so it is recorded at once.
      const int slot = std::atoi(value.c_str());
      std::string error;
      if (Cannon::ApplySlot(m_paths.cannon, slot, error)) {
        m_model.Set(key, value);
        SaveKeyNow(key);
      }
    }
  }
  m_ctx.RefreshAll();
  UpdateFooter();
  const QString oldGame = QString::fromStdString(old.gamePath);
  // The old launcher's disc only when its region's card is the one just filled:
  // a PAL disc would mount the EUR card, without these saves.
  if (SelectedDisc().isEmpty() && !oldGame.isEmpty() && QFileInfo::exists(oldGame) &&
      IsPalDisc(oldGame) == pal) {
    SetSelectedDisc(oldGame);
  }

  QString message = tr("Old memory card transferred from:\n%1\nto:\n%2\n\n%3")
                        .arg(QDir::toNativeSeparators(FromPath(card.path)),
                             QDir::toNativeSeparators(cardFolder), summary);
  if (!old.source.empty()) {
    message += tr("\n\nPrimedGun settings transferred from:\n%1\nUse Save Settings to keep them.")
                   .arg(QDir::toNativeSeparators(FromPath(old.source)));
  } else {
    message += tr("\n\nNo old PrimedGun settings were found next to that memory card.");
  }
  QMessageBox::information(this, title, message);
}

// --- the game --------------------------------------------------------------

void LauncherWindow::OpenPortOptions() {
  if (m_game == nullptr) {
    Play(true);
    return;
  }
  if (!SaveSettings()) {
    return;
  }
  QSaveFile request(QDir(m_paths.userFolder).filePath(QStringLiteral("open_options.request")));
  if (!request.open(QIODevice::WriteOnly) || request.write("open\n") != 5 || !request.commit()) {
    QMessageBox::warning(this, tr("Port Options"), tr("Could not ask the running game to open its options."));
  }
}

void LauncherWindow::Play(bool optionsOnly) {
  const QString disc = SelectedDisc();
  if (m_game != nullptr || (!optionsOnly && disc.isEmpty())) {
    return;
  }
  if (!QFileInfo::exists(m_paths.gameExe)) {
    QMessageBox::critical(this, tr("Play"),
                          tr("The game was not found:\n%1")
                              .arg(QDir::toNativeSeparators(m_paths.gameExe)));
    return;
  }
  if (!SaveSettings()) {
    return;
  }

  m_game = new QProcess(this);
  m_game->setProgram(m_paths.gameExe);
  // The first argument that is not an option is the disc (platform/main.cpp
  // ResolveDiscPath); Qt quotes it, and SDL reads the wide command line.
  m_game->setArguments(optionsOnly ? QStringList{QStringLiteral("--options")}
                                   : QStringList{QDir::toNativeSeparators(disc)});
  m_game->setWorkingDirectory(m_paths.exeFolder);
  // The game logs to stderr all along; a file needs no reader.
  m_game->setProcessChannelMode(QProcess::MergedChannels);
  m_game->setStandardOutputFile(m_paths.lastRunLog, QIODevice::Truncate);
#if defined(Q_OS_WIN)
  // metroid_prime_port.exe is a console program: without this, a console
  // window would open beside the game.
  m_game->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
    constexpr unsigned long kCreateNoWindow = 0x08000000;
    args->flags |= kCreateNoWindow;
  });
#endif
  connect(m_game, &QProcess::finished, this,
          [this](int exitCode, QProcess::ExitStatus status) { OnGameFinished(exitCode, status); });
  connect(m_game, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart || m_game == nullptr) {
      return;
    }
    const QString reason = m_game->errorString();
    m_game->deleteLater();
    m_game = nullptr;
    SetRunning(false);
    QMessageBox::critical(this, tr("Play"), tr("The game could not be started:\n%1").arg(reason));
  });
  m_stopRequested = false;
  m_forceStopOffered = false;
  m_game->start();
  SetRunning(true);
  if (optionsOnly) {
    m_runStatus->setText(tr("Port options open. Close the options window before pressing Play."));
  }
}

void LauncherWindow::Stop() {
  if (m_game == nullptr) {
    return;
  }
  if (m_forceStopOffered) {
    m_game->kill();
    return;
  }
  // WM_CLOSE to the game's window: the game quits through its normal exit and
  // saves its settings on the way.
  m_stopRequested = true;
  m_game->terminate();
  m_runStatus->setText(tr("Stopping..."));
  m_forceStopTimer->start(kForceStopDelayMs);
}

void LauncherWindow::OnGameFinished(int exitCode, QProcess::ExitStatus status) {
  m_forceStopTimer->stop();
  if (m_game != nullptr) {
    m_game->deleteLater();
    m_game = nullptr;
  }
  const bool stopped = m_stopRequested;
  m_stopRequested = false;
  m_forceStopOffered = false;
  m_stopButton->setText(tr("Stop"));
  // An edit not written yet stays an unsaved change.
  m_liveSaveTimer->stop();
  SetRunning(false);
  // The game rewrote port_settings.ini as it closed, with anything the F1
  // overlay changed.
  RefreshFromFile();

  if (m_closeAfterStop) {
    close();
    return;
  }
  if (!stopped && (status == QProcess::CrashExit || exitCode != 0)) {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("PrimedGun"));
    box.setText(status == QProcess::CrashExit ? tr("The game stopped unexpectedly.")
                                              : tr("The game exited with code %1.").arg(exitCode));
    box.setInformativeText(tr("The log of this run explains why."));
    QPushButton* openLog = box.addButton(tr("Open Log"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Close);
    box.exec();
    if (box.clickedButton() == openLog) {
      OpenFile(NewestLog(m_paths));
    }
  }
}

void LauncherWindow::SetRunning(bool running) {
  const bool haveDisc = !SelectedDisc().isEmpty();
  const bool haveGame = QFileInfo::exists(m_paths.gameExe);
  m_selectButton->setEnabled(!running);
  m_playButton->setEnabled(!running && haveDisc && haveGame);
  m_stopButton->setEnabled(running);
  m_optionsButton->setEnabled(haveDisc);
  // The game has the memory card open.
  m_transferButton->setEnabled(!running);
  if (running) {
    if (!m_stopRequested) {
      m_runStatus->setObjectName(QStringLiteral("PrimedGunGood"));
      m_runStatus->setText(tr("Running. Settings you change apply in game at once."));
    }
  } else if (!haveGame) {
    m_runStatus->setObjectName(QStringLiteral("PrimedGunBad"));
    m_runStatus->setText(tr("metroid_prime_port.exe was not found next to the launcher."));
  } else {
    m_runStatus->setObjectName(QStringLiteral("PrimedGunMuted"));
    m_runStatus->setText(QString());
  }
  // A new object name only restyles once the widget is polished again.
  m_runStatus->style()->unpolish(m_runStatus);
  m_runStatus->style()->polish(m_runStatus);
  m_runStatus->setVisible(!m_runStatus->text().isEmpty());
  UpdateFooter();
}

void LauncherWindow::closeEvent(QCloseEvent* event) {
  if (m_game != nullptr) {
    // QProcess would kill the game with the window; stop it properly instead.
    QMessageBox box(this);
    box.setWindowTitle(tr("PrimedGun"));
    box.setText(tr("The game is running. Stop it and close?"));
    QPushButton* stop = box.addButton(tr("Stop and Close"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == stop) {
      m_closeAfterStop = true;
      Stop();
    }
    event->ignore();
    return;
  }
  if (m_model.Dirty()) {
    const auto answer = QMessageBox::question(
        this, tr("PrimedGun"), tr("Save your changed settings before closing?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Cancel || (answer == QMessageBox::Save && !SaveSettings())) {
      event->ignore();
      return;
    }
  }
  m_ini.setValue(QStringLiteral("geometry"), saveGeometry());
  m_ini.sync();
  event->accept();
}

} // namespace PrimedGunLauncher
