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

        // The input is NOT THERE — the host is down, or the path does not exist (yet).
        // Retried with the same backoff Error uses, and it is the ordinary way out of
        // this state: a log that has not been written yet starts here and goes Live the
        // moment it appears (SPEC.md §3, §6.5).
        //
        // Distinct from Error, and the distinction is user-visible: Error is trouble
        // with a source that EXISTS and reads as a failure; Waiting is a source that
        // does not exist and reads as "not yet". Both retry, but only one is a fault.
        // `error` carries the explanation either way, and still never a credential.
        Waiting,

        // Every byte has been delivered and committed, and there will never be another.
        // PUBLISHED LAST, after the final committedSize — the same ordering rule
        // `generation` follows, and for the same reason: a reader that observes
        // Complete is thereby guaranteed to observe the final size. Get this backwards
        // and the watch stops one chunk short, silently losing the last records.
        //
        // Only reachable where loftail PRODUCED the bytes from a fixed input, i.e. an
        // archive member expanded into its spool. A remote file can always grow, so
        // SshFetcher never publishes this and must not be made to: guessing that a log
        // is finished is exactly what invariant #5 forbids.
        Complete,
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

// The state's name for the diagnostic log, NOT for the user (DiagnosticLog.h). It is
// deliberately not translated and deliberately not routed through any of the status-bar
// wording: this is the token somebody greps a bug report for, so it has to mean one thing
// in every build and every locale.
inline const char *fetchStateName(FetchStatus::State state)
{
    switch (state) {
    case FetchStatus::State::Idle:         return "idle";
    case FetchStatus::State::Connecting:   return "connecting";
    case FetchStatus::State::Priming:      return "priming";
    case FetchStatus::State::Live:         return "live";
    case FetchStatus::State::Error:        return "error";
    case FetchStatus::State::Disconnected: return "disconnected";
    case FetchStatus::State::Waiting:      return "waiting";
    case FetchStatus::State::Complete:     return "complete";
    }
    return "?";
}

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

    // Begin filling `spoolDir`, and return.
    //
    // NON-BLOCKING, and that is the contract (M17, ARCHITECTURE.md §6.3.3). This runs on
    // the thread that opened the document. It used to connect, authenticate, prompt and
    // fetch 128 KB before returning — which is why opening a remote log froze the window
    // for as long as the far end took, and why restoring a session containing one showed
    // no window at all until every host had answered. All of that is the implementation's
    // own thread's work now.
    //
    // What the caller gets back is a legal, empty spool in State::Connecting.
    // SpooledLogSource::notReadyYet() reads that as "wait", so the document opens into
    // the state M13 built for a log that has not been written yet: a tab that exists,
    // says what it is doing, and fills in when the bytes arrive (§6.5).
    //
    // Returns false and fills `error` only for a refusal decided WITHOUT I/O — a spool
    // directory that cannot be created, an address naming nothing fetchable. Anything
    // that needs a round trip is published as State::Error instead, and the tab stays and
    // says why (SPEC.md §3).
    //
    // THE PRIME IS PUBLISHED ALL AT ONCE. The first committedSize an implementation
    // publishes must be either the whole format prime or the whole stream — never one
    // short read of it. The document leaves its waiting state at the first committed byte
    // and settles both its format and its encoding from what it can then read, once and
    // for good (Document::resume is a one-way door). libssh2 and libarchive are both free
    // to return less than asked, and Decoder::detect() over 4 KB is not always the same
    // answer as over 64 KB — so a per-chunk publish during the prime would settle a log's
    // format against whatever the first read happened to return. After the prime,
    // publishing per chunk is the point: that is what makes records appear as they arrive.
    virtual bool start(const QString &spoolDir, QString *error) = 0;

    // Ask this fetcher to wind up. NON-BLOCKING, and that is the contract, not an
    // optimisation: the GUI thread reaches this whenever the last tab on a log closes,
    // and a worker that is mid-connect may be twenty seconds from noticing. Joining it
    // there is a freeze — and once a worker can be waiting on the GUI thread for a
    // password (GuiCallGate.h), joining it there is a deadlock.
    //
    // Idempotent, and safe after a failed start(). The spool files stay on disk until
    // the owning SourceSpool goes away.
    virtual void requestStop() = 0;

    // Whether the wind-up has finished: this fetcher's thread has exited, so destroying
    // it will not block. POLLED, by SourceSpoolRegistry's reaper — deliberately, rather
    // than signalled, because "mutex-guarded snapshot, never a signal" is this whole
    // layer's synchronisation model (see status() below) and one queued connection here
    // would be the precedent for the next.
    //
    // True before start(), and true forever after it reads true once.
    virtual bool isStopped() const = 0;

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
