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
#include <QSignalSpy>
#include <QTemporaryFile>

#include "Document.h"
#include "FakeFetcher.h"
#include "LiveController.h"
#include "LogModel.h"
#include "LogSource.h"
#include "MemoryLogSource.h"
#include "RecordIndex.h"

using namespace loftail;

// M12 — a source that can prove its stream is FINISHED, and what the live controller
// does about it (ARCHITECTURE.md §6.4, SPEC.md §3).
//
// UNGATED, and that is the point: LogSource::isComplete() and the ordering rule around
// it are a contract of the live seam, not of libarchive. They are driven here through
// the fake fetcher, so they are covered in a build with no codec linked at all — and
// the archive fetcher that really produces the state is tested separately.
//
// The ordering is the whole risk. The fetcher publishes its final committedSize BEFORE
// the Complete state; the controller reads isComplete() BEFORE refreshing and acts on
// it AFTER ingesting. Break either half and the last chunk is silently lost — no error,
// no warning, just records that never appear.
class TestComplete : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
    static constexpr auto kUrl = "ssh://deploy@web1/var/log/app.log";

    static QString url() { return QString::fromLatin1(kUrl); }

    static QByteArray rec(int sec, const QByteArray &msg)
    {
        QByteArray out = "2026-08-05 00:00:";
        out += QByteArray::number(sec).rightJustified(2, '0');
        out += ",000 [main] INFO  app.core - ";
        out += msg;
        out += '\n';
        return out;
    }

    static bool openDoc(Document &doc)
    {
        return doc.open(url(), QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc());
    }

private slots:
    void anOrdinarySourceIsNeverComplete();
    void everyByteCommittedBeforeCompletionIsIngested();
    void watchingStopsExactlyOnceAndStaysStopped();
    void completionIsNotReportedWhileBytesAreStillArriving();
};

void TestComplete::anOrdinarySourceIsNeverComplete()
{
    // The default is "no", and it is the honest answer for anything loftail did not
    // produce itself: a file somebody else is writing cannot be proven finished, which
    // is the guess invariant #5 exists to forbid.
    MemoryLogSource memory(QByteArrayLiteral("one\ntwo\n"));
    QVERIFY(!memory.isComplete());

    QTemporaryFile file;
    QVERIFY(file.open());
    file.write(rec(1, "hello"));
    file.flush();
    const auto local = openLogSource(file.fileName());
    QVERIFY(local);
    QVERIFY(!local->isComplete());

    // Still false after it stops growing, and after it is fully read: "not growing
    // right now" is not the same claim and must not be confused with it.
    local->refreshSize();
    QVERIFY(!local->isComplete());
}

void TestComplete::everyByteCommittedBeforeCompletionIsIngested()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(url());
    remote->setInitialContent(rec(1, "first"));

    Document doc;
    QVERIFY2(openDoc(doc), qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.size(), 1);

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();

    // The worst case for the ordering, written deliberately: the final bytes land on
    // disk unpublished, then the fetcher publishes size-then-state in one step. A
    // controller that read isComplete() after refreshing would stop here having never
    // seen them.
    remote->appendWithheld(rec(2, "second") + rec(3, "third"));
    remote->markComplete();

    QSignalSpy completed(&live, &LiveController::completed);
    live.checkNow();

    QCOMPARE(doc.index().records.size(), 3);
    QCOMPARE(doc.messageText(doc.index().records.at(2)), QStringLiteral("third"));
    QCOMPARE(completed.count(), 1);
    QVERIFY(doc.source()->isComplete());
}

void TestComplete::watchingStopsExactlyOnceAndStaysStopped()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(url());
    remote->setInitialContent(rec(1, "only"));

    Document doc;
    QVERIFY2(openDoc(doc), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();

    QSignalSpy completed(&live, &LiveController::completed);
    remote->markComplete();

    live.checkNow();
    QCOMPARE(completed.count(), 1);

    // Every later tick is a no-op. Firing again would be a repeated event for something
    // that happened once, and anything wired to it would act twice.
    live.checkNow();
    live.checkNow();
    QCOMPARE(completed.count(), 1);
    QCOMPARE(doc.index().records.size(), 1);
}

void TestComplete::completionIsNotReportedWhileBytesAreStillArriving()
{
    FakeRemoteFarm farm;
    auto remote = farm.at(url());
    remote->setInitialContent(rec(1, "first"));

    Document doc;
    QVERIFY2(openDoc(doc), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();

    QSignalSpy completed(&live, &LiveController::completed);

    // An ordinary tail: growing, not finished. Nothing about M12 may make a still-live
    // source look complete — that would stop the watch on a log still being written.
    remote->append(rec(2, "second"));
    live.checkNow();
    QCOMPARE(doc.index().records.size(), 2);
    QCOMPARE(completed.count(), 0);
    QVERIFY(!doc.source()->isComplete());

    remote->append(rec(3, "third"));
    live.checkNow();
    QCOMPARE(doc.index().records.size(), 3);
    QCOMPARE(completed.count(), 0);

    // Only now, and only then does watching stop.
    remote->markComplete();
    live.checkNow();
    QCOMPARE(completed.count(), 1);
}

QTEST_GUILESS_MAIN(TestComplete)
#include "tst_complete.moc"
