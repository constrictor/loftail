#include "RemoteSpool.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QLockFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#if defined(LOFTAIL_HAVE_SSH)
#include "SshFetcher.h"
#endif

namespace loftail {

namespace {

constexpr auto kSpoolRootName = "spool";
constexpr auto kInstanceLockName = "owner.lock";

// The cache root all instances spool beneath. CacheLocation, not AppConfigLocation:
// a spool is a reproducible copy of somebody else's file and may be enormous, so it
// belongs somewhere the platform is free to clear (no hardcoded paths — CLAUDE.md).
QString spoolRoot()
{
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cache.isEmpty())
        return QString();
    return cache + u'/' + QLatin1String(kSpoolRootName);
}

// A short, filesystem-safe directory name for one remote file.
QString spoolKey(const RemoteLocation &location)
{
    const QByteArray digest =
        QCryptographicHash::hash(location.toString().toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(digest.toHex().left(16));
}

std::unique_ptr<RemoteFetcher> defaultFetcher(const RemoteLocation &location, QString *error)
{
#if defined(LOFTAIL_HAVE_SSH)
    return makeSshFetcher(location, error);
#else
    Q_UNUSED(location);
    if (error) {
        *error = QStringLiteral(
            "SSH support is not built into this copy of loftail, so remote logs "
            "cannot be opened. Rebuild with libssh2 available to enable it.");
    }
    return nullptr;
#endif
}

} // namespace

// --- RemoteSpool -----------------------------------------------------------

RemoteSpool::RemoteSpool(RemoteLocation location, std::unique_ptr<RemoteFetcher> fetcher,
                         QString dir)
    : m_location(std::move(location)), m_fetcher(std::move(fetcher)), m_dir(std::move(dir))
{
}

RemoteSpool::~RemoteSpool()
{
    // Stop the fetcher BEFORE removing the directory: it owns a thread that is
    // writing into it, and stop() joins that thread.
    if (m_fetcher)
        m_fetcher->stop();
    if (!m_dir.isEmpty())
        QDir(m_dir).removeRecursively();
}

FetchStatus RemoteSpool::status() const
{
    return m_fetcher ? m_fetcher->status() : FetchStatus{};
}

QString RemoteSpool::spoolPath(quint64 generation) const
{
    return m_fetcher ? m_fetcher->spoolPath(generation) : QString();
}

void RemoteSpool::poke()
{
    if (m_fetcher)
        m_fetcher->pokeNow();
}

// --- RemoteSpoolRegistry ---------------------------------------------------

RemoteSpoolRegistry::RemoteSpoolRegistry() = default;
RemoteSpoolRegistry::~RemoteSpoolRegistry() = default;

RemoteSpoolRegistry &RemoteSpoolRegistry::instance()
{
    static RemoteSpoolRegistry registry;
    return registry;
}

void RemoteSpoolRegistry::setFetcherFactory(FetcherFactory factory)
{
    m_factory = std::move(factory);
}

QString RemoteSpoolRegistry::instanceDir()
{
    if (m_instanceDir && m_instanceDir->isValid())
        return m_instanceDir->path();

    const QString root = spoolRoot();
    if (root.isEmpty() || !QDir().mkpath(root))
        return QString();

    // Abandoned spools are swept once, when this process first needs the directory —
    // by which point the lock below exists, so a concurrently-starting sibling cannot
    // mistake us for abandoned.
    auto dir = std::make_unique<QTemporaryDir>(root + QStringLiteral("/instance-"));
    if (!dir->isValid())
        return QString();
    dir->setAutoRemove(true);

    auto lock = std::make_unique<QLockFile>(dir->path() + u'/' + QLatin1String(kInstanceLockName));
    lock->setStaleLockTime(0); // never time out: liveness is the owning PID, not a clock
    lock->tryLock(0);

    m_instanceDir = std::move(dir);
    m_instanceLock = std::move(lock);

    // Release the lock and the directory while Qt is still up, rather than in this
    // singleton's own destructor — by then the application object is gone and, in a
    // test, so is the temporary HOME the directory lived under, which leaves QLockFile
    // complaining about a lock file that no longer exists.
    qAddPostRoutine([] { RemoteSpoolRegistry::instance().shutdown(); });

    sweepAbandonedSpools();
    return m_instanceDir->path();
}

void RemoteSpoolRegistry::sweepAbandonedSpools()
{
    const QString root = spoolRoot();
    if (root.isEmpty())
        return;
    const QString mine = m_instanceDir ? m_instanceDir->path() : QString();

    QDir rootDir(root);
    const QStringList siblings =
        rootDir.entryList({QStringLiteral("instance-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : siblings) {
        const QString path = rootDir.filePath(name);
        if (path == mine)
            continue;
        // Several loftail instances may run at once (SPEC.md §3), so "old" is not a
        // safe test — a sibling that has been tailing for a week is not abandoned.
        // Taking its lock is: QLockFile grants it only when the owning process is
        // gone. A lock we cannot take means a live owner, so leave the directory be.
        QLockFile lock(path + u'/' + QLatin1String(kInstanceLockName));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0))
            continue;
        lock.unlock();
        QDir(path).removeRecursively();
    }
}

std::shared_ptr<RemoteSpool> RemoteSpoolRegistry::find(const RemoteLocation &location) const
{
    return m_spools.value(location.toString()).lock();
}

std::shared_ptr<RemoteSpool> RemoteSpoolRegistry::acquire(const RemoteLocation &location,
                                                          QString *error)
{
    const QString key = location.toString();
    if (auto live = m_spools.value(key).lock())
        return live;

    const QString base = instanceDir();
    if (base.isEmpty()) {
        if (error)
            *error = QStringLiteral("Cannot create a local cache directory for remote logs.");
        return nullptr;
    }
    const QString dir = base + u'/' + spoolKey(location);
    if (!QDir().mkpath(dir)) {
        if (error)
            *error = QStringLiteral("Cannot create the local cache directory %1.").arg(dir);
        return nullptr;
    }

    QString fetcherError;
    auto fetcher = m_factory ? m_factory(location, &fetcherError)
                             : defaultFetcher(location, &fetcherError);
    if (!fetcher) {
        QDir(dir).removeRecursively();
        if (error)
            *error = fetcherError;
        return nullptr;
    }

    QString startError;
    if (!fetcher->start(dir, &startError)) {
        QDir(dir).removeRecursively();
        if (error)
            *error = startError.isEmpty()
                ? QStringLiteral("Cannot open %1.").arg(location.toString())
                : startError;
        return nullptr;
    }

    // shared_ptr with an explicit deleter: the constructor is private, so make_shared
    // cannot reach it, and the weak entry must be reaped when the last handle drops.
    std::shared_ptr<RemoteSpool> spool(new RemoteSpool(location, std::move(fetcher), dir),
                                       [key](RemoteSpool *p) {
                                           RemoteSpoolRegistry::instance().m_spools.remove(key);
                                           delete p;
                                       });
    m_spools.insert(key, spool);
    return spool;
}

void RemoteSpoolRegistry::shutdown()
{
    clear();
    if (m_instanceLock) {
        m_instanceLock->unlock();
        m_instanceLock.reset();
    }
    m_instanceDir.reset(); // QTemporaryDir removes the tree on destruction
}

void RemoteSpoolRegistry::clear()
{
    // Only the bookkeeping: a spool still referenced by an open SpooledLogSource is
    // kept alive by that handle and torn down when it drops, which is the point of
    // the weak map. Forgetting the entries just means the next acquire() of the same
    // location builds a fresh spool instead of joining the old one.
    m_spools.clear();
}

} // namespace loftail
