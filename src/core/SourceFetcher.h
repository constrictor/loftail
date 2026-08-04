#pragma once

#include <QString>
#include <QtGlobal>

namespace loftail {

// What a fetcher publishes about the stream it is filling a spool with. Snapshot by
// value: the GUI thread reads this on every watch tick and must never block on it.
struct FetchStatus
{
    enum class State {
        Idle,         // constructed, start() not called
        Connecting,   // establishing the session / opening the input
        Priming,      // connected, doing the initial bulk fetch into the spool
        Live,         // following the input for more bytes
        Error,        // last operation failed; `error` says how. Retried with backoff
        Disconnected, // stopped, deliberately
    };

    State   state = State::Idle;

    // Bumped every time the source file is replaced or truncated. A generation is a
    // NEW spool file, never a rewrite of the current one — the index worker may be
    // mmapping the current one, and record offsets index it (ARCHITECTURE.md §6.3).
    quint64 generation = 0;

    // Bytes durably written to the CURRENT generation's spool file. Published only
    // AFTER the write lands, so a reader clamping to it can never see a torn chunk.
    qint64  committedSize = 0;

    // The source offset that spool byte 0 corresponds to. Non-zero when the user
    // asked to start from the tail of a very large log rather than fetch it whole.
    qint64  baseOffset = 0;

    // How big the finished spool is expected to be, as of the last stat — for the
    // status bar, so a long prime can show how far along it is. 0 when unknown, which
    // is the ordinary case for a decompressed stream: a gzip member does not announce
    // its expanded length. Not used for any correctness decision.
    qint64  totalSize = 0;

    // User-facing failure text for State::Error. NEVER contains a credential.
    QString error;
};

// Fills a local spool file forward from somewhere else, and keeps following it.
//
// This is the seam that keeps *how a log is obtained* out of the rest of the
// application: the spool, the source, the live controller and the whole UI bind to
// this interface, so they are all exercised by a fake in tests with no network and no
// third-party library linked (tests/FakeFetcher.h). SshFetcher, which reads a remote
// file over SFTP, is one implementation.
//
// THREADING. An implementation owns whatever thread it needs, and status() must be
// safe to call from another thread at any time — it is polled from the GUI thread.
// Only ever reads its input, and only ever forward (invariant #9); it writes nothing
// but its own spool.
class SourceFetcher
{
public:
    virtual ~SourceFetcher() = default;

    // Open the input and fetch enough of it that a format sample can be taken; then
    // keep following in the background. Blocking, bounded by the implementation's own
    // timeout, and the ONE call that may prompt the user.
    // Returns false and fills `error` on failure.
    virtual bool start(const QString &spoolDir, QString *error) = 0;

    // Stop following and release the input. Idempotent; safe after a failed start().
    // The spool files stay on disk until the owning SourceSpool goes away.
    virtual void stop() = 0;

    // A consistent snapshot. Thread-safe.
    virtual FetchStatus status() const = 0;

    // Where generation `generation`'s spool file lives. Stable for the life of the
    // fetcher, so a source can reopen it after a generation change.
    virtual QString spoolPath(quint64 generation) const = 0;

    // Ask for a poll right now instead of at the next scheduled one — the user
    // pressed Reconnect, or a test wants a deterministic step. Non-blocking.
    virtual void pokeNow() = 0;
};

} // namespace loftail
