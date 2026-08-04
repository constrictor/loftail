#include "LogSource.h"

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

// An ssh:// URL reads through a local spool that a fetcher fills (§6.3). The spool is
// shared per remote file, so a second Document on the same file — or a rescan after a
// rotation — joins the live one instead of opening a second connection.
std::unique_ptr<LogSource> openRemote(const QString &path, OpenPolicy policy, QString *error)
{
    const auto location = RemoteLocation::parse(path);
    if (!location) {
        if (error)
            *error = QStringLiteral("Not a valid remote log address: %1").arg(path);
        return nullptr;
    }

    // The registry keys on the normalized address string, not on the parsed value:
    // it holds spools for several kinds of source and understands none of them.
    const QString key = location->toString();

    SourceSpoolRegistry &registry = SourceSpoolRegistry::instance();
    std::shared_ptr<SourceSpool> spool = registry.find(key);
    if (!spool) {
        if (policy == OpenPolicy::Reuse) {
            // A rotation mid-tail must never turn into a reconnect: this runs from
            // the watch tick, on the GUI thread.
            if (error)
                *error = QStringLiteral("Not connected to %1.").arg(location->target());
            return nullptr;
        }
        spool = registry.acquire(key, error);
        if (!spool)
            return nullptr;
    }
    return SpooledLogSource::open(std::move(spool));
}

} // namespace

// Platform selection, not mode selection (invariant #5, §6): mmap on POSIX, buffered
// on Windows, falling back to buffered if the mapping fails (e.g. a special file that
// cannot be mmapped). A remote path takes the spool route above instead.
std::unique_ptr<LogSource> openLogSource(const QString &path, OpenPolicy policy, QString *error)
{
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
