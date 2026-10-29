// SPDX-License-Identifier: GPL-3.0-or-later
#include "update_release.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <cstdio>
#include <cstdlib>
using namespace PrimedGunLauncher::Updates;
void Check(bool b,const char* why) { if (!b) { std::fprintf(stderr,"updater: %s\n",why); std::abort(); } }
QJsonObject Make(QString version, bool preview=true, QString suffix="Windows-x64.zip") {
  QString name="PrimedGun-v"+version+"-"+suffix;
  return {{"tag_name","v"+version},{"prerelease",preview},{"assets",QJsonArray{QJsonObject{
    {"name",name},{"state","uploaded"},{"size",1234},{"digest","sha256:"+QString(64,'a')},
    {"browser_download_url","https://github.com/iChris4/MetroidPrimePortVR/releases/download/v"+version+"/"+name}}}}};
}
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);
  Check(CompareVersions("v2.0.0-beta.10","2.0.0-beta.9")>0,"numeric beta ordering");
  Check(CompareVersions("2.0.0","2.0.0-rc.99")>0,"stable newer than rc");
  Check(CompareVersions("2.0.0-rc.1","2.0.0-beta.99")>0,"rc newer than beta");
  Check(CompareVersions("2.0.0-beta.1","2.0.0-alpha.99")>0,"beta newer than alpha");
  Check(CompareVersions("2.1.0-alpha.1","2.0.9")>0,"major/minor before channel");
  Check(CompareVersions("v2.0.0-beta.6","2.0.0-beta.6")==0,"ignore v prefix");
  Check(!ValidVersion("v2.0.0-beta.06")&&!ValidVersion("garbage"),"invalid tags rejected");
  QJsonArray releases{Make("2.0.0-beta.7"),Make("2.0.0-beta.10"),Make("2.0.0-beta.8"),Make("2.0.0-beta.11",true,"Windows-x64-symbols.zip")};
  Check(Select(releases,"2.0.0-beta.6",true).version=="2.0.0-beta.10","highest ready platform release, not API order or symbols");
  Check(!Select(releases,"2.0.0-beta.6",false).valid(),"stable channel excludes previews");
  Check(!Select(releases,"2.0.0-beta.10",true).valid(),"never downgrade or reinstall");
  releases.append(Make("2.0.0",false));
  Check(Select(releases,"2.0.0-beta.6",false).version=="2.0.0","stable channel accepts final");
  auto draft=Make("3.0.0",false); draft["draft"]=true;
  Check(!Select({draft},"2.0.0",true).valid(),"draft ignored");
  auto bad=Make("3.0.0",false); auto assets=bad["assets"].toArray(); auto asset=assets[0].toObject();
  asset["browser_download_url"]="https://example.com/bad.zip"; assets[0]=asset; bad["assets"]=assets;
  Check(!Select({bad},"2.0.0",true).valid(),"foreign download rejected");
  auto digest=QString(64,'a').toUtf8();
  Check(Checksum(digest+"  file.zip\r\n","file.zip")==QString(64,'a'),"exact checksum match");
  Check(Checksum(digest+" *file.zip\n","file.zip")==QString(64,'a'),"binary checksum syntax");
  Check(Checksum(digest+"  other.zip\n","file.zip").isEmpty(),"wrong asset checksum rejected");
  Check(Checksum(digest+"  file.zip\n"+digest+"  file.zip\n","file.zip").isEmpty(),"duplicate checksum rejected");
  if (argc>1) {
    QFile file(QString::fromLocal8Bit(argv[1]));
    Check(file.open(QIODevice::ReadOnly),"live release file readable");
    const auto r=Select(QJsonDocument::fromJson(file.readAll()).array(),"2.0.0-beta.4",true);
    Check(r.valid()&&!r.digest.isEmpty(),"published GitHub schema/asset/digest compatible");
  }
  std::puts("Update version, release selection and checksum checks passed.");
}
