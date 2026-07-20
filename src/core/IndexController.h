#pragma once

#include "Record.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>

QT_BEGIN_NAMESPACE
class QThread;
QT_END_NAMESPACE

namespace loftail {

class Document;
class LogModel;

// One batch of scanned records handed from the worker thread to the GUI thread
// (ARCHITECTURE.md §7.2: "The model is updated on the GUI thread in batches via
// queued signals — batching matters, since per-record signals on a fast scan will
// drown the event loop"). Carries the newly-scanned records plus any interned
// names first seen since the previous batch, in id order, so the GUI-side intern
// tables end up identical to the worker's — the record ids need no remapping.
struct IndexBatch
{
    QVector<Record>  records;
    QVector<QString> newLoggers; // logger names first seen this batch, in id order
    QVector<QString> newThreads; // thread names first seen this batch, in id order
    bool             final = false;
};

// Runs the single-pass Indexer (invariant #9) on a background thread and streams
// IndexBatches back to the GUI thread. Owned by IndexController and moved onto its
// own QThread. Reads the LogSource concurrently with GUI paint reads; that is safe
// for the mmap source (a read-only mapping, no remap during a fixed-size scan).
class IndexWorker : public QObject
{
    Q_OBJECT

public:
    explicit IndexWorker(Document *document);

    // Request cancellation. Safe to call from the GUI thread while the worker runs;
    // the scan checks the flag at each chunk and stops, flushing a final batch.
    void requestCancel() { m_cancel.store(true); }

public slots:
    void run(); // invoked once when the worker's thread starts

signals:
    void batchReady(const loftail::IndexBatch &batch);
    void progress(qint64 bytesProcessed, qint64 totalBytes);
    void finished(bool cancelled);

private:
    Document         *m_document;
    std::atomic<bool> m_cancel{false};
};

// GUI-thread orchestrator: spins up an IndexWorker on a QThread, receives batches,
// and applies them to the LogModel with begin/endInsertRows around each append so
// the view populates during the scan (§7.2). Progress and cancellation are wired
// through. Nothing here touches per-file state except through the Document it was
// given (invariant #7).
class IndexController : public QObject
{
    Q_OBJECT

public:
    IndexController(Document *document, LogModel *model, QObject *parent = nullptr);
    ~IndexController() override;

    // Begin indexing. The Document must already be prepare()'d (source open, format
    // compiled). Safe to call once per controller.
    void start();

    // Request cancellation; the model keeps whatever was scanned so far (§3).
    void cancel();

    bool isRunning() const { return m_running; }

signals:
    void progress(qint64 bytesProcessed, qint64 totalBytes);
    void finished(bool cancelled); // scan reached EOF or was cancelled

private slots:
    void onBatch(const loftail::IndexBatch &batch);
    void onFinished(bool cancelled);

private:
    Document    *m_document;
    LogModel    *m_model;
    QThread     *m_thread = nullptr;
    IndexWorker *m_worker = nullptr;
    bool         m_running = false;
};

} // namespace loftail

Q_DECLARE_METATYPE(loftail::IndexBatch)
