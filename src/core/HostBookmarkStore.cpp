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

#include "HostBookmarkStore.h"

#include "AtomicJson.h"
#include "SchemaVersion.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace loftail {

namespace {

constexpr auto kFileName = "hosts.json";
constexpr auto kHostsKey = "hosts";
constexpr auto kTypesKey = "types";
constexpr auto kSchemaKey = "schemaVersion";

QString authToString(HostBookmark::Auth auth)
{
    switch (auth) {
    case HostBookmark::Auth::KeyFile:  return QStringLiteral("key");
    case HostBookmark::Auth::Password: return QStringLiteral("password");
    case HostBookmark::Auth::Agent:    break;
    }
    return QStringLiteral("agent");
}

HostBookmark::Auth authFromString(const QString &s)
{
    if (s == QLatin1String("key"))
        return HostBookmark::Auth::KeyFile;
    if (s == QLatin1String("password"))
        return HostBookmark::Auth::Password;
    return HostBookmark::Auth::Agent;
}

QJsonObject toJson(const HostBookmark &b)
{
    QJsonObject o;
    o.insert(QStringLiteral("label"), b.label);
    o.insert(QStringLiteral("user"), b.user);
    o.insert(QStringLiteral("host"), b.host);
    o.insert(QStringLiteral("port"), b.port);
    o.insert(QStringLiteral("auth"), authToString(b.auth));
    if (!b.keyFile.isEmpty())
        o.insert(QStringLiteral("keyFile"), b.keyFile);
    o.insert(QStringLiteral("pollMs"), b.pollMs);
    o.insert(QStringLiteral("tailStartBytes"), static_cast<double>(b.tailStartBytes));
    // ADDED KEY, NO SCHEMA BUMP — the tree's standing rule (CLAUDE.md, M20/M24): an older
    // binary reads an absent key as false, which is this option's default, whereas a bumped
    // version makes it refuse the whole file and forget every saved host.
    o.insert(QStringLiteral("compress"), b.compress);
    o.insert(QStringLiteral("connection"), RemoteType::connectionToString(b.connection));
    if (!b.consolePreset.isEmpty())
        o.insert(QStringLiteral("consolePreset"), b.consolePreset);
    QJsonArray paths;
    for (const QString &p : b.paths)
        paths.append(p);
    o.insert(QStringLiteral("paths"), paths);

    // The password key is written ONLY when the user asked for it. Turning the option
    // off must not leave the previous secret sitting in the file, so this is an
    // omission rather than an empty string.
    o.insert(QStringLiteral("savePassword"), b.savePassword);
    if (b.savePassword && !b.password.isEmpty())
        o.insert(QStringLiteral("password"), b.password);
    return o;
}

HostBookmark fromJson(const QJsonObject &o)
{
    HostBookmark b;
    b.label = o.value(QStringLiteral("label")).toString();
    b.user = o.value(QStringLiteral("user")).toString();
    b.host = o.value(QStringLiteral("host")).toString();
    b.port = o.value(QStringLiteral("port")).toInt(RemoteLocation::kDefaultPort);
    b.auth = authFromString(o.value(QStringLiteral("auth")).toString());
    b.keyFile = o.value(QStringLiteral("keyFile")).toString();
    b.pollMs = o.value(QStringLiteral("pollMs")).toInt(1000);
    b.tailStartBytes = static_cast<qint64>(o.value(QStringLiteral("tailStartBytes")).toDouble());
    b.compress = o.value(QStringLiteral("compress")).toBool();
    b.connection =
        RemoteType::connectionFromString(o.value(QStringLiteral("connection")).toString());
    b.consolePreset = o.value(QStringLiteral("consolePreset")).toString();
    const QJsonArray paths = o.value(QStringLiteral("paths")).toArray();
    for (const auto &v : paths) {
        if (v.isString())
            b.paths.append(v.toString());
    }
    b.savePassword = o.value(QStringLiteral("savePassword")).toBool();
    b.password = o.value(QStringLiteral("password")).toString();
    return b;
}

bool sameHost(const HostBookmark &b, const RemoteLocation &location)
{
    // THE TRANSPORT IS PART OF THE IDENTITY (M28): a machine reached over telnet and over SSH
    // is two connections with two accounts, and matching across them would prime one's saved
    // password into the other's login.
    return b.transport() == location.transport && b.user == location.user
        && b.host == location.host && b.port == location.port;
}

} // namespace

RemoteLocation HostBookmark::locationFor(const QString &path) const
{
    RemoteLocation location;
    location.user = user;
    location.host = host;
    location.port = port;
    location.path = path;
    location.transport = transport();
    return location;
}

SshFetchOptions HostBookmark::fetchOptions() const
{
    SshFetchOptions options;
    options.pollMs = pollMs > 0 ? pollMs : 1000;
    options.tailStartBytes = tailStartBytes;
    options.compress = compress;
    return options;
}

QString HostBookmarkStore::defaultDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString HostBookmarkStore::filePath() const
{
    if (m_dir.isEmpty())
        return {};
    return m_dir + u'/' + QLatin1String(kFileName);
}

namespace {

// ONE STEP PER VERSION, APPLIED IN ORDER — SchemaVersion.h's rule. Empty today: v1 is the
// only version there has ever been. It is here so the next bump has an obvious home, and
// so nothing has to invent the shape of a migration under pressure.
//
// It takes no object because a bookmark list migrates entry by entry through fromJson();
// a step that has to rewrite the whole array belongs in all() beside the loop.
void migrateBookmarks(int from)
{
    int v = from;
    // for (; v < 2; ++v) { ...v1 -> v2... }
    Q_UNUSED(v);
}

} // namespace

QVector<HostBookmark> HostBookmarkStore::all() const
{
    QVector<HostBookmark> out;
    const QString path = filePath();
    if (path.isEmpty())
        return out;

    bool ok = false;
    const QJsonDocument doc = AtomicJson::read(path, &ok);
    if (!ok || !doc.isObject())
        return out;
    const QJsonObject root = doc.object();
    // THIS USED TO BE `!=`, AND `!=` WOULD HAVE DESTROYED EVERY BOOKMARK ON EVERY
    // INSTALLATION THE FIRST TIME THE VERSION MOVED. It folded "written by a newer build"
    // together with "not a file we wrote", and the two want opposite answers: an older
    // file is read and migrated forward, a newer one is left alone — including, and
    // especially, by replaceAll(), which would otherwise fold the empty list this
    // returned into the next save and write it back over the file (SchemaVersion.h).
    int version = 0;
    switch (Schema::judge(root, kSchemaVersion, &version)) {
    case Schema::Verdict::Usable:
        break;
    case Schema::Verdict::Unstamped:
    case Schema::Verdict::FromFuture:
        return out;
    }

    if (version < kSchemaVersion) {
        // Kept before the first save rewrites the file at the new version. This one holds
        // remembered passwords where there is no keychain, so it is made private for the
        // same reason writePrivate() exists — the copy is as sensitive as the original.
        if (Schema::backupOnce(path, version))
            QFile::setPermissions(Schema::backupPathFor(path, version),
                                  QFile::ReadOwner | QFile::WriteOwner);
        migrateBookmarks(version);
    }

    const QJsonArray hosts = root.value(QLatin1String(kHostsKey)).toArray();
    out.reserve(hosts.size());
    for (const auto &v : hosts) {
        if (!v.isObject())
            continue;
        HostBookmark b = fromJson(v.toObject());
        if (b.host.isEmpty())
            continue;
        // Names are the identity, and a file written before they were — or edited by
        // hand — may repeat one. The first wins; keeping the rest would put entries in
        // the list that read alike and cannot be removed separately.
        if (HostBookmarkStore::indexOfName(out, b.displayName()) >= 0)
            continue;
        out.append(b);
    }
    return out;
}

bool HostBookmarkStore::sameName(const QString &a, const QString &b)
{
    return a.trimmed().compare(b.trimmed(), Qt::CaseInsensitive) == 0;
}

int HostBookmarkStore::indexOfName(const QVector<HostBookmark> &bookmarks, const QString &name)
{
    for (int i = 0; i < bookmarks.size(); ++i) {
        if (sameName(bookmarks.at(i).displayName(), name))
            return i;
    }
    return -1;
}

bool HostBookmarkStore::replaceAll(const QVector<HostBookmark> &bookmarks) const
{
    QJsonArray hosts;
    bool anySecret = false;
    for (const HostBookmark &b : bookmarks) {
        hosts.append(toJson(b));
        if (b.savePassword && !b.password.isEmpty())
            anySecret = true;
    }
    return writeKey(QLatin1String(kHostsKey), hosts, anySecret);
}

bool HostBookmarkStore::writeKey(const QString &key, const QJsonArray &value,
                                 bool holdsSecret) const
{
    const QString path = filePath();
    if (path.isEmpty())
        return false;

    // THE WRITE HALF, AND THE HALF A STATELESS STORE CANNOT KEEP IN A MEMBER. all() is
    // const and re-reads the file on every call, so there is nowhere to latch a verdict
    // between the read and the write — and every mutation here (save, remove, the Open
    // Remote dialog's OK) is read-fold-write, which over a file this build refused to read
    // means writing an empty list over somebody's whole configuration. Asked of the file
    // itself, so it holds however the caller arrived.
    if (Schema::fileIsFromFuture(path, kSchemaVersion))
        return false;

    // EVERY KEY THIS WRITE DOES NOT OWN IS CARRIED OVER (M28). Before `types` existed the root
    // was rebuilt from scratch, which was harmless while `hosts` was the only key — and would
    // now make saving a host delete every remote type. Only from a file this build can read:
    // an unstamped one is not ours, so nothing in it is worth keeping.
    QJsonObject root;
    bool ok = false;
    const QJsonDocument existing = AtomicJson::read(path, &ok);
    if (ok && existing.isObject()
        && Schema::judge(existing.object(), kSchemaVersion, nullptr) == Schema::Verdict::Usable) {
        root = existing.object();
    }
    root.insert(QLatin1String(kSchemaKey), kSchemaVersion);
    root.insert(key, value);

    // Owner-only whenever a password is in there — in EITHER array, so the secret test runs
    // over what is being kept as well as what is being written. Always writing it private
    // would be tidier still, but this file is also read on a shared config directory, and
    // narrowing permissions people did not ask for is its own surprise — so the restriction
    // follows the secret.
    bool secret = holdsSecret;
    for (const QString &k : {QString::fromLatin1(kHostsKey), QString::fromLatin1(kTypesKey)}) {
        if (k == key)
            continue;
        for (const auto &v : root.value(k).toArray()) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("savePassword")).toBool()
                && !o.value(QStringLiteral("password")).toString().isEmpty()) {
                secret = true;
            }
        }
    }
    const QJsonDocument doc(root);
    return secret ? AtomicJson::writePrivate(path, doc) : AtomicJson::write(path, doc);
}

QVector<RemoteType> HostBookmarkStore::types() const
{
    QVector<RemoteType> out;
    const QString path = filePath();
    if (path.isEmpty())
        return out;
    bool ok = false;
    const QJsonDocument doc = AtomicJson::read(path, &ok);
    if (!ok || !doc.isObject())
        return out;
    // Judged exactly as all() judges it, and for its reason: a newer build's file is read as
    // nothing, and writeKey() then refuses to write over it.
    if (Schema::judge(doc.object(), kSchemaVersion, nullptr) != Schema::Verdict::Usable)
        return out;
    for (const auto &v : doc.object().value(QLatin1String(kTypesKey)).toArray()) {
        if (!v.isObject())
            continue;
        RemoteType t = remoteTypeFromJson(v.toObject());
        if (t.name.trimmed().isEmpty() || indexOfType(out, t.name) >= 0)
            continue;   // a name is the identity; the first wins, as for hosts
        out.append(t);
    }
    return out;
}

int HostBookmarkStore::indexOfType(const QVector<RemoteType> &types, const QString &name)
{
    for (int i = 0; i < types.size(); ++i) {
        if (sameName(types.at(i).name, name))
            return i;
    }
    return -1;
}

bool HostBookmarkStore::replaceTypes(const QVector<RemoteType> &types) const
{
    QJsonArray array;
    bool anySecret = false;
    for (const RemoteType &t : types) {
        array.append(remoteTypeToJson(t));
        if (t.savePassword && !t.password.isEmpty())
            anySecret = true;
    }
    return writeKey(QLatin1String(kTypesKey), array, anySecret);
}

bool HostBookmarkStore::saveType(const RemoteType &type) const
{
    QVector<RemoteType> all = types();
    const int at = indexOfType(all, type.name);
    if (at >= 0)
        all[at] = type;
    else
        all.append(type);
    return replaceTypes(all);
}

bool HostBookmarkStore::removeType(const QString &name) const
{
    QVector<RemoteType> all = types();
    const int at = indexOfType(all, name);
    if (at < 0)
        return true;
    all.removeAt(at);
    return replaceTypes(all);
}

bool HostBookmarkStore::save(const HostBookmark &bookmark) const
{
    QVector<HostBookmark> bookmarks = all();
    const int at = indexOfName(bookmarks, bookmark.displayName());
    if (at >= 0)
        bookmarks[at] = bookmark; // replaced in place: the list order does not shift
    else
        bookmarks.append(bookmark);
    return replaceAll(bookmarks);
}

bool HostBookmarkStore::remove(const QString &name) const
{
    QVector<HostBookmark> bookmarks = all();
    const int at = indexOfName(bookmarks, name);
    if (at < 0)
        return true; // nothing to do
    bookmarks.removeAt(at);
    return replaceAll(bookmarks);
}

HostBookmark HostBookmarkStore::find(const QVector<HostBookmark> &bookmarks,
                                     const RemoteLocation &location, bool *found)
{
    for (const HostBookmark &b : bookmarks) {
        // A bookmark with no user matches a location with no user, and vice versa —
        // they are different connections (see RemoteLocation::target()).
        if (sameHost(b, location)) {
            if (found)
                *found = true;
            return b;
        }
    }
    if (found)
        *found = false;
    return HostBookmark{};
}

int HostBookmarkStore::indexOfTarget(const QVector<HostBookmark> &bookmarks,
                                     const QString &target)
{
    for (int i = 0; i < bookmarks.size(); ++i) {
        if (bookmarks.at(i).locationFor(QString()).target() == target)
            return i;
    }
    return -1;
}

} // namespace loftail
