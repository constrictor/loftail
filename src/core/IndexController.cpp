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

#include "IndexController.h"

#include "Document.h"
#include "Indexer.h"
#include "LogModel.h"
#include "LogSource.h"
#include "RecordIndex.h"

#include <QThread>

namespace loftail {

IndexWorker::IndexWorker(Document *document) : m_document(document) {}

void IndexWorker::run()
{
    LogSource *source = m_document->source();
    if (!source) {
        // Nothing to scan. Since M13 this is the ordinary case rather than a broken
        // one: a document WAITING for a log that has not been written yet has no
        // source, and finishing immediately is what lets it flow through the owner's
        // normal post-index path and get its LiveController — which is the thing that
        // will eventually bring the log in (§6.5).
        emit finished(false);
        return;
    }

    Indexer indexer(m_document->format(), m_document->decoder(), m_document->sourceZone());

    // Track how much has already been streamed so each batch carries only the new
    // records and the intern names first seen since the previous batch.
    int emittedRecords = 0;
    int emittedLoggers = 1; // id 0 == "" is reserved in both intern tables
    int emittedThreads = 1;

    auto flush = [&](const RecordIndex &idx, bool final) {
        // On a non-final flush the last record is still open for continuations, so
        // hold it back (§4); on the final flush everything is settled.
        const int upto = final ? int(idx.records.size())
                               : qMax(emittedRecords, int(idx.records.size()) - 1);

        IndexBatch batch;
        batch.final = final;

        for (int i = emittedLoggers; i < idx.loggers.count(); ++i)
            batch.newLoggers.push_back(idx.loggers.name(i));
        emittedLoggers = idx.loggers.count();

        for (int i = emittedThreads; i < idx.threads.count(); ++i)
            batch.newThreads.push_back(idx.threads.name(i));
        emittedThreads = idx.threads.count();

        if (upto > emittedRecords) {
            batch.records.reserve(upto - emittedRecords);
            for (int i = emittedRecords; i < upto; ++i)
                batch.records.push_back(idx.records.at(i));
            emittedRecords = upto;
        }

        if (!batch.records.isEmpty() || !batch.newLoggers.isEmpty()
            || !batch.newThreads.isEmpty() || final) {
            emit batchReady(batch);
        }
    };

    // Follow a source that grew while it was being scanned, for as long as each pass
    // finds a worthwhile amount more (kFollowMinBytes). The first pass always asks: it is
    // the one a spool's fetcher is most likely to have outrun — a document resumed from
    // waiting starts on its prime, a fraction of what has expanded a few ms later. A
    // small pass stops WITHOUT asking again, which is what leaves the source latched at
    // exactly the size the index covers: the live watch then takes its baseline from
    // there and misses nothing.
    qint64 lastSize = -1;
    const Indexer::Grow grow = [this, &lastSize](qint64 scannedTo) -> qint64 {
        const bool smallPass = lastSize >= 0 && scannedTo - lastSize < kFollowMinBytes;
        lastSize = scannedTo;
        if (smallPass || m_cancel.load())
            return scannedTo;
        return awaitSize(scannedTo);
    };

    bool cancelled = false;
    indexer.index(*source,
                  [this, source](qint64 done, qint64 total) {
                      emit progress(done, total);
                      Q_UNUSED(source);
                      return !m_cancel.load();
                  },
                  &cancelled, flush, grow);

    emit finished(cancelled);
}

void IndexWorker::requestCancel()
{
    m_cancel.store(true);
    {
        // Empty on purpose: taking the mutex orders the store above against a worker
        // that has tested the predicate and not yet blocked, which would otherwise miss
        // this notify and wait for an answer nobody is going to give.
        const std::lock_guard<std::mutex> lock(m_sizeMutex);
    }
    m_sizeReady.notify_all();
}

void IndexWorker::provideSize(qint64 size)
{
    {
        const std::lock_guard<std::mutex> lock(m_sizeMutex);
        m_answer = size;
    }
    m_sizeReady.notify_all();
}

qint64 IndexWorker::awaitSize(qint64 scannedTo)
{
    std::unique_lock<std::mutex> lock(m_sizeMutex);
    m_answer = -1;
    lock.unlock();
    emit sizeNeeded(scannedTo); // queued to the controller on the GUI thread
    lock.lock();
    m_sizeReady.wait(lock, [this] { return m_answer >= 0 || m_cancel.load(); });
    return m_cancel.load() ? scannedTo : m_answer;
}

IndexController::IndexController(Document *document, LogModel *model, QObject *parent)
    : QObject(parent), m_document(document), m_model(model)
{
    // Register once so IndexBatch can cross the queued (thread) connection.
    static const int metaId = qRegisterMetaType<loftail::IndexBatch>("loftail::IndexBatch");
    Q_UNUSED(metaId);
}

IndexController::~IndexController()
{
    if (m_thread) {
        if (m_worker)
            m_worker->requestCancel();
        m_thread->quit();
        m_thread->wait();
    }
}

void IndexController::start()
{
    if (m_running)
        return;
    m_running = true;

    m_thread = new QThread(this);
    m_worker = new IndexWorker(m_document);
    m_worker->moveToThread(m_thread);

    connect(m_thread, &QThread::started, m_worker, &IndexWorker::run);
    connect(m_worker, &IndexWorker::batchReady, this, &IndexController::onBatch);
    connect(m_worker, &IndexWorker::progress, this, &IndexController::progress);
    connect(m_worker, &IndexWorker::sizeNeeded, this, &IndexController::onSizeNeeded);
    connect(m_worker, &IndexWorker::finished, this, &IndexController::onFinished);
    // Tear the thread down once the worker reports completion.
    connect(m_worker, &IndexWorker::finished, m_thread, &QThread::quit);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_thread->start();
}

void IndexController::cancel()
{
    m_cancelRequested = true;
    if (m_worker)
        m_worker->requestCancel();
}

void IndexController::onBatch(const IndexBatch &batch)
{
    RecordIndex &idx = m_document->index();

    // Append new intern names first, in id order, so the GUI-side ids line up with
    // the worker's ids the records already reference.
    for (const QString &name : batch.newLoggers)
        idx.loggers.intern(name);
    for (const QString &name : batch.newThreads)
        idx.threads.intern(name);

    // The NAMES go in now and the RECORDS do not. Interning immediately is what keeps
    // the GUI-side ids identical to the worker's, which the held records already
    // reference; a name is not a row, so nothing on screen moves for it.
    m_pending.append(batch.records);
}

// Hand the whole scan to the model at once, BEFORE the signal: MainWindow::
// onIndexFinished() detects runs, applies filters, seeds the columns from the complete
// intern tables and scrolls to the end, all of which read the index this publishes.
//
// It runs on the CANCELLED path too, and must: the worker flushes a final batch when it
// stops, and "scanning can be cancelled, leaving whatever was scanned so far usable" is
// SPEC.md §3's promise about exactly these records.
void IndexController::onSizeNeeded(qint64 scannedTo)
{
    if (!m_worker || m_cancelRequested)
        return;
    // LiveController's order, and for its reason: a source whose origin was REPLACED must
    // not be refreshed here, because refreshSize() is what adopts the new generation or
    // file, and the offsets this scan holds name bytes in the old one. Answering with the
    // size already scanned ends the scan; the live watch then sees the rotation and
    // rescans, exactly as it would have without this. A shrink is the source's to latch
    // (wasTruncated() is sticky), so the watch learns of that one too.
    LogSource *src = m_document->source();
    qint64 size = scannedTo;
    if (src && !src->wasReplaced())
        size = src->refreshSize();
    m_worker->provideSize(size);
}

void IndexController::onFinished(bool cancelled)
{
    m_running = false;
    m_worker = nullptr; // deleteLater is wired on thread finish

    if (!m_pending.isEmpty()) {
        RecordIndex &idx = m_document->index();
        m_model->beginAppendRows(int(m_pending.size()));
        idx.records.append(m_pending);
        idx.rebuildBlockSums(); // one linear pass, once per scan (§7.2, §11)
        m_model->endAppendRows();
        m_pending.clear();
        m_pending.squeeze();
    }

    emit finished(cancelled);
}

} // namespace loftail
