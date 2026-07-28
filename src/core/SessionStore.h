#pragma once

#include "FormatSettings.h"
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
// on relaunch: every open file, its format, active filters and highlighters, every
// view onto it with its own column layout and wrap mode, and the global window
// geometry and pane layout.
//
// The schema has TWO arrays, because a file and a view are different things: N files
// are open, and one file may have several views onto it. Per-file scope (format,
// filters, highlighters, run selection) lives in `documents`; per-view scope (column
// layout and wrap mode) lives in `views`, whose ORDER is the tab order; window and
// pane layout is global. Filters/highlighters are stored as their portable
// name/index JSON so restoring is the same code path as applying a preset.
//
// Backed by QSettings, which gives its own store the atomic-write guarantee the
// multi-instance case needs (§8.1); the global keys are last-writer-wins (§10).
struct SessionDocument
{
    QString        path;
    FormatSettings format;       // pattern / encoding / source+display zones + run-start (§4, §3a)
    QJsonObject    filters;      // FilterPane portable state (names, not ids)
    QJsonObject    highlighters; // { rules: [...] } — names + palette indices (§8)

    // Run selection (SPEC.md §3a). The run-start PATTERN rides in `format`. This
    // records WHICH run was viewed, by a STABLE key (start byte offset, with the
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

struct Session
{
    int                      schemaVersion = 3; // SessionStore::kSchemaVersion
    QByteArray               geometry;    // QWidget::saveGeometry()
    QByteArray               windowState; // QMainWindow::saveState() — the pane layout
    int                      activeView = 0; // index into `views`
    QVector<SessionDocument> documents;
    QVector<SessionView>     views;

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
    // `timeDisplay` was added WITHIN v2 rather than bumping the version: it is one
    // additive key in the existing shape, read with the legacy `displayZone` key as a
    // fallback (SessionStore.cpp), so a store written by either build round-trips. A
    // version bump is earned by structural change — a new array, a field moved
    // between scopes, a renamed key, a dropped blob — and it costs every session
    // whose version load() does not list.
    static constexpr int kSchemaVersion = 3;

    // Read the whole session (empty documents when nothing was saved, or when the
    // stored schema version is not understood). A v1 or v2 store is migrated.
    static Session load(QSettings &settings);

    // Write the whole session, replacing any previous one. Both arrays are rewritten
    // wholesale so a shrunk list leaves no stale tail.
    static void save(QSettings &settings, const Session &session);

private:
    SessionStore() = delete;
};

} // namespace loftail
