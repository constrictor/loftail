#include "HostBookmarkStore.h"

#include "AtomicJson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace loftail {

namespace {

constexpr auto kFileName = "hosts.json";
constexpr auto kHostsKey = "hosts";
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
    const QJsonArray paths = o.value(QStringLiteral("paths")).toArray();
    for (const QJsonValue &v : paths) {
        if (v.isString())
            b.paths.append(v.toString());
    }
    b.savePassword = o.value(QStringLiteral("savePassword")).toBool();
    b.password = o.value(QStringLiteral("password")).toString();
    return b;
}

bool sameHost(const HostBookmark &b, const QString &user, const QString &host, int port)
{
    return b.user == user && b.host == host && b.port == port;
}

} // namespace

RemoteLocation HostBookmark::locationFor(const QString &path) const
{
    RemoteLocation location;
    location.user = user;
    location.host = host;
    location.port = port;
    location.path = path;
    return location;
}

SshFetchOptions HostBookmark::fetchOptions() const
{
    SshFetchOptions options;
    options.pollMs = pollMs > 0 ? pollMs : 1000;
    options.tailStartBytes = tailStartBytes;
    return options;
}

QString HostBookmarkStore::defaultDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString HostBookmarkStore::filePath() const
{
    if (m_dir.isEmpty())
        return QString();
    return m_dir + u'/' + QLatin1String(kFileName);
}

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
    // Exact-version gating with no migration, as PresetStore and SessionStore do
    // (ARCHITECTURE.md §8): an unrecognised version yields an empty collection rather
    // than a half-understood one.
    if (root.value(QLatin1String(kSchemaKey)).toInt() != kSchemaVersion)
        return out;

    const QJsonArray hosts = root.value(QLatin1String(kHostsKey)).toArray();
    out.reserve(hosts.size());
    for (const QJsonValue &v : hosts) {
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

bool HostBookmarkStore::replaceAll(const QVector<HostBookmark> &bookmarks)
{
    const QString path = filePath();
    if (path.isEmpty())
        return false;

    QJsonArray hosts;
    bool anySecret = false;
    for (const HostBookmark &b : bookmarks) {
        hosts.append(toJson(b));
        if (b.savePassword && !b.password.isEmpty())
            anySecret = true;
    }

    QJsonObject root;
    root.insert(QLatin1String(kSchemaKey), kSchemaVersion);
    root.insert(QLatin1String(kHostsKey), hosts);

    // Owner-only whenever a password is in there. Always writing it private would be
    // tidier still, but this file is also read by the Open Remote dialog on a shared
    // config directory, and narrowing permissions people did not ask for is its own
    // surprise — so the restriction follows the secret.
    const QJsonDocument doc(root);
    return anySecret ? AtomicJson::writePrivate(path, doc) : AtomicJson::write(path, doc);
}

bool HostBookmarkStore::save(const HostBookmark &bookmark)
{
    QVector<HostBookmark> bookmarks = all();
    const int at = indexOfName(bookmarks, bookmark.displayName());
    if (at >= 0)
        bookmarks[at] = bookmark; // replaced in place: the list order does not shift
    else
        bookmarks.append(bookmark);
    return replaceAll(bookmarks);
}

bool HostBookmarkStore::remove(const QString &name)
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
        if (sameHost(b, location.user, location.host, location.port)) {
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
