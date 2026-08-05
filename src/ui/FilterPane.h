#pragma once

#include "AxisEditor.h"

#include <QJsonObject>
#include <QWidget>

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

signals:
    // Emitted whenever any control changes. MainWindow rebuilds the Document's
    // FilterSet from the editor's criteria and reapplies inside a model reset.
    void filtersChanged();

private:
    // Resolve the editor's criteria into the bound Document's FilterSet — names to
    // interned ids, display-zone wall clock to UTC ms. No-op without a document.
    void applyToDocument();

    Document   *m_document = nullptr;
    AxisEditor *m_axes = nullptr;
};

} // namespace loftail
