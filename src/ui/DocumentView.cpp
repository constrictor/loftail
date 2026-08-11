#include "DocumentView.h"

#include "DocumentContext.h"
#include "FindBar.h"
#include "LogModel.h"

#include <QEvent>
#include <QVBoxLayout>

namespace loftail {

DocumentView::DocumentView(DocumentContext *context, QWidget *parent)
    : QWidget(parent), m_context(context)
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    m_logView = new LogView(context->doc.get(), context->model, this);
    m_logView->setObjectName(QStringLiteral("logView")); // test contract, never translated
    m_layout->addWidget(m_logView, 1);

    // The strip is NAMED (SPEC.md §7). A second table under the first explains nothing
    // on its own, and its rows are copies of rows in the log above it, so without a
    // caption the honest reading is a rendering fault. A body-less SectionBox: the same
    // title-row-and-hairline the five filter axes wear, minus the checkbox, so the one
    // caption in the document area is drawn by the same class as every caption in a pane
    // rather than by a hand-styled label that would drift from them.
    m_digestTitle = new SectionBox(tr("Digest"), this);
    m_digestTitle->setObjectName(QStringLiteral("digestTitle")); // test contract
    m_digestTitle->setFlat(true);
    m_digestTitle->setTitleDivider(true);
    m_digestTitle->hide(); // with the strip it names
    m_layout->addWidget(m_digestTitle, 0);

    // The digest strip (M19, SPEC.md §7): under the table, above the Find bar, at
    // stretch 0 so it takes exactly the height it asks for and the table keeps the rest.
    //
    // A sibling of its caption, NOT a child of it, however much a titled box wants to
    // hold the thing it titles: LogView::digestContentLines() caps the strip at a third
    // of parentWidget()'s height, so parenting it to a box whose own height comes from
    // the strip's size hint would make the cap a third of the strip itself and feed the
    // layout its own output.
    m_digestView = new LogView(context->doc.get(), context->digestModel, this,
                               LogView::Role::Digest);
    m_digestView->setObjectName(QStringLiteral("digestView"));
    m_digestView->hide(); // until a rule asks for one
    m_layout->addWidget(m_digestView, 0);

    m_findBar = new FindBar(this);
    m_layout->addWidget(m_findBar);
    connect(m_findBar, &FindBar::findRequested, this, &DocumentView::findRequested);

    // The strip appears and disappears with its content, so the model's own reset is
    // the signal — not a separate flag the window would have to remember to set.
    connect(context->digestModel, &QAbstractItemModel::modelReset,
            this, &DocumentView::refreshDigestVisibility);
    connect(context->digestModel, &QAbstractItemModel::rowsInserted,
            this, &DocumentView::refreshDigestVisibility);
    connect(context->digestModel, &QAbstractItemModel::rowsRemoved,
            this, &DocumentView::refreshDigestVisibility);

    // Columns and horizontal scroll are MIRRORED from the table, one way only — the
    // strip's claim is that a row is "rendered exactly as it is in the log", and column
    // state alone makes that true only at horizontal offset zero. One way, so the two
    // views cannot chase each other.
    connect(m_logView, &LogView::columnLayoutChanged, this, [this] {
        m_digestView->restoreColumnState(m_logView->saveColumnState());
    });
    connect(m_logView, &LogView::horizontalOffsetChanged,
            m_digestView, &LogView::setHorizontalOffset);
    m_digestView->restoreColumnState(m_logView->saveColumnState());

    // Deliberately NOT connected: LogView::recordMenuRequested. The strip's view rows
    // are a different ordinal space — MainWindow::showRecordMenu resolves them against
    // the document's own FilteredIndex — so wiring it up would make the menu act
    // silently on the wrong record. Avoided by omission, which is exactly the kind of
    // thing a later "connect everything" pass restores.

    // Settle it once now. A view is often built AFTER its document's rules are in place
    // — session restore, and a second view of a log that already has a digest — and in
    // that case no model reset will ever arrive to reveal the strip.
    refreshDigestVisibility();

    // Focusing the page focuses the table, so raising a tab puts the caret where the
    // user is about to read rather than on the Find bar.
    setFocusProxy(m_logView);
}

void DocumentView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // The strip's height cap is a fraction of THIS widget's height, so it has to be
    // re-decided when the window changes size — the strip's own resize cannot see it.
    m_digestView->refreshDigestCap();
}

DocumentView::~DocumentView() = default;

void DocumentView::activateFind()
{
    m_findBar->activate();
}

void DocumentView::refreshDigestVisibility()
{
    const bool show = m_context->digestModel && m_context->digestModel->rowCount() > 0;
    // Both, explicitly. The caption is a sibling rather than a parent (see the
    // constructor), so hiding one does not hide the other — and a "Digest" heading over
    // nothing is worse than no heading. Setting the view's own visibility is also what
    // keeps digestView()->isHidden() meaning "the strip is not there".
    m_digestTitle->setVisible(show);
    m_digestView->setVisible(show);
    if (show)
        m_digestView->refreshDigestCap();
}

} // namespace loftail
