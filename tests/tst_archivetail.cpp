// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include "ArchiveFixtures.h"
#include "Document.h"
#include "FakeFetcher.h"
#include "FilteredIndex.h"
#include "LiveController.h"
#include "LogModel.h"
#include "LogSource.h"
#include "ManualFormatProvider.h"
#include "Priority.h"
#include "RecordIndex.h"
#include "SourceSpool.h"

#include <cstring>

using namespace loftail;
using namespace loftail::fixtures;

// M12 — an archived log read end to end (SPEC.md §3, ARCHITECTURE.md §6.4). This is
// tst_remotetail's shape over a real archive: records appear WHILE the member is being
// expanded, driven through LiveController::checkNow(), and then the stream ends.
//
// The acceptance property is the last test: after completion the index must be
// byte-identical to opening the same content as a plain uncompressed file. Anything
// less would mean the record offsets do not describe what the user is reading.
//
// Gated on LOFTAIL_HAVE_ARCHIVE, but needs no network and no credentials — so unlike
// M11's transport this really runs in CI.
class TestArchiveTail : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";

    QString path(const QString &name) const { return m_dir.path() + u'/' + name; }

    static QByteArray rec(int sec, const char *prio, const char *logger, const QByteArray &msg)
    {
        QByteArray out = "2026-08-05 00:00:";
        out += QByteArray::number(sec % 60).rightJustified(2, '0');
        out += ",000 [main] ";
        out += prio;
        out += "  ";
        out += logger;
        out += " - ";
        out += msg;
        out += '\n';
        return out;
    }

    static bool openDoc(Document &doc, const QString &p)
    {
        return doc.open(p, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc());
    }

    // The owner's half of the resume handshake, as MainWindow does it: core holds no
    // pattern, so this is where the provider comes from (invariant #3).
    //
    // Needed by every case here since M17. An archived log opens WAITING — start() no
    // longer expands anything itself, because seeking to a member decompresses the
    // container up to it and that cannot happen on the thread opening the document — so
    // the first records arrive through resume() rather than through open().
    static void wireResume(LiveController &live, Document &doc, LogModel &model)
    {
        QObject::connect(&live, &LiveController::resumeRequested, &live, [&doc, &model] {
            ManualFormatProvider provider(QString::fromLatin1(kPattern));
            model.beginFilterReset();
            const bool ok = doc.resume(provider);
            if (ok) {
                // Exactly what MainWindow::resumeOrSettleDocument does after a successful
                // resume: the intern tables were rebuilt from scratch, and a filter set
                // before the bytes arrived still has to apply to them.
                doc.resolveHighlighters();
                if (doc.filters().anyActive() || doc.viewRestricted())
                    doc.applyFilters();
            }
            model.endFilterReset();
        });
    }

    // Tick until the document has left its waiting state, i.e. the prime has landed and
    // there are real records to reason about. Returns the count it settled on.
    static int pumpToFirstRecords(Document &doc, LiveController &live, int timeoutMs = 30000)
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

    // Drive the live controller until the source reports its stream finished, the way
    // the watch tick would. Returns false if it never does.
    static bool pumpToCompletion(Document &doc, LiveController &live, int timeoutMs = 30000)
    {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < timeoutMs) {
            live.checkNow();
            if (doc.source() && doc.source()->isComplete())
                return true;
            QThread::msleep(2);
        }
        return false;
    }

    static bool sameIndex(const RecordIndex &a, const RecordIndex &b, QString *why);

private slots:
    void init() { SourceSpoolRegistry::instance().clear(); }
    void cleanup() { SourceSpoolRegistry::instance().clear(); }

    void recordsAppearWhileTheMemberIsStillExpanding();
    void aMultiLineRecordSplitAcrossChunksResolves();
    void filtersApplyToRecordsArrivingDuringExpansion();
    void anExpandedMemberIndexesExactlyLikeAPlainFile();
    void aRemoteArchiveChainsTwoFetchers();

private:
    QTemporaryDir m_dir;
};

bool TestArchiveTail::sameIndex(const RecordIndex &a, const RecordIndex &b, QString *why)
{
    if (a.records.size() != b.records.size()) {
        if (why)
            *why = QStringLiteral("record count %1 vs %2")
                       .arg(a.records.size()).arg(b.records.size());
        return false;
    }
    for (int i = 0; i < a.records.size(); ++i) {
        if (std::memcmp(&a.records.at(i), &b.records.at(i), sizeof(Record)) != 0) {
            if (why)
                *why = QStringLiteral("record %1 differs (off %2/%3 len %4/%5 lines %6/%7)")
                           .arg(i)
                           .arg(a.records.at(i).offset).arg(b.records.at(i).offset)
                           .arg(a.records.at(i).length).arg(b.records.at(i).length)
                           .arg(a.records.at(i).lineCount).arg(b.records.at(i).lineCount);
            return false;
        }
    }
    if (a.loggers.names() != b.loggers.names()) {
        if (why)
            *why = QStringLiteral("logger tables differ");
        return false;
    }
    if (a.threads.names() != b.threads.names()) {
        if (why)
            *why = QStringLiteral("thread tables differ");
        return false;
    }
    return true;
}

void TestArchiveTail::recordsAppearWhileTheMemberIsStillExpanding()
{
    // Large enough that start()'s 128 KiB prime cannot cover it, so the rest genuinely
    // arrives through the live seam rather than being there all along.
    QByteArray body;
    for (int i = 0; i < 12000; ++i)
        body += rec(i, "INFO ", "app.core", QByteArrayLiteral("record ") + QByteArray::number(i));

    const QString gz = path(QStringLiteral("growing.log.gz"));
    QVERIFY(writeGzip(gz, body));

    Document doc;
    QVERIFY2(openDoc(doc, gz), qPrintable(doc.lastError()));
    // Opens WAITING now: nothing has been expanded yet, because expanding is the
    // worker's job (§6.4). The tab exists from this instant either way.
    QVERIFY(doc.isWaiting());

    LogModel model(&doc);
    LiveController live(&doc, &model);
    wireResume(live, doc, model);
    live.start();

    // The document picks up the primed head and settles its format on it: the
    // invariant-#9 payoff, where the spool is filled and indexed at the same time, so an
    // archived log fills in as it expands rather than freezing until it is done.
    const int atFirstRecords = pumpToFirstRecords(doc, live);
    QVERIFY2(atFirstRecords > 0, "the prime must leave real records to settle a format on");
    QVERIFY(doc.formatSettled());

    // NOT asserted here: that this first batch was short of the whole member. Nothing
    // pauses after the prime any more — the worker carries straight on — so how much has
    // arrived by the time anything observes it is a race, and a local gzip of this size
    // often wins it. The progressive publication itself is pinned where it can be
    // observed deterministically, at the fetcher
    // (tst_archivefetcher::committedSizeNeverRunsAheadOfTheSpoolFile); what belongs here
    // is that the document opened with NOTHING and got everything through the live seam,
    // which is the isWaiting() above and the count below.

    QVERIFY(pumpToCompletion(doc, live));
    live.checkNow(); // the tick that ingests the final chunk and stops the watch

    QCOMPARE(doc.index().records.size(), 12000);
    QCOMPARE(doc.messageText(doc.index().records.at(11999)), QStringLiteral("record 11999"));
}

void TestArchiveTail::aMultiLineRecordSplitAcrossChunksResolves()
{
    // A record whose continuation lines straddle an expansion chunk boundary. The
    // trailing record is held provisional exactly as it is for a local append, so
    // invariant #2 survives the fact that the bytes arrived in pieces.
    QByteArray body;
    for (int i = 0; i < 6000; ++i)
        body += rec(i, "INFO ", "app.core", QByteArrayLiteral("filler ") + QByteArray::number(i));
    body += rec(1, "ERROR", "app.db", QByteArrayLiteral("stack follows"));
    for (int i = 0; i < 400; ++i)
        body += QByteArrayLiteral("    at frame ") + QByteArray::number(i) + "\n";
    body += rec(2, "INFO ", "app.core", QByteArrayLiteral("after the stack"));

    const QString gz = path(QStringLiteral("multiline.log.gz"));
    QVERIFY(writeGzip(gz, body));

    Document doc;
    QVERIFY2(openDoc(doc, gz), qPrintable(doc.lastError()));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    wireResume(live, doc, model);
    live.start();
    QVERIFY(pumpToCompletion(doc, live));
    live.checkNow();

    QCOMPARE(doc.index().records.size(), 6002);
    const Record &multi = doc.index().records.at(6000);
    QCOMPARE(multi.lineCount, quint16(401)); // the record line plus 400 continuations
    QCOMPARE(doc.messageText(doc.index().records.at(6001)),
             QStringLiteral("after the stack"));
}

void TestArchiveTail::filtersApplyToRecordsArrivingDuringExpansion()
{
    QByteArray body;
    for (int i = 0; i < 9000; ++i) {
        const bool bad = (i % 100) == 0;
        body += rec(i, bad ? "ERROR" : "INFO ", bad ? "app.db" : "app.core",
                    QByteArrayLiteral("record ") + QByteArray::number(i));
    }

    const QString gz = path(QStringLiteral("filtered.log.gz"));
    QVERIFY(writeGzip(gz, body));

    Document doc;
    QVERIFY2(openDoc(doc, gz), qPrintable(doc.lastError()));

    // Filter before the expansion finishes: records arriving later must be judged by
    // the same predicate, exactly as appended records are for a local file (SPEC.md §6).
    doc.filters().priorityEnabled = true;
    doc.filters().minPriority = Priority::Error;
    doc.applyFilters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    wireResume(live, doc, model);
    live.start();
    QVERIFY(pumpToCompletion(doc, live));
    live.checkNow();

    QCOMPARE(doc.index().records.size(), 9000);
    QCOMPARE(doc.filtered().recordCount(), 90); // every hundredth record is an ERROR
}

void TestArchiveTail::anExpandedMemberIndexesExactlyLikeAPlainFile()
{
    // THE ACCEPTANCE TEST. Whatever route the bytes took, the index that comes out must
    // be the one a plain file would have produced — same count, same offsets, same
    // lengths, same interned name tables. Anything else and the offsets in Record do
    // not describe the text the user is reading.
    QByteArray body;
    for (int i = 0; i < 5000; ++i) {
        body += rec(i, (i % 7) ? "INFO " : "WARN ", (i % 3) ? "app.core" : "app.net",
                    QByteArrayLiteral("record ") + QByteArray::number(i));
        if (i % 50 == 0)
            body += QByteArrayLiteral("  continuation of ") + QByteArray::number(i) + "\n";
    }

    const QString tgz = path(QStringLiteral("acceptance.tar.gz"));
    QVERIFY(writeTarGz(tgz, {{QStringLiteral("var/log/app.log"), body}}));

    Document archived;
    QVERIFY2(openDoc(archived, tgz + QStringLiteral("/var/log/app.log")),
             qPrintable(archived.lastError()));
    LogModel model(&archived);
    LiveController live(&archived, &model);
    wireResume(live, archived, model);
    live.start();
    QVERIFY(pumpToCompletion(archived, live));
    live.checkNow();

    QTemporaryFile plainFile(m_dir.path() + QStringLiteral("/plain-XXXXXX.log"));
    QVERIFY(plainFile.open());
    plainFile.write(body);
    plainFile.flush();
    Document plain;
    QVERIFY2(openDoc(plain, plainFile.fileName()), qPrintable(plain.lastError()));

    QString why;
    QVERIFY2(sameIndex(archived.index(), plain.index(), &why), qPrintable(why));
}

void TestArchiveTail::aRemoteArchiveChainsTwoFetchers()
{
    // THE COMPOSED CASE, and the reason the nested spelling has no scheme of its own:
    // an archive is a file type and SSH is a way of reaching a file, so a rotated
    // app.log.1.gz on a server needs no new mechanism — the SSH fetcher supplies the
    // container's bytes and the archive fetcher expands them. Only the transport is
    // faked here, so both fetchers are real and no network is involved.
    QByteArray body;
    for (int i = 0; i < 3000; ++i)
        body += rec(i, "INFO ", "app.core", QByteArrayLiteral("record ") + QByteArray::number(i));

    // Build the compressed container on disk, then hand its bytes to the fake server.
    const QString staging = path(QStringLiteral("staging.log.gz"));
    QVERIFY(writeGzip(staging, body));
    QFile packed(staging);
    QVERIFY(packed.open(QIODevice::ReadOnly));
    const QByteArray compressed = packed.readAll();
    packed.close();

    const QString url = QStringLiteral("ssh://deploy@web1/var/log/app.log.1.gz");
    FakeRemoteFarm farm;
    farm.at(url)->setInitialContent(compressed);

    Document doc;
    QVERIFY2(openDoc(doc, url), qPrintable(doc.lastError()));

    // A single-stream container collapses to its own address, so the expanded log and
    // the raw container are the same string. They must still be two spools: sharing a
    // key made the expansion resolve its own input to itself and recurse.
    QCOMPARE(doc.path(), QStringLiteral("ssh://deploy@web1:22/var/log/app.log.1.gz"));

    LogModel model(&doc);
    LiveController live(&doc, &model);
    wireResume(live, doc, model);
    live.start();
    QVERIFY(pumpToCompletion(doc, live));
    live.checkNow();

    QCOMPARE(doc.index().records.size(), 3000);
    QCOMPARE(doc.messageText(doc.index().records.at(2999)), QStringLiteral("record 2999"));
}

QTEST_GUILESS_MAIN(TestArchiveTail)
#include "tst_archivetail.moc"
