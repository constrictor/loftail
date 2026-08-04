#pragma once

#include "RemoteFetcher.h"
#include "RemoteLocation.h"

#include <QHash>
#include <QString>

#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QLockFile;
class QTemporaryDir;
QT_END_NAMESPACE

namespace loftail {

// One remote file's local spool, and the fetcher filling it (ARCHITECTURE.md §6.3).
//
// SHARED, NOT PER-DOCUMENT — this is the load-bearing decision. A Document holds a
// SpooledLogSource, which holds a shared_ptr to one of these. So:
//   * Document::rescan() on a rotation reopens through the registry, finds the live
//     spool and returns immediately — no reconnect, no password prompt, no hang on
//     the GUI thread mid-tail. That whole class of problem does not arise.
//   * Two tabs on one remote file share one connection and one spool.
//   * Changing a remote file's log format (which reopens the Document) costs nothing.
// The last handle going away tears down the fetcher and deletes the spool files.
class RemoteSpool
{
public:
    ~RemoteSpool();

    RemoteSpool(const RemoteSpool &) = delete;
    RemoteSpool &operator=(const RemoteSpool &) = delete;

    const RemoteLocation &location() const { return m_location; }

    // The fetcher's published snapshot. Cheap and non-blocking: this is on the watch
    // tick path, which runs on the GUI thread several times a second.
    FetchStatus status() const;

    // The spool file backing `generation`.
    QString spoolPath(quint64 generation) const;

    // Ask the fetcher to poll now (Reconnect, or a test stepping deterministically).
    void poke();

private:
    friend class RemoteSpoolRegistry;
    RemoteSpool(RemoteLocation location, std::unique_ptr<RemoteFetcher> fetcher, QString dir);

    RemoteLocation                 m_location;
    std::unique_ptr<RemoteFetcher> m_fetcher;
    QString                        m_dir; // this spool's own directory, removed in ~
};

// The live spools, keyed by normalized location, plus the place the SSH transport is
// installed from. Single instance; called from the GUI thread only.
class RemoteSpoolRegistry
{
public:
    // Builds a fetcher for a location, or returns nullptr and fills `error`. Stage 3
    // installs the libssh2 one; tests install a fake and thereby exercise the entire
    // application — open, tail, rotate, session restore — with no network at all.
    using FetcherFactory =
        std::function<std::unique_ptr<RemoteFetcher>(const RemoteLocation &, QString *error)>;

    static RemoteSpoolRegistry &instance();

    // Replace the fetcher factory. Passing a null factory restores the default (the
    // libssh2 one where SSH is compiled in; "not built in" where it is not).
    void setFetcherFactory(FetcherFactory factory);

    // The live spool for `location`, or nullptr if there is none. No network, no
    // creation — this is what makes a rescan-during-tail free.
    std::shared_ptr<RemoteSpool> find(const RemoteLocation &location) const;

    // The live spool for `location`, creating and starting one if needed. Creating
    // one CONNECTS, which may prompt the user and may block for the connect timeout.
    std::shared_ptr<RemoteSpool> acquire(const RemoteLocation &location, QString *error);

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
    RemoteSpoolRegistry();
    ~RemoteSpoolRegistry();

    FetcherFactory m_factory;
    // Weak, so the last SpooledLogSource dropping its handle tears the spool down.
    QHash<QString, std::weak_ptr<RemoteSpool>> m_spools;
    std::unique_ptr<QTemporaryDir> m_instanceDir;
    std::unique_ptr<QLockFile>     m_instanceLock;
};

} // namespace loftail
