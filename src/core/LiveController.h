#pragma once

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
//     change), with no user notice.
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

    // Run one size/identity check now and ingest/rescan as needed. Wired to the
    // watcher's maybeChanged(); also called directly by tests for determinism.
    void checkNow();

signals:
    // Appended `newRecords` source records (>= 0; 0 when only the trailing record
    // grew in place). Lets the UI refresh discovered subsystem/thread lists + counts.
    void ingested(qint64 newRecords);
    // The file was rotated or truncated and silently reloaded (SPEC.md §3).
    void rescanned();

private:
    void ingestAppended();
    void doRescan();
    void syncBaseline();

    Document    *m_document;
    LogModel    *m_model;
    LiveWatcher *m_watcher = nullptr;
    qint64       m_lastSize = 0;
    quint64      m_lastIdentity = 0;
    bool         m_started = false;
};

} // namespace loftail
