// Throwaway performance harness for the M2a spine (PLAN.md M2: "Measure against
// ARCHITECTURE.md §11 here"). Console-only, links loftail_core (no Widgets), so
// it measures the index/model path in isolation:
//   * indexing throughput (target: >= 100 MB/s single-threaded on a warm file)
//   * block prefix-sum lookup cost (target: rebuild < 20 ms / 1M records)
//   * per-frame paint cost: resolve a scroll line -> record and pull the visible
//     cells via LogModel::data() (target: 60 fps => < 16.6 ms/frame)
//
// Usage: bench_index <file> "<pattern>"
#include "Decoder.h"
#include "Document.h"
#include "Indexer.h"
#include "LogModel.h"
#include "LogSource.h"
#include "RecordIndex.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTextStream>

using namespace loftail;

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    if (argc < 2) {
        out << "usage: bench_index <file> [pattern]\n";
        return 2;
    }
    const QString path = QString::fromLocal8Bit(argv[1]);
    const QString pattern = argc >= 3
        ? QString::fromLocal8Bit(argv[2])
        : QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n");

    // --- indexing throughput -------------------------------------------------
    Document doc;
    QElapsedTimer timer;
    timer.start();
    if (!doc.open(path, pattern, Encoding::Auto)) {
        out << "open failed: " << doc.lastError() << "\n";
        return 1;
    }
    const qint64 indexMs = timer.elapsed();

    const qint64 sizeBytes = doc.source()->size();
    const RecordIndex &idx = doc.index();
    const int records = idx.records.size();
    const double mb = double(sizeBytes) / (1024.0 * 1024.0);
    const double mbPerSec = indexMs > 0 ? mb / (double(indexMs) / 1000.0) : 0.0;

    out << "file          : " << path << "\n";
    out << Qt::fixed << qSetRealNumberPrecision(1);
    out << "size          : " << mb << " MB\n";
    out << "records       : " << records << "\n";
    out << "loggers/threads: " << idx.loggers.count() - 1 << " / " << idx.threads.count() - 1 << "\n";
    out << "total lines   : " << idx.totalLines() << "\n";
    out << "index time    : " << indexMs << " ms\n";
    out << "THROUGHPUT    : " << mbPerSec << " MB/s   (target >= 100)\n";
    out << "----\n";

    if (records == 0)
        return 0;

    // --- prefix-sum rebuild --------------------------------------------------
    timer.restart();
    const int rebuilds = 20;
    RecordIndex &mut = doc.index();
    for (int i = 0; i < rebuilds; ++i)
        mut.rebuildBlockSums();
    const double rebuildMsPerM =
        (double(timer.nsecsElapsed()) / 1e6 / rebuilds) / (double(records) / 1e6);
    out << Qt::fixed << qSetRealNumberPrecision(2);
    out << "prefix rebuild: " << rebuildMsPerM << " ms / 1M records   (target < 20)\n";

    // --- line -> record lookups ----------------------------------------------
    const qint64 lines = idx.totalLines();
    const int lookups = 2'000'000;
    auto *rng = QRandomGenerator::global();
    timer.restart();
    quint64 sink = 0;
    for (int i = 0; i < lookups; ++i)
        sink += quint64(idx.recordAtLine(qint64(rng->bounded(quint32(lines)))));
    const double nsPerLookup = double(timer.nsecsElapsed()) / lookups;
    out << "line->record  : " << nsPerLookup << " ns/lookup (" << lookups << " random lookups)\n";
    out << "----\n";

    // --- simulated paint frames ----------------------------------------------
    // Each "frame" resolves a random scroll position to a record and pulls the
    // cells for ~50 visible rows across all columns through LogModel::data().
    LogModel model(&doc);
    const int columns = model.columnCount();
    const int rowsPerFrame = 50;
    const int frames = 2000;
    timer.restart();
    quint64 charSink = 0;
    for (int fr = 0; fr < frames; ++fr) {
        const qint64 topLine = qint64(rng->bounded(quint32(lines)));
        int r = idx.recordAtLine(topLine);
        for (int row = 0; row < rowsPerFrame && r < records; ++row, ++r)
            for (int c = 0; c < columns; ++c)
                charSink += quint64(model.cellText(r, c).size());
    }
    const double msPerFrame = double(timer.nsecsElapsed()) / 1e6 / frames;
    const double fps = msPerFrame > 0 ? 1000.0 / msPerFrame : 0.0;
    out << "paint frame   : " << msPerFrame << " ms/frame (" << rowsPerFrame << " rows x "
        << columns << " cols)\n";
    out << "               ~" << fps << " fps   (target >= 60 => < 16.6 ms/frame)\n";

    Q_UNUSED(sink);
    Q_UNUSED(charSink);
    return 0;
}
