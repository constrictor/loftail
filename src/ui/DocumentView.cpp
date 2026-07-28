#include "DocumentView.h"

#include "DocumentContext.h"
#include "FindBar.h"

#include <QEvent>
#include <QUuid>
#include <QVBoxLayout>

namespace loftail {

QString DocumentView::makeDockName()
{
    // A UUID, not an ordinal: ordinals shift when files are closed in a different
    // order than they were saved, and a shifted name silently lands a view in
    // another view's saved layout slot.
    return QStringLiteral("docView-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

DocumentView::DocumentView(DocumentContext *context, QWidget *parent)
    : QWidget(parent), m_context(context)
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    m_logView = new LogView(context->doc.get(), context->model, this);
    m_layout->addWidget(m_logView, 1);

    m_findBar = new FindBar(this);
    m_layout->addWidget(m_findBar);
    connect(m_findBar, &FindBar::findRequested, this, &DocumentView::findRequested);

    // Which view is active is decided by focus, which the window tracks globally
    // (QApplication::focusChanged) and walks up to the enclosing DocumentView —
    // that catches focus landing on any descendant, including the Find bar's line
    // edit, which a filter installed here would miss.
    setFocusProxy(m_logView);
}

DocumentView::~DocumentView() = default;

void DocumentView::activateFind()
{
    m_findBar->activate();
}

} // namespace loftail
