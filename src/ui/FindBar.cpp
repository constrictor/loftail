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
    // Shift+Enter has to be taken off the field before QLineEdit sees it — see
    // eventFilter() below, which is where the backwards gesture lives.
    m_edit->installEventFilter(this);
    row->addWidget(m_edit, 1);

    auto *prev = new QToolButton(this);
    prev->setObjectName(QStringLiteral("findPrevious"));
    prev->setText(QStringLiteral("▲")); // up
    prev->setToolTip(tr("Find Previous (Shift+F3, or Shift+Enter in the box)"));
    auto *next = new QToolButton(this);
    next->setObjectName(QStringLiteral("findNext"));
    next->setText(QStringLiteral("▼")); // down
    next->setToolTip(tr("Find Next (F3, or Enter in the box)"));
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

bool FindBar::eventFilter(QObject *watched, QEvent *event)
{
    // Enter searches forward and Shift+Enter searches backwards (SPEC.md §5) — the
    // gesture every find box has, and the one the ▲ button and Shift+F3 already did.
    //
    // It cannot be done on returnPressed(), which is what the forward search is bound
    // to: QLineEdit emits that signal for Return and Enter whatever modifiers are held,
    // so the backwards gesture would arrive as a forward one AND, if both were bound,
    // as both at once. Catching the key before the field sees it is what keeps the two
    // apart. Every other key is passed through untouched — Escape included, which
    // QLineEdit ignores and which therefore still reaches keyPressEvent() above.
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            // Qt::KeypadModifier rides along on the numeric keypad's Enter and says
            // nothing about intent; any other modifier belongs to somebody else's
            // gesture and is not this one.
            if ((key->modifiers() & ~Qt::KeypadModifier) == Qt::ShiftModifier) {
                emit findRequested(false, false);
                return true; // consumed, so no returnPressed() and no forward search
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace loftail
