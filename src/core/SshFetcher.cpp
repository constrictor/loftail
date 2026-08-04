#include "SshFetcher.h"

#include "SshPrompter.h"
#include "SshSession.h"

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>

namespace loftail {

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

} // namespace

// ---------------------------------------------------------------------------

// Follows one remote file over SFTP, appending forward into a local spool.
//
// THREADING. start() runs on the thread that opened the document: it connects (which
// is the one place a person may be asked anything), primes enough bytes for a format
// sample, and only then hands the session to a worker thread that does the tailing.
// That is a handoff, not sharing — a LIBSSH2_SESSION is touched by exactly one thread
// at a time, and the GUI never blocks again after the open.
class SshFetcher final : public RemoteFetcher
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
    bool fetchForward(qint64 fromRemoteOffset, qint64 toRemoteOffset);
    void beginGeneration(qint64 remoteSize);
    bool remoteHeadDiffersFromSpool();
    void setError(const QString &message);
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

    // Fetcher-thread only, so no lock: the last stat, used by the rotation fallback.
    qint64 m_lastMtime = 0;
    qint64 m_lastSize = 0;
};

bool SshFetcher::start(const QString &spoolDir, QString *error)
{
    m_spoolDir = spoolDir;
    setState(FetchStatus::State::Connecting);

    m_session = std::make_unique<SshSession>();
    if (!m_session->connectTo(m_location, sshPrompter(), m_options.timeoutMs, error)) {
        m_session.reset();
        setState(FetchStatus::State::Error);
        return false;
    }
    if (!m_session->openFile(error)) {
        m_session.reset();
        setState(FetchStatus::State::Error);
        return false;
    }

    const SshSession::Attrs attrs = m_session->statPath();
    if (!attrs.valid) {
        if (error)
            *error = QStringLiteral("Cannot read %1 on %2.").arg(m_location.path, m_location.host);
        m_session.reset();
        setState(FetchStatus::State::Error);
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
        const FetchStatus current = status();
        if (error)
            *error = current.error;
        m_session.reset();
        return false;
    }

    setState(FetchStatus::State::Live);
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
    if (state != FetchStatus::State::Error)
        m_status.error.clear();
}

void SshFetcher::setError(const QString &message)
{
    QMutexLocker lock(&m_mutex);
    m_status.state = FetchStatus::State::Error;
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
        setError(QStringLiteral("Cannot write the local cache file %1.").arg(path));
        return;
    }
    spool.close();

    lock.relock();
    m_status.baseOffset = base;
    m_status.committedSize = 0;
    m_status.remoteSize = remoteSize;
    m_status.generation = next; // published LAST, once its file exists and is empty
}

bool SshFetcher::fetchForward(qint64 fromRemoteOffset, qint64 toRemoteOffset)
{
    const quint64 generation = status().generation;
    const QString path = spoolPath(generation);
    QFile spool(path);
    if (!spool.open(QIODevice::Append)) {
        setError(QStringLiteral("Cannot append to the local cache file %1.").arg(path));
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
                         ? QStringLiteral("Lost the connection to %1.").arg(m_location.host)
                         : readError);
            return false;
        }
        if (got == 0)
            break; // the remote file grew no further after all

        if (spool.write(buffer.constData(), got) != got || !spool.flush()) {
            spool.close();
            setError(QStringLiteral("Cannot write to the local cache file %1.").arg(path));
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
        m_status.remoteSize = qMax(m_status.remoteSize, offset);
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
        // The path is gone — mid-rotation, most likely. Say so and try again next
        // tick rather than tearing anything down.
        setError(QStringLiteral("%1 is not readable on %2 right now.")
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
        if (!m_session->openFile(&openError)) {
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

    // No change. Clear a previous error so a blip does not stick in the status bar.
    if (current.state == FetchStatus::State::Error)
        setState(FetchStatus::State::Live);
}

void SshFetcher::tailLoop()
{
    forever {
        {
            QMutexLocker lock(&m_mutex);
            if (m_stopping)
                return;
        }

        pollOnce();

        QMutexLocker lock(&m_mutex);
        if (m_stopping)
            return;
        if (!m_poked) {
            // A failing connection backs off rather than hammering the host.
            const int wait = m_status.state == FetchStatus::State::Error
                ? qMax(m_options.pollMs, 5000)
                : m_options.pollMs;
            m_wake.wait(&m_mutex, static_cast<unsigned long>(wait));
        }
        m_poked = false;
    }
}

std::unique_ptr<RemoteFetcher> makeSshFetcher(const RemoteLocation &location, QString *error)
{
    Q_UNUSED(error);
    return std::make_unique<SshFetcher>(location, sshFetchOptions(location));
}

} // namespace loftail
