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

#include "PresetStore.h"

#include "AtomicJson.h"
#include "SchemaVersion.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

namespace loftail {

namespace {
constexpr auto kKeySchema = "schemaVersion";
constexpr auto kKeyKind = "kind";
constexpr auto kKeyPresets = "presets";
constexpr auto kKeyName = "name";
constexpr auto kKeyContent = "content";
} // namespace

QString PresetStore::defaultDir()
{
    // AppConfigLocation resolves from the org/app name set in main() — no hardcoded
    // paths (CLAUDE.md). Presets are global, so a single directory, not per file.
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString PresetStore::kindToString(Kind kind)
{
    return kind == Kind::Filters ? QStringLiteral("filters")
                                 : QStringLiteral("highlighters");
}

bool PresetStore::kindFromString(const QString &s, Kind *kind)
{
    if (s == QLatin1String("filters")) {
        if (kind)
            *kind = Kind::Filters;
        return true;
    }
    if (s == QLatin1String("highlighters")) {
        if (kind)
            *kind = Kind::Highlighters;
        return true;
    }
    return false;
}

namespace {

// ONE STEP PER VERSION, APPLIED IN ORDER — SchemaVersion.h's rule. Empty today: v1 is the
// only version there has ever been. `presets` is the name -> content map, which is the
// object both the collection file and an exported file hold their content in, so one step
// serves both directions.
void migratePresets(QJsonObject &presets, int from)
{
    int v = from;
    // for (; v < 2; ++v) { ...v1 -> v2... }
    Q_UNUSED(v);
    Q_UNUSED(presets);
}

} // namespace

QString PresetStore::fileFor(Kind kind) const
{
    const QString base = kind == Kind::Filters ? QStringLiteral("filter-presets.json")
                                               : QStringLiteral("highlighter-presets.json");
    return QDir(m_dir).filePath(base);
}

QJsonObject PresetStore::readCollection(Kind kind) const
{
    const QString path = fileFor(kind);
    bool ok = false;
    const QJsonDocument doc = AtomicJson::read(path, &ok);
    if (!ok || !doc.isObject())
        return QJsonObject();
    const QJsonObject root = doc.object();
    // `>` AND NOT `!=`, WHICH IS THE WHOLE OF WHAT "a preset shared today still imports
    // after the format evolves" REQUIRES (see the header, which has claimed it since M5).
    // An older collection is read and migrated forward; only a later one is stood off —
    // and stood off by writeCollection() as well, since every mutation here is
    // read-fold-write and would otherwise replace it with what this build could not read.
    int version = 0;
    switch (Schema::judge(root, kSchemaVersion, &version)) {
    case Schema::Verdict::Usable:
        break;
    case Schema::Verdict::Unstamped:
    case Schema::Verdict::FromFuture:
        return QJsonObject();
    }

    QJsonObject presets = root.value(QLatin1String(kKeyPresets)).toObject();
    if (version < kSchemaVersion) {
        Schema::backupOnce(path, version);
        migratePresets(presets, version);
    }
    return presets;
}

bool PresetStore::writeCollection(Kind kind, const QJsonObject &presets)
{
    if (Schema::fileIsFromFuture(fileFor(kind), kSchemaVersion))
        return false;

    QJsonObject root;
    root.insert(QLatin1String(kKeySchema), kSchemaVersion);
    root.insert(QLatin1String(kKeyKind), kindToString(kind));
    root.insert(QLatin1String(kKeyPresets), presets);
    return AtomicJson::write(fileFor(kind), QJsonDocument(root));
}

QStringList PresetStore::names(Kind kind) const
{
    QStringList out = readCollection(kind).keys();
    out.sort(Qt::CaseInsensitive);
    return out;
}

QJsonObject PresetStore::preset(Kind kind, const QString &name) const
{
    return readCollection(kind).value(name).toObject();
}

bool PresetStore::save(Kind kind, const QString &name, const QJsonObject &content)
{
    if (name.isEmpty())
        return false;
    QJsonObject presets = readCollection(kind);
    presets.insert(name, content); // replaces any existing preset of that name (§9)
    return writeCollection(kind, presets);
}

bool PresetStore::remove(Kind kind, const QString &name)
{
    QJsonObject presets = readCollection(kind);
    if (!presets.contains(name))
        return true; // already gone
    presets.remove(name);
    return writeCollection(kind, presets);
}

bool PresetStore::rename(Kind kind, const QString &from, const QString &to)
{
    if (to.isEmpty())
        return false;
    QJsonObject presets = readCollection(kind);
    if (!presets.contains(from))
        return false;
    const QJsonValue content = presets.value(from);
    presets.remove(from);
    presets.insert(to, content);
    return writeCollection(kind, presets);
}

bool PresetStore::exportPreset(Kind kind, const QString &name, const QString &file) const
{
    const QJsonObject content = preset(kind, name);
    if (content.isEmpty() && !readCollection(kind).contains(name))
        return false;

    QJsonObject root;
    root.insert(QLatin1String(kKeySchema), kSchemaVersion);
    root.insert(QLatin1String(kKeyKind), kindToString(kind));
    root.insert(QLatin1String(kKeyName), name);
    root.insert(QLatin1String(kKeyContent), content);
    return AtomicJson::write(file, QJsonDocument(root));
}

bool PresetStore::importPreset(const QString &file, Kind *kindOut, QString *nameOut)
{
    bool ok = false;
    const QJsonDocument doc = AtomicJson::read(file, &ok);
    if (!ok || !doc.isObject())
        return false;
    const QJsonObject root = doc.object();
    // An EXPORTED file is the one thing here that travels between installations, so it is
    // the one most likely to arrive stamped below this build — which is exactly the case
    // the exact-version test refused. No backup: this file is the user's own, named by
    // them in a file dialog, and importing does not write to it.
    int version = 0;
    if (Schema::judge(root, kSchemaVersion, &version) != Schema::Verdict::Usable)
        return false;

    Kind kind;
    if (!kindFromString(root.value(QLatin1String(kKeyKind)).toString(), &kind))
        return false;
    const QString name = root.value(QLatin1String(kKeyName)).toString();
    if (name.isEmpty())
        return false;
    QJsonObject content = root.value(QLatin1String(kKeyContent)).toObject();
    if (version < kSchemaVersion) {
        // Migrated as a one-entry collection, through the same step the collection file
        // takes, so an imported preset and a stored one of the same age cannot diverge.
        QJsonObject one;
        one.insert(name, content);
        migratePresets(one, version);
        content = one.value(name).toObject();
    }

    if (kindOut)
        *kindOut = kind;
    if (nameOut)
        *nameOut = name;
    return save(kind, name, content);
}

} // namespace loftail
