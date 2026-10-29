// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonArray>
#include <QString>
namespace PrimedGunLauncher::Updates {
struct Release {
  QString version, notes, page, name, url, digest, checksumUrl;
  qint64 size = 0;
  bool valid() const { return !url.isEmpty(); }
};
int CompareVersions(QString a, QString b);
bool ValidVersion(const QString& text);
bool TrustedAsset(const QString& url);
Release Select(const QJsonArray& releases, const QString& current, bool previews);
QString Checksum(const QByteArray& text, const QString& name);
}
