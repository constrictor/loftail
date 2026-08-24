#include "SpooledLogSource.h"

#include "ArchiveLocation.h"
#include "RemoteLocation.h"
#include "SourceSpool.h"

#include <QCoreApplication>
#include <QLocale>

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr() — and these strings are user-facing all the same: they travel up to
// the status bar through Document::lastError() and LiveController::sourceStatusChanged.
// Q_DECLARE_TR_FUNCTIONS is what lets lupdate file them under a name that means
// something rather than under the file they happen to sit in.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::SpooledLogSource)
};
} // namespace


SpooledLogSource::~SpooledLogSource() = default;

std::unique_ptr<SpooledLogSource> SpooledLogSource::open(std::shared_ptr<SourceSpool> spool)
{
    if (!spool)
        return nullptr;

    auto src = std::unique_ptr<SpooledLogSource>(new SpooledLogSource());
    src->m_spool = std::move(spool);

    const FetchStatus status = src->m_spool->status();
    src->adoptGeneration(status.generation);
    src->m_size = qMin(src->m_inner ? src->m_inner->size() : 0, status.committedSize);
    // A fresh source starts clean even when it adopts a generation the fetcher
    // reached by rotating: this IS the rescan that rotation asked for.
    src->m_truncated = false;
    return src;
}

void SpooledLogSource::adoptGeneration(quint64 generation)
{
    m_generation = generation;
    const QString path = m_spool->spoolPath(generation);
    // Reuse the platform's local source over the spool: mmap on POSIX, buffered on
    // Windows. Nothing about the read path is remote-specific.
    m_inner = path.isEmpty() ? nullptr : openLogSource(path);
}

FetchStatus SpooledLogSource::fetchStatus() const
{
    return m_spool ? m_spool->status() : FetchStatus{};
}

bool SpooledLogSource::wasReplaced() const
{
    // The remote file was rotated or truncated: the fetcher started a new spool
    // generation rather than rewriting the one under us, so the bytes this source
    // holds are still intact — they just no longer describe the remote file.
    return m_spool && m_spool->status().generation != m_generation;
}

bool SpooledLogSource::originVanished() const
{
    // A spooled source cannot answer this from a path — there is no local path to stat,
    // and for a remote one the answer costs a round trip. The fetcher already knows,
    // because it is the thing that failed to find the input, so it publishes the answer
    // and this reads it (§6.5).
    //
    // The bytes already spooled stay readable throughout, exactly as an unlinked local
    // file's mapping does. What the caller does about that is its business.
    return m_spool && m_spool->status().state == FetchStatus::State::Waiting;
}

bool SpooledLogSource::notReadyYet() const
{
    if (!m_spool)
        return false;
    const FetchStatus status = m_spool->status();
    if (status.committedSize > 0)
        return false; // there is something to read, whatever the fetcher is doing next

    // Error answers `true` for a different reason from the three states below it —
    // not "on its way" but "it stopped, and the tab stays up to say so" — and the two
    // are one edit away from diverging. Folding them into one label would leave that
    // distinction in a comment only.
    switch (status.state) {
    // NOLINTNEXTLINE(bugprone-branch-clone)
    case FetchStatus::State::Idle:
    case FetchStatus::State::Connecting:
    case FetchStatus::State::Priming:
        return true; // on its way, and nothing has arrived
    case FetchStatus::State::Error:
        // A refusal — a rejected password, a changed host key, a container that would
        // not open. The tab stays and says why (SPEC.md §3): the fetcher's own words
        // reach the placeholder and the status bar through sourceStatusText(), and
        // File ▸ Reconnect is how the user tries again. The refusal usually lands
        // AFTER the wait began — the open enters it on "connecting…" — so the words
        // get there by LiveController::republishWaitReason() on a later tick rather
        // than by the transition that announced the wait (§6.5).
        return true;
    case FetchStatus::State::Live:
        // An EMPTY remote log, which is a real thing and not a wait: it exists, it has
        // been read, and it has nothing in it — the same case as an empty local file,
        // which opens as an ordinary empty tab. Saying "not there yet" here would put a
        // ◦ on a log that has demonstrably appeared, which is the exact distinction the
        // mark exists to draw.
    case FetchStatus::State::Complete:
    case FetchStatus::State::Disconnected:
    case FetchStatus::State::Waiting: // originVanished()'s business, not this one's
        return false;
    }
    return false;
}

bool SpooledLogSource::isComplete() const
{
    // The fetcher publishes Complete only after its final committedSize, so a caller
    // that reads this BEFORE refreshSize() — which LiveController does deliberately —
    // is guaranteed that the refresh which follows sees every remaining byte.
    //
    // Only an expansion ever reaches this state. A remote log's fetcher cannot, and
    // must not be made to: claiming a file somebody else is writing is finished is
    // exactly the guess invariant #5 forbids.
    return m_spool && m_spool->status().state == FetchStatus::State::Complete;
}

qint64 SpooledLogSource::refreshSize()
{
    if (!m_spool)
        return m_size;

    const FetchStatus status = m_spool->status();

    if (status.generation != m_generation) {
        // Rotation: swap to the new spool file. Latch truncation as well, so a caller
        // that ignores wasReplaced() still learns that the byte stream is
        // discontinuous rather than silently reading a different file's offsets.
        adoptGeneration(status.generation);
        m_truncated = true;
        m_size = 0;
    }

    // Clamp to what the fetcher has COMMITTED, not to the spool file's raw size. The
    // fetcher publishes committedSize only after its write lands, so this is what
    // makes a concurrent append impossible to observe half-written — and it is why
    // this class needs no lock against the fetcher thread at all.
    const qint64 spooled = m_inner ? m_inner->refreshSize() : 0;
    const qint64 visible = qMin(spooled, status.committedSize);

    if (visible < m_size)
        m_truncated = true; // defensive: within a generation, committed only grows
    m_size = visible;
    return m_size;
}

QByteArrayView SpooledLogSource::bytes(qint64 offset, qint64 length)
{
    if (!m_inner || offset < 0 || length <= 0 || offset >= m_size)
        return {};
    // Clamp to the committed extent, not to the inner source's own idea of the file:
    // the spool on disk may already be longer than what has been published.
    return m_inner->bytes(offset, qMin(length, m_size - offset));
}

QString sourceStatusText(const LogSource &source, const QString &path)
{
    const auto *spooled = dynamic_cast<const SpooledLogSource *>(&source);
    if (!spooled)
        return {}; // an ordinary local file has nothing to report, ever

    const FetchStatus status = spooled->fetchStatus();
    const QLocale locale;
    const auto sized = [&locale](qint64 bytes) { return locale.formattedDataSize(bytes); };

    switch (status.state) {
    case FetchStatus::State::Idle:
    case FetchStatus::State::Live:
    case FetchStatus::State::Complete:
    case FetchStatus::State::Disconnected:
        // A healthy tail, a finished expansion and a closed source are all the ordinary
        // case. Saying so would be noise that trains the user to ignore the line, and
        // that goes for HOW the log is being read as well: an exec-mode fallback that is
        // keeping up is still just a working tail (§6.3.1). Only trouble gets a line.
        return {};

    case FetchStatus::State::Error:
        return status.error;

    case FetchStatus::State::Waiting:
        // The fetcher's own words where it has any — "the host is down", "no such file
        // there" — because it knows which of those it hit and this does not. The bare
        // fallback is for a fetcher that only managed to say "not there".
        return status.error.isEmpty()
            ? Tr::tr("waiting for %1 to appear").arg(logSourceDisplayName(path))
            : status.error;

    case FetchStatus::State::Connecting:
        return Tr::tr("connecting…");

    case FetchStatus::State::Priming: {
        // Only the path can say which of the two this is: the source is a spool either
        // way, and a spool does not know who fills it.
        const QString verb = ArchiveLocation::isArchivePath(path)
            ? Tr::tr("expanding")
            : Tr::tr("fetching");
        if (status.totalSize > status.committedSize) {
            return Tr::tr("%1 — %2 of %3")
                .arg(verb, sized(status.committedSize), sized(status.totalSize));
        }
        // A raw compressed stream does not record its expanded length, so there is no
        // honest denominator to show. Say what is known rather than inventing one.
        return Tr::tr("%1 — %2 so far").arg(verb, sized(status.committedSize));
    }
    }
    return {};
}

} // namespace loftail
