#include "SourceSpool.h"

#include "ArchiveFetcher.h"
#include "ArchiveLocation.h"
#include "RemoteLocation.h"

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

// A short, filesystem-safe directory name for one spooled log.
QString spoolKey(const QString &key)
{
    const QByteArray digest =
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(digest.toHex().left(16));
}

// Marks a key whose spool holds the EXPANSION of an address rather than the address's
// own bytes. See expandedSpoolKey() in the header for why the two must never collide.
constexpr auto kExpandPrefix = "expand\n";

// The one place that turns a spool key into the fetcher that can fill it. Dispatching
// here rather than in the registry is what keeps the registry ignorant of what it holds.
std::unique_ptr<SourceFetcher> defaultFetcher(const QString &key, QString *error)
{
    if (key.startsWith(QLatin1String(kExpandPrefix))) {
        const QString address = key.mid(qstrlen(kExpandPrefix));
        if (const auto archive = ArchiveLocation::split(address)) {
#if defined(LOFTAIL_HAVE_ARCHIVE)
            return makeArchiveFetcher(*archive, error);
#else
            if (error) {
                *error = QStringLiteral(
                    "Support for compressed and archived logs is not built into this "
                    "copy of loftail. Rebuild with libarchive available to enable it.");
            }
            return nullptr;
#endif
        }
        if (error)
            *error = QStringLiteral("Nothing to expand in %1.").arg(address);
        return nullptr;
    }

    if (RemoteLocation::isRemote(key)) {
#if defined(LOFTAIL_HAVE_SSH)
        if (const auto location = RemoteLocation::parse(key))
            return makeSshFetcher(*location, error);
        if (error)
            *error = QStringLiteral("Not a valid remote log address: %1").arg(key);
        return nullptr;
#else
        if (error) {
            *error = QStringLiteral(
                "SSH support is not built into this copy of loftail, so remote logs "
                "cannot be opened. Rebuild with libssh2 available to enable it.");
        }
        return nullptr;
#endif
    }

    if (error)
        *error = QStringLiteral("No way to fetch %1.").arg(key);
    return nullptr;
}

} // namespace

QString expandedSpoolKey(const QString &address)
{
    return QLatin1String(kExpandPrefix) + address;
}

// --- SourceSpool -----------------------------------------------------------

SourceSpool::SourceSpool(QString key, std::unique_ptr<SourceFetcher> fetcher, QString dir)
    : m_key(std::move(key)), m_fetcher(std::move(fetcher)), m_dir(std::move(dir))
{
}

SourceSpool::~SourceSpool()
{
    // Stop the fetcher BEFORE removing the directory: it owns a thread that is
    // writing into it, and stop() joins that thread.
    if (m_fetcher)
        m_fetcher->stop();
    if (!m_dir.isEmpty())
        QDir(m_dir).removeRecursively();
}

FetchStatus SourceSpool::status() const
{
    return m_fetcher ? m_fetcher->status() : FetchStatus{};
}

QString SourceSpool::spoolPath(quint64 generation) const
{
    return m_fetcher ? m_fetcher->spoolPath(generation) : QString();
}

void SourceSpool::poke()
{
    if (m_fetcher)
        m_fetcher->pokeNow();
}

// --- SourceSpoolRegistry ---------------------------------------------------

SourceSpoolRegistry::SourceSpoolRegistry() = default;
SourceSpoolRegistry::~SourceSpoolRegistry() = default;

SourceSpoolRegistry &SourceSpoolRegistry::instance()
{
    static SourceSpoolRegistry registry;
    return registry;
}

void SourceSpoolRegistry::setFetcherFactory(FetcherFactory factory)
{
    m_factory = std::move(factory);
}

QString SourceSpoolRegistry::instanceDir()
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
    qAddPostRoutine([] { SourceSpoolRegistry::instance().shutdown(); });

    sweepAbandonedSpools();
    return m_instanceDir->path();
}

void SourceSpoolRegistry::sweepAbandonedSpools()
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

std::shared_ptr<SourceSpool> SourceSpoolRegistry::find(const QString &key) const
{
    return m_spools.value(key).lock();
}

std::shared_ptr<SourceSpool> SourceSpoolRegistry::acquire(const QString &key, QString *error)
{
    if (auto live = m_spools.value(key).lock())
        return live;

    const QString base = instanceDir();
    if (base.isEmpty()) {
        if (error)
            *error = QStringLiteral("Cannot create a local cache directory for spooled logs.");
        return nullptr;
    }
    const QString dir = base + u'/' + spoolKey(key);
    if (!QDir().mkpath(dir)) {
        if (error)
            *error = QStringLiteral("Cannot create the local cache directory %1.").arg(dir);
        return nullptr;
    }

    QString fetcherError;
    auto fetcher = m_factory ? m_factory(key, &fetcherError)
                             : defaultFetcher(key, &fetcherError);
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
            *error = startError.isEmpty() ? QStringLiteral("Cannot open %1.").arg(key)
                                          : startError;
        return nullptr;
    }

    // shared_ptr with an explicit deleter: the constructor is private, so make_shared
    // cannot reach it, and the weak entry must be reaped when the last handle drops.
    std::shared_ptr<SourceSpool> spool(new SourceSpool(key, std::move(fetcher), dir),
                                       [key](SourceSpool *p) {
                                           SourceSpoolRegistry::instance().m_spools.remove(key);
                                           delete p;
                                       });
    m_spools.insert(key, spool);
    return spool;
}

void SourceSpoolRegistry::shutdown()
{
    clear();
    if (m_instanceLock) {
        m_instanceLock->unlock();
        m_instanceLock.reset();
    }
    m_instanceDir.reset(); // QTemporaryDir removes the tree on destruction
}

void SourceSpoolRegistry::clear()
{
    // Only the bookkeeping: a spool still referenced by an open SpooledLogSource is
    // kept alive by that handle and torn down when it drops, which is the point of
    // the weak map. Forgetting the entries just means the next acquire() of the same
    // key builds a fresh spool instead of joining the old one.
    m_spools.clear();
}

} // namespace loftail
