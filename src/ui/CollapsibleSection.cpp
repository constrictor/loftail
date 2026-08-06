#include "CollapsibleSection.h"

#include <QToolButton>
#include <QVBoxLayout>

namespace loftail {

CollapsibleSection::CollapsibleSection(const QString &title, QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    m_toggle = new QToolButton(this);
    m_toggle->setObjectName(QStringLiteral("collapsibleToggle"));
    m_toggle->setText(title);
    m_toggle->setCheckable(true);
    m_toggle->setChecked(false);
    m_toggle->setAutoRaise(true);
    m_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_toggle->setArrowType(Qt::RightArrow);
    // Announce the state to a screen reader, which cannot see an arrow glyph.
    m_toggle->setFocusPolicy(Qt::StrongFocus);
    layout->addWidget(m_toggle, 0, Qt::AlignLeft);

    connect(m_toggle, &QToolButton::toggled, this, &CollapsibleSection::applyState);
}

void CollapsibleSection::setContentWidget(QWidget *content)
{
    if (m_content == content)
        return;

    delete m_content;
    m_content = content;
    if (!m_content)
        return;

    m_content->setParent(this);
    qobject_cast<QVBoxLayout *>(layout())->addWidget(m_content);
    m_content->setVisible(m_toggle->isChecked());
}

bool CollapsibleSection::isExpanded() const
{
    return m_toggle->isChecked();
}

void CollapsibleSection::setExpanded(bool expanded)
{
    m_toggle->setChecked(expanded); // applyState() follows from the toggled signal
}

void CollapsibleSection::applyState(bool expanded)
{
    m_toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    if (m_content)
        m_content->setVisible(expanded);

    // A layout only grows a window automatically. Collapsing leaves the extra height
    // behind as empty space unless the window is asked to give it back, and a dialog
    // that grows on every expand and never shrinks walks down the screen.
    if (!expanded) {
        if (QWidget *top = window())
            top->adjustSize();
    }
}

} // namespace loftail
