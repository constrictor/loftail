// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SerialProfileStore.h"

#include "AtomicJson.h"
#include "SchemaVersion.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>

namespace loftail {
namespace {

constexpr auto kFileName = "serial.json";
constexpr auto kSchemaKey = "schemaVersion";
constexpr auto kPresetsKey = "presets";
constexpr auto kDevicesKey = "devices";

QJsonObject deviceToJson(const SerialDeviceBookmark &d)
{
    QJsonObject o;
    o[QStringLiteral("id")] = d.id;
    if (!d.label.isEmpty())
        o[QStringLiteral("label")] = d.label;
    QJsonArray paths;
    for (const QString &p : d.paths)
        paths.append(p);
    o[QStringLiteral("paths")] = paths;
    return o;
}

SerialDeviceBookmark deviceFromJson(const QJsonObject &o)
{
    SerialDeviceBookmark d;
    d.id = o.value(QStringLiteral("id")).toString();
    d.label = o.value(QStringLiteral("label")).toString();
    const QJsonArray paths = o.value(QStringLiteral("paths")).toArray();
    for (const QJsonValue &v : paths) {
        const QString p = v.toString();
        if (!p.isEmpty())
            d.paths.append(p);
    }
    return d;
}

QJsonDocument readDocument(const QString &path)
{
    bool ok = false;
    const QJsonDocument doc = AtomicJson::read(path, &ok);
    if (!ok)
        return {};
    return doc;
}

} // namespace

QString SerialProfileStore::defaultDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString SerialProfileStore::filePath() const
{
    if (m_dir.isEmpty())
        return {};
    return m_dir + u'/' + QLatin1String(kFileName);
}

bool SerialProfileStore::sameName(const QString &a, const QString &b)
{
    return a.trimmed().compare(b.trimmed(), Qt::CaseInsensitive) == 0;
}

int SerialProfileStore::indexOfName(const QVector<SerialProfile> &presets, const QString &name)
{
    for (int i = 0; i < presets.size(); ++i) {
        if (sameName(presets.at(i).name, name))
            return i;
    }
    return -1;
}

QVector<SerialProfile> SerialProfileStore::presets() const
{
    QVector<SerialProfile> out;
    const QJsonObject root = readDocument(filePath()).object();
    // THE THREE-WAY JUDGEMENT AND NEVER `!=` (SchemaVersion.h). This store was written with the
    // folded two-way test, which is the very defect that header records for HostBookmarkStore and
    // PresetStore: `!=` reads "written by a newer build" and "not a file we wrote" alike, and the
    // two want opposite answers. Folded, the first bump of this store would have answered with an
    // empty list AND THEN — replacePresets() being read-fold-write — written that empty list back
    // over every preset the user has.
    if (Schema::judge(root, kSchemaVersion) != Schema::Verdict::Usable)
        return out;
    const QJsonArray presets = root.value(QLatin1String(kPresetsKey)).toArray();
    for (const QJsonValue &v : presets) {
        const SerialProfile p = serialProfileFromJson(v.toObject());
        if (p.name.trimmed().isEmpty())
            continue;   // unnameable, and therefore unpickable
        if (indexOfName(out, p.name) >= 0)
            continue;   // a duplicate from an older file: the first wins, the rest are dropped
        out.append(p);
    }
    return out;
}

QVector<SerialDeviceBookmark> SerialProfileStore::devices() const
{
    QVector<SerialDeviceBookmark> out;
    const QJsonObject root = readDocument(filePath()).object();
    if (Schema::judge(root, kSchemaVersion) != Schema::Verdict::Usable)
        return out;
    const QJsonArray devices = root.value(QLatin1String(kDevicesKey)).toArray();
    for (const QJsonValue &v : devices) {
        const SerialDeviceBookmark d = deviceFromJson(v.toObject());
        if (!d.id.isEmpty())
            out.append(d);
    }
    return out;
}

SerialProfile SerialProfileStore::presetNamed(const QString &name) const
{
    if (name.trimmed().isEmpty())
        return SerialProfile::builtIn();
    const QVector<SerialProfile> all = presets();
    const int at = indexOfName(all, name);
    // FALLS BACK RATHER THAN REFUSING. See the header: a renamed preset is an ordinary state,
    // and the caller logs the fall-back rather than failing the open.
    return at >= 0 ? all.at(at) : SerialProfile::builtIn();
}

bool SerialProfileStore::replacePresets(const QVector<SerialProfile> &presets) const
{
    const QString path = filePath();
    if (path.isEmpty())
        return false;
    // THE WRITE HALF, AND THE HALF THAT IS EASY TO LEAVE OUT. Declining to READ a file from the
    // future is not enough: this store is const and re-reads the file on every call, so every
    // mutation is read-fold-write — and over a file this build refused to read, that means writing
    // an empty list over somebody's whole configuration. Asked of the file itself, so it holds
    // however the caller arrived (SchemaVersion.h).
    if (Schema::fileIsFromFuture(path, kSchemaVersion))
        return false;
    QJsonObject root = readDocument(path).object();
    root[QLatin1String(kSchemaKey)] = kSchemaVersion;
    QJsonArray array;
    for (const SerialProfile &p : presets)
        array.append(serialProfileToJson(p));
    root[QLatin1String(kPresetsKey)] = array;
    return AtomicJson::write(path, QJsonDocument(root));
}

bool SerialProfileStore::savePreset(const SerialProfile &preset) const
{
    QVector<SerialProfile> all = presets();
    const int at = indexOfName(all, preset.name);
    if (at >= 0)
        all[at] = preset;   // replaced IN PLACE, so the list order does not shift under the user
    else
        all.append(preset);
    return replacePresets(all);
}

bool SerialProfileStore::removePreset(const QString &name) const
{
    QVector<SerialProfile> all = presets();
    const int at = indexOfName(all, name);
    if (at < 0)
        return true;   // nothing to do, which is not a failure
    all.remove(at);
    return replacePresets(all);
}

bool SerialProfileStore::saveDevice(const SerialDeviceBookmark &device) const
{
    const QString path = filePath();
    if (path.isEmpty() || device.id.isEmpty())
        return false;
    // The second write funnel, and it needs the guard for replacePresets()' reason: the presets
    // and the devices share one file, so a device write over a future file would take the presets
    // with it.
    if (Schema::fileIsFromFuture(path, kSchemaVersion))
        return false;
    QVector<SerialDeviceBookmark> all = devices();
    int at = -1;
    for (int i = 0; i < all.size(); ++i) {
        if (all.at(i).id == device.id) {
            at = i;
            break;
        }
    }
    if (at >= 0)
        all[at] = device;
    else
        all.append(device);

    QJsonObject root = readDocument(path).object();
    root[QLatin1String(kSchemaKey)] = kSchemaVersion;
    QJsonArray array;
    for (const SerialDeviceBookmark &d : all)
        array.append(deviceToJson(d));
    root[QLatin1String(kDevicesKey)] = array;
    return AtomicJson::write(path, QJsonDocument(root));
}

bool SerialProfileStore::forgetPath(const QString &deviceId, const QString &path) const
{
    QVector<SerialDeviceBookmark> all = devices();
    for (SerialDeviceBookmark &d : all) {
        if (d.id != deviceId)
            continue;
        d.paths.removeAll(path);
        return saveDevice(d);
    }
    return true;
}

} // namespace loftail
