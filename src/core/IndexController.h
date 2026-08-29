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
// own QThread.
//
// IT READS THE LogSource CONCURRENTLY WITH GUI PAINT READS, and that is safe for EVERY
// source rather than only for the mapped one — which is what this comment used to say,
// and the gap it left is bugs.md 25. It is safe for the mapping because the mapping is
// read-only and is not remapped during a fixed-size scan (the live watch does not start
// until onIndexFinished). It is safe for the BUFFERED source, which has to read to
// answer, because LogSource::bytes() fills storage the CALLER supplies: this worker's
// chunk is a local of Indexer::index(), the paint path's is a local of
// LogModel::data(), and the handle underneath them is read positionally, so neither the
// buffer nor the file position is shared. Give the source a read buffer of its own again
// and the two threads free each other's bytes mid-read, silently.
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
