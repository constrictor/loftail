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

#include <QByteArray>

#include "Document.h"
#include "FakeFetcher.h"
#include "FilteredIndex.h"
#include "Highlight.h"
#include "LiveController.h"
#include "LogModel.h"
#include "LogSource.h"
#include "Palette.h"
#include "Priority.h"
#include "RecordIndex.h"

#include <cstring>

using namespace loftail;

// M11 — `tail -f` over a remote log (SPEC.md §3, ARCHITECTURE.md §6.3). This is
// tst_tail's matrix run again against a SpooledLogSource: append (including a
// multi-line record split across two commits), truncation, rotation, and filtered/
// highlighted append, all driven synchronously through LiveController::checkNow().
//
// It is the proof that the promise "live updates work exactly as they do locally"
// is true rather than merely intended — and it needs no server, because the
// transport is a fake the test drives (tests/FakeFetcher.h). Two cases have no local
// counterpart: bytes written to the spool but not yet published must be invisible
// until they are, and a rescan mid-tail must not reconnect.
class TestRemoteTail : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
    static constexpr auto kUrl = "ssh://deploy@web1/var/log/app.log";

    static QString url() { return QString::fromLatin1(kUrl); }

    // A single complete record line (record-start line + trailing newline).
    static QByteArray rec(int sec, const char *thread, const char *prio, const char *logger,
                          const QByteArray &msg)
    {
        QByteArray out = "2026-07-21 00:00:";
        out += QByteArray::number(sec).rightJustified(2, '0');
        out += ",000 [";
        out += thread;
        out += "] ";
        out += prio;
        out += "  ";
        out += logger;
        out += " - ";
        out += msg;
        out += '\n';
        return out;
    }

    // A continuation line: does NOT match recordStartRe, so it attaches to the
    // preceding record (invariant #2 multi-line record).
    static QByteArray cont(const QByteArray &text) { return text + "\n"; }

    static bool openDoc(Document &doc)
    {
        return doc.open(url(), QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc());
    }

    // The convergence assertion: an incrementally-built index must be identical to a
    // one-shot scan of the same final bytes.
    static bool sameIndex(const RecordIndex &a, const RecordIndex &b, QString *why)
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

private slots:
    void opensAndIndexesARemoteLog();
    void appendConvergesWithSplitMultiline();
    void withheldBytesAreNotIngestedUntilCommitted();
    void truncationTriggersRescan();
    void rotationTriggersReindex();
    void rotationOnASlowLinkDoesNotBlankTheTab();
    void rescanDuringTailDoesNotReconnect();
    void filteredAndHighlightedAppend();
    void openFailureLeavesAUsableError();
};

void TestRemoteTail::opensAndIndexesARemoteLog()
{
    FakeRemoteFarm farm;
    QByteArray whole;
    whole += rec(1, "t0", "INFO ", "logger.a", "first");
    whole += rec(2, "t1", "WARN ", "logger.b", "second");
    farm.at(url())->setInitialContent(whole);

    Document doc;
    QVERIFY2(openDoc(doc), qPrintable(doc.lastError()));
    QCOMPARE(doc.path(), RemoteLocation::normalize(url()));
    QCOMPARE(doc.index().records.size(), 2);
    QCOMPARE(doc.index().records.at(1).priorityEnum(), Priority::Warn);
    // The record index holds byte offsets into the spool, exactly as it holds offsets
    // into a local file — nothing downstream can tell the difference (invariant #1).
    QCOMPARE(doc.messageText(doc.index().records.at(0)), QStringLiteral("first"));
}

void TestRemoteTail::appendConvergesWithSplitMultiline()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(url());

    QByteArray whole;
    whole += rec(1, "t0", "INFO ", "logger.a", "first");
    whole += rec(2, "t1", "WARN ", "logger.b", "second");
    remote->setInitialContent(whole);

    Document doc;
    QVERIFY2(openDoc(doc), qPrintable(doc.lastError()));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(model.rowCount(), 2);

    // Chunk 1: a complete record, then a multi-line record left OPEN — the
    // provisional trailing record.
    QByteArray chunk1;
    chunk1 += rec(3, "t0", "ERROR", "logger.a", "third");
    chunk1 += rec(4, "t2", "INFO ", "logger.d", "line0");
    chunk1 += cont("    continuation one");
    remote->append(chunk1);
    live.checkNow();

    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(doc.index().records.at(3).lineCount, quint16(2));

    // Chunk 2: another continuation (the provisional record grows), then a new record.
    QByteArray chunk2;
    chunk2 += cont("    continuation two");
    chunk2 += rec(5, "t1", "INFO ", "logger.e", "fifth");
    remote->append(chunk2);
    live.checkNow();

    QCOMPARE(model.rowCount(), 5);
    QCOMPARE(doc.index().records.at(3).lineCount, quint16(3));

    // Convergence with a one-shot scan of the same bytes. The reference Document
    // joins the SAME live spool, which is itself the sharing contract at work.
    Document reference;
    QVERIFY(openDoc(reference));
    QString why;
    QVERIFY2(sameIndex(doc.index(), reference.index(), &why), qPrintable(why));
    QCOMPARE(doc.index().totalLines(), reference.index().totalLines());
}

void TestRemoteTail::withheldBytesAreNotIngestedUntilCommitted()
{
    // No local counterpart: a fetcher thread is mid-write while the GUI thread ticks.
    // The committed-size clamp is what stands in for a lock, so the half-written
    // record must be invisible — not partially indexed, not indexed as garbage.
    FakeRemoteFarm farm;
    auto remote = farm.at(url());
    remote->setInitialContent(rec(1, "t0", "INFO ", "logger.a", "first"));

    Document doc;
    QVERIFY(openDoc(doc));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(model.rowCount(), 1);

    const QByteArray second = rec(2, "t1", "WARN ", "logger.b", "second");
    // Half a record on disk, nothing published.
    remote->appendWithheld(second.left(second.size() / 2));
    live.checkNow();
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(doc.index().records.at(0).lineCount, quint16(1));

    // The rest lands, and only now is any of it published.
    remote->appendWithheld(second.mid(second.size() / 2));
    live.checkNow();
    QCOMPARE(model.rowCount(), 1); // still nothing: publication is the gate

    remote->publish();
    live.checkNow();
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(doc.index().records.at(1).priorityEnum(), Priority::Warn);
    QCOMPARE(doc.messageText(doc.index().records.at(1)), QStringLiteral("second"));
}

void TestRemoteTail::truncationTriggersRescan()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(url());

    QByteArray whole;
    for (int i = 0; i < 6; ++i)
        whole += rec(i, "t0", "INFO ", "logger.a", QByteArray("m") + QByteArray::number(i));
    remote->setInitialContent(whole);

    Document doc;
    QVERIFY(openDoc(doc));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(model.rowCount(), 6);

    int rescans = 0;
    connect(&live, &LiveController::rescanned, &live, [&] { ++rescans; });

    // The remote writer truncated and rewrote (copytruncate). The fetcher answers with
    // a new spool generation, so the model reloads silently — SPEC.md §3, no notice.
    QByteArray fresh;
    fresh += rec(0, "t9", "FATAL", "logger.new", "after truncate a");
    fresh += rec(1, "t9", "FATAL", "logger.new", "after truncate b");
    remote->replaceWith(fresh);
    live.checkNow();

    QCOMPARE(rescans, 1);
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(doc.index().records.at(0).priorityEnum(), Priority::Fatal);

    Document reference;
    QVERIFY(openDoc(reference));
    QString why;
    QVERIFY2(sameIndex(doc.index(), reference.index(), &why), qPrintable(why));
}

void TestRemoteTail::rotationTriggersReindex()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(url());
    remote->setInitialContent(rec(1, "t0", "INFO ", "logger.old", "before rotate"));

    Document doc;
    QVERIFY(openDoc(doc));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(model.rowCount(), 1);

    int rescans = 0;
    connect(&live, &LiveController::rescanned, &live, [&] { ++rescans; });

    // A remote logrotate: the path now names a different file. Locally that is caught
    // by re-stat'ing the path; remotely by the generation the fetcher published.
    QByteArray rotated;
    rotated += rec(1, "t9", "INFO ", "logger.new", "after rotate a");
    rotated += rec(2, "t9", "ERROR", "logger.new", "after rotate b");
    remote->replaceWith(rotated);
    live.checkNow();

    QCOMPARE(rescans, 1);
    QCOMPARE(model.rowCount(), 2);
    QVERIFY(doc.index().loggers.names().contains(QStringLiteral("logger.new")));

    // Tailing continues on the new file, and appends after a rotation still ingest.
    remote->append(rec(3, "t9", "WARN ", "logger.new", "after rotate c"));
    live.checkNow();
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(rescans, 1); // the append was not mistaken for another rotation
}

void TestRemoteTail::rotationOnASlowLinkDoesNotBlankTheTab()
{
    // THE REGRESSION THE TWO-CALL-SITE RULE PREVENTS (LogSource.h, notReadyYet()).
    //
    // notReadyYet() and originVanished() both mean "nothing to read", and folding them
    // into one test in LiveController::checkNow() reads as obvious tidying. It is not:
    // after a rotation, wasReplaced() rescans onto the new generation, and the very next
    // tick finds it Priming with nothing committed yet. On a slow link that lasts long
    // enough for the two-second vanish grace to expire — and the view of a log that is
    // perfectly fine, and merely rotating, would be blanked into "no longer there".
    //
    // Without this test the merge would be reintroduced, look correct, and only misbehave
    // against a real remote host mid-logrotate.
    FakeRemoteFarm farm;
    auto remote = farm.at(url());
    remote->setInitialContent(rec(1, "t0", "INFO ", "logger.old", "before rotate"));

    Document doc;
    QVERIFY(openDoc(doc));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.setVanishGrace(0); // no grace at all: the strictest form of the question
    live.start();
    QCOMPARE(model.rowCount(), 1);

    int waits = 0;
    connect(&live, &LiveController::waitingChanged, &live,
            [&](bool waiting, const QString &) {
                if (waiting)
                    ++waits;
            });

    // The rotation has been noticed and the new file is not here yet.
    remote->beginReplacing(4096);
    live.checkNow(); // sees the new generation: rescan
    live.checkNow(); // and now: new generation adopted, still nothing committed
    live.checkNow();

    QCOMPARE(waits, 0);
    QVERIFY(!doc.isWaiting());

    QByteArray rotated;
    rotated += rec(1, "t9", "INFO ", "logger.new", "after rotate a");
    rotated += rec(2, "t9", "ERROR", "logger.new", "after rotate b");
    remote->finishReplacing(rotated);
    live.checkNow();

    QCOMPARE(waits, 0);
    QCOMPARE(model.rowCount(), 2);
}

void TestRemoteTail::rescanDuringTailDoesNotReconnect()
{
    // The reason the spool is shared rather than per-Document: a rotation mid-tail
    // calls Document::rescan() on the GUI thread, and a reconnect there would block
    // the UI and could prompt for a password behind the user's back.
    FakeRemoteFarm farm;
    auto remote = farm.at(url());
    remote->setInitialContent(rec(1, "t0", "INFO ", "logger.a", "first"));

    Document doc;
    QVERIFY(openDoc(doc));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(remote->startCount(), 1);

    remote->replaceWith(rec(1, "t0", "INFO ", "logger.b", "second"));
    live.checkNow();

    QCOMPARE(model.rowCount(), 1);
    // The whole point: still exactly one connection, after a full rescan.
    QCOMPARE(remote->startCount(), 1);
}

void TestRemoteTail::filteredAndHighlightedAppend()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(url());

    // logger.hot first appears only in the appended chunk, so the highlight rule is
    // inert until LiveController re-resolves it against the grown intern table.
    QByteArray whole;
    whole += rec(1, "t0", "INFO ", "logger.quiet", "info one");
    whole += rec(2, "t0", "ERROR", "logger.quiet", "error one");
    remote->setInitialContent(whole);

    Document doc;
    QVERIFY(openDoc(doc));

    doc.filters().priorityEnabled = true;
    doc.filters().minPriority = Priority::Warn;
    doc.applyFilters();

    HighlightRule rule;
    rule.match.loggerEnabled = true;
    rule.match.loggerNames = QStringList{QStringLiteral("logger.hot")};
    rule.background = 3;
    doc.highlighters().rules.append(rule);
    doc.resolveHighlighters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();

    QVERIFY(doc.filtered().active());
    QCOMPARE(model.rowCount(), 1); // only the initial ERROR is visible

    QByteArray chunk;
    chunk += rec(3, "t1", "INFO ", "logger.quiet", "info two");
    chunk += rec(4, "t1", "WARN ", "logger.hot", "warn hot");
    chunk += rec(5, "t1", "ERROR", "logger.quiet", "error two");
    remote->append(chunk);
    live.checkNow();

    // Two of the three appended records pass the filter → 1 + 2 = 3 visible.
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(doc.filtered().recordCount(), 3);
    QCOMPARE(doc.index().records.size(), 5);
    QCOMPARE(doc.filtered().sourceRow(0), 1);
    QCOMPARE(doc.filtered().sourceRow(1), 3);
    QCOMPARE(doc.filtered().sourceRow(2), 4);

    // Highlighting resolves for an APPENDED record whose subsystem was interned only
    // during the append — the paint path decodes it out of the spool.
    const QColor hot = model.highlightColor(1, /*background=*/true);
    QVERIFY(hot.isValid());
    QCOMPARE(hot, HighlightPalette::color(3, /*dark=*/false));
    QVERIFY(!model.highlightColor(2, /*background=*/true).isValid());
    QVERIFY(!model.highlightColor(0, /*background=*/true).isValid());

    Document reference;
    QVERIFY(openDoc(reference));
    QString why;
    QVERIFY2(sameIndex(doc.index(), reference.index(), &why), qPrintable(why));
}

void TestRemoteTail::openFailureLeavesAUsableError()
{
    FakeRemoteFarm farm;
    farm.at(url())->setStartFailure(QStringLiteral("web1: Connection timed out"));

    Document doc;
    QVERIFY(!openDoc(doc));
    // The transport's wording survives all the way to Document::lastError(), which is
    // what the status bar shows. "Cannot open ssh://..." alone would be useless.
    QCOMPARE(doc.lastError(), QStringLiteral("web1: Connection timed out"));
}

QTEST_GUILESS_MAIN(TestRemoteTail)
#include "tst_remotetail.moc"
