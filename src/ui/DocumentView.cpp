#include "DocumentView.h"

#include "DocumentContext.h"
#include "FindBar.h"

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
    m_layout->addWidget(m_logView, 1);

    m_findBar = new FindBar(this);
    m_layout->addWidget(m_findBar);
    connect(m_findBar, &FindBar::findRequested, this, &DocumentView::findRequested);

    // Focusing the page focuses the table, so raising a tab puts the caret where the
    // user is about to read rather than on the Find bar.
    setFocusProxy(m_logView);
}

DocumentView::~DocumentView() = default;

void DocumentView::activateFind()
{
    m_findBar->activate();
}

} // namespace loftail
