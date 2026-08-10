#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QtGlobal>

QT_BEGIN_NAMESPACE
class QFileSystemWatcher;
class QTimer;
QT_END_NAMESPACE

namespace loftail {

class Document;
class LogModel;

// M6 — the watch mechanism (SPEC.md §3, invariant #5). QFileSystemWatcher alone is
// unreliable: it misses updates on many network mounts and drops a path when the
// file is rotated out from under it. So the watch is BELT-AND-BRACES — a filesystem
// watcher (on the file AND its directory, to catch a rename/replace) PLUS a
// low-frequency size-poll timer as a fallback. Both funnel into a single
// maybeChanged() signal; the controller re-checks size/identity and decides what
// happened. UI-free (QtCore only) so the whole thing is testable without a window,
// and the controller can also be driven synchronously (checkNow()) for determinism.
//
// M11: a remote path is POLL-ONLY — there is no local path to watch, and the poll
// costs a local stat of the spool rather than a network round trip (§6.3).
class LiveWatcher : public QObject
{
    Q_OBJECT

public:
    explicit LiveWatcher(QObject *parent = nullptr);
    ~LiveWatcher() override;

    // (Re)watch `path` and its containing directory, and start the poll timer.
    void watch(const QString &path);
    void stop();

    // The size-poll fallback cadence. Small enough to feel live on a network mount
    // the watcher cannot signal, large enough to be negligible on a local file the
    // watcher already covers. Default 750 ms.
    void setPollInterval(int ms);

signals:
    // The file may have changed (watcher fired, or a poll tick elapsed). The
    // controller re-stats to learn whether it grew, shrank, or was replaced.
    void maybeChanged();

private:
    void ensureWatched(); // (re)add the file path after a rotation dropped it

    QFileSystemWatcher *m_fsw = nullptr;
    QTimer             *m_poll = nullptr;
    QString             m_path;
    QString             m_dir;
};

// M6 — live ingestion (SPEC.md §3). Owns a LiveWatcher and, on each change, either
//   * ingests the appended tail — continuing the single forward pass from the last
//     confirmed record boundary (invariant #9), holding the trailing record as
//     provisional so a record that grows with continuation lines resolves correctly
//     (invariant #2), extending the block prefix sums in place (invariant #1), and
//     feeding appended records through the ACTIVE filters/highlighters unchanged; or
//   * silently rescans on a rotation or truncation (size shrink or file-identity
//     change), with no user notice; or
//   * clears the document to the WAITING state when the log is no longer there at all,
//     and resumes it when it comes back (M13, §6.5). Waiting is where a log that has
//     never existed starts, so the same tick serves "not written yet" and "just
//     deleted" — there is no separate machinery for the two, and no mode either way.
// It manipulates the LogModel with begin/endInsert/Remove and dataChanged, exactly
// as IndexController does for the initial scan. Runs entirely on the GUI thread; the
// initial off-thread IndexController scan must have finished before start() (they
// must not mutate the index concurrently).
class LiveController : public QObject
{
    Q_OBJECT

public:
    LiveController(Document *document, LogModel *model, QObject *parent = nullptr);
    ~LiveController() override;

    // Begin watching + polling. Seeds the size/identity baseline from the source as
    // it stands after the initial scan, so the first check ingests anything that
    // arrived while that scan ran.
    void start();
    void stop();

    void setPollInterval(int ms);

    // The model showing the DIGEST subset of this document (M19), or nullptr when the
    // owner has none. Held so the digest's wholesale ordinal remap can be bracketed by
    // a model reset BEFORE the mutation, exactly as doRescan() brackets the main model,
    // rather than being reset after the fact. Non-owning; must outlive this controller.
    void setDigestModel(LogModel *model) { m_digestModel = model; }

    // What the last ingest tick's records did to the Tab and Notify actions (M19).
    // Stashed rather than carried in a new signal: the owner reads it at the top of its
    // `ingested` handler, so no metatype and no signal-ordering question arises. Reset
    // at the start of every tick.
    struct BatchAlerts
    {
        int tabMatches = 0;    // records matching an enabled rule carrying Tab
        int notifyMatches = 0; // ... carrying Notify
    };
    const BatchAlerts &lastBatchAlerts() const { return m_lastAlerts; }

    // How long the log must be MISSING before the document is cleared to the waiting
    // state. This is hysteresis, and it is what keeps a rotation silent: `logrotate`
    // renames and then recreates, and a check landing in that gap would otherwise blank
    // the view for a tick before the new file was noticed. A real deletion is only
    // reported this much later, which nobody can perceive. Default 2000 ms; tests set
    // it to 0 to make the transition deterministic.
    void setVanishGrace(int ms) { m_vanishGraceMs = ms; }

    // Run one size/identity check now and ingest/rescan as needed. Wired to the
    // watcher's maybeChanged(); also called directly by tests for determinism.
    void checkNow();

signals:
    // Appended `newRecords` source records (>= 0; 0 when only the trailing record
    // grew in place). Lets the UI refresh discovered subsystem/thread lists + counts.
    void ingested(qint64 newRecords);
    // The file was rotated or truncated and silently reloaded (SPEC.md §3).
    void rescanned();

    // What this source is doing, for the status bar, or empty when there is nothing to
    // report (sourceStatusText). Emitted only when the text CHANGES, so the 750 ms tick
    // costs a string compare rather than a repaint.
    void sourceStatusChanged(const QString &text);

    // The source's stream is finished and watching has stopped, permanently. Fires at
    // most once per source, after the last bytes have been ingested.
    void completed();

    // The document entered or left the waiting state (M13, §6.5). `reason` is the
    // user-facing sentence while waiting, and empty when leaving it. Watching does NOT
    // stop — waiting is precisely the state in which the watch is the only thing
    // making progress.
    void waitingChanged(bool waiting, const QString &reason);

    // The log a waiting document is waiting for is BACK, and the document is ready to
    // be resumed. The owner must respond by calling Document::resume() with a provider
    // built from the file's remembered pattern.
    //
    // A signal rather than a call, because this class is UI-free core and has no
    // pattern to build a provider from — the pattern lives in the owner and must stay
    // there (invariant #3). The consequence is worth stating plainly: a document whose
    // owner never connects this stays waiting forever, by design rather than by
    // accident. Tests connect a one-line lambda; MainWindow does the full job.
    void resumeRequested();

private:
    void ingestAppended();
    // Run the highlight rules over the records this tick appended, for the three
    // actions that need to know as the record ARRIVES rather than as it is painted
    // (M19). Highlighting was entirely lazy and pull-based before this; the whole cost
    // of it for a document whose rules only colour is one anyEnabled() walk of the rule
    // list, which is the first line of the body.
    void runMatchActions(int firstNewRow, bool provisionalChanged, int provisionalRow);
    void doRescan();
    void syncBaseline();
    void publishSourceStatus();
    // The waiting half of checkNow(): returns true when it handled this tick.
    void checkWhileWaiting();
    void beginWaiting(const QString &reason);

    Document    *m_document;
    LogModel    *m_model;
    LogModel    *m_digestModel = nullptr;
    BatchAlerts  m_lastAlerts;
    LiveWatcher *m_watcher = nullptr;
    qint64       m_lastSize = 0;
    bool         m_started = false;
    // How long the origin has been gone, against the grace period above. Real elapsed
    // time rather than a count of checks, because checks are not evenly spaced: the
    // filesystem watcher fires them too, and a rotation produces a burst of them at
    // exactly the moment the path is briefly empty. Counting those would shorten the
    // grace period precisely when it is needed. Invalid == the origin is present.
    QElapsedTimer m_vanishedSince;
    int           m_vanishGraceMs = 2000;
    // Latched once the source reports its stream finished. checkNow() is reachable
    // from a stray watcher tick and directly from tests, and completion is a one-time
    // event: without this it would be re-announced on every later call.
    bool         m_completed = false;
    QString      m_lastStatusText;
};

} // namespace loftail
