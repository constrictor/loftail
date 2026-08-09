#include "FilterPane.h"

#include "AxisEditor.h"
#include "Document.h"
#include "Filter.h"
#include "MatchCriteria.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace loftail {

namespace {
// How long a filter pass has to have taken before the next edit is allowed to wait
// for company, and how long it then waits. The threshold is a few frames: below it
// the pass is invisible and deferring would only add lag; above it the user is
// already watching the view stutter per keystroke.
constexpr qint64 kApplyDebounceThresholdMs = 40;
constexpr int    kApplyDebounceMs = 150;
} // namespace

FilterPane::FilterPane(QWidget *parent) : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    // A header row for the one thing the pane could not say and the one thing it could
    // not do: whether anything is being filtered out, and "stop". Both matter because
    // the pane is TABBED behind three others — switch to Runs and every axis is out of
    // sight while still in force — and because clearing meant visiting five axes by
    // hand, with no other way back to an unfiltered view.
    auto *header = new QHBoxLayout;
    header->setContentsMargins(6, 4, 6, 0);
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("filterSummary"));
    header->addWidget(m_summary, 1);
    m_clearButton = new QToolButton(this);
    m_clearButton->setObjectName(QStringLiteral("clearFilters"));
    m_clearButton->setText(tr("Clear"));
    m_clearButton->setToolTip(tr("Switch every axis off and tick every value again, "
                                 "leaving the whole log visible."));
    header->addWidget(m_clearButton);
    outer->addLayout(header);
    connect(m_clearButton, &QAbstractButton::clicked, this, &FilterPane::clearAll);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("filterScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    // The two metadata axes ship enabled so their controls act on the first click
    // (SPEC.md §6); applyToDocument() collapses the resulting no-op state.
    m_axes = new AxisEditor(AxisEditor::Defaults{/*priorityOn=*/true, /*loggerOn=*/true}, scroll);
    // Every axis shows its controls whether it is switched on or not — deliberately NOT
    // setCollapsible(true), which is what the Highlighters pane needs and this pane
    // briefly borrowed to save height. Height is the wrong thing to buy here: a pane
    // whose controls appear only once their axis is ticked cannot be read, only
    // explored, and the user has to switch a filter ON to find out whether it was the
    // one they wanted. Off, Qt greys the body, which says "not in force" without the
    // pane relaying out under the pointer. The scroll area above absorbs the height.
    scroll->setWidget(m_axes);

    // Filter context (SPEC.md §6), injected into the message-text axis rather than
    // added to this pane: context widens the MESSAGE search and only it — a context
    // record is one the message filter rejected but every other axis still admits —
    // so it is part of that search, not a sixth thing to configure. Putting it there
    // also makes the checkable group grey it out exactly when it does nothing.
    auto *contextRow = new QWidget(this);
    contextRow->setObjectName(QStringLiteral("contextRow"));
    contextRow->setToolTip(tr(
        "Also show records either side of each match, dimmed — like grep -B/-A. "
        "Neighbours still have to pass the other filters."));
    auto *contextLayout = new QHBoxLayout(contextRow);
    contextLayout->setContentsMargins(0, 0, 0, 0);
    // It shares the message axis's toggle row now, so it has to earn its width: the
    // two words this used to spend on "Before:" and "After:" are a minus and a plus,
    // with the words themselves on the spin boxes' own tooltips.
    auto makeSpin = [contextRow](const QString &prose) {
        auto *spin = new QSpinBox(contextRow);
        spin->setRange(0, Document::kMaxContext);
        spin->setAccelerated(true);
        spin->setToolTip(prose);
        spin->setAccessibleName(prose);
        // Sized for the value people type, not for kMaxContext. The same
        // Ignored-plus-floor trick the time editors use, and needed for the same
        // reason: a four-digit spin box asks for ~70 px, two of them plus the toggles
        // made the message axis the widest thing in the pane, and the pane pays that
        // in a horizontal scrollbar whose first casualty is the All/None/Invert
        // column. A maximum as well as a floor, because unlike a time bound there is
        // nothing to see in a wider box — Ignored expands, and without the cap these
        // two would eat every spare pixel of a wide dock.
        spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        spin->setMinimumWidth(46);
        spin->setMaximumWidth(62);
        return spin;
    };
    m_contextBefore = makeSpin(tr("Records to show before each match"));
    m_contextAfter = makeSpin(tr("Records to show after each match"));
    m_contextBefore->setObjectName(QStringLiteral("contextBefore"));
    m_contextAfter->setObjectName(QStringLiteral("contextAfter"));
    contextLayout->addWidget(new QLabel(tr("Context"), contextRow));
    // Glyphs, never translated (ARCHITECTURE.md §9.1) — U+2212, the minus that
    // matches the plus in width, not a hyphen.
    contextLayout->addWidget(new QLabel(QString::fromUtf8("−"), contextRow));
    contextLayout->addWidget(m_contextBefore);
    contextLayout->addWidget(new QLabel(QStringLiteral("+"), contextRow));
    contextLayout->addWidget(m_contextAfter);
    m_axes->addTextExtra(contextRow);

    m_applyTimer = new QTimer(this);
    m_applyTimer->setSingleShot(true);
    m_applyTimer->setInterval(kApplyDebounceMs);
    connect(m_applyTimer, &QTimer::timeout, this, &FilterPane::applyNow);

    // One handler for every control in the pane, the editor's and this pane's alike:
    // a context edit is a filter edit and must not be able to grow behavior a tick in
    // the axis list lacks.
    auto edited = [this] { scheduleApply(); };
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
    // Land a deferred edit on the document it was made against. After the rebind
    // applyToDocument() would write it into the file the user just switched TO, and
    // the one they made it on would keep a FilterSet that no longer matches its pane.
    if (m_applyTimer && m_applyTimer->isActive())
        applyNow();

    m_document = document;
    m_axes->setDocument(document);
    setEnabled(document != nullptr);
    // A different file is a different amount of work; do not carry the last one's
    // measurement into it. The first pass on the new document is synchronous and
    // measures itself.
    m_lastApplyMs = 0;
    applyToDocument();
}

void FilterPane::scheduleApply()
{
    if (m_lastApplyMs < kApplyDebounceThresholdMs) {
        applyNow();
        return;
    }
    m_applyTimer->start(); // restarts the wait: the burst is not over yet
}

void FilterPane::clearAll()
{
    // Everything this pane owns, not only the editor's axes: context is a filter the
    // user set, and a "Clear" that left two dimmed neighbours either side of every
    // match would not have cleared the filters. The spinners are blocked so the whole
    // reset still produces the ONE changed() the editor emits.
    {
        const QSignalBlocker blockBefore(m_contextBefore);
        const QSignalBlocker blockAfter(m_contextAfter);
        m_contextBefore->setValue(0);
        m_contextAfter->setValue(0);
    }
    m_axes->clearAll(); // emits changed() -> scheduleApply()
}

bool FilterPane::hasActiveFilters() const
{
    if (!m_document)
        return false;
    // The resolved set, not the widgets: an axis that is ticked but excludes nothing
    // is collapsed to inactive by applyToDocument(), and calling that "filtered" would
    // put a marker on every file the moment it opened.
    return m_document->filters().anyActive() || m_document->contextBefore() > 0
           || m_document->contextAfter() > 0;
}

void FilterPane::updateSummary()
{
    const bool active = hasActiveFilters();
    // Only on a CHANGE. applyToDocument() runs on every index-progress tick — the
    // scan calls refreshDiscoveredLists() as it turns up subsystems — and the answer
    // is the same every time. Repainting a label was harmless; re-setting the dock's
    // window title was not, because these panes ship tabified and that title is a
    // QTabBar entry, so each write relaid out the tab bar. Under a compositor the
    // resulting storm starved the GUI thread badly enough that indexing sat at 0%.
    // (Offscreen and Xvfb both absorb it, which is why only the real desktop showed
    // it — worth remembering next time a pane looks fine under test and not in use.)
    if (m_summaryActive.has_value() && *m_summaryActive == active)
        return;
    m_summaryActive = active;

    m_clearButton->setEnabled(active);
    m_summary->setText(active ? tr("Filtering") : tr("No filters"));
    // Greyed while inactive, so the word carries the state as well as saying it.
    QPalette pal = m_summary->palette();
    pal.setColor(QPalette::WindowText,
                 qApp->palette(m_summary).color(active ? QPalette::Active
                                                       : QPalette::Disabled,
                                                QPalette::WindowText));
    m_summary->setPalette(pal);
    emit activityChanged(active);
}

void FilterPane::applyNow()
{
    m_applyTimer->stop();
    // filtersChanged() is connected directly, so MainWindow's whole re-filter — the
    // forward pass, the model reset, the repaints — runs inside this call and is what
    // the measurement is of.
    QElapsedTimer elapsed;
    elapsed.start();
    applyToDocument();
    emit filtersChanged();
    m_lastApplyMs = elapsed.elapsed();
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
    // Synchronous, whatever the last pass cost: a restore is one edit, not a burst,
    // and the caller expects the visible set to have been recomputed on return.
    applyNow();
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
    if (!m_document) {
        updateSummary();
        return;
    }

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

    // Every route that changes the FilterSet passes through here, so this is the one
    // place the header and the dock marker have to be kept in step from.
    updateSummary();
}

} // namespace loftail
