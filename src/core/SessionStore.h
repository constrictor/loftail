#pragma once

#include "Record.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

QT_BEGIN_NAMESPACE
class QSettings;
QT_END_NAMESPACE

namespace loftail {

// Session persistence (SPEC.md §10, ARCHITECTURE.md §8, §12). What loftail restores
// on relaunch: every open file, its active filters and highlighters, every view onto
// it with its own column layout and wrap mode, and the global window geometry and pane
// layout. NOT how a log is READ — the format, encoding, zones and run-start pattern are
// settings, resolved from the tree on every open (M20, LogSettings.h).
//
// The schema has TWO arrays, because a file and a view are different things: N files
// are open, and one file may have several views onto it. Per-file scope (filters,
// highlighters, run selection) lives in `documents`; per-view scope (column layout and
// wrap mode) lives in `views`, whose ORDER is the tab order; window and pane layout is
// global. Filters/highlighters are stored as their portable
// name/index JSON so restoring is the same code path as applying a preset.
//
// Backed by QSettings, which gives its own store the atomic-write guarantee the
// multi-instance case needs (§8.1); the global keys are last-writer-wins (§10).
struct SessionDocument
{
    QString path;

    // ---- READ ONLY, AND ONLY ONCE (M21) ---------------------------------------------
    //
    // Everything below used to be this array's reason for existing: a log's filters, its
    // highlight rules and which run it was on. They are per-FILE state, so they moved to
    // one record per log (LogFileStore.h), which is what makes them survive closing the
    // tab — the session only ever remembered a log while it was open in one.
    //
    // save() no longer writes any of them. load() still reads them, so the first launch
    // after the upgrade can hand them to the pool; the first quit then takes them off the
    // disk, and after that these are permanently empty. No schema bump came with the
    // removal: a removed key is exactly what a backward read handles.
    QJsonObject filters;      // FilterPane portable state (names, not ids)
    QJsonObject highlighters; // { rules: [...] } — names + palette indices (§8)

    // NO FORMAT. A log's format, encoding, source zone, timestamp display and run-start
    // pattern live in the settings tree (M20, LogSettings.h) and are resolved from the
    // path on restore, exactly as an ordinary open resolves them. They were carried
    // here as well until M20, and the copy was the reason a Preferences edit did not
    // reach a restored tab. Removing keys needs no schema bump: an older file simply
    // has some this build does not read.

    // Run selection (SPEC.md §3a). The run-start PATTERN belongs to the settings tree.
    // This records WHICH run was viewed, by a STABLE key (start byte offset, with the
    // start timestamp as a fallback hint) rather than the ordinal, which shifts as
    // the file grows. runAll == the explicit "all runs" view; otherwise a
    // selectedRunStartOffset >= 0 names a specific run; the default (nothing saved)
    // re-resolves to the newest run.
    bool   runAll = false;
    qint64 selectedRunStartOffset = -1;
    qint64 selectedRunStartTimestamp = Record::kNoTimestamp;
};

// One view onto one file, in tab order. Several may name the same `documentIndex` —
// that is a file opened in two independently-scrolled views.
struct SessionView
{
    int        documentIndex = 0; // index into Session::documents
    QByteArray columnState;       // QHeaderView state: order, sizes, hidden (§5)
    int        wrapMode = 0;      // LogView::WrapMode
};

// One config-file editor page, and WHERE ON THE BAR it sat (SPEC.md §4, §10).
//
// The `views` array's ORDER is the order the log tabs sit in, which was the whole layout
// a session had to carry while every page was a log. With a second kind of page the
// order alone cannot say how the two interleave, so an editor records its absolute tab
// position and the restore inserts it there — see SessionStore.h's schema note.
struct SessionEditor
{
    // THE CONFIG ADDRESS, never the log it was opened from. The same config is reachable
    // from several logs, so restoring it as "log #2's config" would reopen a different
    // file the moment that log's setting moved — and this lets an editor page outlive
    // the tab of the log that opened it. Already normalized and password-free, because
    // ConfigLocation is what produced it.
    QString address;
    int     tabIndex = 0;
    // PRESENCE, NOT VALUE. Only a syntax the USER chose is written; a guess is re-made on
    // restore from the file as it stands, which is right because the file may have
    // changed. `syntaxChosen` false means "nothing was stored", and reading a stored 0
    // (PlainText) as a choice would bring every restored tab back uncoloured — the trap
    // four other stores in this project already record.
    bool    syntaxChosen = false;
    int     syntax = 0;
};

struct Session
{
    int                      schemaVersion = 4; // SessionStore::kSchemaVersion
    QByteArray               geometry;    // QWidget::saveGeometry()
    QByteArray               windowState; // QMainWindow::saveState() — the pane layout
    int                      activeView = 0; // index into `views`
    // Which TAB was in front, absolute on the bar. Distinct from `activeView` because
    // the bar now holds two kinds of page: with no editors the two are equal, which is
    // exactly what makes the v3 migration a copy.
    int                      activeTab = 0;
    QVector<SessionDocument> documents;
    QVector<SessionView>     views;
    QVector<SessionEditor>   editors;

    bool hasDocuments() const { return !documents.isEmpty(); }
    // The document `view` belongs to, or nullptr if the index is out of range.
    const SessionDocument *documentFor(const SessionView &view) const
    {
        if (view.documentIndex < 0 || view.documentIndex >= documents.size())
            return nullptr;
        return &documents.at(view.documentIndex);
    }
};

class SessionStore
{
public:
    // 2 — added the `views` array (multiple files, multiple views per file), moved
    // columnState from the document to the view, and replaced `activeDocument` with
    // `activeView`.
    //
    // 3 — open files became tabs in a central document well instead of dock widgets,
    // so `views` dropped its per-view `dockName` (tab order carries what it used to)
    // and `windowState` shrank to the pane layout alone.
    //
    // Older stores are migrated on read rather than discarded, EXCEPT their
    // windowState: a v1 blob describes a window with a central widget and no document
    // docks, and a v2 one a window whose central widget is collapsed to nothing —
    // restoring either into this shell yields a mangled layout with no diagnostic
    // (for v2, a document area squeezed to zero width).
    //
    // `timeDisplay` was added WITHIN v2 rather than bumping the version, and M20
    // REMOVED the whole format group without one either: an added key is additive, and
    // a removed key is exactly what a backward read handles — an older store simply
    // carries fields this build does not consult. A version bump is earned by
    // structural change — a new array, a field moved
    // between scopes, a renamed key, a dropped blob — and it costs every session
    // whose version load() does not list.
    // 4 — added the `editors` array (config-file editor pages, SPEC.md §4) and the
    // `activeTab` index beside `activeView`. A NEW ARRAY, so by the rule above it earns
    // the bump — and the bump costs nothing, because load() lists 3 and migrates it: a
    // v3 store restores with no editors and activeTab copied from activeView, which is
    // what it means when every page is a log.
    static constexpr int kSchemaVersion = 4;

    // Read the whole session (empty documents when nothing was saved, or when the
    // stored schema version is not understood). A v1, v2 or v3 store is migrated.
    static Session load(QSettings &settings);

    // Write the whole session, replacing any previous one. Both arrays are rewritten
    // wholesale so a shrunk list leaves no stale tail.
    static void save(QSettings &settings, const Session &session);

    // Public: see FormatDetector.h — a deleted function says so more clearly than an
    // inaccessible one, and this class is a namespace of static functions.
    SessionStore() = delete;
};

} // namespace loftail
