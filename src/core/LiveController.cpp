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

#include "LiveController.h"

#include "ContextEmitter.h"
#include "Decoder.h"
#include "DiagnosticLog.h"
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
#include <QPointer>
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

    // BEFORE the baseline is taken, and that order is the point. A document that opened
    // with no bytes may have some by now: the scan that just finished runs on a worker
    // while a spool fills behind it, so the whole of an archived or remote log can land
    // between the open and this call. Take the baseline first and that arrival is
    // already behind us — there is no growth left for checkNow() to notice, and the
    // format and the encoding stay unjudged for the rest of the session (§6.5).
    //
    // refreshSize(), never size(): a source reports the size it last latched, and the
    // one thing this case guarantees is that somebody else did the reading. Asking the
    // stale number is how a log with every one of its records already indexed reports
    // that it has no bytes — which is exactly what it did until this line said refresh.
    if (!m_document->isWaiting() && !m_document->formatSettled() && m_document->source()
        && m_document->source()->refreshSize() > 0) {
        if (!settleFirstBytes())
            return; // this controller no longer exists — see settleFirstBytes()
    }

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
        // Retry and NOT Replaced: this runs on EVERY tick until the log becomes
        // readable, so announcing here would repeat one sentence twice a second for as
        // long as the permission stands. The rotation that led here has already been
        // announced, or failed to re-read and deliberately said nothing.
        if (logSourceAvailable(m_document->path()))
            doRescan(ReloadCause::Retry);
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

    // A RECONNECT THAT HAS NOT DELIVERED A BYTE YET IS NOT A ROTATION TO ACT ON. A
    // spooled fetcher answers a re-established link by opening a fresh generation and
    // re-fetching from the top, so the generation moves the moment it connects — before
    // the prime has committed anything. Acting on that would rescan against an empty
    // spool: the cached records the outage was spent showing would blank for a tick and
    // then come back, which is precisely the flicker this whole state exists to avoid.
    //
    // BEFORE refreshSize(), and that order is the whole of it — refreshSize() is what
    // ADOPTS the new generation, and once it has, the index's offsets name bytes in a
    // file that no longer holds them, so every painted record would decode to nothing.
    // There is no way back from that; the only fix is not to take the step.
    //
    // notReadyYet() is the question ("committed nothing, and still on its way"), and it
    // is asked only of a document that is already stale, so an ordinary rotation on a
    // healthy link is untouched.
    if (replaced && m_document->isStale() && src->notReadyYet()) {
        publishSourceStatus();
        beginStale(sourceStatusText(*src, m_document->path()));
        return;
    }

    // Refresh the source size (re-maps so reads cannot run past the live EOF). This
    // also latches truncation when the file shrank below what we indexed.
    const qint64 newSize = src->refreshSize();
    const bool truncated = src->wasTruncated() || newSize < m_lastSize;

    if (replaced || truncated) {
        m_vanishedSince.invalidate(); // something IS at the origin: a rotation, not a deletion
        // Whatever the cached records were showing, this rescan replaces them with what
        // is there now — so the strip and the tab mark that said they were cached come
        // off here, before the reader is handed the fresh copy.
        endStale();
        // Which of the two the reader is told, and it is classified HERE from the finer
        // facts rather than from `truncated`, which is a catch-all (ReloadCause).
        // `replaced` first, and that order is load-bearing: a rename-and-recreate
        // rotation almost always lands a smaller file at the path, so testing the shrink
        // first would call nearly every real rotation a truncation. A shrink that is not
        // a replacement is a copytruncate; anything left is a rewrite in place, which
        // SPEC.md §3 says counts as replacing it. A spooled source answers `replaced`
        // for all three — it re-fetches into a new generation and cannot tell them
        // apart — so Truncated is structurally unreachable for a remote log, which
        // SPEC.md §3 says out loud rather than leaving to be discovered.
        doRescan(!replaced && newSize < m_lastSize ? ReloadCause::Truncated
                                                   : ReloadCause::Replaced);
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
        const QString reason = reported.isEmpty()
            ? waitingForText(m_document->path(),
                             waitCauseFor(m_document->path(), WaitCause::Gone))
            : reported;

        // TWO ANSWERS, and which one this is depends on whether there is anything left
        // to read. A remote log that has already been fetched is still readable out of
        // its local spool after the link drops, and emptying the tab there throws away
        // the only copy the reader has of a machine they can no longer reach — at the
        // moment they most want to read it. So it stays on screen and says so beside
        // itself instead of in place of itself (SPEC.md §3).
        if (canShowCachedWhileGone()) {
            beginStale(reason);
            publishSourceStatus();
            return;
        }
        beginWaiting(reason);
        return;
    }
    m_vanishedSince.invalidate();
    endStale();

    // A log that opened with NO BYTES has had nothing judged about it — not its format
    // and not its encoding (§6.5) — and this is where that is put right, before a single
    // record of it is read. Reading first and settling afterwards is not the same thing:
    // the records would be cut and decoded by whatever an empty sample produced, which
    // for a UTF-16 log is garbage, and nothing would ever read them again.
    //
    // Asked on the SIZE, not on growth over the baseline. The bytes may have turned up
    // before this watch began — a spool filling while the initial scan ran — in which
    // case the baseline already includes them and there is no growth to notice.
    if (!m_document->formatSettled() && newSize > 0) {
        if (!settleFirstBytes())
            return; // this controller no longer exists — see settleFirstBytes()
        if (m_document->isWaiting())
            return; // it went again between the tick and the open; the wait has it
    } else if (newSize > m_lastSize) {
        ingestAppended();
    } else {
        m_lastSize = newSize; // no growth (or a spurious watcher tick)
    }

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
    //
    // notReadyYet() is the second half of the spooled question and is asked ONLY here
    // and in Document::prepare() — never in checkNow()'s vanish branch, where it would
    // blank the view on every slow-link rotation (LogSource.h). It is what holds the
    // wait until bytes actually arrive rather than until the fetcher merely changes
    // state, which matters because resume() cannot be un-done: settling a format against
    // an empty sample leaves it unsettled forever.
    const bool back = src ? !(src->originVanished() || src->notReadyYet())
                          : logSourceAvailable(m_document->path());

    // Keep the status line current either way: a spooled source spends this whole time
    // publishing why it cannot reach the log, which is the one thing worth showing.
    publishSourceStatus();
    // And keep the VIEW current with it. The status bar was the only surface that
    // tracked a changing reason; the placeholder and the tab tooltip were written once,
    // at the transition, and a spooled log transitions while it is still "connecting…".
    republishWaitReason();
    if (!back) {
        // THE presence-retry line (DiagnosticLog.h), and the one place in loftail where
        // throttling is not a nicety but the only thing that makes the record possible at
        // all: this runs on the 750 ms watch tick, so an unthrottled line here would be
        // 4800 an hour per waiting document and would bury every other kind of evidence
        // in the file within minutes. One a minute, with the suppressed count, says the
        // same thing — loftail is still looking and here is how hard.
        diagLogEvery(60000, "wait", m_document->path(),
                     QStringLiteral("still waiting for %1").arg(m_document->path()));
        return;
    }

    m_vanishedSince.invalidate();
    // The owner has the pattern and therefore the provider; it calls Document::resume()
    // (invariant #3). It may decline — the log can vanish again between this check and
    // that open — in which case the document is still waiting and the next tick tries
    // again, which is why this does not assume success.
    //
    // Guarded, for the reason settleFirstBytes() sets out: answering this can end with
    // the owner rebuilding the document and deleting this controller outright.
    QPointer<LiveController> alive(this);
    emit resumeRequested();
    if (!alive)
        return;
    if (m_document->isWaiting()) {
        // ONE tick of this is ordinary — the log went again between the check above and
        // the open — and a document that does it for ever is a document that can never
        // come back. Until this line that case wrote NOTHING: `back` said yes, so the
        // presence-retry line above is not reached, and the owner declines in silence.
        // Eighty attempts a minute with no record of any of them is precisely what
        // SPEC.md's "Diagnostics" promises there will not be. Its own key, so it does
        // not throttle against the line above: the two say different things.
        diagLogEvery(60000, "wait", QStringLiteral("resume:") + m_document->path(),
                     QStringLiteral("%1 looked ready but resuming it declined")
                         .arg(m_document->path()));
        return;
    }

    syncBaseline();
    // "records=N" rather than prose: this file is untranslated and greppable by design,
    // and a count with no plural to get wrong reads the same at 1 as at 100000.
    diagLog("wait", QStringLiteral("%1 is back — resumed, records=%2")
                        .arg(m_document->path())
                        .arg(m_document->index().records.size()));
    emit waitingChanged(false, QString());
    emit rescanned(); // the visible set was replaced wholesale, exactly as a rotation
}

// The first bytes of a log that was open and empty. A format and an encoding are settled
// against REAL BYTES or not at all (§6.5), and an empty file has none to offer — so a log
// created before its writer has logged anything (the freshly rolled file of a service
// that has not started yet, the very file somebody opens to watch it start) opens as an
// ordinary tab with nothing settled, and this is where the settling it could not do
// happens.
//
// It asks the OWNER, through the same signal and for the same reason the waiting seam
// does: the pattern lives there and core must never hold one (invariant #3). What comes
// back is a document reopened, re-decoded and re-indexed from the top — Document::resume()
// — which is why the appended bytes must not be ingested here as well.
//
// RETURNS FALSE WHEN THIS CONTROLLER NO LONGER EXISTS, and every caller must obey it.
// Answering this signal can end in the owner being told a different format — the dialog
// it raises is the one the open owed — and applying one rebuilds the document, which
// destroys the live controller and builds a new one. The emit then returns into a `this`
// that has been deleted. A QPointer is what notices, since a QObject clears its weak
// references from its own destructor whether it was deleted or deleteLater()'d.
bool LiveController::settleFirstBytes()
{
    QPointer<LiveController> alive(this);
    emit resumeRequested();
    if (!alive)
        return false;

    // The log can go again between the tick that saw it grow and the open that settles
    // it, exactly as it can on a resume from waiting. Then it IS waiting, and the waiting
    // branch owns it from the next tick.
    if (m_document->isWaiting() || !m_document->source())
        return true;

    if (!m_document->formatSettled()) {
        // Nobody answered — a Document driven with no owner connected to the signal,
        // which is a supported way to use this class (the signal's own note says so).
        // The bytes still have to land, so this falls back to an ordinary append rather
        // than dropping them: the format is whatever the empty open compiled, which for
        // a manual pattern is already the right one.
        ingestAppended();
        return true;
    }

    syncBaseline();
    diagLog("wait", QStringLiteral("%1 has its first bytes — format settled, records=%2")
                        .arg(m_document->path())
                        .arg(m_document->index().records.size()));
    // The whole visible set was replaced, exactly as after a rotation: the records were
    // cut and decoded again by a format nobody had when the tab opened.
    emit rescanned();
    return true;
}

void LiveController::beginWaiting(const QString &reason)
{
    // A stale document that ends up here has lost the records it was showing them for —
    // a reconnect that came back to an empty log, a rescan that failed. Take the strip
    // and the tab mark off explicitly: Document::enterWaiting() clears the flag below,
    // but the surfaces showing it only ever learn from the signal.
    endStale();

    // A transition, so it is never throttled — this is the line that says WHEN a log the
    // user was reading stopped being there, which is the question the retries below it
    // are only context for.
    diagLog("wait", QStringLiteral("%1 has gone — %2").arg(m_document->path(), reason));

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

bool LiveController::canShowCachedWhileGone() const
{
    // SPOOLED ONLY. A local log's bytes are in the file that has just been deleted, and
    // a Document that waits for a local log deliberately RELEASES its source: holding an
    // unlinked inode open pins bytes nobody will read and keeps the writer's disk space
    // from coming back, which is what invariant #5 is about. A spool is loftail's own
    // file in loftail's own cache directory — nobody else's to be disturbed by — and the
    // source is kept across a wait anyway, because it owns the fetcher that is retrying.
    // So this costs nothing that is not already being paid.
    if (!logPathIsSpooled(m_document->path()))
        return false;

    // And only when there IS something cached. A remote log whose host was down from the
    // start has an empty index and nothing to show, so it waits in the ordinary way and
    // the placeholder carries the explanation — which is also what keeps the mark on a
    // tab meaning "this one has no records" and the strip meaning "these records are old".
    return !m_document->index().records.isEmpty();
}

void LiveController::beginStale(const QString &reason)
{
    // Reached on EVERY tick of an outage, which may run for hours, so the transition and
    // the restatement are told apart here rather than by the receiver: nothing is emitted
    // unless the sentence on screen would actually change.
    if (m_document->isStale() && m_document->staleReason() == reason)
        return;

    const bool wasStale = m_document->isStale();
    m_document->enterStale(reason);
    if (!m_document->isStale())
        return; // refused: the document is waiting, and that state owns the screen

    if (!wasStale) {
        // A transition, so never throttled: this is the line that says when the log the
        // user was reading stopped arriving, with the record count they are left looking
        // at — the two facts a bug report about "it just stopped" needs.
        diagLog("wait", QStringLiteral("%1 went out of reach — %2 (showing %3 cached "
                                       "records)")
                            .arg(m_document->path(), reason)
                            .arg(m_document->index().records.size()));
    }
    // NO model reset and no index work anywhere on this path. Nothing about the visible
    // set has moved, so the reader keeps their scroll position, their selection and
    // their filters over records that are exactly as true as they were a second ago.
    emit staleChanged(true, reason);
}

void LiveController::endStale()
{
    if (!m_document->isStale())
        return;
    diagLog("wait", QStringLiteral("%1 is reachable again").arg(m_document->path()));
    m_document->leaveStale();
    emit staleChanged(false, QString());
}

// The reason a document is waiting for is not settled at the transition. Since M17 a
// spooled open enters the wait on "connecting…" and the worker answers afterwards —
// refused, host down, an archive holding no such member, an SFTP subsystem that timed
// out — and SshFetcher republishes several different reasons over the life of one wait.
// Announcing only the first left the view saying "connecting…" for ever with nothing
// connecting (SPEC.md §3: the view says what it is waiting for).
void LiveController::republishWaitReason()
{
    LogSource *src = m_document->source();
    if (!src)
        return; // a LOCAL wait: nothing to ask, and the transition's sentence stands

    const QString text = sourceStatusText(*src, m_document->path());
    // NEVER an empty one. sourceStatusText() is empty for every healthy state, and the
    // receiver writes what arrives straight into the placeholder — so an unconditional
    // re-emit would blank the view of any waiting document the moment its fetcher had
    // nothing to complain about. Silence is not a new reason.
    if (text.isEmpty())
        return;
    // Compared against what is ON SCREEN, not against m_lastStatusText: start() latches
    // that one before anything has compared it to the reason prepare() stored, so
    // piggybacking on publishSourceStatus()'s guard would silently skip the first
    // correction — the one case this exists for.
    if (text == m_document->waitReason())
        return;

    m_document->restateWaitReason(text);
    emit waitingChanged(true, text);
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

void LiveController::doRescan(ReloadCause cause)
{
    // Reload in place (SPEC.md §3: no dialog, nothing to press, nothing lost — only a
    // passing note in the status bar, emitted at the bottom). A full model reset is the
    // right signal — the visible set is wholesale replaced — with the re-index and
    // any active-filter recompute done between begin/end so rowCount() is consistent.
    m_model->beginFilterReset();
    if (m_digestModel)
        m_digestModel->beginFilterReset();
    const bool ok = m_document->rescan();
    if (ok) {
        // The intern tables were rebuilt, so rebind highlight rules to the new ids and
        // re-materialize the visible subset if a filter is active. refreshHighlighting()
        // rather than resolveHighlighters() because rescan() cleared the digest along
        // with the filtered subset — every ordinal it held named a record that no longer
        // exists — and it has to be recomputed over the records that replaced them.
        m_document->refreshHighlighting();
        if (m_document->filters().anyActive() || m_document->viewRestricted())
            m_document->applyFilters();
    }
    if (m_digestModel)
        m_digestModel->endFilterReset();
    m_model->endFilterReset();

    syncBaseline();
    // The status bar says this in passing and for five seconds (SPEC.md §3); the file
    // says it permanently, which is the half a bug report can be built on — a reload is
    // the thing a reader later describes as "it jumped" or "it lost my place", and the
    // transient notice is gone long before they think to mention it. Untranslated and
    // greppable by design, `cause` included, exactly as the outcome beside it is.
    const char *why = "retry";
    switch (cause) {
    case ReloadCause::Replaced:  why = "replaced";  break;
    case ReloadCause::Truncated: why = "truncated"; break;
    case ReloadCause::Retry:     break;
    }
    diagLog("wait", QStringLiteral("%1 reloaded — %2, %3, records=%4")
                        .arg(m_document->path(), QString::fromLatin1(why),
                             QString::fromLatin1(ok ? "rescanned" : "could not reopen"))
                        .arg(m_document->index().records.size()));

    // BEFORE rescanned(), which must stay the LAST statement of this function: that is
    // what makes handing control to eight external subscribers safe without a QPointer
    // dance on `this` (contrast settleFirstBytes(), which is not last and needs one).
    // Only a re-read that actually worked is announced — a failed reopen would be
    // claiming the log was reloaded when it was not — and never a Retry, which fires on
    // every tick that an unreadable log is still unreadable.
    if (ok && cause != ReloadCause::Retry)
        emit reloaded(cause);
    emit rescanned();
}

void LiveController::ingestAppended()
{
    RecordIndex &idx = m_document->index();
    LogSource *src = m_document->source();
    const Decoder &dec = m_document->decoder();
    FilteredIndex &filtered = m_document->filtered();
    const bool filterActive = filtered.active();

    const int oldCount = int(idx.records.size());
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
        oldCount > 0 && std::memcmp(tail.data(), &oldLast, sizeof(Record)) != 0;
    const int base = oldCount > 0 ? oldCount - 1 : 0; // source row of tail[0]
    const int firstTailNew = oldCount > 0 ? 1 : 0;    // index of the first brand-new tail record

    if (!filterActive) {
        // ---- Unfiltered (identity view): view rows == source rows --------------
        if (provisionalChanged)
            idx.records[base] = tail[0]; // in-place height/flip update of the tail

        const int newCount = int(tail.size()) - firstTailNew;

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
        // orphaned. So the resume point widens to the start of its -B window.
        //
        // That start is contextWindowStart(), NOT base - before: `before` counts
        // records the non-text axes admit (ContextEmitter.h), and with a priority or
        // subsystem filter on, those are not contiguous in ordinal space. Subtracting
        // would stop short of the window and strand the orphans.
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
            candidateStart = (before > 0 && wasMatch != nowMatch)
                                 ? m_document->contextWindowStart(base, before)
                                 : base;
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
        // How much of the -A window that match has already spent. The visible rows
        // above lastMatch are exactly its trailing-context rows — contiguous in view
        // order, because a later match would itself be lastMatch — so counting them
        // recovers the figure. It is right whenever it is below `after`, and says
        // "window closed" whenever it equals it, which is all the emitter asks.
        st.sinceMatch = st.lastMatch >= 0 ? filtered.trailingCountFrom(st.lastMatch + 1) : 0;

        QVector<QPair<int, bool>> passing; // (source row, is context)
        passing.reserve(tail.size() + before);
        emitWithContext(
            candidateStart, int(idx.records.size()) - 1, before, after, st,
            [this, &idx](int row) { return m_document->inContextStream(idx.records.at(row)); },
            [this, &idx](int row) { return m_document->matchesTextAxis(idx.records.at(row)); },
            [&passing](int row, bool isContext) { passing.append({row, isContext}); });

        if (!passing.isEmpty()) {
            const int firstViewRow = filtered.recordCount();
            m_model->beginAppendRows(int(passing.size()));
            for (const auto &p : passing)
                filtered.appendVisible(p.first, idx.records.at(p.first), p.second);
            filtered.extendCompactSums(firstViewRow);
            m_model->endAppendRows();
        }
    }

    // Newly-interned subsystems may match highlight rules; rebind ids cheaply. This
    // must precede runMatchActions() — the rules it is about to evaluate are the ones
    // just rebound, and a subsystem interned by this very batch is exactly the case a
    // rule naming it was written for.
    m_document->resolveHighlighters();
    runMatchActions(oldCount, provisionalChanged, base);

    const qint64 newSourceRecords = tail.size() - firstTailNew;
    syncBaseline();
    emit ingested(newSourceRecords);
}

void LiveController::runMatchActions(int firstNewRow, bool provisionalChanged,
                                     int provisionalRow)
{
    m_lastAlerts = BatchAlerts();

    // The whole cost for a document whose rules only colour: one walk of the rule list
    // per tick, touching no record and decoding nothing (ARCHITECTURE.md §7.5). Every
    // action below is opt-in per rule, so this is the ordinary case and not the corner.
    static constexpr HighlightActions kLiveActions =
        HighlightAction::Digest | HighlightAction::Tab | HighlightAction::Notify;
    const HighlighterSet &set = m_document->highlighters();
    if (!set.anyEnabled(kLiveActions))
        return;

    // --- Tab and Notify: per record, over the genuinely new ones -----------------
    //
    // The run bound is asked first and is integer comparisons only, for §3a's reason
    // and so the tab marker and the digest cannot disagree about the same record.
    if (set.anyEnabled(HighlightAction::Tab | HighlightAction::Notify)) {
        const RecordIndex &idx = m_document->index();
        for (int row = qMax(0, firstNewRow); row < idx.records.size(); ++row) {
            const Record &rec = idx.records.at(row);
            if (!m_document->inRunBound(rec))
                continue;
            const ActionMatch m = set.matchActions(
                rec, HighlightAction::Tab | HighlightAction::Notify,
                [this, &rec] { return m_document->messageText(rec); });
            if (m.tab >= 0)
                ++m_lastAlerts.tabMatches;
            if (m.notify >= 0)
                ++m_lastAlerts.notifyMatches;
        }
    }

    // --- Digest: a wholesale ordinal remap, so bracket the reset ----------------
    //
    // FilteredIndex has no interior replace and a digest row can move backwards in the
    // list when one rule's newest match overtakes another's, so the update republishes
    // the whole (at most rule-count long) ordinal list. That is a model RESET, not an
    // append — cheap at this size, and bracketed BEFORE the mutation the way doRescan()
    // brackets the main model rather than reset after the fact.
    if (m_digestModel)
        m_digestModel->beginFilterReset();
    m_document->updateDigestAfterAppend(firstNewRow, provisionalChanged, provisionalRow);
    if (m_digestModel)
        m_digestModel->endFilterReset();
}

} // namespace loftail
