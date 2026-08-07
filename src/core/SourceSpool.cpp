#include "SourceSpool.h"

#include "ArchiveFetcher.h"
#include "ArchiveLocation.h"
#include "RemoteLocation.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QLockFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimerEvent>

#if defined(LOFTAIL_HAVE_SSH)
#include "SshFetcher.h"
#endif

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr() — and these strings are user-facing all the same: they travel up to
// the status bar through Document::lastError() and LiveController::sourceStatusChanged.
// Q_DECLARE_TR_FUNCTIONS is what lets lupdate file them under a name that means
// something rather than under the file they happen to sit in.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::SourceSpool)
};
} // namespace


namespace {

constexpr auto kSpoolRootName = "spool";
constexpr auto kInstanceLockName = "owner.lock";

// How often the reaper checks whether a retired fetcher's thread has exited, and how
// long shutdown() waits between checks and in total. Nobody is waiting on the first two;
// the third is a cap on how long quitting can take, not a target.
constexpr int kReapIntervalMs = 250;
constexpr int kDrainSliceMs = 20;
constexpr int kDrainBudgetMs = 2000;

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

// A short, filesystem-safe directory name for one ACQUISITION of a spooled log.
//
// The digest names the log, which is what makes a spool directory identifiable while
// debugging. The serial is what stops two acquisitions of the SAME log from sharing a
// directory — and they overlap now that a dead spool's directory is removed when its
// fetcher's thread finally exits rather than the instant the spool is dropped
// (SourceSpoolRegistry::retire). Reopening a log whose previous fetcher is still winding
// up would otherwise hand the new spool the old one's directory, and the reaper would
// then delete a live spool's files out from under it.
QString spoolDirName(const QString &key, quint64 serial)
{
    const QByteArray digest =
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(digest.toHex().left(16)) + QStringLiteral("-%1").arg(serial);
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
                *error = Tr::tr(
                    "Support for compressed and archived logs is not built into this "
                    "copy of loftail. Rebuild with libarchive available to enable it.");
            }
            return nullptr;
#endif
        }
        if (error)
            *error = Tr::tr("Nothing to expand in %1.").arg(address);
        return nullptr;
    }

    if (RemoteLocation::isRemote(key)) {
#if defined(LOFTAIL_HAVE_SSH)
        if (const auto location = RemoteLocation::parse(key))
            return makeSshFetcher(*location, error);
        if (error)
            *error = Tr::tr("Not a valid remote log address: %1").arg(key);
        return nullptr;
#else
        if (error) {
            *error = Tr::tr(
                "SSH support is not built into this copy of loftail, so remote logs "
                "cannot be opened. Rebuild with libssh2 available to enable it.");
        }
        return nullptr;
#endif
    }

    if (error)
        *error = Tr::tr("No way to fetch %1.").arg(key);
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
    // Hand both to the registry rather than stopping and deleting here. The fetcher owns
    // a thread that is writing into this directory, so the directory cannot go until the
    // thread has — and waiting for that is exactly what this destructor must not do: it
    // runs on the GUI thread every time the last tab on a log closes, and the thread may
    // be twenty seconds into a connect (SourceSpoolRegistry::retire).
    SourceSpoolRegistry::instance().retire(std::move(m_fetcher), m_dir);
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

void SourceSpool::cancel()
{
    // Ask, and do not wait: this runs on the GUI thread from the Cancel button, and the
    // whole point of cancelling a gigabyte expansion is not to sit through the rest of
    // it. The fetcher publishes Disconnected immediately, so the status line is right at
    // once even though the worker takes a moment to notice.
    if (m_fetcher)
        m_fetcher->requestStop();
}

// --- SourceSpoolRegistry ---------------------------------------------------

// Polls the retired fetchers and buries the ones that have stopped.
//
// A TIMER RATHER THAN QThread::finished, deliberately. "A mutex-guarded snapshot,
// polled; never a signal" is this whole layer's synchronisation model — it is why
// SpooledLogSource needs no lock against the fetcher thread at all (SourceFetcher.h) —
// and one queued connection here is the precedent for the next one. Nothing is waiting
// on this: a spool directory that lingers a quarter of a second longer costs nothing.
//
// No Q_OBJECT and no signals of its own, so this needs no moc.
class SourceSpoolRegistry::Reaper : public QObject
{
public:
    void arm()
    {
        if (m_timer == 0)
            m_timer = startTimer(kReapIntervalMs);
    }

protected:
    void timerEvent(QTimerEvent *event) override
    {
        if (event->timerId() != m_timer)
            return;
        if (SourceSpoolRegistry::instance().collectRetired() > 0)
            return;
        killTimer(m_timer); // nothing left to bury; stop waking up for it
        m_timer = 0;
    }

private:
    int m_timer = 0;
};

SourceSpoolRegistry::SourceSpoolRegistry() = default;
SourceSpoolRegistry::~SourceSpoolRegistry() = default;

void SourceSpoolRegistry::retire(std::unique_ptr<SourceFetcher> fetcher, const QString &dir)
{
    if (!fetcher) {
        if (!dir.isEmpty())
            QDir(dir).removeRecursively();
        return;
    }

    // Outside the lock: requestStop() takes the fetcher's own mutex, and the fetcher's
    // worker takes this one when it drops a nested spool (see acquire()'s deleter).
    fetcher->requestStop();

    if (fetcher->isStopped()) {
        // The common case by far — a fetcher whose worker had already finished, or one
        // that never started a worker at all. No timer, no wait, no bookkeeping.
        fetcher.reset();
        if (!dir.isEmpty())
            QDir(dir).removeRecursively();
        return;
    }

    QObject *reaper = nullptr;
    {
        QMutexLocker lock(&m_mutex);
        m_retired.push_back(Retired{std::move(fetcher), dir});
        reaper = m_reaper.get();
    }

    // May be null in a core test that never opened a spool through acquire(), and on any
    // thread but the GUI one. Both are fine: shutdown() drains whatever is left, and it
    // is the only thing that has to.
    if (reaper) {
        QMetaObject::invokeMethod(
            reaper, [reaper]() { static_cast<Reaper *>(reaper)->arm(); },
            Qt::QueuedConnection);
    }
}

int SourceSpoolRegistry::collectRetired()
{
    // Swap the list out and inspect it unlocked. isStopped() takes the fetcher's mutex
    // and ~SourceFetcher can drop a nested spool — which takes this one — so neither may
    // run under it.
    std::vector<Retired> pending;
    {
        QMutexLocker lock(&m_mutex);
        pending.swap(m_retired);
    }

    std::vector<Retired> stillRunning;
    for (Retired &r : pending) {
        if (!r.fetcher->isStopped()) {
            stillRunning.push_back(std::move(r));
            continue;
        }
        r.fetcher.reset(); // joins instantly: its thread has already exited
        if (!r.dir.isEmpty())
            QDir(r.dir).removeRecursively();
    }

    const int remaining = static_cast<int>(stillRunning.size());
    if (remaining > 0) {
        QMutexLocker lock(&m_mutex);
        for (Retired &r : stillRunning)
            m_retired.push_back(std::move(r));
    }
    return remaining;
}

void SourceSpoolRegistry::drainRetired(int budgetMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (collectRetired() > 0) {
        if (elapsed.elapsed() >= budgetMs)
            break;
        // msleep, NOT processEvents(): this runs during teardown, after the main window
        // is gone, and re-entering the event loop there is how an orderly exit turns
        // into a crash.
        QThread::msleep(kDrainSliceMs);
    }

    QMutexLocker lock(&m_mutex);
    for (Retired &r : m_retired) {
        // Out of budget with a thread still running — a connect to a host that is not
        // answering, most likely. Deleting the fetcher would join that thread and hang
        // the quit, which is the one thing worse than what this does instead: let the
        // object leak, and leave its directory. The process is seconds from exiting, and
        // the next launch's sweepAbandonedSpools() removes the tree — an instance
        // directory whose lock file is gone is granted to whoever asks (see below).
        (void) r.fetcher.release();
    }
    m_retired.clear();
}

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

    // Built here, on the thread that does every acquire(), so that retire() — which is
    // called from wherever a spool's last handle is dropped — only ever has to post to
    // it, never to create it.
    if (!m_reaper)
        m_reaper = std::make_unique<Reaper>();

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
    QMutexLocker lock(&m_mutex);
    return m_spools.value(key).lock();
}

void SourceSpoolRegistry::forget(const QString &key)
{
    QMutexLocker lock(&m_mutex);
    m_spools.remove(key);
}

std::shared_ptr<SourceSpool> SourceSpoolRegistry::acquire(const QString &key, QString *error)
{
    if (auto live = find(key))
        return live;

    const QString base = instanceDir();
    if (base.isEmpty()) {
        if (error)
            *error = Tr::tr("Cannot create a local cache directory for spooled logs.");
        return nullptr;
    }
    const QString dir = base + u'/' + spoolDirName(key, ++m_serial);
    if (!QDir().mkpath(dir)) {
        if (error)
            *error = Tr::tr("Cannot create the local cache directory %1.").arg(dir);
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
            *error = startError.isEmpty() ? Tr::tr("Cannot open %1.").arg(key)
                                          : startError;
        return nullptr;
    }

    // shared_ptr with an explicit deleter: the constructor is private, so make_shared
    // cannot reach it, and the weak entry must be reaped when the last handle drops.
    //
    // This deleter runs on WHICHEVER THREAD dropped the last handle, which for a remote
    // archive is the archive fetcher's own worker letting go of the container. Hence
    // forget() rather than a bare remove — and hence the delete happening after it and
    // outside the lock, because ~SourceSpool stops a fetcher and must not do that with
    // the registry held.
    std::shared_ptr<SourceSpool> spool(new SourceSpool(key, std::move(fetcher), dir),
                                       [key](SourceSpool *p) {
                                           SourceSpoolRegistry::instance().forget(key);
                                           delete p;
                                       });
    {
        QMutexLocker lock(&m_mutex);
        m_spools.insert(key, spool);
    }
    return spool;
}

void SourceSpoolRegistry::shutdown()
{
    clear();
    // The reaper's timer will never fire again — this runs as a post-routine, after the
    // event loop has stopped — so the draining has to happen here, by hand and bounded.
    drainRetired(kDrainBudgetMs);
    m_reaper.reset();
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
    QMutexLocker lock(&m_mutex);
    m_spools.clear();
}

} // namespace loftail
