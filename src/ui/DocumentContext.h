#pragma once

#include "FormatSettings.h"

#include <QJsonObject>
#include <QVector>

#include <memory>
#include <optional>

namespace loftail {

class Document;
class DocumentView;
class LogModel;
class IndexController;
class LiveController;

// A run selection to re-resolve once the (async) index finishes, set only by
// session restore. Runs are detected after indexing, so the saved run identity
// (by start offset/timestamp, not ordinal) is re-resolved in onIndexFinished and
// consumed there. Absent for a normal open, which defaults to the newest run.
struct RunRestore
{
    bool   all = false;
    qint64 startOffset = -1;
    qint64 startTimestamp = 0;
};

// Everything the window owns for ONE open file: the Document itself (which holds
// the per-file state proper, invariant #7) plus the machinery built around it —
// the model, the indexing and live-tail controllers, the format choice, and the
// views showing it.
//
// This exists so none of that lives at window scope. With several files open the
// window holds a vector of contexts and an active *view* pointer; the panes still
// bind to the active Document by signal (ARCHITECTURE.md §12.3), never to a
// context, so nothing below the UI learns that several files can be open.
//
// One context can back SEVERAL DocumentViews: a second view onto the same file
// shares the index, filters, highlighters, run selection and live controller, and
// differs only in scroll position, selection, wrap mode and column layout — all of
// which live in the LogView.
class DocumentContext
{
public:
    DocumentContext();
    ~DocumentContext(); // stops and destroys live -> controller -> model, in that order

    DocumentContext(const DocumentContext &) = delete;
    DocumentContext &operator=(const DocumentContext &) = delete;

    // Stop the live watcher and the indexing worker without tearing the context
    // down, so a rescan can rebuild them over the same Document.
    void stopWorkers();

    std::unique_ptr<Document> doc;

    // Owned bare (not QObject-parented to the window): the context's lifetime is
    // what governs them, and the destructor order above matters — the live watcher
    // references the controller's model and the Document.
    LogModel        *model = nullptr;
    IndexController *controller = nullptr;
    LiveController  *live = nullptr;

    // The format choice for this file (SPEC.md §4). Held here as UI configuration;
    // the source of truth across sessions is the per-file FormatCache. The pattern
    // never reaches the view, filters, or highlighters (invariant #3).
    FormatSettings settings;

    // Set only by session restore; consumed once indexing finishes (§3a).
    std::optional<RunRestore> pendingRunRestore;

    // The Filters pane's widget state for THIS file. The pane is global and follows
    // the active document, but it does not itself hydrate from a Document — its
    // checkbox/text state is not derivable from the FilterSet. So the window stashes
    // it here when switching away and puts it back when switching in; an empty
    // object means "the defaults", which is what a freshly-opened file gets.
    // Highlighters need no equivalent: HighlighterPane::setDocument already reloads
    // its rules from the Document it binds to.
    QJsonObject filterState;

    // Indexing progress, kept per file so a background file can keep scanning while
    // the status bar shows whichever file is active.
    bool indexing = false;
    int  progressPercent = 0;

    // The views showing this file. Non-owning: each view is owned by its dock.
    QVector<DocumentView *> views;
};

} // namespace loftail
