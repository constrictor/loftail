#include "LiveController.h"

#include "ContextEmitter.h"
#include "Decoder.h"
#include "Document.h"
#include "FilteredIndex.h"
#include "Indexer.h"
#include "LogModel.h"
#include "LogSource.h"
#include "RecordIndex.h"
#include "RemoteLocation.h"
#include "SpooledLogSource.h"

#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QPair>
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

    // A spooled log has no watchable path on this machine: what grows is the local
    // cache, not the path. For a remote log QFileSystemWatcher would either fail or,
    // worse, latch onto a same-named local file; for an archived one the container is
    // a real file, but watching it would fire on a rewrite that does not change what
    // has already been expanded. Poll only — and the poll is cheap regardless of where
    // the log lives, because the GUI-side check is an atomic read plus one local stat
    // of the spool (ARCHITECTURE.md §6.3). The FETCH cadence is the fetcher thread's
    // own, not this timer's.
    if (logPathIsSpooled(path)) {
        m_dir.clear();
        m_poll->start();
        return;
    }

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
    if (m_path.isEmpty() || logPathIsSpooled(m_path))
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
    // Size only: the identity half of the old baseline moved into the source itself,
    // which is the only thing that knows how to re-resolve its own origin.
    LogSource *src = m_document ? m_document->source() : nullptr;
    m_lastSize = src ? src->size() : 0;
}

void LiveController::start()
{
    // A document with no source is not a broken one any more: it may be WAITING for a
    // log that has not been written yet, in which case this watch is the only thing
    // that will ever bring it in (§6.5). Only a document with no path at all — nothing
    // to watch and nothing to wait for — is refused.
    if (m_started || !m_document || m_document->path().isEmpty())
        return;
    m_started = true;
    syncBaseline();
    // Publish once up front rather than leaving the status line blank until the first
    // poll tick 750 ms later. It matters most for a document that opens WAITING, where
    // the fetcher's explanation of why the log is not there is the only thing there is
    // to say about it.
    publishSourceStatus();
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
    if (m_completed)
        return; // the stream ended; there is nothing left that could change
    if (!m_document)
        return;

    if (m_document->isWaiting()) {
        checkWhileWaiting();
        return;
    }

    LogSource *src = m_document->source();
    if (!src) {
        // No source and not waiting: a rescan failed for a reason other than the log
        // being absent (it is there but unreadable). Retry on the next tick, as before.
        if (logSourceAvailable(m_document->path()))
            doRescan();
        return;
    }

    // Take this BEFORE the refresh below, not after. The fetcher publishes Complete
    // only once its final committedSize is published, so a source that says "finished"
    // here guarantees that the refresh and ingest that follow see every remaining byte.
    // Reading it afterwards would let the last chunk land in the window between the two
    // and be dropped — the records would simply never appear, with no error anywhere.
    const bool sourceFinished = src->isComplete();

    // Rotation-by-replace: the thing at the source's ORIGIN is no longer the thing it
    // holds — a rename+recreate at a local path, or a rotated remote file. Only the
    // source can answer this, because what has to be re-resolved differs per source
    // (invariant #5, §6): a mapped fd still follows the inode it mapped and so sees
    // nothing, and re-stats its path; a spooled remote source compares generations.
    const bool replaced = src->wasReplaced();

    // Refresh the source size (re-maps so reads cannot run past the live EOF). This
    // also latches truncation when the file shrank below what we indexed.
    const qint64 newSize = src->refreshSize();
    const bool truncated = src->wasTruncated() || newSize < m_lastSize;

    if (replaced || truncated) {
        m_vanishedSince.invalidate(); // something IS at the origin: a rotation, not a deletion
        doRescan();
        return;
    }

    // Vanished is checked AFTER replaced, and the order is the contract: a completed
    // rotation reads as replaced and rescans silently, and only a path with nothing at
    // it at all reaches here. The grace period covers the gap in a rotation that has
    // renamed but not yet recreated — without it, a check landing in that gap would
    // blank the view for a tick and then reload it, which is exactly the flicker
    // SPEC.md §3 promises rotation does not produce.
    if (src->originVanished()) {
        if (!m_vanishedSince.isValid())
            m_vanishedSince.start();
        if (m_vanishedSince.elapsed() < m_vanishGraceMs)
            return;
        // The transport's own words where it has any (a dropped link, a log removed on
        // the far end), because "is no longer there" is a guess this end cannot make
        // about a machine it can no longer reach.
        const QString reported = sourceStatusText(*src, m_document->path());
        beginWaiting(reported.isEmpty()
                         ? waitingForText(m_document->path(), WaitCause::Gone)
                         : reported);
        return;
    }
    m_vanishedSince.invalidate();

    if (newSize > m_lastSize)
        ingestAppended();
    else
        m_lastSize = newSize; // no growth (or a spurious watcher tick)

    publishSourceStatus();

    if (sourceFinished) {
        // Nothing can ever arrive again, so stop looking. This is an absence of work,
        // NOT a mode: there is no setting, nothing on screen changes, and the follow
        // control is untouched — after this the newest record simply stops moving,
        // exactly as it does for a local file nobody is writing (SPEC.md §3).
        m_completed = true;
        m_watcher->stop();
        m_started = false;
        emit completed();
    }
}

// One tick of a document that is waiting for its log: is it back yet, and if so, hand
// off to the owner to resume it.
void LiveController::checkWhileWaiting()
{
    LogSource *src = m_document->source();

    // Two different questions, because the two kinds of waiting know different things.
    // A LOCAL log has no source while it waits, so the path is all there is to ask; a
    // SPOOLED one keeps its source precisely so its fetcher can go on trying, and the
    // fetcher — not the path — is what knows whether it got through.
    const bool back = src ? !src->originVanished()
                          : logSourceAvailable(m_document->path());

    // Keep the status line current either way: a spooled source spends this whole time
    // publishing why it cannot reach the log, which is the one thing worth showing.
    publishSourceStatus();
    if (!back)
        return;

    m_vanishedSince.invalidate();
    // The owner has the pattern and therefore the provider; it calls Document::resume()
    // (invariant #3). It may decline — the log can vanish again between this check and
    // that open — in which case the document is still waiting and the next tick tries
    // again, which is why this does not assume success.
    emit resumeRequested();
    if (m_document->isWaiting())
        return;

    syncBaseline();
    emit waitingChanged(false, QString());
    emit rescanned(); // the visible set was replaced wholesale, exactly as a rotation
}

void LiveController::beginWaiting(const QString &reason)
{
    // A full model reset: the visible set is being emptied wholesale, which is the same
    // signal doRescan() uses for the same reason.
    m_model->beginFilterReset();
    m_document->enterWaiting(reason);
    m_model->endFilterReset();

    m_lastSize = 0;
    m_vanishedSince.invalidate();
    // Watching deliberately continues. Waiting is the one state where the watch is the
    // only thing making progress — stopping it here is how the log would never come
    // back (contrast completed(), where there is genuinely nothing left to look at).
    emit waitingChanged(true, reason);
}

void LiveController::publishSourceStatus()
{
    LogSource *src = m_document ? m_document->source() : nullptr;
    const QString text = src ? sourceStatusText(*src, m_document->path()) : QString();
    if (text == m_lastStatusText)
        return;
    m_lastStatusText = text;
    emit sourceStatusChanged(text);
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
        if (m_document->filters().anyActive() || m_document->viewRestricted())
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

        // Not restricted here (an active run would materialize the filtered view), so
        // this only grows the run LIST for the pane; ordering vs the rows above is
        // irrelevant since nothing is being excluded.
        m_document->updateRunsAfterAppend(oldCount);
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

        // Fold new records into the run list BEFORE evaluating candidates: a new
        // run-start marker makes the selected (previously last) run's end finite, so
        // acceptsInView() below rejects the new-run records and the view freezes at
        // the boundary — no already-admitted rows to remove ("stay on current run").
        m_document->updateRunsAfterAppend(oldCount);

        // Where to resume the emission from, and therefore what to un-emit first.
        //
        // Without filter context this is the row the provisional record itself
        // occupies: it is the only record whose bytes can have changed, so at most
        // its own view row can be wrong. With leading context (-B) configured it can
        // also have PULLED IN neighbours: a provisional that was a match dragged up
        // to `before` records in with it, and if it stops matching those rows are
        // orphaned. So the resume point widens to base - before.
        //
        // It widens only on a genuine FLIP of the provisional's emission class,
        // because that is the only case where re-emitting produces something
        // different. If it was and still is a match, popping just it leaves the
        // emitter at lastEmitted == base-1 (the suffix invariant, ContextEmitter.h),
        // so the re-emit writes zero context rows and re-appends it unchanged — and
        // paying `before`+1 row removals per tick for that would churn the tail and
        // shift a detached reader's scroll position on every append.
        //
        // Note the resume point and the pop point are the SAME row, deliberately: a
        // match inside the popped window that the scan did not restart far enough to
        // reach would be dropped from the view for good.
        const int before = m_document->contextBefore();
        const int after = m_document->contextAfter();

        int candidateStart = base + 1; // skip the unchanged provisional
        if (provisionalChanged) {
            const bool wasMatch = filtered.lastMatchSource() == base;
            const bool nowMatch = m_document->acceptsInView(idx.records.at(base));
            candidateStart = (before > 0 && wasMatch != nowMatch) ? qMax(0, base - before) : base;
        }
        if (oldCount == 0)
            candidateStart = 0; // nothing was emitted yet; the whole tail is new

        const int popCount = filtered.trailingCountFrom(candidateStart);
        if (popCount > 0) {
            m_model->beginRemoveTail(popCount);
            for (int i = 0; i < popCount; ++i)
                filtered.popLastVisible();
            filtered.extendCompactSums(filtered.recordCount());
            m_model->endRemoveTail();
        }

        // Evaluate the candidate records against the active filter; integer axes
        // first, message text last (invariant #4) — the SAME emitter the initial pass
        // runs, resumed from whatever survived the pop, so appended records enter the
        // view exactly where a one-shot scan would have put them. Nothing about the
        // resume state is cached: both fields are read back off the visible subset,
        // which is why a rescan or a wait needs no bookkeeping here.
        ContextState st;
        st.lastEmitted = filtered.lastVisibleSource();
        st.lastMatch = filtered.lastMatchSource();

        QVector<QPair<int, bool>> passing; // (source row, is context)
        passing.reserve(tail.size() + before);
        emitWithContext(
            candidateStart, idx.records.size() - 1, before, after, st,
            [this, &idx](int row) { return m_document->inRunBound(idx.records.at(row)); },
            [this, &idx](int row) { return m_document->matchesFilters(idx.records.at(row)); },
            [&passing](int row, bool isContext) { passing.append({row, isContext}); });

        if (!passing.isEmpty()) {
            const int firstViewRow = filtered.recordCount();
            m_model->beginAppendRows(passing.size());
            for (const auto &p : passing)
                filtered.appendVisible(p.first, idx.records.at(p.first), p.second);
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
