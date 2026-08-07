#include <QtTest>

#include <QByteArray>

#include "Document.h"
#include "FakeFetcher.h"
#include "LiveController.h"
#include "LogModel.h"
#include "LogSource.h"
#include "ManualFormatProvider.h"
#include "Priority.h"
#include "RecordIndex.h"
#include "SpooledLogSource.h"

using namespace loftail;

// M13 — a REMOTE log that is not there (SPEC.md §3, ARCHITECTURE.md §6.5), which is
// the half of the feature a path cannot answer. A local wait is decided by stat'ing
// the path; a spooled one is decided by the fetcher, because there is no local path
// to stat and asking the far end costs a round trip on every watch tick.
//
// That difference produces the one asymmetry worth pinning here: a waiting local
// document RELEASES its source, and a waiting spooled one KEEPS it. The spooled
// source owns the shared spool, the spool owns the fetcher, and the fetcher is the
// thing doing the retrying — let go of it and the log never comes back.
//
// Network-free and ungated: only the transport is fake (tests/FakeFetcher.h), so the
// document, the live seam and the spool are all exercised for real.
class TestWaitingRemote : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
    static constexpr auto kUrl = "ssh://deploy@web1/var/log/app.log";

    static QString url() { return QString::fromLatin1(kUrl); }

    static QByteArray rec(int sec, const char *prio, const char *logger, const char *msg)
    {
        QByteArray out = "2026-08-05 00:00:";
        out += QByteArray::number(sec).rightJustified(2, '0');
        out += ",000 [main] ";
        out += prio;
        out += "  ";
        out += logger;
        out += " - ";
        out += msg;
        out += '\n';
        return out;
    }

    // The owner's half of the resume handshake, as MainWindow does it: core has no
    // pattern to build a provider from, so this is where one comes from (invariant #3).
    static void wireResume(LiveController &live, Document &doc, LogModel &model, int *resumes)
    {
        QObject::connect(&live, &LiveController::resumeRequested, &live, [&doc, &model, resumes] {
            ManualFormatProvider provider(QString::fromLatin1(kPattern));
            model.beginFilterReset();
            const bool ok = doc.resume(provider);
            model.endFilterReset();
            if (ok)
                ++*resumes;
        });
    }

private slots:
    void unreachableHostOpensWaitingAndFillsInWhenItReturns();
    void aRemoteLogRemovedMidTailWaitsAndKeepsItsSpool();
    void aRefusalStillFailsTheOpen();

    // M17 — a spool with nothing in it yet waits, and the three states that means.
    void aConnectingSpoolWaitsUntilItsFirstBytes();
    void aRefusedConnectWaitsAndSaysWhy();
    void anEmptyRemoteLogIsNotAWait();
};

void TestWaitingRemote::unreachableHostOpensWaitingAndFillsInWhenItReturns()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(url());
    remote->setInitialContent(rec(0, "INFO ", "net.io", "hello")
                              + rec(1, "ERROR", "db.pool", "boom"));
    remote->setInitiallyUnavailable(QStringLiteral("Cannot reach web1:22 — Connection refused"));

    Document doc;
    ManualFormatProvider provider(QString::fromLatin1(kPattern));
    // The open SUCCEEDS against a host that is down. Before M13 this was the one way a
    // remote log could refuse to open at all.
    QVERIFY(doc.prepare(url(), provider, Encoding::Utf8, QTimeZone::utc()));
    QVERIFY(doc.isWaiting());
    QVERIFY(doc.lastError().isEmpty());
    QVERIFY(!doc.formatSettled());

    // And it KEEPS its source, unlike a local wait: that source holds the spool, and
    // the spool holds the fetcher that is retrying.
    QVERIFY(doc.source());
    QVERIFY(doc.source()->originVanished());
    QCOMPARE(doc.source()->size(), 0);

    // The status line says what the fetcher said, rather than a generic sentence: only
    // the transport knows whether the host was unreachable or the path was missing.
    QVERIFY(sourceStatusText(*doc.source(), doc.path())
                .contains(QStringLiteral("Connection refused")));

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.setVanishGrace(0);
    int resumes = 0;
    wireResume(live, doc, model, &resumes);
    live.start();

    live.checkNow();
    QVERIFY(doc.isWaiting()); // still down, still nothing
    QCOMPARE(resumes, 0);

    remote->becomeAvailable();
    live.checkNow();

    QCOMPARE(resumes, 1);
    QVERIFY(!doc.isWaiting());
    QCOMPARE(model.rowCount(), 2);
    // Settled from the bytes that arrived, exactly as a local wait does.
    QVERIFY(doc.formatSettled());
    QCOMPARE(doc.index().records.at(1).priorityEnum(), Priority::Error);

    // Ordinary tailing from here.
    remote->append(rec(2, "WARN ", "net.io", "slow"));
    live.checkNow();
    QCOMPARE(model.rowCount(), 3);
}

void TestWaitingRemote::aRemoteLogRemovedMidTailWaitsAndKeepsItsSpool()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(url());
    remote->setInitialContent(rec(0, "INFO ", "app", "one") + rec(1, "INFO ", "app", "two"));

    Document doc;
    QVERIFY(doc.open(url(), QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    QCOMPARE(doc.index().records.size(), 2);

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.setVanishGrace(0);
    int resumes = 0;
    wireResume(live, doc, model, &resumes);
    live.start();

    int waits = 0;
    connect(&live, &LiveController::waitingChanged, &live, [&](bool w, const QString &) {
        if (w)
            ++waits;
    });

    remote->becomeUnavailable(QStringLiteral("/var/log/app.log is not readable on web1 right now."));
    live.checkNow();

    QCOMPARE(waits, 1);
    QVERIFY(doc.isWaiting());
    QCOMPARE(model.rowCount(), 0);
    // THE ASYMMETRY: the source stays. Releasing it would drop the last handle on the
    // spool, tear down the fetcher, and leave nothing retrying — the log would never
    // come back, and nothing would say so.
    QVERIFY(doc.source());
    QCOMPARE(remote->stopCount(), 0);

    remote->becomeAvailable();
    live.checkNow();

    QCOMPARE(resumes, 1);
    QVERIFY(!doc.isWaiting());
    QCOMPARE(model.rowCount(), 2);
    // One connection throughout: the wait was ridden out on the spool that was already
    // there, and a rescan mid-tail must never reconnect (§6.3).
    QCOMPARE(remote->startCount(), 1);
}

void TestWaitingRemote::aRefusalStillFailsTheOpen()
{
    // A refusal decided with NO I/O — no transport was ever built, because the address
    // named none that could be. Those still fail the open outright, with no document to
    // show; everything that needed a round trip now opens a waiting tab instead, which
    // is aRefusedConnectWaitsAndSaysWhy() below.
    FakeRemoteFarm farm;
    farm.at(url())->setStartFailure(QStringLiteral("Authentication to deploy@web1 failed."));

    Document doc;
    ManualFormatProvider provider(QString::fromLatin1(kPattern));
    QVERIFY(!doc.prepare(url(), provider, Encoding::Utf8, QTimeZone::utc()));
    QVERIFY(!doc.isWaiting());
    QCOMPARE(doc.lastError(), QStringLiteral("Authentication to deploy@web1 failed."));
}

void TestWaitingRemote::aConnectingSpoolWaitsUntilItsFirstBytes()
{
    // The shape of every remote open since M17: the document exists before the connect
    // has finished, so it opens WAITING over a legal, empty spool and fills in when the
    // bytes arrive. Everything below the wait is M13's, unchanged.
    FakeRemoteFarm farm;
    auto remote = farm.at(url());
    remote->setInitialContent(rec(1, "INFO ", "boot", "one") + rec(2, "WARN ", "boot", "two"));
    remote->setConnectDelayed();

    Document doc;
    ManualFormatProvider provider(QString::fromLatin1(kPattern));
    QVERIFY(doc.prepare(url(), provider, Encoding::Utf8, QTimeZone::utc()));

    // Waiting, not failed — and the reason is the transport's own word for the state it
    // is in rather than "the log is not there", which would be wrong: nobody has looked.
    QVERIFY(doc.isWaiting());
    QVERIFY(doc.lastError().isEmpty());
    QVERIFY(doc.waitReason().contains(QStringLiteral("connecting")));
    // The format has NOT settled: settling it against the empty sample would be a guess
    // about a log nobody has seen, and resume() is the only thing that can revisit it.
    QVERIFY(!doc.formatSettled());
    QVERIFY(doc.source()); // the spool is kept — it is what is doing the connecting

    LogModel model(&doc);
    LiveController live(&doc, &model);
    int resumes = 0;
    wireResume(live, doc, model, &resumes);
    live.start();

    // Still connecting: a tick must not take it out of the wait, because there is still
    // nothing to settle a format from.
    live.checkNow();
    QCOMPARE(resumes, 0);
    QVERIFY(doc.isWaiting());

    remote->becomeAvailable();
    live.checkNow();

    QCOMPARE(resumes, 1);
    QVERIFY(!doc.isWaiting());
    QVERIFY(doc.formatSettled());
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(remote->startCount(), 1);
}

void TestWaitingRemote::aRefusedConnectWaitsAndSaysWhy()
{
    // A rejected password or a changed host key now KEEPS the tab and reports the
    // transport's own words (SPEC.md §3). It is still not retried on a timer — the
    // fetcher latches its refusal — so what the user gets is a tab that explains itself
    // and File ▸ Reconnect.
    FakeRemoteFarm farm;
    auto remote = farm.at(url());
    remote->setInitialContent(rec(1, "INFO ", "boot", "one"));
    remote->setConnectRefusal(QStringLiteral("Authentication to deploy@web1 failed."));

    Document doc;
    ManualFormatProvider provider(QString::fromLatin1(kPattern));
    QVERIFY(doc.prepare(url(), provider, Encoding::Utf8, QTimeZone::utc()));

    QVERIFY(doc.isWaiting());
    QVERIFY(doc.lastError().isEmpty()); // waiting is a state, not a failure
    QCOMPARE(doc.waitReason(), QStringLiteral("Authentication to deploy@web1 failed."));
    QVERIFY(doc.source());
}

void TestWaitingRemote::anEmptyRemoteLogIsNotAWait()
{
    // The correction that keeps notReadyYet() honest. A remote log that EXISTS and has
    // nothing in it has been looked at, found, and read — so it opens as an ordinary
    // empty tab, exactly as an empty local file does. Marking it ◦ would say "not there
    // yet" about a log that is demonstrably there, which is the one distinction that
    // mark exists to draw.
    FakeRemoteFarm farm;
    auto remote = farm.at(url());
    remote->setInitialContent(QByteArray()); // present, zero length

    Document doc;
    ManualFormatProvider provider(QString::fromLatin1(kPattern));
    QVERIFY(doc.prepare(url(), provider, Encoding::Utf8, QTimeZone::utc()));

    QVERIFY(!doc.isWaiting());
    QVERIFY(doc.waitReason().isEmpty());
    QCOMPARE(doc.index().recordCount(), 0);

    // And it tails from there like any other log.
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    remote->append(rec(1, "INFO ", "boot", "first line at last"));
    live.checkNow();
    QCOMPARE(model.rowCount(), 1);
}

QTEST_GUILESS_MAIN(TestWaitingRemote)
#include "tst_waitingremote.moc"
