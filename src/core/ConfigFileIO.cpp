#include "ConfigFileIO.h"

#include "PromptRelay.h"
#include "RemoteLocation.h"
#include "SshPrompter.h"

#if defined(LOFTAIL_HAVE_SSH)
#include "SshSession.h"
#endif

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QElapsedTimer>
#include <QSaveFile>
#include <QTimer>

#include <QThread>

#include <atomic>
#include <mutex>
#include <vector>

namespace loftail {

namespace {
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::ConfigFileIO)
};

// A config file is a config file, not a log: it is read whole, into an editor, by
// somebody about to change it. A cap keeps a mistyped path — a core dump, a database, a
// log — from being pulled into a QPlainTextEdit that would then try to lay it out.
constexpr qint64 kMaxConfigBytes = 16LL * 1024 * 1024;

// The same bound the log transport puts on a connect. A config file open is a deliberate
// gesture with somebody waiting, so it gets the attended timeout rather than the shorter
// unattended one a background retry uses.
constexpr int kConnectTimeoutMs = 20000;
} // namespace

ConfigReadResult readConfigFile(const QString &address)
{
    ConfigReadResult out;

    if (RemoteLocation::isRemote(address)) {
        // Reached only by a caller that did not check configAddressIsRemote() first: a
        // remote read is a connect, and a connect does not belong on the thread that
        // asked for it. ConfigTransfer is the way in.
        out.error = Tr::tr("%1 is on another machine and must be read over SSH.")
                        .arg(RemoteLocation::withoutPassword(address));
        return out;
    }

    const QFileInfo info(address);
    if (!info.exists()) {
        // THE SUPPORTED CASE, not a failure. The editor opens empty and Save creates it.
        out.ok = true;
        out.existed = false;
        return out;
    }
    if (info.isDir()) {
        out.error = Tr::tr("%1 is a directory, not a config file.").arg(address);
        return out;
    }
    if (info.size() > kMaxConfigBytes) {
        out.error = Tr::tr("%1 is too large to edit as a config file (%2 MB).")
                        .arg(address)
                        .arg(info.size() / (1024LL * 1024));
        return out;
    }

    QFile file(address);
    if (!file.open(QIODevice::ReadOnly)) {
        // "There and shut" is a different sentence from "not there", and this is where
        // they are told apart: a file whose mode is 000 must not be described as one
        // that has not appeared yet.
        out.error = Tr::tr("Cannot read %1: %2").arg(address, file.errorString());
        return out;
    }
    out.bytes = file.readAll();
    out.ok = true;
    out.existed = true;
    return out;
}

ConfigWriteResult writeConfigFile(const QString &address, const QByteArray &bytes)
{
    ConfigWriteResult out;

    if (RemoteLocation::isRemote(address)) {
        out.error = Tr::tr("%1 is on another machine and must be written over SSH.")
                        .arg(RemoteLocation::withoutPassword(address));
        return out;
    }

    const QFileInfo info(address);
    const QDir dir = info.absoluteDir();
    if (!dir.exists()) {
        // REFUSED, BY NAME, and deliberately not created. The inverse of AtomicJson's
        // rule, and inverted on purpose: that one writes loftail's own tree, where
        // creating a missing directory is right. Here a missing directory almost always
        // means a mistyped path, and the useful answer is to say which one — not to
        // sprout a config tree somewhere the user did not ask for.
        out.error = Tr::tr("There is no directory %1, so %2 cannot be saved. "
                           "Create the directory first, or correct the path in "
                           "File ▸ Preferences.")
                        .arg(dir.absolutePath(), info.fileName());
        return out;
    }

    // Read the mode BEFORE the write, while the original inode is still there.
    const bool existed = info.exists();
    const QFile::Permissions before = existed ? QFile::permissions(address) : QFile::Permissions();

    QSaveFile file(address);
    if (!file.open(QIODevice::WriteOnly)) {
        out.error = file.errorString();
        return out;
    }
    if (file.write(bytes) != bytes.size()) {
        out.error = file.errorString();
        file.cancelWriting(); // the target keeps its previous contents
        return out;
    }
    if (!file.commit()) {
        out.error = file.errorString();
        return out;
    }

    // AFTER the rename, for the reason AtomicJson::writePrivate() records: commit()
    // replaces the file, so a mode set on the temporary would not survive it. The
    // difference is that this RESTORES what was there rather than imposing a mode of its
    // own — a config that was 0640 and group-readable must not come back 0644 because
    // loftail happened to save it.
    if (existed && before != QFile::Permissions() && QFile::permissions(address) != before) {
        if (!QFile::setPermissions(address, before)) {
            // Reported rather than swallowed: the file IS saved, so this is not a
            // failure of the write, but somebody whose config just became world-readable
            // needs to be told.
            out.ok = true;
            out.error = Tr::tr("%1 was saved, but its original permissions could not be "
                               "restored.")
                            .arg(info.fileName());
            return out;
        }
    }

    out.ok = true;
    return out;
}

bool configAddressIsRemote(const QString &address)
{
    return RemoteLocation::isRemote(address);
}

bool configAddressIsWritable(const QString &address, QString *reason)
{
#if !defined(LOFTAIL_HAVE_SSH)
    if (RemoteLocation::isRemote(address)) {
        if (reason) {
            // The same sentence a remote LOG open gives in this configuration, and for
            // the same reason: what is missing is a dependency, not a feature.
            *reason = Tr::tr("This copy of loftail was built without SSH support, so a "
                             "config file on another machine cannot be opened.");
        }
        return false;
    }
#else
    Q_UNUSED(address);
    Q_UNUSED(reason);
#endif
    return true;
}

// --- ConfigTransfer ---------------------------------------------------------

struct ConfigTransfer::Shared
{
    // Set when the owner goes. The worker checks it before doing anything expensive and
    // again before reporting, so an abandoned transfer stops as soon as it can and never
    // reports to a destroyed object.
    std::atomic<bool> abandoned{false};

    // GUARDED because abort() is reached from the OWNER's thread while the worker is
    // inside libssh2 — the one call on SshSession that is safe to make concurrently, and
    // the reason it is safe is that it shuts the socket and touches nothing else.
    std::mutex        mutex;
#if defined(LOFTAIL_HAVE_SSH)
    SshSession       *session = nullptr;
#endif

    // OWNED HERE, and that placement is the whole point: the worker holds a strong
    // reference to this block, so the relay it asks through cannot be destroyed while a
    // connect is still using it. Stateless by design, so one per transfer is free.
    PromptRelay       relay;
};

ConfigTransfer::ConfigTransfer(QObject *parent)
    : QObject(parent), m_shared(std::make_shared<Shared>())
{
}

ConfigTransfer::~ConfigTransfer()
{
    // ABANDON, NEVER JOIN. Waiting here would make closing a tab on a host that is not
    // answering cost the whole connect timeout — and because the worker can be blocked
    // asking this very thread for a password, that wait can be a deadlock. The abort
    // makes the blocking call return now; the detached thread then finds `abandoned` set
    // and ends without reporting.
    m_shared->abandoned = true;
#if defined(LOFTAIL_HAVE_SSH)
    std::scoped_lock lock(m_shared->mutex);
    if (m_shared->session)
        m_shared->session->abort();
#endif
}

namespace {
#if defined(LOFTAIL_HAVE_SSH)

// A QThread, NOT a std::thread, and that is not a style choice.
//
// SshSession::connectTo() ends in QTcpSocket::waitForConnected(), and a QAbstractSocket
// needs the Qt event dispatcher of the thread it is used on. A raw std::thread has no
// QThreadData and therefore no dispatcher, so that call dereferenced a null one and
// crashed inside libQt6Network — a SEGV on a near-null address, on the worker thread,
// which is what AddressSanitizer caught in CI. SshFetcher::Worker is a QThread subclass
// for exactly this reason; this follows it.
class TransferThread : public QThread
{
public:
    std::function<void()> body;
    void run() override { body(); }
};

// Threads that have been started and not yet reaped.
//
// A finished one is deleted when the next transfer starts. Anything STILL RUNNING when
// the process ends stays here — reachable, so the leak checker does not report it, which
// is the device SourceSpool::drainRetired() uses for an abandoned fetcher and for the
// same reason: the alternative is joining, and joining a worker that may be blocked
// asking the application thread for a password is a deadlock.
std::mutex g_workersMutex;
std::vector<QThread *> g_workers;

// Delete the ones that have already ended. Caller holds g_workersMutex.
void reapFinishedLocked()
{
    for (auto it = g_workers.begin(); it != g_workers.end();) {
        if ((*it)->isFinished()) {
            delete *it;
            it = g_workers.erase(it);
        } else {
            ++it;
        }
    }
}

// Start `body` on a thread of its own, reaping any that have already finished.
void startWorker(std::function<void()> body)
{
    std::scoped_lock lock(g_workersMutex);
    reapFinishedLocked();
    auto *worker = new TransferThread;
    worker->body = std::move(body);
    g_workers.push_back(worker);
    worker->start();
}
#endif
} // namespace

namespace {
#if defined(LOFTAIL_HAVE_SSH)
// Connect for `address` and hand the open session to `body`, which does the one
// operation this transfer is for. Everything both directions share lives here.
template <class Body>
QString withSession(const QString &address, SshPrompter *prompter,
                    const std::shared_ptr<ConfigTransfer::Shared> &shared, Body body)
{
    const auto location = RemoteLocation::parse(address);
    if (!location || !location->isValid()) {
        return QCoreApplication::translate(
                   "loftail::ConfigFileIO", "Not a valid remote address: %1")
            .arg(RemoteLocation::withoutPassword(address));
    }

    // ONE CONNECT AT A TIME PER HOST, which is what keeps "one prompt per host" true now
    // that a config transfer can be in flight beside a log's own reconnect.
    SshConnectHold hold(location->target(), [shared]() { return shared->abandoned.load(); });
    if (!hold.held() || shared->abandoned)
        return {}; // asked to stop; the caller reports nothing

    auto session = std::make_unique<SshSession>();
    session->setAbandonCheck([shared]() { return shared->abandoned.load(); });
    {
        std::scoped_lock lock(shared->mutex);
        shared->session = session.get();
    }
    // Whatever happens below, the pointer must stop being publishable before the session
    // is destroyed, or a late abort() would aim at freed memory.
    struct Unpublish
    {
        std::shared_ptr<ConfigTransfer::Shared> shared;
        ~Unpublish()
        {
            std::scoped_lock lock(shared->mutex);
            shared->session = nullptr;
        }
    } unpublish{shared};

    QString error;
    if (!session->connectTo(*location, prompter, kConnectTimeoutMs, &error, nullptr))
        return error;
    if (shared->abandoned)
        return {};
    return body(*session, location->path);
}
#endif
} // namespace

void drainConfigTransfers(int budgetMs)
{
#if !defined(LOFTAIL_HAVE_SSH)
    Q_UNUSED(budgetMs);
#else
    // WAITING HERE IS THE POINT, and it is the one place this layer waits at all.
    //
    // A transfer is abandoned rather than joined when its tab goes (see ~ConfigTransfer),
    // which is right while the process is alive: the thread notices within a poll slice
    // and ends on its own. At SHUTDOWN that is not enough. Qt's own globals — the socket
    // engine handlers, the thread data — are torn down when the application object goes,
    // and a worker still inside QTcpSocket at that moment writes through a pointer that
    // has just become null. It crashed exactly there: a SEGV in
    // QAbstractSocketPrivate::initSocketLayer(), on the worker, under a leak-checking
    // run whose exit timing loses that race every time.
    //
    // So the owner drains before it goes. The wait is BOUNDED because a budget that can
    // be exceeded is better than a quit that can hang: every transfer has already been
    // told to stop by the time this runs, and the connect loop checks that between
    // 250 ms slices, so the budget is ample rather than tight.
    QElapsedTimer clock;
    clock.start();
    forever {
        {
            std::scoped_lock lock(g_workersMutex);
            reapFinishedLocked();
            if (g_workers.empty())
                return;
        }
        if (clock.elapsed() >= budgetMs)
            return; // left parked and reachable; better than blocking the quit
        QThread::msleep(10);
    }
#endif
}

void ConfigTransfer::startRead(const QString &address)
{
#if !defined(LOFTAIL_HAVE_SSH)
    ConfigReadResult out;
    configAddressIsWritable(address, &out.error);
    QTimer::singleShot(0, this, [this, out]() { emit readFinished(out); });
#else
    auto shared = m_shared;
    QPointer<ConfigTransfer> self(this);
    startWorker([address, shared, self]() {
        ConfigReadResult out;
        const QString error = withSession(
            address, &shared->relay, shared, [&out, &address](SshSession &session, const QString &path) {
                QString why;
                if (!session.readFileAt(path, &out.bytes, &out.existed, &why))
                    return why;
                if (out.bytes.size() > kMaxConfigBytes) {
                    out.bytes.clear();
                    return QCoreApplication::translate(
                               "loftail::ConfigFileIO",
                               "%1 is too large to edit as a config file.")
                        .arg(RemoteLocation::withoutPassword(address));
                }
                out.ok = true;
                return QString();
            });
        if (!error.isEmpty()) {
            out.ok = false;
            out.error = error;
        }
        if (shared->abandoned || !QCoreApplication::instance())
            return;
        // Back on the application thread. The QPointer is only ever DEREFERENCED there,
        // which is what makes carrying it across legal: if the owner went in the
        // meantime, this simply does nothing.
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, out]() {
                if (self)
                    emit self->readFinished(out);
            },
            Qt::QueuedConnection);
    });
#endif
}

void ConfigTransfer::startWrite(const QString &address, const QByteArray &bytes)
{
#if !defined(LOFTAIL_HAVE_SSH)
    Q_UNUSED(bytes);
    ConfigWriteResult out;
    configAddressIsWritable(address, &out.error);
    QTimer::singleShot(0, this, [this, out]() { emit writeFinished(out); });
#else
    auto shared = m_shared;
    QPointer<ConfigTransfer> self(this);
    startWorker([address, bytes, shared, self]() {
        ConfigWriteResult out;
        const QString error =
            withSession(address, &shared->relay, shared,
                        [&out, &bytes](SshSession &session, const QString &path) {
                            QString why;
                            if (!session.writeFileAt(path, bytes, &why))
                                return why;
                            out.ok = true;
                            return QString();
                        });
        if (!error.isEmpty()) {
            out.ok = false;
            out.error = error;
        }
        if (shared->abandoned || !QCoreApplication::instance())
            return;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, out]() {
                if (self)
                    emit self->writeFinished(out);
            },
            Qt::QueuedConnection);
    });
#endif
}

} // namespace loftail
