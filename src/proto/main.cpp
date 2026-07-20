// Throwaway M2a scrolling prototype (PLAN.md M2 risk note: "Build LogView against
// a real log FIRST"). Opens a file, indexes it synchronously, and shows the
// exact-geometry LogViewProto so scrolling can be validated by eye against a real
// multi-hundred-MB log. Not the product entry point — that is src/main.cpp.
//
// Usage: loftail_proto <file> ["<pattern>"]
#include "Document.h"
#include "LogModel.h"
#include "LogSource.h"
#include "LogViewProto.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QMainWindow>
#include <QScrollBar>
#include <QTimer>
#include <QStatusBar>

using namespace loftail;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    if (argc < 2) {
        qWarning("usage: loftail_proto <file> [pattern]");
        return 2;
    }

    const QString path = QString::fromLocal8Bit(argv[1]);
    const QString pattern = argc >= 3
        ? QString::fromLocal8Bit(argv[2])
        : QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n");

    // One Document, held in a one-element owner vector (invariant #7): even the
    // prototype must not reach for "the current file" globally.
    std::vector<std::unique_ptr<Document>> documents;
    documents.push_back(std::make_unique<Document>());
    Document *doc = documents.front().get();

    QElapsedTimer timer;
    timer.start();
    if (!doc->open(path, pattern)) {
        qWarning("open failed: %s", qPrintable(doc->lastError()));
        return 1;
    }
    const qint64 ms = timer.elapsed();

    auto *model = new LogModel(doc);
    auto *view = new LogViewProto(doc, model);
    model->setParent(view);

    QMainWindow window;
    window.setCentralWidget(view);
    window.resize(1100, 700);
    const double mb = double(doc->source()->size()) / (1024.0 * 1024.0);
    window.statusBar()->addWidget(new QLabel(
        QStringLiteral("%1  |  %2 records  |  %3 MB indexed in %4 ms (%5 MB/s)")
            .arg(path)
            .arg(doc->index().records.size())
            .arg(mb, 0, 'f', 1)
            .arg(ms)
            .arg(ms > 0 ? mb / (double(ms) / 1000.0) : 0.0, 0, 'f', 1)));

    // Start at the file's end, following (SPEC.md §3).
    view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
    window.show();

    // Optional headless screenshot for verification: LOFTAIL_SHOT=<png> grabs a
    // frame (scrolled to a middle position so multi-line records are visible) and
    // exits. Throwaway, like the rest of this prototype.
    const QByteArray shot = qgetenv("LOFTAIL_SHOT");
    if (!shot.isEmpty()) {
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum() / 2);
        QTimer::singleShot(200, &app, [&]() {
            window.grab().save(QString::fromLocal8Bit(shot));
            app.quit();
        });
    }
    return app.exec();
}
