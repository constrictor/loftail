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

#include <QSignalSpy>
#include <QTemporaryFile>

#include "Decoder.h"
#include "Document.h"
#include "IndexController.h"
#include "Indexer.h"
#include "LogFormat.h"
#include "LogModel.h"
#include "PatternCompiler.h"
#include "RecordIndex.h"

#include "MemoryLogSource.h"

using namespace loftail;

// Worker-thread batched indexing (M2b, ARCHITECTURE.md §7.2): the Indexer runs off
// the GUI thread and streams IndexBatches that IndexController applies to the
// LogModel with begin/endInsertRows. These tests assert the streamed result equals
// a synchronous index (no records lost, ids aligned), that inserts arrive in
// contiguous batches (no partial-batch corruption), and that the cancel seam works.
// GUILESS: a QCoreApplication event loop drives the queued cross-thread signals.
class TestIndexController : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";

    // A log large enough to span several read chunks, so the worker emits multiple
    // batches. A handful of loggers/threads exercises interning across batches.
    static QByteArray makeLog(int records)
    {
        QByteArray out;
        out.reserve(records * 70);
        for (int i = 0; i < records; ++i) {
            out += QByteArrayLiteral("2026-07-21 00:00:00,000 [t");
            out += QByteArray::number(i % 4);
            out += QByteArrayLiteral("] INFO  logger.");
            out += QByteArray::number(i % 5);
            out += QByteArrayLiteral(" - message body number ");
            out += QByteArray::number(i);
            out += '\n';
        }
        return out;
    }

    static bool writeLog(QTemporaryFile &file, const QByteArray &bytes)
    {
        if (!file.open())
            return false;
        file.write(bytes);
        file.flush();
        return true;
    }

private slots:
    void streamsEveryRecord();
    void insertsAreContiguousBatches();
    void indexerCancelSeamIsDeterministic();
    void controllerCancelStaysConsistent();
};

void TestIndexController::streamsEveryRecord()
{
    // ~7 MB => at least two 4 MB chunks => multiple batches.
    const QByteArray log = makeLog(100000);
    QTemporaryFile file;
    QVERIFY(writeLog(file, log));

    Document doc;
    QVERIFY2(doc.prepare(file.fileName(), QString::fromLatin1(kPattern), Encoding::Utf8,
                         QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QVERIFY(doc.source()->size() > Indexer::kChunkBytes); // guarantees >1 chunk

    LogModel model(&doc);
    IndexController controller(&doc, &model);
    QSignalSpy finishedSpy(&controller, &IndexController::finished);

    controller.start();
    QVERIFY(finishedSpy.wait(30000));
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toBool(), false); // not cancelled

    QCOMPARE(model.rowCount(), 100000);
    QCOMPARE(doc.index().records.size(), 100000);

    // Ids align with the names streamed alongside them: a spot check that the
    // GUI-side intern tables reconstructed the same mapping the worker used.
    const RecordIndex &idx = doc.index();
    QCOMPARE(idx.loggers.name(idx.records.at(3).loggerId), QStringLiteral("logger.3"));
    QCOMPARE(idx.threads.name(idx.records.at(2).threadId), QStringLiteral("t2"));
    QCOMPARE(idx.records.at(99999).priorityEnum(), Priority::Info);
    QVERIFY(idx.totalLines() >= idx.records.size());
}

void TestIndexController::insertsAreContiguousBatches()
{
    const QByteArray log = makeLog(60000);
    QTemporaryFile file;
    QVERIFY(writeLog(file, log));

    Document doc;
    QVERIFY(doc.prepare(file.fileName(), QString::fromLatin1(kPattern), Encoding::Utf8,
                        QTimeZone::utc()));

    LogModel model(&doc);
    IndexController controller(&doc, &model);

    int lastRowCount = 0;
    int batches = 0;
    bool contiguous = true;
    connect(&model, &QAbstractItemModel::rowsInserted, &model,
            [&](const QModelIndex &, int first, int last) {
                ++batches;
                if (first != lastRowCount) // each batch appends at the tail
                    contiguous = false;
                lastRowCount = last + 1;
            });

    QSignalSpy finishedSpy(&controller, &IndexController::finished);
    controller.start();
    QVERIFY(finishedSpy.wait(30000));

    QVERIFY(contiguous);
    QVERIFY(batches >= 1);
    QCOMPARE(lastRowCount, 60000);
    QCOMPARE(model.rowCount(), 60000);
}

void TestIndexController::indexerCancelSeamIsDeterministic()
{
    // The seam the worker relies on: a progress callback returning false stops the
    // scan, sets *cancelled, and still delivers a final batch flush.
    auto compiled = PatternCompiler::compile(QString::fromLatin1(kPattern));
    QVERIFY(compiled);
    const LogFormat format = compiled.value();

    const QByteArray log = makeLog(100000);
    MemoryLogSource src(log);
    Decoder dec = Decoder::detect(src.bytesCopy(0, 64 * 1024), Encoding::Utf8);
    Indexer indexer(format, dec, QTimeZone::utc());

    bool cancelled = false;
    bool sawFinalBatch = false;
    int calls = 0;
    const RecordIndex idx = indexer.index(
        src,
        [&](qint64, qint64) { ++calls; return false; }, // cancel on the first chunk
        &cancelled,
        [&](const RecordIndex &, bool final) { sawFinalBatch = sawFinalBatch || final; });

    QVERIFY(cancelled);
    QVERIFY(sawFinalBatch);           // remainder is flushed on cancel
    QVERIFY(idx.records.size() < 100000); // stopped early, partial index usable
    QVERIFY(idx.totalLines() >= 0);
}

void TestIndexController::controllerCancelStaysConsistent()
{
    const QByteArray log = makeLog(200000); // large, so cancel can land mid-scan
    QTemporaryFile file;
    QVERIFY(writeLog(file, log));

    Document doc;
    QVERIFY(doc.prepare(file.fileName(), QString::fromLatin1(kPattern), Encoding::Utf8,
                        QTimeZone::utc()));

    LogModel model(&doc);
    IndexController controller(&doc, &model);

    // Cancel as soon as the first progress arrives.
    connect(&controller, &IndexController::progress, &controller,
            [&](qint64, qint64) { controller.cancel(); });

    QSignalSpy finishedSpy(&controller, &IndexController::finished);
    controller.start();
    QVERIFY(finishedSpy.wait(30000));
    QCOMPARE(finishedSpy.count(), 1);

    // Whether or not the cancel raced the final chunk, the model and index must
    // agree exactly and never exceed the full record count (no corruption).
    QCOMPARE(model.rowCount(), doc.index().records.size());
    QVERIFY(model.rowCount() <= 200000);
}

QTEST_GUILESS_MAIN(TestIndexController)
#include "tst_indexcontroller.moc"
