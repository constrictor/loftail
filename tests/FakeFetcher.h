#pragma once

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QString>

#include <atomic>
#include <memory>

#include "SourceFetcher.h"
#include "RemoteLocation.h"
#include "SourceSpool.h"
#if defined(LOFTAIL_HAVE_ARCHIVE)
#include "ArchiveFetcher.h"
#include "ArchiveLocation.h"
#endif

namespace loftail {

// A SourceFetcher the test drives by hand (M11). This is what makes the entire
// remote feature — opening, indexing, tailing, rotation, truncation, session
// restore, the whole UI flow — testable with NO network and no libssh2 linked.
// Everything above SourceFetcher is exercised for real; only the transport is fake.
//
// The test holds a shared_ptr<FakeRemote> and pokes it (append/withhold/replace)
// while the registry separately owns a FakeFetcher wrapping the same object, so the
// controls stay valid no matter when the spool is torn down.
class FakeRemote
{
public:
    // --- Controls, called from the test ------------------------------------

    // The content the file has when it is first opened. Set before opening.
    void setInitialContent(const QByteArray &bytes) { m_initial = bytes; }

    // Make start() FAIL, as a refused password or a changed host key would — a
    // refusal, which opens no document at all. Contrast setInitiallyUnavailable().
    void setStartFailure(const QString &message) { m_startFailure = message; }

    // Make start() SUCCEED into State::Waiting with no spool file, as a real fetcher
    // does for a host that is down or a log that has not been written yet (M13, §6.5).
    // The distinction from setStartFailure() above is the whole point of the state: a
    // refusal has nothing to wait for, an absence does.
    void setInitiallyUnavailable(const QString &message)
    {
        m_unavailable = true;
        m_unavailableMessage = message;
    }

    // Make start() SUCCEED into State::Connecting with nothing committed — a fetcher
    // whose worker is still shaking hands, which since M17 is how EVERY remote open
    // begins. The test ends it with becomeAvailable().
    //
    // Note what this fake cannot prove: it returns from start() instantly, so it says
    // nothing about start() not blocking. That is tst_asyncconnect's job.
    void setConnectDelayed() { m_openState = FetchStatus::State::Connecting; }

    // Make start() SUCCEED into State::Error with nothing committed — a rejected
    // password, a changed host key, a container that would not open. Since M17 that
    // opens a tab which says why, rather than no tab at all (SPEC.md §3); contrast
    // setStartFailure(), which is now only for refusals decided with no I/O.
    void setConnectRefusal(const QString &message)
    {
        m_openState = FetchStatus::State::Error;
        m_openMessage = message;
    }

    // The wait ended: the host came back, or the log was finally written. Publishes
    // the initial content as generation 1 and goes Live, which is what start() would
    // have done had it succeeded.
    void becomeAvailable()
    {
        m_unavailable = false;
        m_openState = FetchStatus::State::Idle;
        writeWhole(pathFor(1), m_initial);

        QMutexLocker lock(&m_mutex);
        m_written = m_initial.size();
        m_status.generation = 1;
        m_status.committedSize = m_written;
        m_status.totalSize = m_written;
        m_status.error.clear();
        m_status.state = FetchStatus::State::Live;
    }

    // The log went away mid-tail — deleted on the far end, or the connection dropped.
    // The spool keeps every byte it already had; only the state changes, which is
    // exactly what a real fetcher does (it never rewrites a live generation).
    void becomeUnavailable(const QString &message)
    {
        m_unavailable = true;
        QMutexLocker lock(&m_mutex);
        m_status.state = FetchStatus::State::Waiting;
        m_status.error = message;
    }

    // How big the source really is, when that differs from how much has been delivered.
    //
    // Without this the fake reports totalSize == committedSize at every moment, which is
    // "everything has arrived" by definition — so a reader that waits for the rest, as
    // an archive listing does, could never be shown a container that is still coming. A
    // real fetcher learns the total from a stat before it has fetched a byte of it.
    // Cleared by setting 0.
    void setTotalSize(qint64 total)
    {
        QMutexLocker lock(&m_mutex);
        m_pinnedTotal = total;
        if (total > 0)
            m_status.totalSize = total;
    }

    // Append and publish: the ordinary "the writer wrote another line" case.
    void append(const QByteArray &bytes)
    {
        writeToCurrent(bytes);
        QMutexLocker lock(&m_mutex);
        m_status.committedSize = m_written;
        m_status.totalSize = m_pinnedTotal > 0 ? m_pinnedTotal
                                               : m_status.baseOffset + m_written;
    }

    // Append to the spool file but DO NOT publish it — a chunk that has landed on
    // disk while the fetcher is still mid-write. A reader must not see these bytes.
    void appendWithheld(const QByteArray &bytes) { writeToCurrent(bytes); }

    // Publish everything written so far, including previously withheld bytes.
    void publish()
    {
        QMutexLocker lock(&m_mutex);
        m_status.committedSize = m_written;
        m_status.totalSize = m_status.baseOffset + m_written;
    }

    // The remote file was rotated or truncated: the fetcher starts a NEW spool
    // generation rather than rewriting the one the index may be mmapping. Both remote
    // events look identical from here, which is the point — a generation bump is the
    // single signal for "the byte stream is discontinuous" (ARCHITECTURE.md §6.3).
    void replaceWith(const QByteArray &bytes)
    {
        QMutexLocker lock(&m_mutex);
        const quint64 next = m_status.generation + 1;
        lock.unlock();

        writeWhole(pathFor(next), bytes);

        lock.relock();
        m_written = bytes.size();
        m_status.committedSize = m_written;
        m_status.totalSize = m_written;
        m_status.baseOffset = 0;
        m_status.generation = next; // bumped LAST, after its bytes are on disk
    }

    // A rotation on a SLOW LINK: the generation is bumped and its spool file created,
    // but nothing has been fetched into it yet. The state a real fetcher passes through
    // between beginGeneration() and its first committed chunk, and the one that used to
    // be over in a microsecond because the whole prime happened inside start().
    //
    // This is the shape that would blank a healthy tab if notReadyYet() were ever folded
    // into checkNow()'s vanish branch, so it exists to be tested rather than to be
    // convenient. finishReplacing() ends it.
    void beginReplacing(qint64 expectedSize)
    {
        QMutexLocker lock(&m_mutex);
        const quint64 next = m_status.generation + 1;
        lock.unlock();

        writeWhole(pathFor(next), QByteArray());

        lock.relock();
        m_written = 0;
        m_status.committedSize = 0;
        m_status.totalSize = expectedSize;
        m_status.baseOffset = 0;
        m_status.state = FetchStatus::State::Priming;
        m_status.generation = next; // bumped LAST, after its (empty) file is on disk
    }

    void finishReplacing(const QByteArray &bytes)
    {
        writeToCurrent(bytes);
        QMutexLocker lock(&m_mutex);
        m_status.committedSize = m_written;
        m_status.totalSize = m_written;
        m_status.state = FetchStatus::State::Live;
    }

    // The stream is finished — what an archive expansion publishes when the member has
    // been read to its end (M12). Committing the outstanding bytes FIRST and the state
    // LAST is the ordering the real fetcher promises and the live controller relies on:
    // whoever observes Complete is thereby guaranteed to observe the final size. A test
    // that wants to prove the controller loses nothing writes the last bytes with
    // appendWithheld() and then calls this.
    void markComplete()
    {
        QMutexLocker lock(&m_mutex);
        m_status.committedSize = m_written;
        m_status.totalSize = m_status.baseOffset + m_written;
        m_status.state = FetchStatus::State::Complete;
    }

    // The connection dropped mid-tail. The spool keeps whatever it had.
    void failWith(const QString &message)
    {
        QMutexLocker lock(&m_mutex);
        m_status.state = FetchStatus::State::Error;
        m_status.error = message;
    }

    int startCount() const { return m_startCount; }
    int stopCount() const { return m_stopCount; }
    int pokeCount() const { return m_pokeCount; }

    // --- The SourceFetcher side --------------------------------------------

    // NOTE WHAT THIS CANNOT PROVE. A fake returns instantly whatever the contract says,
    // so nothing here demonstrates that a real start() does not block — that is
    // tst_asyncconnect's job, with a fake that genuinely sleeps. What the fake IS for is
    // everything above the transport: the states a real fetcher passes through, and how
    // the document, the live seam and the UI behave in each.
    bool start(const QString &spoolDir, QString *error)
    {
        ++m_startCount;
        if (!m_startFailure.isEmpty()) {
            if (error)
                *error = m_startFailure;
            return false;
        }
        m_dir = spoolDir;
        if (m_openState != FetchStatus::State::Idle) {
            // Opened into a state with no bytes behind it: connecting, or refused. Both
            // leave the spool legal and empty, which is what notReadyYet() reads.
            QMutexLocker lock(&m_mutex);
            m_status.state = m_openState;
            m_status.error = m_openMessage;
            return true;
        }
        if (m_unavailable) {
            // Started successfully with nothing to show: the spool exists and is empty,
            // and the fetcher behind it keeps trying. This is the shape a real
            // SshFetcher takes for an unreachable host — the open succeeds, the
            // document opens WAITING, and the log arrives when it arrives.
            QMutexLocker lock(&m_mutex);
            m_status.state = FetchStatus::State::Waiting;
            m_status.error = m_unavailableMessage;
            return true;
        }
        writeWhole(pathFor(1), m_initial);

        QMutexLocker lock(&m_mutex);
        m_written = m_initial.size();
        m_status.generation = 1;
        m_status.committedSize = m_written;
        m_status.totalSize = m_pinnedTotal > 0 ? m_pinnedTotal : m_written;
        m_status.state = FetchStatus::State::Live;
        return true;
    }

    void requestStop()
    {
        ++m_stopCount;
        QMutexLocker lock(&m_mutex);
        if (m_status.state != FetchStatus::State::Complete)
            m_status.state = FetchStatus::State::Disconnected;
    }

    // A fake owns no thread, so it has always finished winding up. A fake that WANTS to
    // model a worker still running past requestStop() — which is what makes closing a
    // tab mid-connect worth testing — sets this.
    void setStopsSlowly(bool slowly) { m_stopsSlowly = slowly; }
    void finishStopping() { m_stopsSlowly = false; }
    bool isStopped() const { return !m_stopsSlowly || m_stopCount == 0; }

    FetchStatus status() const
    {
        QMutexLocker lock(&m_mutex);
        return m_status;
    }

    QString spoolPath(quint64 generation) const { return pathFor(generation); }

    void poke() { ++m_pokeCount; }

private:
    QString pathFor(quint64 generation) const
    {
        if (m_dir.isEmpty())
            return QString();
        return m_dir + QStringLiteral("/gen-%1.log").arg(generation);
    }

    void writeToCurrent(const QByteArray &bytes)
    {
        QMutexLocker lock(&m_mutex);
        const QString path = pathFor(m_status.generation);
        lock.unlock();
        QFile f(path);
        if (!f.open(QIODevice::Append))
            return;
        f.write(bytes);
        f.flush();
        f.close();
        lock.relock();
        m_written += bytes.size();
    }

    static void writeWhole(const QString &path, const QByteArray &bytes)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        f.write(bytes);
        f.flush();
        f.close();
    }

    mutable QMutex m_mutex;
    FetchStatus    m_status;
    QString        m_dir;
    QByteArray     m_initial;
    QString        m_startFailure;
    // Idle means "open normally". Anything else is a state start() lands in with no
    // bytes committed — see setConnectDelayed() and setConnectRefusal().
    FetchStatus::State m_openState = FetchStatus::State::Idle;
    QString        m_openMessage;
    bool           m_unavailable = false;
    QString        m_unavailableMessage;
    qint64         m_written = 0;
    // Non-zero when the test has said how big the source really is; see setTotalSize().
    qint64         m_pinnedTotal = 0;
    bool           m_stopsSlowly = false;
    // Atomic, not plain ints: these three are bumped on whichever thread drives the
    // fetcher and read by a test watching from another — tst_archivemembers polls
    // startCount() from a worker while the main thread opens the container. They sit
    // outside m_mutex deliberately, because a test that took the fetcher's own lock to
    // read a counter would serialise against the thing it is trying to observe.
    // (Found by ThreadSanitizer; ARCHITECTURE.md §13.)
    std::atomic<int> m_startCount{0};
    std::atomic<int> m_stopCount{0};
    std::atomic<int> m_pokeCount{0};
};

// The SourceFetcher the registry owns; all behavior lives in the shared FakeRemote.
class FakeFetcher final : public SourceFetcher
{
public:
    explicit FakeFetcher(std::shared_ptr<FakeRemote> remote) : m_remote(std::move(remote)) {}

    bool start(const QString &spoolDir, QString *error) override
    {
        return m_remote->start(spoolDir, error);
    }
    void requestStop() override { m_remote->requestStop(); }
    bool isStopped() const override { return m_remote->isStopped(); }
    FetchStatus status() const override { return m_remote->status(); }
    QString spoolPath(quint64 generation) const override { return m_remote->spoolPath(generation); }
    void pokeNow() override { m_remote->poke(); }

private:
    std::shared_ptr<FakeRemote> m_remote;
};

// Installs the fake transport for the lifetime of the object and hands out one
// FakeRemote per remote location. Construct one per test function.
class FakeRemoteFarm
{
public:
    FakeRemoteFarm()
    {
        // Keep spool files out of the developer's real cache directory.
        QStandardPaths::setTestModeEnabled(true);
        m_remotes = std::make_shared<QHash<QString, std::shared_ptr<FakeRemote>>>();
        auto remotes = m_remotes;
        SourceSpoolRegistry::instance().setFetcherFactory(
            [remotes](const QString &key, QString *error) -> std::unique_ptr<SourceFetcher> {
#if defined(LOFTAIL_HAVE_ARCHIVE)
                // ONLY THE TRANSPORT IS FAKE. An expansion key is handed to the real
                // archive fetcher, which then opens its own input — and that input is
                // a remote address, so it comes back through here and gets the fake.
                // That is how a remote archive is exercised end to end with no network:
                // two real fetchers chained, over faked bytes (ARCHITECTURE.md §6.4).
                if (key.startsWith(QStringLiteral("expand\n"))) {
                    const auto archive =
                        ArchiveLocation::split(key.mid(qstrlen("expand\n")));
                    if (archive)
                        return makeArchiveFetcher(*archive, error);
                }
#endif
                const auto it = remotes->constFind(key);
                if (it == remotes->constEnd()) {
                    if (error)
                        *error = QStringLiteral("No fake remote registered for %1").arg(key);
                    return nullptr;
                }
                return std::make_unique<FakeFetcher>(*it);
            });
    }

    ~FakeRemoteFarm()
    {
        SourceSpoolRegistry::instance().clear();
        SourceSpoolRegistry::instance().setFetcherFactory(nullptr);
        QStandardPaths::setTestModeEnabled(false);
    }

    FakeRemoteFarm(const FakeRemoteFarm &) = delete;
    FakeRemoteFarm &operator=(const FakeRemoteFarm &) = delete;

    // The fake behind `url`, created on first request. `url` is normalized, so a test
    // may register with one spelling and open with another.
    std::shared_ptr<FakeRemote> at(const QString &url)
    {
        const QString key = RemoteLocation::normalize(url);
        auto &slot = (*m_remotes)[key];
        if (!slot)
            slot = std::make_shared<FakeRemote>();
        return slot;
    }

private:
    std::shared_ptr<QHash<QString, std::shared_ptr<FakeRemote>>> m_remotes;
};

} // namespace loftail
