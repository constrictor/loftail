#include "SshFetcher.h"

#include "SshPrompter.h"
#include "SshSession.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr() — and these strings are user-facing all the same: they travel up to
// the status bar through Document::lastError() and LiveController::sourceStatusChanged.
// Q_DECLARE_TR_FUNCTIONS is what lets lupdate file them under a name that means
// something rather than under the file they happen to sit in.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::SshFetcher)
};
} // namespace


namespace {

// Read size per SFTP round trip. Large enough that priming a big log is not dominated
// by round-trip latency, small enough that committedSize advances often during a long
// prime so the user sees the view fill rather than a frozen count.
constexpr qint64 kChunkBytes = 256 * 1024;

// Fetched synchronously in start(), before the Document takes its 64 KB format sample
// (Document::prepare) — so autodetection and the format preview see real bytes rather
// than an empty file. The rest streams in on the fetcher thread.
constexpr qint64 kPrimeBytes = 128 * 1024;

// Compared against the spool's head when a server's FSTAT cannot be trusted to follow
// the handle. Only ever read on suspicion, never on the ordinary poll path.
constexpr qint64 kHeadProbeBytes = 4096;

// Ceiling on an UNATTENDED reconnect's own timeout (M13). Not a patience setting: the
// worker thread is joined by stop(), which the GUI thread reaches when the last tab on
// a log closes, so this is the worst case for closing a tab on a host that is down.
constexpr int kRetryTimeoutMs = 5000;

} // namespace

// ---------------------------------------------------------------------------

// Follows one remote file over SFTP, appending forward into a local spool.
//
// THREADING. start() runs on the thread that opened the document: it connects (which
// is the one place a person may be asked anything), primes enough bytes for a format
// sample, and only then hands the session to a worker thread that does the tailing.
// That is a handoff, not sharing — a LIBSSH2_SESSION is touched by exactly one thread
// at a time, and the GUI never blocks again after the open.
class SshFetcher final : public SourceFetcher
{
public:
    SshFetcher(RemoteLocation location, SshFetchOptions options)
        : m_location(std::move(location)), m_options(options)
    {
    }

    ~SshFetcher() override { stop(); }

    bool start(const QString &spoolDir, QString *error) override;
    void stop() override;

    FetchStatus status() const override
    {
        QMutexLocker lock(&m_mutex);
        return m_status;
    }

    QString spoolPath(quint64 generation) const override
    {
        if (m_spoolDir.isEmpty())
            return QString();
        return m_spoolDir + QStringLiteral("/gen-%1.log").arg(generation);
    }

    void pokeNow() override
    {
        QMutexLocker lock(&m_mutex);
        m_poked = true;
        // An explicit ask is the one thing that clears a refusal: the user has been
        // told why loftail stopped trying and has decided to try again anyway.
        m_reconnectRefused = false;
        m_wake.wakeAll();
    }

private:
    class Worker : public QThread
    {
    public:
        explicit Worker(SshFetcher *owner) : m_owner(owner) {}
        void run() override { m_owner->tailLoop(); }

    private:
        SshFetcher *m_owner;
    };

    void tailLoop();
    void pollOnce();
    bool establish(SshPrompter *prompter, QString *error, SshSession::Failure *failure);
    void reconnect();
    bool fetchForward(qint64 fromRemoteOffset, qint64 toRemoteOffset);
    void beginGeneration(qint64 remoteSize);
    bool remoteHeadDiffersFromSpool();
    void setError(const QString &message);
    void setWaiting(const QString &message);
    void setState(FetchStatus::State state);

    RemoteLocation  m_location;
    SshFetchOptions m_options;
    QString         m_spoolDir;

    std::unique_ptr<SshSession> m_session; // owned by start()'s thread, then by m_worker
    std::unique_ptr<Worker>     m_worker;

    mutable QMutex m_mutex;
    QWaitCondition m_wake;
    FetchStatus    m_status;
    bool           m_stopping = false;
    bool           m_poked = false;
    // Latched when a reconnect was REFUSED rather than merely unsuccessful — a changed
    // host key, rejected credentials, or a password needed with nobody to ask. The loop
    // then stops attempting reconnects and only waits, because retrying gets the same
    // answer forever. pokeNow() (File ▸ Reconnect, which does have a prompter) clears
    // it, so the user can always ask again; nothing else can.
    bool           m_reconnectRefused = false;

    // Fetcher-thread only, so no lock: the last stat, used by the rotation fallback.
    qint64 m_lastMtime = 0;
    qint64 m_lastSize = 0;
};

// Connect, open the remote file, and prime enough of it for a format sample. On success
// the session is live and the state is Live. Shared by start(), which runs on the thread
// that opened the document and may prompt, and by the worker's reconnect, which may not
// — the only difference between the two is the prompter, which is why this takes one.
bool SshFetcher::establish(SshPrompter *prompter, QString *error, SshSession::Failure *failure)
{
    // An unattended retry is IMPATIENT where the first attempt is patient, and that is
    // about closing rather than about connecting. stop() joins this thread, and it is
    // reached from the GUI thread when the last tab on a log closes — so however long a
    // connect blocks for is how long closing a tab on a dead host freezes the window.
    // A retry has nothing to lose by giving up early: the next one is seconds away.
    const int timeout = prompter ? m_options.timeoutMs
                                 : qMin(m_options.timeoutMs, kRetryTimeoutMs);

    // A retry out of Waiting stays Waiting until it actually gets somewhere. Announcing
    // "connecting…" on every attempt would flap the state several times a minute, and
    // because originVanished() reads it, the document upstream would bounce out of the
    // waiting state and straight back into it — a flickering view for a log that has
    // not moved. "Connecting" is for the first attempt, which a person is watching.
    if (status().state != FetchStatus::State::Waiting)
        setState(FetchStatus::State::Connecting);

    m_session = std::make_unique<SshSession>();
    if (!m_session->connectTo(m_location, prompter, timeout, error, failure)) {
        m_session.reset();
        return false;
    }
    if (!m_session->openFile(error, failure)) {
        m_session.reset();
        return false;
    }

    const SshSession::Attrs attrs = m_session->statPath();
    if (!attrs.valid) {
        if (error)
            *error = Tr::tr("Cannot read %1 on %2.").arg(m_location.path, m_location.host);
        // Connected, authenticated, opened — and then could not stat it. Whatever that
        // is, it is about the file rather than the trust, so it is worth trying again.
        *failure = SshSession::Failure::NoSuchFile;
        m_session.reset();
        return false;
    }
    m_lastMtime = attrs.mtime;
    m_lastSize = attrs.size;

    setState(FetchStatus::State::Priming);
    beginGeneration(attrs.size);

    // Enough for the format sample, synchronously, so the Document does not open on
    // an empty file and autodetect nothing.
    const qint64 base = status().baseOffset;
    const qint64 primeTo = qMin(attrs.size, base + kPrimeBytes);
    if (primeTo > base && !fetchForward(base, primeTo)) {
        if (error)
            *error = status().error;
        *failure = SshSession::Failure::Unreachable; // the transfer died mid-prime
        m_session.reset();
        return false;
    }

    setState(FetchStatus::State::Live);
    // Standing remark, not an error: this server would not do SFTP, so the log is being
    // read by running commands on it. Slower, and rotation is detected the weaker way,
    // so say which transport is in use rather than leaving it to be deduced (§6.3.1).
    {
        QMutexLocker lock(&m_mutex);
        m_status.note = m_session->mode() == SshSession::Mode::Exec
            ? Tr::tr("reading with shell commands — %1 does not offer SFTP")
                  .arg(m_location.host)
            : QString();
    }
    *failure = SshSession::Failure::None;
    return true;
}

bool SshFetcher::start(const QString &spoolDir, QString *error)
{
    m_spoolDir = spoolDir;

    SshSession::Failure failure = SshSession::Failure::None;
    if (!establish(sshPrompter(), error, &failure)) {
        // A host that is down and a log that has not been written yet are NOT failures
        // to open — they are the ordinary way a waiting log starts (SPEC.md §3, §6.5).
        // Return success with an empty spool and let the worker keep trying: the
        // document opens, says it is waiting, and fills in when the log turns up.
        //
        // Everything else really is a refusal — a changed host key, rejected
        // credentials, a cancelled prompt, or an unattended context that cannot ask —
        // and retrying it on a timer would get the same answer forever while hammering
        // a host loftail has just declined to talk to.
        if (failure != SshSession::Failure::Unreachable
            && failure != SshSession::Failure::NoSuchFile) {
            setState(FetchStatus::State::Error);
            return false;
        }
        setWaiting(error && !error->isEmpty() ? *error
                                              : Tr::tr("Cannot reach %1.").arg(m_location.host));
    }

    m_worker = std::make_unique<Worker>(this);
    m_worker->start();
    return true;
}

void SshFetcher::stop()
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_stopping && !m_worker)
            return;
        m_stopping = true;
        m_wake.wakeAll();
    }
    if (m_worker) {
        m_worker->wait();
        m_worker.reset();
    }
    // Only now is the session ours again: the worker was the last user of it.
    m_session.reset();
    setState(FetchStatus::State::Disconnected);
}

void SshFetcher::setState(FetchStatus::State state)
{
    QMutexLocker lock(&m_mutex);
    m_status.state = state;
    // Waiting carries its explanation the same way Error does, so neither clears it.
    if (state != FetchStatus::State::Error && state != FetchStatus::State::Waiting)
        m_status.error.clear();
}

void SshFetcher::setError(const QString &message)
{
    QMutexLocker lock(&m_mutex);
    m_status.state = FetchStatus::State::Error;
    m_status.error = message;
}

void SshFetcher::setWaiting(const QString &message)
{
    // Not an error: the host or the log is not there, and this fetcher is still trying.
    // SpooledLogSource::originVanished() reads this, which is how the document upstream
    // knows to show itself as waiting rather than as broken (§6.5).
    QMutexLocker lock(&m_mutex);
    m_status.state = FetchStatus::State::Waiting;
    m_status.error = message;
}

// Open a fresh spool file and publish it as the new generation. Never rewrites the
// current one: the index worker may be mmapping it, and record offsets index it.
void SshFetcher::beginGeneration(qint64 remoteSize)
{
    QMutexLocker lock(&m_mutex);
    const quint64 next = m_status.generation + 1;
    lock.unlock();

    // Where in the remote file this generation starts. Whole file by default, so a
    // remote log opens exactly like a local one; a tail start is opt-in for a log too
    // large to copy down.
    qint64 base = 0;
    if (m_options.tailStartBytes > 0 && remoteSize > m_options.tailStartBytes)
        base = remoteSize - m_options.tailStartBytes;

    const QString path = spoolPath(next);
    QFile spool(path);
    if (!spool.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(Tr::tr("Cannot write the local cache file %1.").arg(path));
        return;
    }
    spool.close();

    lock.relock();
    m_status.baseOffset = base;
    m_status.committedSize = 0;
    m_status.totalSize = remoteSize;
    m_status.generation = next; // published LAST, once its file exists and is empty
}

bool SshFetcher::fetchForward(qint64 fromRemoteOffset, qint64 toRemoteOffset)
{
    const quint64 generation = status().generation;
    const QString path = spoolPath(generation);
    QFile spool(path);
    if (!spool.open(QIODevice::Append)) {
        setError(Tr::tr("Cannot append to the local cache file %1.").arg(path));
        return false;
    }

    QByteArray buffer;
    buffer.resize(kChunkBytes);
    qint64 offset = fromRemoteOffset;

    while (offset < toRemoteOffset) {
        {
            QMutexLocker lock(&m_mutex);
            if (m_stopping)
                break;
            // A rotation started while this loop ran: abandon it, the new generation
            // owns the spool now.
            if (m_status.generation != generation)
                break;
        }

        const qint64 want = qMin(kChunkBytes, toRemoteOffset - offset);
        QString readError;
        const qint64 got = m_session->readAt(offset, buffer.data(), want, &readError);
        if (got < 0) {
            spool.close();
            setError(readError.isEmpty()
                         ? Tr::tr("Lost the connection to %1.").arg(m_location.host)
                         : readError);
            return false;
        }
        if (got == 0)
            break; // the remote file grew no further after all

        if (spool.write(buffer.constData(), got) != got || !spool.flush()) {
            spool.close();
            setError(Tr::tr("Cannot write to the local cache file %1.").arg(path));
            return false;
        }
        offset += got;

        // Publish only AFTER the bytes are on disk. That ordering is the entire
        // synchronisation between this thread and the GUI thread: a reader clamps to
        // committedSize, so it can never observe a half-written chunk (§6.3).
        QMutexLocker lock(&m_mutex);
        if (m_status.generation != generation)
            break;
        m_status.committedSize = offset - m_status.baseOffset;
        m_status.totalSize = qMax(m_status.totalSize, offset);
    }

    spool.close();
    return true;
}

bool SshFetcher::remoteHeadDiffersFromSpool()
{
    const FetchStatus current = status();
    const qint64 want = qMin(kHeadProbeBytes, current.committedSize);
    if (want <= 0)
        return false;

    QByteArray remote;
    remote.resize(want);
    QString readError;
    if (m_session->readAt(current.baseOffset, remote.data(), want, &readError) != want)
        return false;

    QFile spool(spoolPath(current.generation));
    if (!spool.open(QIODevice::ReadOnly))
        return false;
    const QByteArray local = spool.read(want);
    spool.close();
    return local != remote;
}

void SshFetcher::pollOnce()
{
    const SshSession::Attrs byName = m_session->statPath();
    if (!byName.valid) {
        if (!m_session->isConnected()) {
            // The connection dropped, not the file. Let go of the dead session so the
            // next turn of the loop re-establishes it — before M13 nothing ever did,
            // and a link that blipped stayed broken until the log was reopened.
            m_session.reset();
            setWaiting(Tr::tr("Lost the connection to %1 — reconnecting…")
                           .arg(m_location.host));
            return;
        }
        // Connected, and the path is not there: mid-rotation, or the log really has
        // been removed on the far end. Either way it is a WAIT — the next poll
        // resolves a rotation, and a removal is what the document upstream shows as
        // waiting (§6.5). Nothing is torn down either way.
        setWaiting(Tr::tr("%1 is not readable on %2 right now.")
                       .arg(m_location.path, m_location.host));
        return;
    }

    const FetchStatus current = status();
    const qint64 consumed = current.baseOffset + current.committedSize;

    bool rotated = false;
    if (byName.size < consumed) {
        // Shrank below what we already read: truncation, or a rotate to a shorter file.
        rotated = true;
    } else if (m_session->fstatTracksHandle()) {
        // The inode substitute: our handle still refers to the file we opened, while
        // stat() re-resolves the name. A disagreement means the name now points
        // somewhere else — including the same-size rotate that a size check misses.
        const SshSession::Attrs byHandle = m_session->statHandle();
        if (byHandle.valid && byHandle.size != byName.size)
            rotated = true;
    } else if (byName.mtime > m_lastMtime && byName.size == m_lastSize) {
        // FSTAT cannot be trusted on this server. Fall back to comparing the head of
        // the file — but only on suspicion (it changed without growing), never as
        // part of the ordinary poll.
        rotated = remoteHeadDiffersFromSpool();
    }

    m_lastMtime = byName.mtime;
    m_lastSize = byName.size;

    if (rotated) {
        m_session->closeFile();
        QString openError;
        SshSession::Failure openFailure = SshSession::Failure::None;
        if (!m_session->openFile(&openError, &openFailure)) {
            // Mid-rotation the new file may not exist for a moment, and after a removal
            // it never will: both are a wait, and the next poll sorts out which.
            if (openFailure == SshSession::Failure::NoSuchFile)
                setWaiting(openError);
            else
                setError(openError);
            return;
        }
        const SshSession::Attrs fresh = m_session->statPath();
        beginGeneration(fresh.valid ? fresh.size : 0);
        setState(FetchStatus::State::Priming);
        const FetchStatus started = status();
        if (fresh.valid && fresh.size > started.baseOffset)
            fetchForward(started.baseOffset, fresh.size);
        setState(FetchStatus::State::Live);
        return;
    }

    if (byName.size > consumed) {
        if (fetchForward(consumed, byName.size))
            setState(FetchStatus::State::Live);
        return;
    }

    // No change. Clear a previous error or wait so a blip does not stick in the status
    // bar, and so a log that reappeared stops reporting itself as missing.
    if (current.state == FetchStatus::State::Error
        || current.state == FetchStatus::State::Waiting) {
        setState(FetchStatus::State::Live);
    }
}

// One unattended attempt to (re)establish the session, from the worker thread.
//
// NO PROMPTER, deliberately and without exception. A prompt is a modal dialog on the
// GUI thread, and marshalling one out of here would mean a dialog appearing while the
// user is doing something else, for a log they may have opened hours ago. So a retry
// gets the SSH agent, the usual key files, and any password already accepted for this
// host — the common case, and entirely automatic. Anything that genuinely needs a
// person says so and waits to be asked again through File ▸ Reconnect, which runs on
// the GUI thread and does have a prompter (§6.3, §6.5).
void SshFetcher::reconnect()
{
    QString error;
    SshSession::Failure failure = SshSession::Failure::None;
    if (establish(nullptr, &error, &failure))
        return;

    if (failure == SshSession::Failure::Unreachable
        || failure == SshSession::Failure::NoSuchFile) {
        setWaiting(error);
        return;
    }

    // Refused, or needing a person there is nobody to be. Either way the next hundred
    // attempts get the same answer, so stop making them and say why; the state stays
    // Waiting for NeedsPerson, because from the user's side the log is still coming
    // once they sign in, and Error for a genuine refusal.
    if (failure == SshSession::Failure::NeedsPerson)
        setWaiting(error);
    else
        setError(error);
    QMutexLocker lock(&m_mutex);
    m_reconnectRefused = true;
}

void SshFetcher::tailLoop()
{
    forever {
        bool refused = false;
        {
            QMutexLocker lock(&m_mutex);
            if (m_stopping)
                return;
            refused = m_reconnectRefused;
        }

        if (m_session)
            pollOnce();
        else if (!refused)
            reconnect();
        // else: loftail has been told no. Sleep until poked (File ▸ Reconnect) or
        // stopped, rather than asking a host that has already refused.

        QMutexLocker lock(&m_mutex);
        if (m_stopping)
            return;
        if (!m_poked) {
            // A connection that is failing, or a log that is not there yet, backs off
            // rather than hammering the host. Waiting paces with Error because it is
            // the same network cost — the difference between them is what the user is
            // told, not how often loftail tries.
            const bool slow = m_status.state == FetchStatus::State::Error
                || m_status.state == FetchStatus::State::Waiting;
            const int wait = slow ? qMax(m_options.pollMs, 5000) : m_options.pollMs;
            m_wake.wait(&m_mutex, static_cast<unsigned long>(wait));
        }
        m_poked = false;
    }
}

std::unique_ptr<SourceFetcher> makeSshFetcher(const RemoteLocation &location, QString *error)
{
    Q_UNUSED(error);
    return std::make_unique<SshFetcher>(location, sshFetchOptions(location));
}

} // namespace loftail
