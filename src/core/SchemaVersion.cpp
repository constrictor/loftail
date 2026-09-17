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


#include "SchemaVersion.h"

#include "AtomicJson.h"

#include <QFile>
#include <QJsonValue>

namespace loftail {

namespace {
// The key every store stamps. Never translated (ARCHITECTURE.md §9.1), and spelled
// here once so that a store cannot drift from the helper that reads it.
constexpr auto kSchemaVersionKey = "schemaVersion";
} // namespace

int Schema::versionOf(const QJsonObject &root)
{
    const QJsonValue v = root.value(QLatin1String(kSchemaVersionKey));
    // isDouble() rather than toInt()'s own default, so a stamp written as a string —
    // which a hand edit produces readily — is Unstamped rather than 0-by-coincidence.
    // They reach the same verdict today; they would stop doing so the moment anything
    // wanted to tell "damaged" from "absent".
    if (!v.isDouble())
        return 0;
    const int n = v.toInt(0);
    return n > 0 ? n : 0;
}

Schema::Verdict Schema::judge(int version, int current)
{
    if (version <= 0)
        return Verdict::Unstamped;
    if (version > current)
        return Verdict::FromFuture;
    return Verdict::Usable;
}

Schema::Verdict Schema::judge(const QJsonObject &root, int current, int *version)
{
    const int v = versionOf(root);
    if (version)
        *version = v;
    return judge(v, current);
}

bool Schema::fileIsFromFuture(const QString &path, int current)
{
    if (path.isEmpty())
        return false;
    bool ok = false;
    const QJsonDocument doc = AtomicJson::read(path, &ok);
    if (!ok || !doc.isObject())
        return false;
    return judge(doc.object(), current) == Verdict::FromFuture;
}

QString Schema::backupPathFor(const QString &path, int from)
{
    return path + QStringLiteral(".v%1.bak").arg(from);
}

bool Schema::backupOnce(const QString &path, int from)
{
    if (path.isEmpty() || from <= 0)
        return false;
    const QString backup = backupPathFor(path, from);
    // NEVER OVERWRITTEN. The backup is of the state this build found, and a later
    // launch that finds the same old file again would copy the same bytes; but a
    // launch that finds a file this build has since migrated and something else has
    // since damaged would copy the damage over the good copy. Existing wins.
    if (QFile::exists(backup))
        return true;
    return QFile::copy(path, backup);
}

} // namespace loftail
