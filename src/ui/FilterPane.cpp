#include "FilterPane.h"

#include "AxisEditor.h"
#include "Document.h"
#include "Filter.h"
#include "MatchCriteria.h"

#include <QFrame>
#include <QScrollArea>
#include <QVBoxLayout>

namespace loftail {

FilterPane::FilterPane(QWidget *parent) : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    // The two metadata axes ship enabled so their controls act on the first click
    // (SPEC.md §6); applyToDocument() collapses the resulting no-op state.
    m_axes = new AxisEditor(AxisEditor::Defaults{/*priorityOn=*/true, /*loggerOn=*/true}, scroll);
    scroll->setWidget(m_axes);

    connect(m_axes, &AxisEditor::changed, this, [this] {
        applyToDocument();
        emit filtersChanged();
    });

    setDocument(nullptr);
}

// ---------------------------------------------------------------------------
// Document binding
// ---------------------------------------------------------------------------

void FilterPane::setDocument(Document *document)
{
    m_document = document;
    m_axes->setDocument(document);
    setEnabled(document != nullptr);
    applyToDocument();
}

void FilterPane::refreshTimeBounds()
{
    m_axes->refreshTimeBounds();
    // The instant is unchanged, but the FilterSet holds UTC ms derived from the old
    // rendering; re-resolve so the two cannot disagree.
    applyToDocument();
}

void FilterPane::refreshDiscoveredLists()
{
    m_axes->refreshDiscoveredLists();
    // Keep the Document's FilterSet in step with the (re)populated lists so a
    // subsequent apply resolves ids against what is now shown, not a stale set. This
    // does not emit filtersChanged() — MainWindow reapplies explicitly when the scan
    // finishes; a plain repopulation should not itself trigger a recompute storm.
    applyToDocument();
}

// ---------------------------------------------------------------------------
// Portable state snapshot (presets + session restore)
// ---------------------------------------------------------------------------

QJsonObject FilterPane::saveState() const
{
    return m_axes->criteria().toJson();
}

void FilterPane::restoreState(const QJsonObject &o)
{
    m_axes->setCriteria(MatchCriteria::fromJson(o));
    // Merge the restored selection with whatever the scan has discovered so far, then
    // push it into the document and let the caller recompute the visible set.
    m_axes->refreshDiscoveredLists();
    applyToDocument();
    emit filtersChanged();
}

// ---------------------------------------------------------------------------
// Filtering by pointing (the record menu)
// ---------------------------------------------------------------------------
//
// Nothing here does any work of its own: the AxisEditor makes the edit and emits
// changed(), which the constructor already turns into applyToDocument() +
// filtersChanged(). That is the point — a menu edit and a hand edit are the same
// edit, so neither can grow behavior the other lacks.

void FilterPane::showOnlyValue(ValueAxis axis, const QString &name)
{
    m_axes->showOnlyValue(axis, name);
}

void FilterPane::hideValue(ValueAxis axis, const QString &name)
{
    m_axes->hideValue(axis, name);
}

void FilterPane::setMinimumPriority(Priority p)
{
    m_axes->setMinimumPriority(p);
}

void FilterPane::setTimeBound(TimeBound which, qint64 utcMs)
{
    m_axes->setTimeBound(which, utcMs);
}

void FilterPane::setTimeRange(qint64 fromUtcMs, qint64 toUtcMs)
{
    m_axes->setTimeRange(fromUtcMs, toUtcMs);
}

// ---------------------------------------------------------------------------
// Build the FilterSet
// ---------------------------------------------------------------------------

void FilterPane::applyToDocument()
{
    if (!m_document)
        return;

    // AbsentField::Matches — a record that never carried the field an axis tests is
    // never hidden by a filter on it (SPEC.md §4, §6); highlighting resolves the same
    // criteria with the opposite answer.
    //
    // NoOpAxes::Collapse — both metadata axes are enabled by default (SPEC.md §6),
    // which would otherwise put every file behind an active FilterSet and cost a
    // materialized compact index for no benefit. An axis whose selection excludes
    // NOTHING is written inactive: the checkbox stays ticked and responds instantly to
    // the first real narrowing, while FilteredIndex keeps its allocation-free identity
    // path (ARCHITECTURE.md §7.2). Both collapses are exact, not heuristic.
    m_document->filters() = m_axes->criteria().resolve(
        m_document->index(), m_document->format(), m_document->displayZone(),
        AbsentField::Matches, NoOpAxes::Collapse);
}

} // namespace loftail
