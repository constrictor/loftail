#include "FindBar.h"

#include "UiColors.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>

namespace loftail {

FindBar::FindBar(QWidget *parent) : QWidget(parent)
{
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(4, 2, 4, 2);

    row->addWidget(new QLabel(tr("Find:"), this));
    m_edit = new QLineEdit(this);
    m_edit->setObjectName(QStringLiteral("findEdit")); // findChild, for tests
    m_edit->setClearButtonEnabled(true);
    m_edit->setPlaceholderText(tr("Search visible records..."));
    ensureReadablePlaceholder(m_edit);
    row->addWidget(m_edit, 1);

    auto *prev = new QToolButton(this);
    prev->setObjectName(QStringLiteral("findPrevious"));
    prev->setText(QStringLiteral("▲")); // up
    prev->setToolTip(tr("Find Previous (Shift+F3)"));
    auto *next = new QToolButton(this);
    next->setObjectName(QStringLiteral("findNext"));
    next->setText(QStringLiteral("▼")); // down
    next->setToolTip(tr("Find Next (F3)"));
    row->addWidget(prev);
    row->addWidget(next);

    m_regex = new QCheckBox(tr("Regex"), this);
    m_regex->setObjectName(QStringLiteral("findRegex"));
    m_case = new QCheckBox(tr("Case"), this);
    m_case->setObjectName(QStringLiteral("findCase"));
    row->addWidget(m_regex);
    row->addWidget(m_case);

    // What the last search did: which match of how many, whether it wrapped, or why
    // there was nothing to go to (SPEC.md §5). It lives HERE rather than in the window's
    // status bar, which is rewritten on every ingest tick and every tab switch and would
    // wipe it within the second on a live log.
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("findStatus")); // findChild, for tests
    row->addWidget(m_status);

    auto *close = new QToolButton(this);
    close->setObjectName(QStringLiteral("findClose"));
    close->setText(QStringLiteral("✕"));
    close->setToolTip(tr("Close (Esc)"));
    row->addWidget(close);

    // Typing (or toggling an option) restarts the search from the top so the first
    // match is found without needing to press Enter twice.
    connect(m_edit, &QLineEdit::textChanged, this, [this](const QString &) { emit findRequested(true, true); });
    connect(m_edit, &QLineEdit::returnPressed, this, [this] { emit findRequested(true, false); });
    connect(m_regex, &QCheckBox::toggled, this, [this](bool) { emit findRequested(true, true); });
    connect(m_case, &QCheckBox::toggled, this, [this](bool) { emit findRequested(true, true); });
    connect(next, &QToolButton::clicked, this, [this] { emit findRequested(true, false); });
    connect(prev, &QToolButton::clicked, this, [this] { emit findRequested(false, false); });
    connect(close, &QToolButton::clicked, this, [this] { hide(); emit closed(); });

    hide();
}

QString FindBar::pattern() const { return m_edit->text(); }
bool FindBar::regex() const { return m_regex->isChecked(); }
bool FindBar::caseSensitive() const { return m_case->isChecked(); }

void FindBar::activate()
{
    // Whatever the last search reported is about a query that is about to be replaced,
    // and a stale "3 of 47" over a fresh empty box is a lie.
    m_status->clear();
    show();
    m_edit->setFocus();
    m_edit->selectAll();
}

void FindBar::setStatus(const QString &text)
{
    m_status->setText(text);
}

void FindBar::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        hide();
        emit closed();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace loftail
