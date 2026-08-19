#include <QtTest>

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "ArchiveFixtures.h"
#include "ArchiveLocation.h"
#include "Document.h"
#include "LiveController.h"
#include "LogModel.h"
#include "LogSource.h"
#include "ManualFormatProvider.h"
#include "RemoteLocation.h"
#include "SourceSpool.h"
#include "SpooledLogSource.h"

using namespace loftail;
using namespace loftail::fixtures;

// An archived log opened while its container is not there yet (SPEC.md §3,
// ARCHITECTURE.md §6.4/§6.5). tst_waiting covers the same promise for a plain local
// path and tst_waitingremote for a spooled one; this is the case that fell between them
// and was broken from M12 to here.
//
// WHAT WENT WRONG IS ONE INVARIANT. A waiting SPOOLED document keeps its source, because
// the source owns the spool and the spool owns the fetcher that is retrying — and an
// archive whose container was missing had no source at all, because
// ArchiveFetcher::start() failed the open outright. Nothing was retrying, so nothing
// could ever succeed: Document::resume() re-attaches to a spool under OpenPolicy::Reuse
// and the registry had none. The tab waited for ever, and it kept waiting after the
// container appeared.
//
// So the first assertion of nearly every case here is doc.source() != nullptr. That is
// the mechanism; the recovery is what the user sees.
//
// Gated on LOFTAIL_HAVE_ARCHIVE like every other real-archive test, and network-free.
// What can be asked without libarchive — that an absent file is told apart from an
// unreadable one — is in tst_remotelocation and tst_waiting, ungated.
class TestArchiveWaiting : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";

    QString path(const QString &name) const { return m_dir.path() + u'/' + name; }

    static QByteArray body()
    {
        return QByteArrayLiteral(
            "2026-08-05 00:00:01,000 [main] INFO  app.core - first\n"
            "2026-08-05 00:00:02,000 [main] WARN  app.core - second\n");
    }

    static bool openDoc(Document &doc, const QString &p)
    {
        return doc.prepare(p, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc());
    }

    // The owner's half of the resume handshake, exactly as MainWindow does it: core
    // holds no pattern, so the provider is built here (invariant #3).
    static void wireResume(LiveController &live, Document &doc, LogModel &model)
    {
        QObject::connect(&live, &LiveController::resumeRequested, &live, [&doc, &model] {
            ManualFormatProvider provider(QString::fromLatin1(kPattern));
            model.beginFilterReset();
            doc.resume(provider);
            model.endFilterReset();
        });
    }

    // Tick the live seam until the document has records, the way the watch would.
    static int pumpToFirstRecords(Document &doc, LiveController &live, int timeoutMs = 20000)
    {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < timeoutMs) {
            live.checkNow();
            if (!doc.isWaiting() && doc.index().records.size() > 0)
                return doc.index().records.size();
            QThread::msleep(2);
        }
        return 0;
    }

    // Tick for a while without expecting anything to change.
    static void pumpFor(LiveController &live, int ms)
    {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < ms) {
            live.checkNow();
            QThread::msleep(2);
        }
    }

    // Put `content` at `p` in one step. A container written in place would be opened
    // half-written by the fetcher's 100 ms retry, which is a race and not the subject.
    bool placeTarGz(const QString &p, const QVector<Member> &members)
    {
        const QString staging = m_dir.path() + QStringLiteral("/staging.bin");
        QFile::remove(staging);
        if (!writeTarGz(staging, members))
            return false;
        return QFile::rename(staging, p);
    }

    bool placeGzip(const QString &p, const QByteArray &content)
    {
        const QString staging = m_dir.path() + QStringLiteral("/staging.bin");
        QFile::remove(staging);
        if (!writeGzip(staging, content))
            return false;
        return QFile::rename(staging, p);
    }

private slots:
    void init() { SourceSpoolRegistry::instance().clear(); }
    void cleanup() { SourceSpoolRegistry::instance().clear(); }

    void aMemberOpenedBeforeItsContainerExistsRecoversWhenItAppears();
    void aBareCompressedStreamDoesTheSame();
    void theReasonNamesTheContainerAndComesFromTheFetcher();
    void anUnreadableContainerRefusesAndSaysSoRatherThanSayingItIsMissing();
    void anAddressNeedingAMemberStillFailsOutright();

private:
    QTemporaryDir m_dir;
};

void TestArchiveWaiting::aMemberOpenedBeforeItsContainerExistsRecoversWhenItAppears()
{
    const QString container = path(QStringLiteral("bundle.tar.gz"));
    const QString address = container + QStringLiteral("/var/log/app.log");
    QVERIFY(!QFileInfo::exists(container));

    Document doc;
    // The open SUCCEEDS into a wait, exactly as a plain absent path does.
    QVERIFY2(openDoc(doc, address), qPrintable(doc.lastError()));
    QVERIFY(doc.isWaiting());
    QVERIFY(doc.lastError().isEmpty());
    // THE REGRESSION. A spooled document that is waiting keeps its source, because that
    // source owns the spool and the spool owns the fetcher that is looking for the
    // container. With no source there is nothing retrying and resume() can never find a
    // spool to reuse — which is precisely how this waited for ever.
    QVERIFY(doc.source());
    QVERIFY(dynamic_cast<SpooledLogSource *>(doc.source()));

    LogModel model(&doc);
    LiveController live(&doc, &model);
    wireResume(live, doc, model);
    live.start();

    // Nothing there yet, and no amount of ticking invents it.
    pumpFor(live, 300);
    QVERIFY(doc.isWaiting());
    QCOMPARE(doc.index().records.size(), 0);

    QVERIFY(placeTarGz(container, {{QStringLiteral("var/log/app.log"), body()}}));

    QCOMPARE(pumpToFirstRecords(doc, live), 2);
    QVERIFY(!doc.isWaiting());
    QVERIFY(doc.waitReason().isEmpty());
    QVERIFY(doc.formatSettled()); // settled on the bytes that arrived, never on nothing
}

void TestArchiveWaiting::aBareCompressedStreamDoesTheSame()
{
    // Broader than the bug was filed as: a single-stream container names no member at
    // all, so nothing about the address distinguishes it — what mattered was only that
    // the container was not there.
    const QString gz = path(QStringLiteral("app.log.gz"));
    QVERIFY(!QFileInfo::exists(gz));

    Document doc;
    QVERIFY2(openDoc(doc, gz), qPrintable(doc.lastError()));
    QVERIFY(doc.isWaiting());
    QVERIFY(doc.source());

    LogModel model(&doc);
    LiveController live(&doc, &model);
    wireResume(live, doc, model);
    live.start();

    pumpFor(live, 300);
    QVERIFY(doc.isWaiting());

    QVERIFY(placeGzip(gz, body()));

    QCOMPARE(pumpToFirstRecords(doc, live), 2);
    QVERIFY(!doc.isWaiting());
}

void TestArchiveWaiting::theReasonNamesTheContainerAndComesFromTheFetcher()
{
    const QString container = path(QStringLiteral("later.tar.gz"));
    const QString address = container + QStringLiteral("/var/log/app.log");

    Document doc;
    QVERIFY2(openDoc(doc, address), qPrintable(doc.lastError()));
    QVERIFY(doc.isWaiting());

    // What is missing is the CONTAINER, and the reason has to name it: the member
    // cannot be looked for and saying "app.log has not appeared yet" points at a file
    // whose existence nothing here knows anything about.
    QVERIFY2(doc.waitReason().contains(QStringLiteral("later.tar.gz")),
             qPrintable(doc.waitReason()));

    // And it is the FETCHER's own sentence, not the transition's. That is what makes it
    // republishable: LiveController::republishWaitReason() asks the source on every
    // tick, and returns immediately when there is no source — which is why the reason
    // used to freeze at whatever the open happened to say.
    QVERIFY(doc.source());
    QCOMPARE(doc.waitReason(), sourceStatusText(*doc.source(), address));
}

void TestArchiveWaiting::anUnreadableContainerRefusesAndSaysSoRatherThanSayingItIsMissing()
{
#if defined(Q_OS_WIN)
    QSKIP("POSIX permission bits");
#else
    const QString container = path(QStringLiteral("locked.tar.gz"));
    QVERIFY(placeTarGz(container, {{QStringLiteral("var/log/app.log"), body()}}));
    QVERIFY(QFile::setPermissions(container, QFileDevice::WriteOwner));
    if (QFileInfo(container).isReadable())
        QSKIP("running as root: a mode-000 file is still readable");

    const QString address = container + QStringLiteral("/var/log/app.log");

    // Not an absence — the container is right there — so it is a REFUSAL that keeps its
    // tab and says why (M17), never a wait for something to appear. What must not
    // happen is the two being folded together: a container the user can see in their
    // file manager described as one that has not turned up.
    Document doc;
    QVERIFY2(openDoc(doc, address), qPrintable(doc.lastError()));
    QVERIFY(doc.source());
    const QString reason = doc.waitReason();
    QVERIFY(!reason.isEmpty());
    QVERIFY2(!reason.contains(QStringLiteral("has not appeared")), qPrintable(reason));
    QVERIFY2(!reason.contains(QStringLiteral("waiting for")), qPrintable(reason));

    // And it stays refused rather than polling a permission bit for the life of the
    // tab. File ▸ Reconnect is the deliberate gesture for asking again.
    LogModel model(&doc);
    LiveController live(&doc, &model);
    wireResume(live, doc, model);
    live.start();
    pumpFor(live, 300);
    QCOMPARE(doc.index().records.size(), 0);
    QCOMPARE(doc.waitReason(), reason);

    QVERIFY(QFile::setPermissions(container,
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner));
#endif
}

void TestArchiveWaiting::anAddressNeedingAMemberStillFailsOutright()
{
    // M17's rule, and the fix above must not soften it: what can be decided with NO I/O
    // still fails the open. A multi-member container with no member picked names no log,
    // and waiting will never give it one — whether or not the container exists.
    const QString missing = path(QStringLiteral("nomember.zip"));
    Document absent;
    QVERIFY(!openDoc(absent, missing));
    QVERIFY(!absent.isWaiting());
    QVERIFY(!absent.lastError().isEmpty());

    const QString present = path(QStringLiteral("real.zip"));
    QVERIFY(writeZip(present, {{QStringLiteral("a.log"), body()},
                               {QStringLiteral("b.log"), body()}}));
    Document there;
    QVERIFY(!openDoc(there, present));
    QVERIFY(!there.isWaiting());
    QVERIFY(!there.lastError().isEmpty());
}

QTEST_MAIN(TestArchiveWaiting)
#include "tst_archivewaiting.moc"
