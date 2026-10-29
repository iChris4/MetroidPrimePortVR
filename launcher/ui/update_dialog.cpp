// SPDX-License-Identifier: GPL-3.0-or-later
#include "update_dialog.h"
#include "update_release.h"
#include "tab_context.h"
#include <QApplication>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
namespace PrimedGunLauncher {
namespace {
class UpdateDialog final : public QDialog {
  TabContext& ctx;
  QLabel* status;
  QCheckBox* previews;
  QPushButton *check, *action;
  QTextBrowser* notes;
  QProgressBar* progress;
  QProcess* transfer = nullptr;
  Updates::Release release;
  QString work, archive;
  bool verified = false;
  bool busy = false;
  void Error(const QString& message) {
    busy=false; status->setText(message); check->setEnabled(true);
    previews->setEnabled(true); action->setEnabled(release.valid()); progress->hide();
  }
  void Fetch(const QString& url, const QString& output, qint64 maximum,
             std::function<void()> done) {
    busy=true; check->setEnabled(false); previews->setEnabled(false); action->setEnabled(false);
    progress->setRange(0,0); progress->show();
    transfer=new QProcess(this);
#ifdef Q_OS_WIN
    const QString curl=QDir(qEnvironmentVariable("SystemRoot", "C:/Windows")).filePath("System32/curl.exe");
#else
    const QString curl=QStringLiteral("curl");
#endif
    auto* process=transfer;
    process->setProgram(curl);
    process->setArguments({"--fail","--location","--proto","=https","--proto-redir","=https",
        "--connect-timeout","15","--max-time",maximum>8388608 ? "900" : "45",
        "--max-filesize",QString::number(maximum),"--silent","--show-error",
        "--user-agent","PrimedGun-Updater/" PRIMEDGUN_LAUNCHER_VERSION,
        "--header","Accept: application/vnd.github+json","--output",output,"--url",url});
    connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError error) {
      if (error==QProcess::FailedToStart) { transfer=nullptr; process->deleteLater(); Error(tr("Could not start the system download client: %1").arg(process->errorString())); }
    });
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,
      [this,process,output,maximum,done](int code,QProcess::ExitStatus state) {
        transfer=nullptr;
        const auto detail=QString::fromUtf8(process->readAllStandardError()).left(600);
        process->deleteLater();
        if (code!=0 || state!=QProcess::NormalExit || QFileInfo(output).size()>maximum) {
          Error(tr("Update download failed. Check your connection and try again. %1").arg(detail)); return;
        }
        busy=false; progress->hide(); done();
      });
    process->start();
  }
  void Offer() {
    busy=false; check->setEnabled(true); previews->setEnabled(true); progress->hide();
    if (!release.valid()) { status->setText(tr("You are up to date for the selected release channel.")); return; }
    status->setText(tr("PrimedGun %1 is available (%2 MB).").arg(release.version).arg(release.size/1048576.0,0,'f',1));
    notes->setPlainText(release.notes);
    action->setText(tr("Download update")); action->setEnabled(true);
  }
  void Check() {
    release={}; verified=false; action->setEnabled(false); notes->clear();
    status->setText(tr("Checking GitHub releases..."));
    QSettings prefs(ctx.paths.launcherIni,QSettings::IniFormat);
    prefs.setValue("updates/include_previews",previews->isChecked());
    Fetch("https://api.github.com/repos/iChris4/MetroidPrimePortVR/releases?per_page=100",
      work+"/releases.json",8388608,[this] {
        QFile file(work+"/releases.json"); if (!file.open(QIODevice::ReadOnly)) { Error(tr("Could not read release information.")); return; }
        QJsonParseError error; auto doc=QJsonDocument::fromJson(file.readAll(),&error);
        if (error.error!=QJsonParseError::NoError || !doc.isArray()) { Error(tr("GitHub returned invalid release information.")); return; }
        release=Updates::Select(doc.array(),QStringLiteral(PRIMEDGUN_LAUNCHER_VERSION),previews->isChecked());
        Offer();
      });
  }
  void Download() {
    status->setText(tr("Downloading %1...").arg(release.name));
    archive=work+"/update.zip";
    Fetch(release.url,archive,release.size,[this] {
      status->setText(tr("Verifying SHA-256 checksum..."));
      QFile file(archive); QCryptographicHash hash(QCryptographicHash::Sha256);
      if (!file.open(QIODevice::ReadOnly) || file.size()!=release.size || !hash.addData(&file) ||
          QString::fromLatin1(hash.result().toHex())!=release.digest.toLower()) {
        Error(tr("Update verification failed. Nothing has been installed. Check for updates to retry."));
        action->setEnabled(false); return;
      }
      verified=true; busy=false; check->setEnabled(true); previews->setEnabled(true);
      status->setText(tr("Download verified. Close the game before installing. Your user folder will be preserved."));
      action->setText(tr("Install and restart launcher")); action->setEnabled(true);
    });
  }
  void StartDownload() {
    if (!release.digest.isEmpty()) { Download(); return; }
    if (release.checksumUrl.isEmpty()) { Error(tr("This release has no SHA-256 checksum. Open its release page to update manually.")); action->setEnabled(false); return; }
    status->setText(tr("Downloading checksums..."));
    Fetch(release.checksumUrl,work+"/checksums.txt",1048576,[this] {
      QFile f(work+"/checksums.txt");
      if (f.open(QIODevice::ReadOnly)) release.digest=Updates::Checksum(f.readAll(),release.name);
      if (release.digest.isEmpty()) { Error(tr("No unambiguous checksum was found for this Windows package.")); action->setEnabled(false); return; }
      Download();
    });
  }
  void Install() {
#ifdef Q_OS_WIN
    if (!ctx.prepareUpdate || !ctx.prepareUpdate()) return;
    QFile script(":/update-windows.ps1");
    if (!script.copy(work+"/apply-update.ps1")) { Error(tr("Could not prepare the update helper. Check for updates to retry.")); return; }
    QProcess updater;
    updater.setProgram(QDir(qEnvironmentVariable("SystemRoot", "C:/Windows")).filePath("System32/WindowsPowerShell/v1.0/powershell.exe"));
    updater.setArguments({"-NoProfile","-NonInteractive","-ExecutionPolicy","Bypass","-File",work+"/apply-update.ps1",
      "-Archive",archive,"-Destination",ctx.paths.exeFolder,"-LauncherPid",QString::number(QCoreApplication::applicationPid()),
      "-ExpectedHash",release.digest});
    updater.setWorkingDirectory(work);
    updater.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
    if (!updater.startDetached()) { Error(tr("Could not start the updater. Your installation has not changed.")); return; }
    QApplication::quit();
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(work));
#endif
  }
public:
  explicit UpdateDialog(TabContext& context):QDialog(context.window),ctx(context) {
    setAttribute(Qt::WA_DeleteOnClose); setWindowTitle(tr("PrimedGun updates")); resize(620,500);
    work=QStandardPaths::writableLocation(QStandardPaths::TempLocation)+"/PrimedGun-update-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(work);
    auto* layout=new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Installed: %1").arg(QStringLiteral(PRIMEDGUN_LAUNCHER_VERSION))));
    previews=new QCheckBox(tr("Include alpha, beta and release candidate versions"));
    QSettings prefs(ctx.paths.launcherIni,QSettings::IniFormat);
    previews->setChecked(prefs.value("updates/include_previews",QStringLiteral(PRIMEDGUN_LAUNCHER_VERSION).contains('-')).toBool());
    layout->addWidget(previews); check=new QPushButton(tr("Check for updates")); layout->addWidget(check);
    status=new QLabel; status->setWordWrap(true); layout->addWidget(status);
    progress=new QProgressBar; progress->hide(); layout->addWidget(progress);
    notes=new QTextBrowser; notes->setOpenExternalLinks(false); layout->addWidget(notes,1);
    action=new QPushButton(tr("Download update")); action->setEnabled(false); layout->addWidget(action);
    auto* page=new QPushButton(tr("Open GitHub releases")); layout->addWidget(page);
    connect(page,&QPushButton::clicked,this,[this] { QDesktopServices::openUrl(QUrl(release.valid()?release.page:"https://github.com/iChris4/MetroidPrimePortVR/releases")); });
    connect(check,&QPushButton::clicked,this,[this] { Check(); });
    connect(action,&QPushButton::clicked,this,[this] { if (verified) Install(); else StartDownload(); });
    QTimer::singleShot(0,this,[this] { Check(); });
  }
  ~UpdateDialog() override { if (transfer) { transfer->kill(); transfer->waitForFinished(1000); } }
};
}
void CheckForUpdates(TabContext& ctx) {
  static QPointer<UpdateDialog> current;
  if (current) { current->raise(); current->activateWindow(); return; }
  current=new UpdateDialog(ctx); current->show();
}
}
