#include <QtTest>

#include <QElapsedTimer>
#include <QFile>
#include <QIODevice>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QThread>
#include <QTimeZone>

#include <atomic>
#include <memory>

#include "Document.h"
#include "LogSource.h"
#include "ManualFormatProvider.h"
#include "SourceSpool.h"
#include "SpooledLogSource.h"

using namespace loftail;

namespace {

// A fetcher whose worker genuinely takes its time, which is the one thing
// tests/FakeFetcher.h cannot model: a fake that returns instantly satisfies any
// contract, including the one this milestone replaced.
//
// It sleeps in slices and checks its stop flag between them — exactly as SshFetcher's
// chunked connect and ArchiveFetcher's expansion loop do — so "asked to stop mid-connect"
// is a real question here rather than a formality.
class SlowFetcher final : public SourceFetcher
{
public:
    explicit SlowFetcher(int connectMs) : m_connectMs(connectMs) {}
    ~SlowFetcher() override
    {
        requestStop();
        m_worker.wait();
    }

    bool start(const QString &spoolDir, QString *error) override
    {
        Q_UNUSED(error);
        m_spoolDir = spoolDir;
        {
            QMutexLocker lock(&m_mutex);
            m_status.state = FetchStatus::State::Connecting;
        }
        m_worker.owner = this;
        m_worker.start();
        return true;
    }

    void requestStop() override
    {
        QMutexLocker lock(&m_mutex);
        m_stopping = true;
        if (m_status.state != FetchStatus::State::Complete)
            m_status.state = FetchStatus::State::Disconnected;
    }

    bool isStopped() const override { return !m_worker.isRunning(); }

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

    void pokeNow() override {}

    std::atomic_int connectAttempts{0};

private:
    struct Worker : QThread
    {
        SlowFetcher *owner = nullptr;
        void run() override { owner->connectSlowly(); }
    };

    bool stopping() const
    {
        QMutexLocker lock(&m_mutex);
        return m_stopping;
    }

    void connectSlowly()
    {
        ++connectAttempts;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < m_connectMs) {
            if (stopping())
                return; // abandoned, exactly as an abandoned connect is
            QThread::msleep(5);
        }

        const QString path = spoolPath(1);
        QFile spool(path);
        if (!spool.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        const QByteArray body =
            QByteArrayLiteral("2026-08-05 00:00:01,000 [main] INFO   app - arrived\n");
        spool.write(body);
        spool.flush();
        spool.close();

        QMutexLocker lock(&m_mutex);
        if (m_stopping)
            return;
        m_status.generation = 1;
        m_status.committedSize = body.size();
        m_status.totalSize = body.size();
        m_status.state = FetchStatus::State::Live;
    }

    int            m_connectMs;
    QString        m_spoolDir;
    Worker         m_worker;
    mutable QMutex m_mutex;
    FetchStatus    m_status;
    bool           m_stopping = false;
};

} // namespace

// M17 — THE TEST THAT PINS THE MILESTONE (ARCHITECTURE.md §6.3.3).
//
// Everything else about the async open is checked through tests/FakeFetcher.h, and none
// of it can prove the thing the milestone is actually about: a fake that returns from
// start() instantly satisfies the blocking contract just as well as the non-blocking one.
// So this installs a transport that genuinely takes a second, and asks the three
// questions a user would.
class TestAsyncConnect : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
    static constexpr auto kUrl = "ssh://deploy@web1/var/log/app.log";

    static QString url() { return QString::fromLatin1(kUrl); }

    // Install a transport that takes `connectMs` to connect, for the life of the object.
    struct SlowFarm
    {
        explicit SlowFarm(int connectMs)
        {
            QStandardPaths::setTestModeEnabled(true);
            SourceSpoolRegistry::instance().setFetcherFactory(
                [connectMs, this](const QString &, QString *) {
                    auto fetcher = std::make_unique<SlowFetcher>(connectMs);
                    last = fetcher.get();
                    return fetcher;
                });
        }
        ~SlowFarm()
        {
            SourceSpoolRegistry::instance().clear();
            SourceSpoolRegistry::instance().setFetcherFactory(nullptr);
            QStandardPaths::setTestModeEnabled(false);
        }
        SlowFetcher *last = nullptr;
    };

private slots:
    void openingReturnsBeforeTheConnectDoes();
    void theTabExistsAndWaitsWhileItConnects();
    void closingATabMidConnectDoesNotWait();
    void shutdownMidConnectDoesNotHang();
};

void TestAsyncConnect::openingReturnsBeforeTheConnectDoes()
{
    // THE FREEZE, stated as a measurement. Opening used to run the whole connect on this
    // thread — TCP, handshake, authentication, stat and a 128 KB prime — so a host that
    // was slow to answer was a window that did not repaint.
    SlowFarm farm(2000);

    Document doc;
    ManualFormatProvider provider(QString::fromLatin1(kPattern));

    QElapsedTimer clock;
    clock.start();
    const bool opened = doc.prepare(url(), provider, Encoding::Utf8, QTimeZone::utc());
    const qint64 took = clock.elapsed();

    QVERIFY(opened);
    QVERIFY2(took < 250, qPrintable(QStringLiteral("prepare() blocked for %1 ms").arg(took)));
}

void TestAsyncConnect::theTabExistsAndWaitsWhileItConnects()
{
    // And what the user gets for that: not an empty window, but the document — waiting,
    // saying so, and filling in on its own.
    SlowFarm farm(400);

    Document doc;
    ManualFormatProvider provider(QString::fromLatin1(kPattern));
    QVERIFY(doc.prepare(url(), provider, Encoding::Utf8, QTimeZone::utc()));

    QVERIFY(doc.isWaiting());
    QVERIFY(doc.lastError().isEmpty()); // waiting is a state, not a failure
    QVERIFY(!doc.waitReason().isEmpty());
    QVERIFY(doc.source());
    QVERIFY(doc.source()->notReadyYet());

    // The connect finishes on its own thread and the source has bytes, with nothing on
    // this thread having waited for it.
    QTRY_VERIFY_WITH_TIMEOUT(!doc.source()->notReadyYet(), 5000);
    QVERIFY(doc.source()->refreshSize() > 0);
}

void TestAsyncConnect::closingATabMidConnectDoesNotWait()
{
    // Closing the last tab on a log drops the last handle to its spool, and that used to
    // JOIN the fetcher's thread — so closing a tab on a host that was not answering cost
    // as long as the connect had left to run. On the GUI thread that is a freeze, and
    // once a worker can be waiting on the GUI thread for a password it is a deadlock.
    SlowFarm farm(5000);

    auto doc = std::make_unique<Document>();
    ManualFormatProvider provider(QString::fromLatin1(kPattern));
    QVERIFY(doc->prepare(url(), provider, Encoding::Utf8, QTimeZone::utc()));
    QVERIFY(doc->isWaiting());

    QElapsedTimer clock;
    clock.start();
    doc.reset(); // the last handle: ~SpooledLogSource → ~SourceSpool → retire()
    const qint64 took = clock.elapsed();

    QVERIFY2(took < 250,
             qPrintable(QStringLiteral("closing mid-connect took %1 ms").arg(took)));
    // Asked to stop, and it actually gives up rather than running the connect out.
    QVERIFY(farm.last);
    QTRY_VERIFY_WITH_TIMEOUT(farm.last->isStopped(), 3000);
}

void TestAsyncConnect::shutdownMidConnectDoesNotHang()
{
    // Quitting with a connect in flight. The drain is bounded and the stragglers are
    // abandoned rather than joined, so this cannot become a hang on exit.
    SlowFarm farm(5000);

    auto doc = std::make_unique<Document>();
    ManualFormatProvider provider(QString::fromLatin1(kPattern));
    QVERIFY(doc->prepare(url(), provider, Encoding::Utf8, QTimeZone::utc()));

    QElapsedTimer clock;
    clock.start();
    SourceSpoolRegistry::instance().shutdown();
    const qint64 took = clock.elapsed();

    QVERIFY2(took < 4000, qPrintable(QStringLiteral("shutdown took %1 ms").arg(took)));
    doc.reset();
}

QTEST_MAIN(TestAsyncConnect)
#include "tst_asyncconnect.moc"
