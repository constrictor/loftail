#pragma once

#include "SourceFetcher.h"

#include <QHash>
#include <QMutex>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

QT_BEGIN_NAMESPACE
class QLockFile;
class QTemporaryDir;
QT_END_NAMESPACE

namespace loftail {

// Spool keys are NAMESPACED BY WHAT FILLS THEM, because one address can legitimately
// need two spools at once. A remote `app.log.gz` is fetched as a container (by the SSH
// fetcher) and expanded from it (by the archive fetcher) — and because a single-stream
// container collapses to its own plain path, both of those are the *same address
// string*. Sharing a key between them is not merely ambiguous: the expansion's own input
// lookup would resolve to the expansion itself — an archive fetcher whose input is its
// own spool, waiting forever for bytes only it could produce.
//
// (That used to read "it would build a second one and recurse forever", which was true
// while the registry published its entry only after start() returned. start() no longer
// blocks, so the inner lookup now happens after publication and would FIND the outer
// spool rather than build another. The prefix is what prevents either reading.)
//
// The prefix never escapes the registry. Document::path(), the session and the
// format-cache key all hold the plain address (ArchiveLocation.h).
QString expandedSpoolKey(const QString &address);

// One log's local spool, and the fetcher filling it (ARCHITECTURE.md §6.3).
//
// SHARED, NOT PER-DOCUMENT — this is the load-bearing decision. A Document holds a
// SpooledLogSource, which holds a shared_ptr to one of these. So:
//   * Document::rescan() on a rotation reopens through the registry, finds the live
//     spool and returns immediately — no reconnect, no password prompt, no hang on
//     the GUI thread mid-tail. That whole class of problem does not arise.
//   * Two tabs on one log share one connection and one spool.
//   * Changing a log's format (which reopens the Document) costs nothing.
// The last handle going away tears down the fetcher and deletes the spool files.
//
// KEYED BY THE NORMALIZED PATH STRING, not by a parsed address: the registry never
// needs to understand what it holds, and the two kinds of fetcher have nothing in
// common to parse into. Whoever builds the fetcher is the only party that has to know
// how to read the key.
class SourceSpool
{
public:
    ~SourceSpool();

    SourceSpool(const SourceSpool &) = delete;
    SourceSpool &operator=(const SourceSpool &) = delete;

    // The normalized path this spool holds a copy of. Registry key and nothing more.
    const QString &key() const { return m_key; }

    // The fetcher's published snapshot. Cheap and non-blocking: this is on the watch
    // tick path, which runs on the GUI thread several times a second.
    FetchStatus status() const;

    // The spool file backing `generation`.
    QString spoolPath(quint64 generation) const;

    // Ask the fetcher to poll now (Reconnect, or a test stepping deterministically).
    void poke();

    // Stop filling this spool, for good. Used when the user cancels the initial scan:
    // no LiveController is created for a cancelled open, so without this the fetcher
    // would go on expanding into a spool nobody will ever read. The bytes already
    // written stay — the cancelled scan left them usable (SPEC.md §3).
    void cancel();

private:
    friend class SourceSpoolRegistry;
    SourceSpool(QString key, std::unique_ptr<SourceFetcher> fetcher, QString dir);

    QString                        m_key;
    std::unique_ptr<SourceFetcher> m_fetcher;
    QString                        m_dir; // this spool's own directory, removed in ~
};

// The live spools, keyed by normalized path, plus the place a transport is installed
// from. Single instance.
//
// THREADING, and it is not "the GUI thread only" however much the rest of this file
// reads that way. acquire(), find() and setFetcherFactory() ARE GUI-thread-only, and
// the first of those relies on it: acquire() is re-entrant (a remote `app.log.gz`
// acquires the expansion's key, whose fetcher acquires the container's), so its
// check-then-insert is deliberately not atomic and cannot be made so without
// deadlocking on itself.
//
// But the map is also written from wherever the LAST shared_ptr to a spool is dropped,
// and that is not this thread. ArchiveFetcher resets its input — a SpooledLogSource
// holding the container's spool — from its own worker. So m_spools is guarded, and the
// lock is held only across the map access, never across start() or a destructor.
class SourceSpoolRegistry
{
public:
    // Builds a fetcher for a normalized path, or returns nullptr and fills `error`.
    // The default one dispatches on the path; tests install a fake and thereby
    // exercise the entire application — open, tail, rotate, session restore — with no
    // network and no codec at all.
    using FetcherFactory =
        std::function<std::unique_ptr<SourceFetcher>(const QString &key, QString *error)>;

    static SourceSpoolRegistry &instance();

    // Replace the fetcher factory. Passing a null factory restores the default (the
    // real one for each kind of path where it is compiled in; "not built in" where
    // it is not).
    void setFetcherFactory(FetcherFactory factory);

    // The live spool for `key`, or nullptr if there is none. No network, no creation —
    // this is what makes a rescan-during-tail free.
    std::shared_ptr<SourceSpool> find(const QString &key) const;

    // The live spool for `key`, creating and starting one if needed. Creating one
    // OPENS the input, which may prompt the user and may block for a connect timeout.
    std::shared_ptr<SourceSpool> acquire(const QString &key, QString *error);

    // Forget the live-spool bookkeeping. Spools still held by an open source stay
    // alive; used by tests between cases.
    void clear();

    // Release the instance directory and its lock. Registered as a Qt post-routine so
    // it runs while the application object is still alive, which is when the cache
    // directory is still guaranteed to exist.
    void shutdown();

    // The directory this process spools into. Created on first use, inside the user's
    // cache location (never config — spools can be gigabytes) and locked, so that a
    // second loftail instance can tell a live sibling from an abandoned one.
    QString instanceDir();

    // Remove spool directories left behind by instances that are no longer running.
    // Uses the lock file each instance holds, so a LIVE sibling's spools are never
    // deleted out from under it (SPEC.md §3 allows several instances at once).
    void sweepAbandonedSpools();

    // Take a dead spool's fetcher off the caller's hands: ask it to stop, and destroy it
    // — and remove `dir` — once it says it has. THE CALLER NEVER WAITS.
    //
    // This is what makes closing a tab instant on a host that is down. A fetcher's
    // thread may be twenty seconds into a connect, and after M17 it may additionally be
    // blocked on the GUI thread for a password (GuiCallGate.h), so ~SourceSpool cannot
    // join it: on the GUI thread that is a freeze in the first case and a deadlock in
    // the second. The directory outlives the spool by however long the worker takes to
    // notice, which costs some cache space and nothing else — nobody is reading it.
    //
    // Callable from any thread: the last handle to a remote archive's container spool is
    // dropped by the archive fetcher's own worker.
    void retire(std::unique_ptr<SourceFetcher> fetcher, const QString &dir);

private:
    class Reaper;
    friend class Reaper;

    struct Retired
    {
        std::unique_ptr<SourceFetcher> fetcher;
        QString                        dir;
    };

    // Destroy every retired fetcher that has stopped, and remove its directory. Returns
    // how many are still running. GUI thread; called from the reaper's timer.
    int collectRetired();
    // Give the retired fetchers up to `budgetMs` to stop, then abandon the stragglers.
    // Called only from shutdown(), where there is no event loop left to reap with.
    void drainRetired(int budgetMs);
    SourceSpoolRegistry();
    ~SourceSpoolRegistry();

    // Drop `key` from the map. Called from a spool's deleter, on whatever thread let go
    // of the last handle — which is why it takes the lock and why it is separate from
    // the delete that follows it: ~SourceSpool must not run under m_mutex.
    void forget(const QString &key);

    FetcherFactory m_factory; // GUI thread only, installed before any open

    mutable QMutex m_mutex; // guards m_spools and m_retired, and nothing else
    // Weak, so the last SpooledLogSource dropping its handle tears the spool down.
    QHash<QString, std::weak_ptr<SourceSpool>> m_spools;
    std::vector<Retired>                       m_retired;
    std::unique_ptr<Reaper>                    m_reaper; // GUI thread only

    // GUI thread only: created on the first acquire(), released by shutdown().
    quint64                        m_serial = 0; // distinguishes acquisitions of one key
    std::unique_ptr<QTemporaryDir> m_instanceDir;
    std::unique_ptr<QLockFile>     m_instanceLock;
};

} // namespace loftail
