#include "LiveController.h"

#include "Decoder.h"
#include "Document.h"
#include "FilteredIndex.h"
#include "Indexer.h"
#include "LogModel.h"
#include "LogSource.h"
#include "RecordIndex.h"

#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>

#include <cstring>

namespace loftail {

// ---------------------------------------------------------------------------
// LiveWatcher
// ---------------------------------------------------------------------------

LiveWatcher::LiveWatcher(QObject *parent) : QObject(parent)
{
    m_fsw = new QFileSystemWatcher(this);
    m_poll = new QTimer(this);
    m_poll->setInterval(750);

    // A file change, or a directory change (create/rename/replace at the path),
    // both mean "re-check". On a directory change also re-add the file path, since
    // the watcher drops it when the file is rotated out.
    connect(m_fsw, &QFileSystemWatcher::fileChanged, this, [this] { emit maybeChanged(); });
    connect(m_fsw, &QFileSystemWatcher::directoryChanged, this, [this] {
        ensureWatched();
        emit maybeChanged();
    });
    // The poll is the network-mount fallback (the watcher is unreliable there).
    connect(m_poll, &QTimer::timeout, this, [this] {
        ensureWatched();
        emit maybeChanged();
    });
}

LiveWatcher::~LiveWatcher() = default;

void LiveWatcher::watch(const QString &path)
{
    stop();
    m_path = path;
    m_dir = QFileInfo(path).absolutePath();
    if (!m_dir.isEmpty() && QDir(m_dir).exists())
        m_fsw->addPath(m_dir);
    ensureWatched();
    m_poll->start();
}

void LiveWatcher::stop()
{
    m_poll->stop();
    const QStringList files = m_fsw->files();
    const QStringList dirs = m_fsw->directories();
    if (!files.isEmpty())
        m_fsw->removePaths(files);
    if (!dirs.isEmpty())
        m_fsw->removePaths(dirs);
}

void LiveWatcher::setPollInterval(int ms)
{
    m_poll->setInterval(ms);
}

void LiveWatcher::ensureWatched()
{
    if (m_path.isEmpty())
        return;
    if (m_fsw->files().contains(m_path))
        return;
    if (QFileInfo::exists(m_path))
        m_fsw->addPath(m_path);
}

// ---------------------------------------------------------------------------
// LiveController
// ---------------------------------------------------------------------------

LiveController::LiveController(Document *document, LogModel *model, QObject *parent)
    : QObject(parent), m_document(document), m_model(model)
{
    m_watcher = new LiveWatcher(this);
    connect(m_watcher, &LiveWatcher::maybeChanged, this, &LiveController::checkNow);
}

LiveController::~LiveController() = default;

void LiveController::setPollInterval(int ms)
{
    m_watcher->setPollInterval(ms);
}

void LiveController::syncBaseline()
{
    LogSource *src = m_document ? m_document->source() : nullptr;
    m_lastSize = src ? src->size() : 0;
    m_lastIdentity = src ? src->identity() : 0;
}

void LiveController::start()
{
    if (m_started || !m_document || !m_document->source())
        return;
    m_started = true;
    syncBaseline();
    m_watcher->watch(m_document->path());
}

void LiveController::stop()
{
    m_started = false;
    if (m_watcher)
        m_watcher->stop();
}

void LiveController::checkNow()
{
    LogSource *src = m_document ? m_document->source() : nullptr;
    if (!src) {
        // The source is gone (a previous rescan hit a moment when the path did not
        // exist — the gap between a rotate's rename and recreate). If the path is
        // back, reload it silently; otherwise wait for the next tick.
        if (m_document && pathIdentity(m_document->path()) != 0)
            doRescan();
        return;
    }

    // Rotation-by-replace: the file now at the path is a DIFFERENT inode than the
    // one our source holds (rename + recreate). Our mapped fd still follows the old
    // inode, so we must stat the path to see this (invariant #5, §6).
    const quint64 pathId = pathIdentity(m_document->path());
    const bool replaced = pathId != 0 && m_lastIdentity != 0 && pathId != m_lastIdentity;

    // Refresh the source size (re-maps so reads cannot run past the live EOF). This
    // also latches truncation when the file shrank below what we indexed.
    const qint64 newSize = src->refreshSize();
    const bool truncated = src->wasTruncated() || newSize < m_lastSize;

    if (replaced || truncated) {
        doRescan();
        return;
    }

    if (newSize > m_lastSize)
        ingestAppended();
    else
        m_lastSize = newSize; // no growth (or a spurious watcher tick)
}

void LiveController::doRescan()
{
    // Silent reload (SPEC.md §3: no dialog, no notice). A full model reset is the
    // right signal — the visible set is wholesale replaced — with the re-index and
    // any active-filter recompute done between begin/end so rowCount() is consistent.
    m_model->beginFilterReset();
    const bool ok = m_document->rescan();
    if (ok) {
        // The intern tables were rebuilt, so rebind highlight rules to the new ids
        // and re-materialize the visible subset if a filter is active.
        m_document->resolveHighlighters();
        if (m_document->filters().anyActive())
            m_document->applyFilters();
    }
    m_model->endFilterReset();

    syncBaseline();
    emit rescanned();
}

void LiveController::ingestAppended()
{
    RecordIndex &idx = m_document->index();
    LogSource *src = m_document->source();
    const Decoder &dec = m_document->decoder();
    FilteredIndex &filtered = m_document->filtered();
    const bool filterActive = filtered.active();

    const int oldCount = idx.records.size();
    const qint64 startOffset = oldCount > 0 ? idx.records.last().offset : dec.bomLength();
    Record oldLast{};
    if (oldCount > 0)
        oldLast = idx.records.last();

    Indexer indexer(m_document->format(), dec, m_document->sourceZone());
    const QVector<Record> tail = indexer.scanAppendedTail(*src, startOffset, idx.loggers, idx.threads);

    if (tail.isEmpty()) {
        // Growth we could not turn into a record yet (e.g. a partial trailing line
        // with no terminator). Leave m_lastSize where it is so the NEXT tick, once
        // more bytes have arrived, re-reads this same region and completes it.
        return;
    }

    // tail[0] re-reads the previous provisional record (same start offset); it may
    // have grown or flipped unparsed->parsed. tail[1..] are brand new.
    const bool provisionalChanged =
        oldCount > 0 && std::memcmp(&tail[0], &oldLast, sizeof(Record)) != 0;
    const int base = oldCount > 0 ? oldCount - 1 : 0; // source row of tail[0]
    const int firstTailNew = oldCount > 0 ? 1 : 0;    // index of the first brand-new tail record

    if (!filterActive) {
        // ---- Unfiltered (identity view): view rows == source rows --------------
        if (provisionalChanged)
            idx.records[base] = tail[0]; // in-place height/flip update of the tail

        const int newCount = tail.size() - firstTailNew;

        if (newCount > 0) {
            m_model->beginAppendRows(newCount); // first == idx.records.size() (oldCount)
            idx.records.reserve(idx.records.size() + newCount);
            for (int j = firstTailNew; j < tail.size(); ++j)
                idx.records.append(tail[j]);
            // Sums were valid for oldCount; if the tail record changed we must also
            // recompute from it, so extend from (base) in that case, else oldCount.
            idx.extendBlockSums(provisionalChanged ? base : oldCount);
            m_model->endAppendRows();
        } else if (provisionalChanged) {
            idx.extendBlockSums(base);
        }

        // A grown trailing record changed height with no new rows to trigger the
        // view's geometry refresh — signal it explicitly.
        if (provisionalChanged)
            m_model->notifyRowChanged(base);
    } else {
        // ---- Filtered: extend the visible subset in place (invariant #1) --------
        // First update the SOURCE index so message/highlight decode and a later
        // filter-clear see correct data; this emits no model signals (model rows are
        // the filtered subset).
        if (provisionalChanged)
            idx.records[base] = tail[0];
        for (int j = firstTailNew; j < tail.size(); ++j)
            idx.records.append(tail[j]);
        idx.extendBlockSums(provisionalChanged ? base : oldCount);

        // If the provisional record changed and was visible, drop its (last) view
        // row so it can be re-evaluated against the filter along with the new tail.
        int candidateStart = firstTailNew; // by default skip the unchanged provisional
        if (provisionalChanged) {
            if (filtered.lastVisibleSource() == base) {
                m_model->beginRemoveTail(1);
                filtered.popLastVisible();
                filtered.extendCompactSums(filtered.recordCount());
                m_model->endRemoveTail();
            }
            candidateStart = 0; // re-consider tail[0] (== source row `base`)
        }

        // Evaluate the candidate records against the active filter; integer axes
        // first, message text last (invariant #4) — same predicate as the initial
        // pass, so appended records pass through the filters unchanged.
        QVector<int> passing;
        passing.reserve(tail.size());
        for (int j = candidateStart; j < tail.size(); ++j) {
            const int srcRow = base + j;
            const Record &rec = idx.records.at(srcRow);
            if (m_document->filters().accepts(rec, [this, &rec] {
                    return m_document->messageText(rec);
                })) {
                passing.append(srcRow);
            }
        }

        if (!passing.isEmpty()) {
            const int firstViewRow = filtered.recordCount();
            m_model->beginAppendRows(passing.size());
            for (int srcRow : passing)
                filtered.appendVisible(srcRow, idx.records.at(srcRow));
            filtered.extendCompactSums(firstViewRow);
            m_model->endAppendRows();
        }
    }

    // Newly-interned subsystems may match highlight rules; rebind ids cheaply.
    m_document->resolveHighlighters();

    const qint64 newSourceRecords = tail.size() - firstTailNew;
    syncBaseline();
    emit ingested(newSourceRecords);
}

} // namespace loftail
