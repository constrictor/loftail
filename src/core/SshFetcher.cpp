#include "SshFetcher.h"

#include "PromptRelay.h"
#include "SshPrompter.h"
#include "SshSession.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QElapsedTimer>
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

// Ceiling on an UNATTENDED reconnect's own timeout (M13). This used to be the worst case
// for closing a tab on a host that is down, because stop() joined the worker; nothing
// joins it now (SourceSpool.h, retire()). The value survives its original reason on a
// better one: a background retry has nothing to lose by giving up early — the next
// attempt is seconds away — and every second it spends is a second the unreachable host
// is not being left alone. Do not delete this along with the reason it was written for.
constexpr int kRetryTimeoutMs = 5000;

// How often the head compare may run when a STALLED SIZE is the only rotation signal
// there is — a server with no `stat`, and therefore no mtime (M16, §6.3.1). Paced by the
// clock rather than by the poll because a stalled size is also exactly what an idle log
// looks like, and an idle log must not cost a read per second forever.
constexpr qint64 kStallProbeMs = 30000;

// The `wc` rung reads the whole file to answer, so a log measured that way is polled far
// more slowly than one that can be stat'd, and abandoned outright once it is large. Both
// numbers are about the SERVER: observing a log must not disturb the machine producing
// it (invariant #5), and a note in the status bar is not a substitute for not doing it.
constexpr int    kWcMinPollMs = 15000;
constexpr qint64 kWcAbandonBytes = 64 * 1024 * 1024;

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

    // The one place a worker is joined, and it is not a wait: the reaper destroys a
    // retired fetcher only once isStopped() reads true, so this returns at once.
    ~SshFetcher() override
    {
        requestStop();
        if (m_worker) {
            m_worker->wait();
            m_worker.reset();
        }
        // Only now is the session ours again: the worker was the last user of it.
        resetSession();
    }

    bool start(const QString &spoolDir, QString *error) override;
    void requestStop() override;

    bool isStopped() const override
    {
        // isFinished() is safe from any thread, and m_worker itself is written only by
        // start() — before any other thread can hold this fetcher — and cleared only by
        // the destructor, which the reaper reaches only once this has read true.
        return !m_worker || m_worker->isFinished();
    }

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
    bool stallProbeDue();
    void setError(const QString &message);
    void setWaiting(const QString &message);
    void setState(FetchStatus::State state);

    bool stopping() const
    {
        QMutexLocker lock(&m_mutex);
        return m_stopping;
    }

    // Make the live session reachable by abort(), or stop making it so. Called only from
    // the thread that OWNS the session, and always with the withdrawal happening before
    // the object goes away — which is what makes requestStop()'s abort safe to issue from
    // the GUI thread at any moment: under m_mutex, m_abortable is either a live session
    // or null, never a dangling one.
    void publishSession(SshSession *session)
    {
        QMutexLocker lock(&m_mutex);
        m_abortable = session;
    }

    void resetSession()
    {
        publishSession(nullptr);
        m_session.reset();
    }

    RemoteLocation  m_location;
    SshFetchOptions m_options;
    QString         m_spoolDir;

    std::unique_ptr<SshSession> m_session; // owned by start()'s thread, then by m_worker
    std::unique_ptr<Worker>     m_worker;
    // The same session as m_session when there is one, but guarded — the one handle
    // another thread may touch, and only to call abort(). See publishSession().
    SshSession                 *m_abortable = nullptr;

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
    qint64 m_lastMtime = kUnknownMtime;
    qint64 m_lastSize = 0;
    // Fetcher-thread only. Left INVALID so the first stall probes at once; restarted only
    // when a compare actually runs, and deliberately not reset when the log grows — a
    // bursty log alternates growing and stalling, and resetting on growth would fire the
    // compare on every other poll, which is the whole thing this exists to prevent.
    QElapsedTimer m_stallProbe;
};

// Connect, open the remote file, and prime enough of it for a format sample. On success
// the session is live and the state is Live. Shared by start(), which runs on the thread
// that opened the document and may prompt, and by the worker's reconnect, which may not
// — the only difference between the two is the prompter, which is why this takes one.
bool SshFetcher::establish(SshPrompter *prompter, QString *error, SshSession::Failure *failure)
{
    // An unattended retry is IMPATIENT where the first attempt is patient. A person
    // watching a connect they asked for will wait; a timer retrying in the background
    // has nothing to lose by giving up early, because the next attempt is seconds away
    // and every second spent in this one is a second the host is not being left alone.
    const int timeout = prompter ? m_options.timeoutMs
                                 : qMin(m_options.timeoutMs, kRetryTimeoutMs);

    // A retry out of Waiting stays Waiting until it actually gets somewhere. Announcing
    // "connecting…" on every attempt would flap the state several times a minute, and
    // because originVanished() reads it, the document upstream would bounce out of the
    // waiting state and straight back into it — a flickering view for a log that has
    // not moved. "Connecting" is for the first attempt, which a person is watching.
    if (status().state != FetchStatus::State::Waiting)
        setState(FetchStatus::State::Connecting);

    resetSession();
    // One connect at a time to this host, so that N files on it still cost ONE password
    // prompt — the property that used to fall out of every connect running on the GUI
    // thread, and that has to be arranged now that they do not (SshPrompter.h). Held
    // until this function returns, which is deliberately past authentication: the point
    // is for the waiters to find the password in the cache rather than ask for it.
    SshConnectHold hold(m_location.target(), [this]() { return stopping(); });
    if (!hold.held()) {
        if (error)
            *error = Tr::tr("Cancelled while waiting to connect to %1.").arg(m_location.host);
        *failure = SshSession::Failure::Unreachable;
        return false;
    }

    m_session = std::make_unique<SshSession>();
    // So a connect gives up as soon as nobody wants it any more, rather than running out
    // its timeout first. Read from this thread only, which is why it can look at
    // m_stopping through the ordinary accessor.
    m_session->setAbandonCheck([this]() { return stopping(); });
    publishSession(m_session.get());

    // Every question goes through the relay, whichever thread this is on. On the
    // application thread the gate runs it inline, so an interactive open behaves exactly
    // as it did; off it, the question travels and the answer comes back. A null prompter
    // is still a null prompter — the relay refuses on its behalf rather than asking.
    PromptRelay relay(prompter);
    SshPrompter *asker = prompter ? &relay : nullptr;
    if (!m_session->connectTo(m_location, asker, timeout, error, failure)) {
        resetSession();
        return false;
    }
    if (!m_session->openFile(error, failure)) {
        resetSession();
        return false;
    }

    const SshSession::Attrs attrs = m_session->statPath();
    if (!attrs.valid) {
        if (error)
            *error = Tr::tr("Cannot read %1 on %2.").arg(m_location.path, m_location.host);
        // Connected, authenticated, opened — and then could not stat it. Whatever that
        // is, it is about the file rather than the trust, so it is worth trying again.
        *failure = SshSession::Failure::NoSuchFile;
        resetSession();
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
        resetSession();
        return false;
    }

    setState(FetchStatus::State::Live);
    // Standing remark, not an error: this server would not do SFTP, so the log is being
    // read by running commands on it. Slower, and rotation is detected the weaker way,
    // so say which transport is in use rather than leaving it to be deduced (§6.3.1).
    // How MUCH weaker depends on what the server can be measured with, so the note says
    // that too. Set once here: a re-settle on a later rotation can leave it slightly
    // stale, which is worth less than a status bar that rewrites itself mid-tail.
    {
        QMutexLocker lock(&m_mutex);
        if (m_session->mode() != SshSession::Mode::Exec) {
            m_status.note.clear();
        } else {
            switch (m_session->sizeSource()) {
            case SizeSource::Wc:
                m_status.note = Tr::tr("reading with shell commands — %1 offers neither "
                                       "SFTP nor `stat` nor `ls`, so the log is measured "
                                       "by reading all of it and is checked rarely")
                                    .arg(m_location.host);
                break;
            case SizeSource::Ls:
                m_status.note = Tr::tr("reading with shell commands — %1 offers neither "
                                       "SFTP nor `stat`, so a rotation is noticed within "
                                       "about half a minute rather than at once")
                                    .arg(m_location.host);
                break;
            case SizeSource::Stat:
            case SizeSource::None:
                m_status.note = Tr::tr("reading with shell commands — %1 does not offer "
                                       "SFTP")
                                    .arg(m_location.host);
                break;
            }
        }
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

void SshFetcher::requestStop()
{
    QMutexLocker lock(&m_mutex);
    if (m_stopping)
        return;
    m_stopping = true;
    m_wake.wakeAll();

    // Wakes a worker sleeping between polls; the abort below is for one that is inside
    // libssh2, where the only thing that would otherwise end the wait is the session
    // timeout. Between them there is no state in which asking this fetcher to stop
    // leaves it blocked for longer than a moment — which is what lets the registry bury
    // it without waiting and lets quitting stay quick (SourceSpool.h, retire()).
    if (m_abortable)
        m_abortable->abort();
    // Published HERE rather than when the worker actually exits, because Cancel keeps
    // the spool readable and its document on screen (SourceSpool::cancel): the state a
    // reader sees the moment it cancels should be the state it asked for, not the one
    // it gets a poll later. Nothing waits on the worker to make this true.
    m_status.state = FetchStatus::State::Disconnected;
    m_status.error.clear();
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

bool SshFetcher::stallProbeDue()
{
    if (m_stallProbe.isValid() && m_stallProbe.elapsed() < kStallProbeMs)
        return false;
    // start(), not restart(): restart() on a timer that was never started reports a
    // meaningless elapsed value, and this one is deliberately invalid to begin with.
    m_stallProbe.start();
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
            resetSession();
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

    // Measured by reading the whole file, and now large enough that doing so once every
    // kWcMinPollMs is no longer a reasonable thing to do to somebody else's machine.
    // Reported rather than quietly throttled further: the remedy is one utility away.
    if (m_session->sizeSource() == SizeSource::Wc && byName.size > kWcAbandonBytes) {
        setError(Tr::tr("%1 has grown past %2 MB, and %3 offers no way to measure it "
                        "except by reading all of it. Installing `stat` or `ls` on the "
                        "server fixes this.")
                     .arg(m_location.path)
                     .arg(kWcAbandonBytes / (1024 * 1024))
                     .arg(m_location.host));
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
    } else if (byName.mtime == kUnknownMtime) {
        // This server has no `stat`, so "it changed without growing" cannot be asked at
        // all — there is no mtime to ask it about. What is left is a size that has
        // STALLED, which is also exactly what an idle log looks like, so the compare is
        // paced by the clock instead: at most one small read every kStallProbeMs.
        //
        // THIS BRANCH MUST COME BEFORE THE ONE BELOW. kUnknownMtime is -1, and -1 > -1
        // is false, so falling through to the mtime comparison would silently switch
        // rotation detection off on exactly the servers that need it most.
        //
        // A growing log never reaches here — the size test below is false while it
        // grows — so the standing cost is paid only by a log nobody is writing to.
        if (byName.size == m_lastSize && stallProbeDue())
            rotated = remoteHeadDiffersFromSpool();
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
            // it never will: both are a wait, and the next poll sorts out which. So is a
            // link that died while reopening, which an exec session can now report from
            // here — everything Failure calls retryable-unattended waits rather than
            // erroring, because an error is a claim that waiting will not help.
            if (openFailure == SshSession::Failure::NoSuchFile
                || openFailure == SshSession::Failure::Unreachable)
                setWaiting(openError);
            else
                setError(openError);
            return;
        }
        const SshSession::Attrs fresh = m_session->statPath();
        // The assignment above recorded the size of the file we have just rotated AWAY
        // from. Under the stalled-size rule that would read as a stall on the next poll
        // and spend a head compare answering a question nobody asked.
        if (fresh.valid)
            m_lastSize = fresh.size;
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
            int wait = slow ? qMax(m_options.pollMs, 5000) : m_options.pollMs;
            // A log that can only be measured by reading all of it is polled far more
            // slowly than the user asked for, and that is not a tuning decision to leave
            // to them: the default is once a second, and once a second is a whole log
            // re-read every second on the machine being observed (invariant #5).
            if (m_session && m_session->sizeSource() == SizeSource::Wc)
                wait = qMax(wait, kWcMinPollMs);
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
