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

    // Make start() fail, as an unreachable host or a refused password would.
    void setStartFailure(const QString &message) { m_startFailure = message; }

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
        writeWhole(pathFor(1), m_initial);

        QMutexLocker lock(&m_mutex);
        m_written = m_initial.size();
        m_status.generation = 1;
        m_status.committedSize = m_written;
        m_status.totalSize = m_written;
        m_status.state = FetchStatus::State::Live;
        return true;
    }

    void stop()
    {
        ++m_stopCount;
        QMutexLocker lock(&m_mutex);
        m_status.state = FetchStatus::State::Disconnected;
    }

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
    qint64         m_written = 0;
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
    void stop() override { m_remote->stop(); }
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
