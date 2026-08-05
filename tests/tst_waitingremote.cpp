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
    // A host that says no is not a host that is not there. A changed key or a rejected
    // password gets the same answer however long loftail waits, so it stays an open
    // failure with no document and no retry — which is also what stops loftail
    // hammering a host it has just been refused by.
    FakeRemoteFarm farm;
    farm.at(url())->setStartFailure(QStringLiteral("Authentication to deploy@web1 failed."));

    Document doc;
    ManualFormatProvider provider(QString::fromLatin1(kPattern));
    QVERIFY(!doc.prepare(url(), provider, Encoding::Utf8, QTimeZone::utc()));
    QVERIFY(!doc.isWaiting());
    QCOMPARE(doc.lastError(), QStringLiteral("Authentication to deploy@web1 failed."));
}

QTEST_GUILESS_MAIN(TestWaitingRemote)
#include "tst_waitingremote.moc"
