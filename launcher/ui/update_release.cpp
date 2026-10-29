// SPDX-License-Identifier: GPL-3.0-or-later
#include "update_release.h"
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
namespace PrimedGunLauncher::Updates {
namespace {
const QRegularExpression versionPattern(QStringLiteral(R"(^v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-(alpha|beta|rc)\.(0|[1-9][0-9]*))?$)"));
QString Canonical(QString s) { if (s.startsWith('v')) s.remove(0, 1); return s; }
}
bool ValidVersion(const QString& text) { return versionPattern.match(text).hasMatch(); }
int CompareVersions(QString a, QString b) {
  auto x = versionPattern.match(a), y = versionPattern.match(b);
  if (!x.hasMatch() || !y.hasMatch()) return 0;
  for (int i : {1,2,3,4,5}) {
    QString left = x.captured(i), right = y.captured(i);
    if (i == 4) {
      const QStringList stages{QStringLiteral("alpha"),QStringLiteral("beta"),QStringLiteral("rc"),QString()};
      int l=stages.indexOf(left), r=stages.indexOf(right);
      if (l != r) return l < r ? -1 : 1;
    } else {
      if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
      int c=QString::compare(left,right); if (c) return c < 0 ? -1 : 1;
    }
  }
  return 0;
}
bool TrustedAsset(const QString& url) {
  return url.startsWith(QStringLiteral("https://github.com/iChris4/MetroidPrimePortVR/releases/download/"));
}
Release Select(const QJsonArray& releases, const QString& current, bool previews) {
  Release best;
  for (const auto& value : releases) {
    auto r=value.toObject(); auto tag=r["tag_name"].toString();
    if (r["draft"].toBool() || !ValidVersion(tag) || CompareVersions(tag,current)<=0 ||
        (!previews && (r["prerelease"].toBool() || tag.contains('-')))) continue;
    if (best.valid() && CompareVersions(tag,best.version)<=0) continue;
    Release candidate; candidate.version=Canonical(tag); candidate.notes=r["body"].toString();
    candidate.page=QStringLiteral("https://github.com/iChris4/MetroidPrimePortVR/releases/tag/")+tag;
    const auto expected=QStringLiteral("PrimedGun-v%1-Windows-x64.zip").arg(candidate.version);
    for (const auto& asset : r["assets"].toArray()) {
      auto a=asset.toObject(); const auto url=a["browser_download_url"].toString();
      if (!TrustedAsset(url) || a["state"].toString()!=QStringLiteral("uploaded")) continue;
      if (a["name"].toString()==QStringLiteral("SHA256SUMS.txt")) candidate.checksumUrl=url;
      if (a["name"].toString()==expected) {
        candidate.name=expected; candidate.url=url; candidate.size=a["size"].toVariant().toLongLong();
        candidate.digest=a["digest"].toString();
        if (candidate.digest.startsWith(QStringLiteral("sha256:"))) candidate.digest.remove(0,7);
        else candidate.digest.clear();
        if (!QRegularExpression(QStringLiteral("^[0-9a-fA-F]{64}$")).match(candidate.digest).hasMatch()) candidate.digest.clear();
      }
    }
    if (candidate.valid() && candidate.size>0 && candidate.size<=2147483648LL) best=candidate;
  }
  return best;
}
QString Checksum(const QByteArray& text, const QString& name) {
  const QRegularExpression line(QStringLiteral(R"(^([0-9a-fA-F]{64})[ 	]+\*?(.+)$)"));
  QString found;
  for (auto s : QString::fromUtf8(text).split('\n')) {
    const auto m=line.match(s.trimmed());
    if (m.hasMatch() && m.captured(2)==name) {
      if (!found.isEmpty()) return {}; // ambiguous manifest
      found=m.captured(1).toLower();
    }
  }
  return found;
}
}
