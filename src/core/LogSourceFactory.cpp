#include "LogSource.h"

#include "ArchiveLocation.h"
#include "BufferedLogSource.h"
#include "RemoteLocation.h"
#include "SourceSpool.h"
#include "SpooledLogSource.h"
#if defined(Q_OS_WIN)
#else
#include "MappedLogSource.h"
#include <QFile>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace loftail {

namespace {

// A log that is not directly readable as a local file reads through a local spool that
// a fetcher fills (§6.3, §6.4). The spool is shared per log, keyed by the normalized
// address, so a second Document on the same log — or a rescan after a rotation — joins
// the live one instead of connecting or expanding a second time.
std::unique_ptr<LogSource> openSpooled(const QString &key, const QString &reuseError,
                                       OpenPolicy policy, QString *error)
{
    SourceSpoolRegistry &registry = SourceSpoolRegistry::instance();
    std::shared_ptr<SourceSpool> spool = registry.find(key);
    if (!spool) {
        if (policy == OpenPolicy::Reuse) {
            // A rotation mid-tail must never turn into a reconnect or a fresh
            // expansion: this runs from the watch tick, on the GUI thread.
            if (error)
                *error = reuseError;
            return nullptr;
        }
        spool = registry.acquire(key, error);
        if (!spool)
            return nullptr;
    }
    return SpooledLogSource::open(std::move(spool));
}

std::unique_ptr<LogSource> openRemote(const QString &path, OpenPolicy policy, QString *error)
{
    const auto location = RemoteLocation::parse(path);
    if (!location) {
        if (error)
            *error = QStringLiteral("Not a valid remote log address: %1").arg(path);
        return nullptr;
    }
    // The registry keys on the normalized address string, not on the parsed value: it
    // holds spools for several kinds of source and understands none of them.
    return openSpooled(location->toString(),
                       QStringLiteral("Not connected to %1.").arg(location->target()),
                       policy, error);
}

std::unique_ptr<LogSource> openArchive(const ArchiveLocation &location, OpenPolicy policy,
                                       QString *error)
{
    if (location.needsMember()) {
        // An address that names a multi-member container and no member cannot be
        // opened. The member is picked once, at the interactive entry point, so
        // reaching here means an address was persisted or typed without one.
        if (error) {
            *error = QStringLiteral("%1 holds several logs; open it again and choose one.")
                         .arg(logSourceDisplayPath(location.container));
        }
        return nullptr;
    }
    const QString key = location.toString();
    return openSpooled(key, QStringLiteral("%1 is no longer expanded.").arg(key), policy,
                       error);
}

} // namespace

// Platform selection, not mode selection (invariant #5, §6): mmap on POSIX, buffered
// on Windows, falling back to buffered if the mapping fails (e.g. a special file that
// cannot be mmapped). A path that has to be fetched or expanded takes the spool route
// above instead.
std::unique_ptr<LogSource> openLogSource(const QString &path, OpenPolicy policy, QString *error)
{
    // Archive before transport, and the order is the point: a remote archive is an
    // archive whose container happens to live on another machine, so it resolves here
    // and the SSH fetcher is reached later, as the archive fetcher's own input (§6.4).
    if (const auto archive = ArchiveLocation::split(path))
        return openArchive(*archive, policy, error);

    if (RemoteLocation::isRemote(path))
        return openRemote(path, policy, error);

#if defined(Q_OS_WIN)
    return BufferedLogSource::open(path);
#else
    if (auto mapped = MappedLogSource::open(path))
        return mapped;
    return BufferedLogSource::open(path);
#endif
}

quint64 pathIdentity(const QString &path)
{
#if defined(Q_OS_WIN)
    // Windows file-identity via GetFileInformationByHandle lands with the M6
    // Windows work; until then rotation-by-replace there falls back to size checks.
    Q_UNUSED(path);
    return 0;
#else
    struct stat st{};
    const QByteArray local = QFile::encodeName(path);
    if (::stat(local.constData(), &st) != 0)
        return 0;
    // Same formula as MappedLogSource::identity() so the two are comparable.
    return (static_cast<quint64>(st.st_dev) << 32) ^ static_cast<quint64>(st.st_ino);
#endif
}

} // namespace loftail
