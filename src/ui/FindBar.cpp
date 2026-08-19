#include "FindBar.h"

#include "UiColors.h"

#include <QCheckBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QToolButton>

namespace loftail {
namespace {

// How the row's spare width is split between the two elastic items. The query box is
// what a reader types into and keeps the larger share; the status keeps enough of one
// that the ordinary wordings — `2 of 7`, `1 of 7, wrapped to the top` — fit without
// eliding at an ordinary window width, and grows with the window rather than against a
// "longest wording" constant that translation and an unbounded match count would both
// outrun.
constexpr int kQueryStretch  = 3;
constexpr int kStatusStretch = 2;

} // namespace

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
    row->addWidget(m_edit, kQueryStretch);

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
    // The label may NOT be allowed to size itself from its text. The query box is the
    // row's other elastic item, so every pixel the wording grows by is a pixel taken
    // from the box — and everything laid out between the two, which is every control
    // the user clicks, slides left by exactly that much. Stepping through matches with
    // ▼ then walks the Case checkbox under a stationary pointer the moment a wrap note
    // appears, and the next click restarts the search case-sensitively. So the cell is
    // text-independent: QSizePolicy::Ignored drops the label's width hint out of the
    // layout's sum, and its own stretch share is what it gets, at every bar width. The
    // text is elided into whatever that comes to (updateStatusText below).
    //
    // Ignored must NOT be paired with setMaximumWidth — the two pull opposite ways and
    // the combination lays the row out on top of itself (see CLAUDE.md, the Filters
    // pane's context spinners). A stretch share is the whole mechanism here.
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    row->addWidget(m_status, kStatusStretch);

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
// The report as it was given, not as the label happens to be rendering it — which is
// elided to whatever width the bar has (updateStatusText below).
QString FindBar::status() const { return m_statusText; }
bool FindBar::regex() const { return m_regex->isChecked(); }
bool FindBar::caseSensitive() const { return m_case->isChecked(); }

void FindBar::activate()
{
    // Whatever the last search reported is about a query that is about to be replaced,
    // and a stale "3 of 47" over a fresh empty box is a lie.
    setStatus(QString());
    reveal();
    // Again, unconditionally: reveal() is a no-op on a bar that is already open, and
    // Ctrl+F on an open bar still means "give me the box to type in".
    m_edit->setFocus();
    m_edit->selectAll();
}

void FindBar::reveal()
{
    // isHidden() and not isVisible(): the question is whether the bar is MEANT to be on
    // screen, which is what hide() and show() set, and isVisible() is additionally false
    // for the whole of a window that has not been shown yet.
    if (!isHidden())
        return; // already open — selecting the query here would arm the reader's next
                // keystroke to destroy the very text they are stepping through, and
                // moving the focus would take it off whatever they had put it on.
    show();
    m_edit->setFocus(); // so Escape closes it — see the header
}

void FindBar::setStatus(const QString &text)
{
    m_statusText = text;
    updateStatusText();
}

void FindBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // The layout is applied before this arrives (QLayout filters the resize on its own
    // parent), so the label's width is already the new one.
    updateStatusText();
}

// "The column elides; the tooltip does not" — the house rule the log table and the
// highlighter list already follow. The label's cell is settled by the bar's width alone,
// so a long report is cut to fit rather than allowed to push the controls about, and
// only a report that was actually cut short offers the full text on hover.
//
// It is cut in the MIDDLE, for the reason a crowded tab label is (SPEC.md §5a): both
// ends of this wording carry something — `1 of 7` at the front, `wrapped to the top` at
// the back — and eliding from the right takes away the wrap note, which is the half the
// reader does not already know.
void FindBar::updateStatusText()
{
    const int width = m_status->width();
    if (m_statusText.isEmpty() || width <= 0) {
        m_status->setText(m_statusText);
        m_status->setToolTip(QString());
        return;
    }
    const QString shown = m_status->fontMetrics().elidedText(m_statusText, Qt::ElideMiddle, width);
    m_status->setText(shown);
    m_status->setToolTip(shown == m_statusText ? QString() : m_statusText);
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
