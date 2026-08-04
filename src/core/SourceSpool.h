#pragma once

#include "SourceFetcher.h"

#include <QHash>
#include <QString>

#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QLockFile;
class QTemporaryDir;
QT_END_NAMESPACE

namespace loftail {

// Spool keys are NAMESPACED BY WHAT FILLS THEM, because one address can legitimately
// need two spools at once. A remote `app.log.gz` is fetched as a container (by the SSH
// fetcher) and expanded from it (by the archive fetcher) — and because a single-stream
// container collapses to its own plain path, both of those are the *same address
// string*. Sharing a key between them is not merely ambiguous: the expansion's own
// input lookup would find the expansion, and since the registry publishes its entry
// only after start() returns, it would build a second one and recurse forever.
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
// from. Single instance; called from the GUI thread only.
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

private:
    SourceSpoolRegistry();
    ~SourceSpoolRegistry();

    FetcherFactory m_factory;
    // Weak, so the last SpooledLogSource dropping its handle tears the spool down.
    QHash<QString, std::weak_ptr<SourceSpool>> m_spools;
    std::unique_ptr<QTemporaryDir> m_instanceDir;
    std::unique_ptr<QLockFile>     m_instanceLock;
};

} // namespace loftail
