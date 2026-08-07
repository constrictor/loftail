#include "ArchiveFetcher.h"

#include "ArchiveReader.h"
#include "LogSource.h"
#include "RemoteLocation.h"
#include "SpooledLogSource.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QDeadlineTimer>
#include <QFile>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QStorageInfo>
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
    Q_DECLARE_TR_FUNCTIONS(loftail::ArchiveFetcher)
};
} // namespace


namespace {

// Written per pass through the expansion loop. Large enough that decompression is not
// dominated by loop overhead, small enough that committedSize advances often during a
// long expansion, so the user watches the view fill rather than a frozen count.
constexpr qint64 kChunkBytes = 256 * 1024;

// Expanded synchronously in start(), before the Document takes its 64 KB format sample
// (Document::prepare) — so autodetection and the format preview see real bytes rather
// than an empty file. The rest streams in on the fetcher thread.
constexpr qint64 kPrimeBytes = 128 * 1024;

// How long the reader waits for a container that is still arriving before asking again
// whether it ever will. Only reached for a REMOTE container: a local one is complete
// the moment it is opened.
constexpr int kAwaitSliceMs = 100;

// What an unknown expanded size is guessed at, as a multiple of the compressed input,
// for the free-space check alone. Text compresses around 10:1, so this is the right
// order of magnitude for a log and errs toward refusing an open that would fill the
// disk. It bounds nothing: an archive that clears the check expands in full.
constexpr qint64 kAssumedRatio = 10;

} // namespace

// ---------------------------------------------------------------------------

// Expands one archive member forward into a local spool.
//
// THE INPUT IS AN ORDINARY LogSource, which is what makes a remote archive free: for a
// local container it is a MappedLogSource, for one on another machine it is a
// SpooledLogSource over the SSH fetcher's own spool, and nothing here can tell (§6.4).
// So two fetchers chain — SSH downloads the .tar.gz while this expands what has
// arrived — rather than either knowing about the other.
//
// THREADING. start() runs on the thread that opened the document: it opens the
// container (which for a remote one is the single place a person may be asked
// anything), expands enough bytes for a format sample, and only then hands the input
// and the stream to a worker thread. That is a handoff, not sharing — which is also
// why the per-instance form of the refreshSize() rule matters here (SpooledLogSource.h):
// this fetcher's input is private to it and is refreshed only from whichever of its own
// threads currently holds it.
class ArchiveFetcher final : public SourceFetcher
{
public:
    explicit ArchiveFetcher(ArchiveLocation location) : m_location(std::move(location)) {}

    // The one place a worker is joined, and it is not a wait: the reaper destroys a
    // retired fetcher only once isStopped() reads true, so this returns at once.
    ~ArchiveFetcher() override
    {
        requestStop();
        if (m_worker) {
            m_worker->wait();
            m_worker.reset();
        }
        // Only now are these ours again: the worker was the last user of them.
        m_stream.reset();
        m_input.reset();
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

    // Nothing to poke: an expansion is one continuous pass, not a poll loop. The
    // generation never changes either — a member is written once and never rotates —
    // so wasReplaced() is permanently false for an archived log.
    void pokeNow() override {}

private:
    class Worker : public QThread
    {
    public:
        explicit Worker(ArchiveFetcher *owner) : m_owner(owner) {}
        void run() override
        {
            // Opening the member is the worker's first job, not something start() has
            // already done. For a compressed tar that means decompressing the container
            // until the member is reached, and for a container that is not there yet — a
            // `.gz` on a host that is down — waiting for it to appear; neither belongs
            // on the thread that opened the document (§6.4, §6.5).
            if (!m_owner->awaitContainer())
                return;
            m_owner->expandRest();
        }

    private:
        ArchiveFetcher *m_owner;
    };

    // Open the member, check the space, and prime enough for a format sample. Called
    // from start() when the container is readable, and from the worker when it was not.
    bool beginExpansion(QString *error);
    // Block until the container's transport has something to give, then beginExpansion().
    // Worker thread only. False if the fetcher was stopped or the open failed.
    bool awaitContainer();
    void expandRest();
    bool checkFreeSpace(qint64 expandedSize, QString *error) const;
    // Expands at most `limit` bytes (0 meaning "to the end"). Returns false on error;
    // sets `finished` when the member has been read to its end.
    bool expand(qint64 limit, bool *finished);
    void publishHeldCommits();
    bool awaitInput();
    void beginGeneration(qint64 expandedSize);
    void publishComplete();
    void setError(const QString &message);
    void setState(FetchStatus::State state);
    bool stopping() const;

    ArchiveLocation m_location;
    QString         m_spoolDir;

    std::unique_ptr<LogSource>     m_input;  // the container's bytes; see the note above
    std::unique_ptr<ArchiveStream> m_stream;
    std::unique_ptr<Worker>        m_worker;

    // Worker-thread only: true while the prime is running, so expand() records what it
    // has written without publishing it. m_heldCommit is the pending figure and is
    // written under m_mutex like the rest of the status it belongs to.
    bool   m_holdBackCommits = false;
    qint64 m_heldCommit = 0;

    mutable QMutex m_mutex;
    QWaitCondition m_wake;
    FetchStatus    m_status;
    bool           m_stopping = false;
};

bool ArchiveFetcher::start(const QString &spoolDir, QString *error)
{
    m_spoolDir = spoolDir;
    setState(FetchStatus::State::Connecting);

    QString openError;
    // As BYTES, not as a log: openLogSource() on a container means "expand it", which
    // for a single-stream container is this very fetcher (LogSource.h).
    //
    // STAYS ON THE CALLER'S THREAD while everything after it moves to the worker, and
    // that is deliberate rather than an oversight: this reaches
    // SourceSpoolRegistry::acquire(), which is GUI-thread-only and re-entrant by design
    // (SourceSpool.h). It costs nothing to leave here now — for a remote container it
    // builds an SSH fetcher whose own start() no longer blocks either.
    m_input = openContainerSource(m_location.container, OpenPolicy::Interactive, &openError);
    if (!m_input) {
        if (error) {
            *error = openError.isEmpty()
                ? Tr::tr("Cannot open %1.").arg(m_location.container)
                : openError;
        }
        setError(error && !error->isEmpty() ? *error : openError);
        return false;
    }

    // EVERYTHING BLOCKING IS THE WORKER'S FROM HERE. Opening the member means seeking to
    // it, and for a compressed tar that decompresses the container until it is reached —
    // on the thread that opened the document, which is how a large `.tar.gz` used to
    // freeze the window for as long as the scan took. Waiting for a container that is not
    // there yet has always been the worker's (§6.5); this simply stops treating the two
    // cases differently, since neither can happen here.
    //
    // What the caller gets back is a legal, empty spool in State::Connecting, which
    // notReadyYet() reads as "wait": the tab appears at once and fills in as the member
    // expands. From then on it stays in Priming — "the initial bulk fetch into the spool"
    // — until Complete. Never Live, which means "following the input for more bytes": an
    // expansion has a fixed amount of work and then stops, so there is nothing to follow.
    m_worker = std::make_unique<Worker>(this);
    m_worker->start();
    return true;
}

// Wait until the container has bytes to read, then open the member. Worker thread only —
// this is everything about start() that can block for an unbounded time, which since M17
// is all of it: seeking to a member decompresses the container up to it.
bool ArchiveFetcher::awaitContainer()
{
    forever {
        if (stopping())
            return false;
        if (!m_input) // start() never got one
            return false;
        m_input->refreshSize();
        // notReadyYet() as well as originVanished(), and without it this loop falls
        // straight through on a remote container: its fetcher is Connecting, not
        // Waiting, so the vanish test says no while there is not a byte to read.
        // beginExpansion() would then see a zero-length input — checkFreeSpace() would
        // silently guess that nothing is needed, and ArchiveStream::open() would get no
        // header until the await inside readBlock happened to supply one.
        if (!m_input->originVanished() && !m_input->notReadyYet())
            break;
        QMutexLocker lock(&m_mutex);
        if (m_stopping)
            return false;
        m_wake.wait(&m_mutex, QDeadlineTimer(kAwaitSliceMs));
    }

    QString beginError;
    if (!beginExpansion(&beginError))
        return false;
    return m_stream != nullptr; // false when the prime finished the whole member
}

bool ArchiveFetcher::beginExpansion(QString *error)
{
    // A local container is whole the moment it is opened, so it needs no await and is
    // handed to the reader as seekable. Only a container still being fetched from
    // another machine gets one — and pays for it by being read as a stream.
    AwaitInput await;
    if (dynamic_cast<SpooledLogSource *>(m_input.get()))
        await = [this] { return awaitInput(); };

    QString streamError;
    m_stream = ArchiveStream::open(m_input.get(), std::move(await),
                                   ArchiveLocation::isSingleStreamName(m_location.container),
                                   &streamError);
    if (!m_stream) {
        if (error)
            *error = streamError;
        setError(streamError);
        m_input.reset();
        return false;
    }

    if (!m_stream->seekToMember(m_location.member, &streamError)) {
        if (error)
            *error = streamError;
        setError(streamError);
        m_stream.reset();
        m_input.reset();
        return false;
    }

    // Refuse before writing anything rather than filling the disk and failing partway.
    // This is the failure that actually happens: a 200 MB .gz is 2 GB expanded, and the
    // cache lives on the user's home filesystem.
    QString spaceError;
    if (!checkFreeSpace(m_stream->currentSize(), &spaceError)) {
        if (error)
            *error = spaceError;
        setError(spaceError);
        m_stream.reset();
        m_input.reset();
        return false;
    }

    setState(FetchStatus::State::Priming);
    beginGeneration(m_stream->currentSize());

    // Enough for the format sample before anything upstream is told there are bytes, so
    // the Document does not settle its format and its encoding against a fraction of one.
    // Published all at once — the rule in SourceFetcher.h — and BEFORE publishComplete()
    // below, or a member small enough to finish inside the prime would announce itself
    // finished while still reporting a committed size of zero, inverting the one ordering
    // the live controller relies on.
    bool finished = false;
    m_holdBackCommits = true;
    const bool primed = expand(kPrimeBytes, &finished);
    m_holdBackCommits = false;
    if (!primed) {
        if (error)
            *error = status().error;
        m_stream.reset();
        m_input.reset();
        return false;
    }
    publishHeldCommits();

    if (finished) {
        // A small member expanded entirely during the prime. There is no work left for
        // a worker thread to do, and the stream ends here. The null m_stream is what
        // tells the caller so — start() then spawns no worker, and awaitContainer()
        // returns having nothing left to hand on to expandRest().
        m_stream.reset();
        m_input.reset();
        publishComplete();
    }
    return true;
}

void ArchiveFetcher::requestStop()
{
    QMutexLocker lock(&m_mutex);
    if (m_stopping)
        return;
    m_stopping = true;
    m_wake.wakeAll();

    // Published HERE rather than when the worker actually exits, because Cancel keeps
    // the spool readable and its document on screen (SourceSpool::cancel): the state a
    // reader sees the moment it cancels should be the state it asked for, not the one
    // it gets a poll later. Nothing waits on the worker to make this true.
    //
    // A finished expansion stays finished. Reporting Disconnected here would tell the
    // live controller that the stream might yet grow, undoing what completion means.
    if (m_status.state != FetchStatus::State::Complete)
        m_status.state = FetchStatus::State::Disconnected;
}

bool ArchiveFetcher::stopping() const
{
    QMutexLocker lock(&m_mutex);
    return m_stopping;
}

void ArchiveFetcher::setState(FetchStatus::State state)
{
    QMutexLocker lock(&m_mutex);
    m_status.state = state;
    // Waiting carries its explanation the same way Error does, so neither clears it.
    if (state != FetchStatus::State::Error && state != FetchStatus::State::Waiting)
        m_status.error.clear();
}

void ArchiveFetcher::setError(const QString &message)
{
    QMutexLocker lock(&m_mutex);
    m_status.state = FetchStatus::State::Error;
    m_status.error = message;
}

// Whether the container has more bytes coming. A local container is whole the moment it
// is opened, so end of file is the end. A remote one is still being downloaded, and the
// honest answer comes from the fetcher underneath: keep waiting while it is priming or
// live, stop when it has failed or been disconnected.
bool ArchiveFetcher::awaitInput()
{
    auto *spooled = dynamic_cast<SpooledLogSource *>(m_input.get());
    if (!spooled)
        return false;

    const qint64 before = spooled->size();
    for (;;) {
        if (stopping())
            return false;

        // Refresh BEFORE reading the state, and once more after a terminal state is
        // seen. Either order alone loses the race where the fetcher commits its last
        // chunk and then disconnects: the bytes are on disk but the state says stop.
        if (spooled->refreshSize() > before)
            return true;

        const FetchStatus upstream = spooled->fetchStatus();
        bool exhausted = false;
        switch (upstream.state) {
        case FetchStatus::State::Idle:
        case FetchStatus::State::Error:
        case FetchStatus::State::Disconnected:
        case FetchStatus::State::Complete:
            exhausted = true;
            break;
        case FetchStatus::State::Waiting:
            // The container is not there — the host is down, or it has not been written
            // yet. That is a WAIT, not an end: the transport is still trying, and when it
            // succeeds these bytes arrive. Treating it as exhausted would finish the
            // expansion as an empty log and never revisit it (§6.5).
            break;
        case FetchStatus::State::Connecting:
        case FetchStatus::State::Priming:
        case FetchStatus::State::Live:
            // A healthy transport with nothing left to send. This case is NOT optional:
            // an SSH fetcher tails forever and so never reaches a terminal state, while
            // libarchive always reads past a gzip member to look for a concatenated
            // one. Without it the expansion of a remote archive would block here having
            // already produced every byte — a hang, not caution.
            exhausted = upstream.totalSize > 0 && upstream.committedSize >= upstream.totalSize;
            break;
        }
        if (exhausted) {
            // Nothing more will arrive, so this is the end of the input. Whatever was
            // expanded stays readable; a container cut short surfaces as a
            // decompression error from libarchive rather than as silence.
            //
            // A container that is later rewritten is NOT re-expanded — reopening the
            // log does that — so an expansion finishing here really is finished, and
            // may say so. Rotation of the container is the transport's business one
            // level down, where a generation bump handles it.
            return spooled->refreshSize() > before;
        }

        QMutexLocker lock(&m_mutex);
        m_wake.wait(&m_mutex, QDeadlineTimer(kAwaitSliceMs));
    }
}

bool ArchiveFetcher::checkFreeSpace(qint64 expandedSize, QString *error) const
{
    const QStorageInfo storage(m_spoolDir);
    if (!storage.isValid() || storage.bytesAvailable() < 0)
        return true; // cannot tell; do not refuse an open over a question we cannot answer

    // The archive records the expanded size for a tar or zip member. A raw stream does
    // not, so guess from the compressed input — which is exactly where a zip bomb wins,
    // and is why this is a courtesy rather than a defence.
    const qint64 needed = expandedSize > 0
        ? expandedSize
        : (m_input ? m_input->size() * kAssumedRatio : 0);
    if (needed <= 0 || storage.bytesAvailable() >= needed)
        return true;

    if (error) {
        const QLocale locale;
        *error = Tr::tr(
                     "Not enough space in the cache directory to expand %1: it needs "
                     "about %2 and there is %3 free.")
                     .arg(logSourceDisplayName(m_location.toString()),
                          locale.formattedDataSize(needed),
                          locale.formattedDataSize(storage.bytesAvailable()));
    }
    return false;
}

void ArchiveFetcher::beginGeneration(qint64 expandedSize)
{
    QMutexLocker lock(&m_mutex);
    const quint64 next = m_status.generation + 1;
    lock.unlock();

    const QString path = spoolPath(next);
    QFile spool(path);
    if (!spool.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(Tr::tr("Cannot write the local cache file %1.").arg(path));
        return;
    }
    spool.close();

    lock.relock();
    m_status.baseOffset = 0; // an expansion always starts at the member's first byte
    m_status.committedSize = 0;
    m_heldCommit = 0; // the hold's tally starts over with the generation
    // -1 from the archive means "not recorded", which a raw gzip stream never is. 0
    // then means unknown, and the status bar says "so far" rather than "of N".
    m_status.totalSize = expandedSize > 0 ? expandedSize : 0;
    m_status.generation = next; // published LAST, once its file exists and is empty
}

bool ArchiveFetcher::expand(qint64 limit, bool *finished)
{
    if (finished)
        *finished = false;

    const quint64 generation = status().generation;
    const QString path = spoolPath(generation);
    QFile spool(path);
    if (!spool.open(QIODevice::Append)) {
        setError(Tr::tr("Cannot append to the local cache file %1.").arg(path));
        return false;
    }

    QByteArray buffer;
    buffer.resize(kChunkBytes);
    qint64 written = 0;

    while (limit == 0 || written < limit) {
        if (stopping())
            break;

        const qint64 want = limit == 0 ? kChunkBytes : qMin(kChunkBytes, limit - written);
        QString readError;
        const qint64 got = m_stream->read(buffer.data(), want, &readError);
        if (got < 0) {
            spool.close();
            setError(readError.isEmpty()
                         ? Tr::tr("Cannot expand %1.").arg(m_location.container)
                         : readError);
            return false;
        }
        if (got == 0) {
            if (finished)
                *finished = true;
            break;
        }

        if (spool.write(buffer.constData(), got) != got || !spool.flush()) {
            // Running out of room mid-expansion is the ordinary way this fails, and it
            // deserves its own sentence: "cannot write" would send someone looking for
            // a permissions problem. Whatever expanded stays indexed and readable — a
            // partly recovered log beats an empty window.
            const bool full = spool.error() == QFileDevice::ResourceError;
            spool.close();
            setError(full
                         ? Tr::tr("Ran out of space while expanding %1. What was "
                                          "expanded so far is still shown.")
                               .arg(logSourceDisplayName(m_location.toString()))
                         : Tr::tr("Cannot write to the local cache file %1.").arg(path));
            return false;
        }
        written += got;

        // Publish only AFTER the bytes are on disk. That ordering is the entire
        // synchronisation between this thread and the reader: a reader clamps to
        // committedSize, so it can never observe a half-written chunk (§6.3).
        QMutexLocker lock(&m_mutex);
        m_heldCommit += got;
        m_status.totalSize = qMax(m_status.totalSize, m_heldCommit);
        // Withheld during the prime, and only during it, so that the first size anyone
        // sees covers the whole format sample rather than one short read of it —
        // archive_read_data() is free to return less than asked. See SourceFetcher.h.
        if (!m_holdBackCommits)
            m_status.committedSize = m_heldCommit;
    }

    spool.close();
    return true;
}

void ArchiveFetcher::publishHeldCommits()
{
    QMutexLocker lock(&m_mutex);
    if (m_heldCommit > m_status.committedSize)
        m_status.committedSize = m_heldCommit;
}

void ArchiveFetcher::expandRest()
{
    bool finished = false;
    if (!expand(0, &finished))
        return; // the error is already published, and what expanded stays readable
    if (finished)
        publishComplete();
}

void ArchiveFetcher::publishComplete()
{
    QMutexLocker lock(&m_mutex);
    // committedSize is already final — every write published it before this runs — so
    // observing Complete guarantees observing the final size. See SourceFetcher.h.
    m_status.totalSize = m_status.committedSize;
    m_status.error.clear();
    m_status.state = FetchStatus::State::Complete;
}

std::unique_ptr<SourceFetcher> makeArchiveFetcher(const ArchiveLocation &location,
                                                  QString *error)
{
    if (!location.isOpenable()) {
        if (error)
            *error = Tr::tr("No log chosen inside %1.").arg(location.container);
        return nullptr;
    }
    return std::make_unique<ArchiveFetcher>(location);
}

} // namespace loftail
