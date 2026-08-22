#pragma once

#include "AxisEditor.h"

#include <QJsonObject>
#include <QWidget>

#include <optional>

class QLabel;
class QSpinBox;
class QTimer;
class QToolButton;

namespace loftail {

class Document;

// M4 — the Filters side pane (SPEC.md §6, §8). A dockable widget, NOT a modal
// dialog: every axis is visible and toggles with a single click. It binds to the
// ACTIVE document by signal (invariant #7): setDocument() rebinds it, never a
// global "current file". It owns no per-file state beyond its own widget selections
// — the authoritative FilterSet lives in the Document; the pane writes into it and
// emits filtersChanged(), which MainWindow turns into a Document::applyFilters()
// wrapped in a model reset.
//
// The axis controls themselves live in the shared `AxisEditor`, which a highlight
// rule's editor also uses, so filtering and highlighting offer the same criteria and
// behave the same way while doing opposite things with the result. This pane is what
// remains around it: the document binding and the resolve into the Document's
// FilterSet.
class FilterPane : public QWidget
{
    Q_OBJECT

public:
    explicit FilterPane(QWidget *parent = nullptr);

    // Rebind to a document (or nullptr to clear). Repopulates the auto-discovered
    // subsystem/thread lists from its intern tables and enables the time/thread
    // axes according to whether the format carries those fields.
    void setDocument(Document *document);

    // `document` is about to be destroyed. Drops any deferred edit aimed at it and
    // unbinds, so nothing later resolves names through its freed intern table. The
    // owner calls this BEFORE it destroys a context — see the implementation for the
    // crash this prevents.
    void documentClosing(const Document *document);

    // Refresh the auto-discovered lists from the document's intern tables — called
    // when indexing progresses/finishes so newly-seen subsystems/threads appear
    // (SPEC.md §6: the lists are discovered as the file is scanned).
    void refreshDiscoveredLists();

    // Re-render the time-range editors after the display zone moves (a timestamp
    // display-mode change, SPEC.md §4): the editors hold wall clock, so leaving the
    // digits alone would silently re-point the bounds at a different instant.
    void refreshTimeBounds();

    // Portable, name-based snapshot of the pane's filter state (SPEC.md §9, §10).
    // Used for both filter presets (create-from-current-state / apply) and per-file
    // session restore. Carries subsystem/thread NAMES, never interned ids, so it is
    // portable across files and a re-index. restoreState() applies a snapshot and
    // emits filtersChanged() so the caller recomputes the visible set.
    //
    // It is the AxisEditor's criteria plus this pane's own controls — currently the
    // two context spinners, which are not match criteria and so are not in
    // MatchCriteria (a highlight rule shares that type and has no use for them). They
    // are written only when non-zero, so a pane with no context set serializes
    // byte-identically to one from before the feature existed and neither store's
    // schema version has to move.
    QJsonObject saveState() const;
    void restoreState(const QJsonObject &state);

    // --- Filtering by pointing (the record menu, SPEC.md §5) -------------------
    //
    // One edit to one axis, taken from the record under the cursor rather than typed.
    // These go through the pane — and so through the AxisEditor's controls — rather
    // than writing into the Document's FilterSet directly, for two reasons: the
    // widget state is not derivable from the resolved set (which is why the window
    // stashes it per file, DocumentContext::filterState), so a direct write would
    // leave the pane showing something else and the next hand edit would undo the
    // menu's; and the ticks visibly moving IS the undo story, since there is no undo
    // stack. Each emits filtersChanged() exactly once, like a click in the pane.
    void showOnlyValue(ValueAxis axis, const QString &name);
    void hideValue(ValueAxis axis, const QString &name);
    void setMinimumPriority(Priority p);
    void setTimeBound(TimeBound which, qint64 utcMs);
    void setTimeRange(qint64 fromUtcMs, qint64 toUtcMs);

    // Back to an unfiltered view in one action: every axis to its default, every
    // discovered value ticked, context off. The pane's own Clear button and the
    // View menu's Clear Filters both land here.
    void clearAll();

    // Whether the bound document is currently having anything hidden from it — asked
    // of the RESOLVED FilterSet, so an axis that is switched on but excludes nothing
    // does not count. Drives the pane header and the dock's own marker.
    bool hasActiveFilters() const;

signals:
    // Emitted whenever any control changes. MainWindow rebuilds the Document's
    // FilterSet from the editor's criteria and reapplies inside a model reset.
    void filtersChanged();

    // hasActiveFilters() may have changed. The pane is one of four tabbed together,
    // so for three tabs out of four the axes are out of sight while still in force;
    // MainWindow marks the dock title with this.
    void activityChanged(bool active);

private:
    // Resolve the editor's criteria into the bound Document's FilterSet — names to
    // interned ids, display-zone wall clock to UTC ms. No-op without a document.
    void applyToDocument();

    // Apply an edit, now or shortly. Every control in the pane goes through
    // scheduleApply(); applyNow() is the thing it eventually does.
    //
    // The point is that a filter edit is not one operation but two: resolving the
    // criteria, which is trivial, and the full forward pass over the index that
    // filtersChanged() sets off — a pass that decodes a message per record on the
    // text axis, resets the model and repaints every view. On an ordinary log that is
    // microseconds and must stay synchronous, because "type and the view has already
    // narrowed" is the pane's whole feel and because a caller that edits a control and
    // then reads the result is entitled to. On a multi-GB index it is the thing that
    // runs once per KEYSTROKE.
    //
    // So the coalescing is measured, not assumed: applyNow() times its own pass, and
    // scheduleApply() defers only while the last one was slow enough to notice. A
    // small file never waits — nothing in the suite had to learn about this — and a
    // huge one starts coalescing after the first pass has proved it needs to.
    void scheduleApply();
    void applyNow();
    // Tell MainWindow whether anything is being filtered out, so it can mark this
    // pane's TAB — the only place the answer is now shown, and the only place it was
    // ever needed, since the pane is tabbed behind three others. Called from
    // applyToDocument(), which every route that touches the FilterSet goes through.
    void updateActivity();

    Document   *m_document = nullptr;
    AxisEditor *m_axes = nullptr;
    QTimer     *m_applyTimer = nullptr;
    qint64      m_lastApplyMs = 0; // how long the previous pass took, per above
    // What the dock marker was last told. Unset until the first update, so the initial
    // state is published once rather than assumed.
    std::optional<bool> m_activeState;
    // Filter context (M15, SPEC.md §6), grep's -B/-A. Here and not in the AxisEditor:
    // that widget is shared with a highlight rule's editor, where "show the two
    // records either side" means nothing — highlighting removes nothing to begin with.
    QSpinBox   *m_contextBefore = nullptr;
    QSpinBox   *m_contextAfter = nullptr;
};

} // namespace loftail
