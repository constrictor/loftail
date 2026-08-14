#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include "DiagnosticLog.h"

#include <thread>
#include <vector>

using namespace loftail;

// loftail's own log (SPEC.md §3 "Diagnostics", DiagnosticLog.h). Core-only and ungated:
// nothing here needs SSH, an archive or a keychain, and the file has to behave identically
// in every configuration — it is the one artefact a bug report is built around.
//
// std::thread rather than QThread for the concurrency case, per ARCHITECTURE.md §13.1:
// QThread::wait() joins through a QWaitCondition, whose annotations break TSan's
// happens-before chain against a system libQt6Core that was not built under it.
class TestDiagLog : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QStringList lines() const
    {
        QFile f(diagLogPath());
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll())
            .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }

private slots:
    void init();
    void aLineCarriesItsTimeAreaAndMessage();
    void theFirstLineIdentifiesTheBuild();
    void repeatsAreCollapsedAndCounted();
    void distinctKeysThrottleIndependently();
    void itRollsOverExactlyOnceAtTheCap();
    void concurrentWritersProduceWholeLines();
};

void TestDiagLog::init()
{
    QVERIFY(m_dir.isValid());
    // A fresh directory per case: the throttle state and the session-start latch are
    // process-wide, and diagLogSetDirectory() is what resets both.
    const QString sub = m_dir.filePath(QString::number(QTest::currentTestFunction()
                                                       ? qHash(QString::fromUtf8(
                                                             QTest::currentTestFunction()))
                                                       : 0));
    QDir().mkpath(sub);
    diagLogSetDirectory(sub);
    QFile::remove(diagLogPath());
    QFile::remove(diagLogPath() + QStringLiteral(".1"));
    diagLogSetDirectory(sub); // re-arm after removing the file underneath it
}

void TestDiagLog::aLineCarriesItsTimeAreaAndMessage()
{
    diagLog("ssh", QStringLiteral("connect attempt: ssh://host/var/log/app.log"));

    const QStringList out = lines();
    QCOMPARE(out.size(), 2); // the session-start line, then ours
    const QString line = out.last();
    QVERIFY2(line.contains(QLatin1String("[ssh]")), qPrintable(line));
    QVERIFY2(line.contains(QLatin1String("connect attempt")), qPrintable(line));
    // ISO-8601 UTC with milliseconds, so a diagnostic line can be lined up against a
    // log4cplus record without anybody having to work out which zone it is in.
    const QString stamp = line.left(line.indexOf(QLatin1Char(' ')));
    QVERIFY2(QDateTime::fromString(stamp, Qt::ISODateWithMs).isValid(), qPrintable(stamp));
}

void TestDiagLog::theFirstLineIdentifiesTheBuild()
{
    // Written by the first write of any kind, not only by an explicit session start —
    // otherwise a rolled-over file would carry no identity at all.
    diagLog("ssh", QStringLiteral("anything"));
    const QStringList out = lines();
    QVERIFY(!out.isEmpty());
    QVERIFY2(out.first().contains(QLatin1String("[app]")), qPrintable(out.first()));
    QVERIFY2(out.first().contains(QLatin1String("loftail")), qPrintable(out.first()));
    QVERIFY2(out.first().contains(QLatin1String("starting")), qPrintable(out.first()));
}

void TestDiagLog::repeatsAreCollapsedAndCounted()
{
    // The case the whole mechanism exists for: a host that is down, retried every few
    // seconds, or a waiting document polling every 750 ms. Unthrottled this is thousands
    // of lines an hour and the file is useless.
    for (int i = 0; i < 50; ++i)
        diagLogEvery(60000, "wait", QStringLiteral("/var/log/app.log"),
                     QStringLiteral("still waiting for /var/log/app.log"));

    QStringList out = lines();
    QCOMPARE(out.size(), 2); // session start + exactly one waiting line
    QVERIFY2(!out.last().contains(QLatin1String("since the last")), qPrintable(out.last()));

    // The suppressed ones are COUNTED, never discarded: a log that under-reported how
    // often loftail tried would be lying about the one thing it is read to answer. A zero
    // window forces the next one through without waiting a minute for it.
    diagLogEvery(0, "wait", QStringLiteral("/var/log/app.log"),
                 QStringLiteral("still waiting for /var/log/app.log"));
    out = lines();
    QCOMPARE(out.size(), 3);
    QVERIFY2(out.last().contains(QLatin1String("(+49 since the last)")), qPrintable(out.last()));
}

void TestDiagLog::distinctKeysThrottleIndependently()
{
    // Two logs waiting at once must not silence each other — the key is what separates
    // them, and getting this wrong would make a multi-tab session report only one of them.
    diagLogEvery(60000, "wait", QStringLiteral("a.log"), QStringLiteral("waiting for a"));
    diagLogEvery(60000, "wait", QStringLiteral("b.log"), QStringLiteral("waiting for b"));
    diagLogEvery(60000, "wait", QStringLiteral("a.log"), QStringLiteral("waiting for a"));

    const QStringList out = lines();
    QCOMPARE(out.size(), 3); // session start + one per key, the third suppressed
    QVERIFY(out.at(1).contains(QLatin1String("waiting for a")));
    QVERIFY(out.at(2).contains(QLatin1String("waiting for b")));
}

void TestDiagLog::itRollsOverExactlyOnceAtTheCap()
{
    const QString filler(2000, QLatin1Char('x'));
    // Past the cap, but not so far past that a second rollover could be reached — the
    // claim is that the total on disk is bounded by twice the cap, not that it is pruned.
    const int needed = int(kDiagLogMaxBytes / filler.size()) + 8;
    for (int i = 0; i < needed; ++i)
        diagLog("app", filler);

    QVERIFY2(QFile::exists(diagLogPath() + QStringLiteral(".1")),
             "the rolled-over copy is missing: the cap was never reached, or it was deleted");
    QVERIFY(QFileInfo(diagLogPath()).size() <= kDiagLogMaxBytes + filler.size() + 200);
    QVERIFY(QFileInfo(diagLogPath() + QStringLiteral(".1")).size()
            <= kDiagLogMaxBytes + filler.size() + 200);
    // The fresh file identifies its own run, so an excerpt taken after a rollover still
    // says which binary produced it.
    const QStringList out = lines();
    QVERIFY(!out.isEmpty());
    QVERIFY2(out.first().contains(QLatin1String("starting")), qPrintable(out.first()));
}

void TestDiagLog::concurrentWritersProduceWholeLines()
{
    // Fetcher threads write to this, so the claim is that a line is never interleaved with
    // another — not merely that nothing crashes. Every line must therefore still parse as
    // "<stamp> [area] message" when eight threads write at once.
    constexpr int kThreads = 8;
    constexpr int kEach = 200;
    std::vector<std::thread> writers;
    writers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([t]() {
            for (int i = 0; i < kEach; ++i) {
                diagLog("ssh", QStringLiteral("thread %1 line %2").arg(t).arg(i));
            }
        });
    }
    for (std::thread &w : writers)
        w.join();

    const QStringList out = lines();
    QCOMPARE(out.size(), kThreads * kEach + 1); // + the session-start line
    for (const QString &line : out) {
        QVERIFY2(line.contains(QLatin1String("] ")), qPrintable(line));
        const QString stamp = line.left(line.indexOf(QLatin1Char(' ')));
        QVERIFY2(QDateTime::fromString(stamp, Qt::ISODateWithMs).isValid(), qPrintable(line));
    }
}

QTEST_MAIN(TestDiagLog)
#include "tst_diaglog.moc"
