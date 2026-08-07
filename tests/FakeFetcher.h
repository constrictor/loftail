#pragma once

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QString>

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

    // The wait ended: the host came back, or the log was finally written. Publishes
    // the initial content as generation 1 and goes Live, which is what start() would
    // have done had it succeeded.
    void becomeAvailable()
    {
        m_unavailable = false;
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

    // Append and publish: the ordinary "the writer wrote another line" case.
    void append(const QByteArray &bytes)
    {
        writeToCurrent(bytes);
        QMutexLocker lock(&m_mutex);
        m_status.committedSize = m_written;
        m_status.totalSize = m_status.baseOffset + m_written;
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

    bool start(const QString &spoolDir, QString *error)
    {
        ++m_startCount;
        if (!m_startFailure.isEmpty()) {
            if (error)
                *error = m_startFailure;
            return false;
        }
        m_dir = spoolDir;
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
        m_status.totalSize = m_written;
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
    bool           m_unavailable = false;
    QString        m_unavailableMessage;
    qint64         m_written = 0;
    bool           m_stopsSlowly = false;
    int            m_startCount = 0;
    int            m_stopCount = 0;
    int            m_pokeCount = 0;
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
