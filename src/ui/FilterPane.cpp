#include "FilterPane.h"

#include "AxisEditor.h"
#include "Document.h"
#include "Filter.h"
#include "MatchCriteria.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
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

    // Filter context (SPEC.md §6), injected into the message-text axis rather than
    // added to this pane: context widens the MESSAGE search and only it — a context
    // record is one the message filter rejected but every other axis still admits —
    // so it is part of that search, not a sixth thing to configure. Putting it there
    // also makes the checkable group grey it out exactly when it does nothing.
    auto *contextRow = new QWidget(this);
    contextRow->setToolTip(tr(
        "Also show records either side of each match, dimmed — like grep -B/-A. "
        "Neighbours still have to pass the other filters."));
    auto *contextLayout = new QHBoxLayout(contextRow);
    contextLayout->setContentsMargins(0, 0, 0, 0);
    auto makeSpin = [contextRow] {
        auto *spin = new QSpinBox(contextRow);
        spin->setRange(0, Document::kMaxContext);
        spin->setAccelerated(true);
        return spin;
    };
    m_contextBefore = makeSpin();
    m_contextAfter = makeSpin();
    m_contextBefore->setObjectName(QStringLiteral("contextBefore"));
    m_contextAfter->setObjectName(QStringLiteral("contextAfter"));
    contextLayout->addWidget(new QLabel(tr("Context:"), contextRow));
    contextLayout->addWidget(new QLabel(tr("Before:"), contextRow));
    contextLayout->addWidget(m_contextBefore);
    contextLayout->addWidget(new QLabel(tr("After:"), contextRow));
    contextLayout->addWidget(m_contextAfter);
    contextLayout->addStretch(1);
    m_axes->addTextExtra(contextRow);

    // One handler for every control in the pane, the editor's and this pane's alike:
    // a context edit is a filter edit and must not be able to grow behavior a tick in
    // the axis list lacks.
    auto edited = [this] {
        applyToDocument();
        emit filtersChanged();
    };
    connect(m_axes, &AxisEditor::changed, this, edited);
    connect(m_contextBefore, &QSpinBox::valueChanged, this, edited);
    connect(m_contextAfter, &QSpinBox::valueChanged, this, edited);

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
    QJsonObject o = m_axes->criteria().toJson();
    // Written only when set, following MatchCriteria's own rule for optional keys: an
    // untouched pane must serialize exactly as it did before context existed, or every
    // stored preset and session would have to be re-versioned to gain two zeroes.
    if (m_contextBefore->value() > 0)
        o.insert(QStringLiteral("contextBefore"), m_contextBefore->value());
    if (m_contextAfter->value() > 0)
        o.insert(QStringLiteral("contextAfter"), m_contextAfter->value());
    return o;
}

void FilterPane::restoreState(const QJsonObject &o)
{
    m_axes->setCriteria(MatchCriteria::fromJson(o));
    // Blocked: the single filtersChanged() at the end of this function is the pane's
    // one notification for the whole restore, exactly as it was before the spinners
    // existed. An absent key means zero — what every pre-context preset and session
    // says, and what it meant then.
    {
        const QSignalBlocker blockBefore(m_contextBefore);
        const QSignalBlocker blockAfter(m_contextAfter);
        m_contextBefore->setValue(o.value(QStringLiteral("contextBefore")).toInt(0));
        m_contextAfter->setValue(o.value(QStringLiteral("contextAfter")).toInt(0));
    }
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

    // Not part of the resolve: context is not a match criterion but a widening of
    // what the resolved predicate admits, so it lives beside the FilterSet on the
    // document rather than inside it (SPEC.md §6, ContextEmitter.h).
    m_document->setContext(m_contextBefore->value(), m_contextAfter->value());
}

} // namespace loftail
