#include "RemoteLocation.h"

#include <QFileInfo>
#include <QLatin1String>
#include <QUrl>

namespace loftail {

namespace {

// A remote path may be written relative to the login directory ("~/app.log"). A URL
// has no way to spell that — its path always starts at '/' — so the two forms are
// converted on the way in and out, and RemoteLocation::path holds the form SFTP wants.
QString pathFromUrl(const QString &urlPath)
{
    if (urlPath.startsWith(QLatin1String("/~")))
        return urlPath.mid(1);
    return urlPath;
}

QString pathToUrl(const QString &remotePath)
{
    if (remotePath.startsWith(u'/'))
        return remotePath;
    return u'/' + remotePath;
}

} // namespace

bool RemoteLocation::isRemote(const QString &s)
{
    return s.startsWith(QLatin1String("ssh://"), Qt::CaseInsensitive)
        || s.startsWith(QLatin1String("sftp://"), Qt::CaseInsensitive);
}

std::optional<RemoteLocation> RemoteLocation::parse(const QString &s)
{
    if (!isRemote(s))
        return std::nullopt;

    const QUrl url(s, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty())
        return std::nullopt;

    RemoteLocation loc;
    loc.user = url.userName(QUrl::FullyDecoded);
    loc.host = url.host();
    loc.port = url.port(kDefaultPort);
    loc.path = pathFromUrl(url.path(QUrl::FullyDecoded));
    // A password in the URL is dropped on the floor, not stored and not used. Keeping
    // it would put a credential into the session file and the recent-files menu the
    // moment the URL became a Document path; the user is prompted instead.
    if (!loc.isValid())
        return std::nullopt;
    return loc;
}

QString RemoteLocation::normalize(const QString &s)
{
    if (const auto loc = parse(s))
        return loc->toString();
    return s;
}

QString RemoteLocation::toString() const
{
    QUrl url;
    url.setScheme(QStringLiteral("ssh"));
    if (!user.isEmpty())
        url.setUserName(user);
    url.setHost(host);
    url.setPort(port); // always spelled, so ssh://h/p and ssh://h:22/p are one string
    url.setPath(pathToUrl(path));
    return url.toString(QUrl::FullyEncoded);
}

QString RemoteLocation::target() const
{
    if (user.isEmpty())
        return QStringLiteral("%1:%2").arg(host).arg(port);
    return QStringLiteral("%1@%2:%3").arg(user, host).arg(port);
}

// --- Path-shaped helpers shared by core and UI -----------------------------

QString logSourceDisplayName(const QString &path)
{
    if (const auto loc = RemoteLocation::parse(path)) {
        const QString name = QFileInfo(loc->path).fileName();
        return QStringLiteral("%1 (%2)").arg(name.isEmpty() ? loc->path : name,
                                             loc->displayHost());
    }
    return QFileInfo(path).fileName();
}

QString logSourceDisplayPath(const QString &path)
{
    if (const auto loc = RemoteLocation::parse(path))
        return loc->toString();
    return path;
}

bool logSourceAvailable(const QString &path)
{
    if (RemoteLocation::isRemote(path)) {
        // Optimistic by design: the honest answer costs a connection, and this is
        // called from session restore, where blocking would be a hang. A host that
        // turns out to be unreachable surfaces as an open failure instead.
        return RemoteLocation::parse(path).has_value();
    }
    const QFileInfo info(path);
    return info.exists() && info.isReadable();
}

} // namespace loftail
