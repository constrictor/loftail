// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FindBar.h"

#include "UiColors.h"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
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

// Typing is a BURST, and a search is not free. MainWindow::runFind() walks the visible
// rows for the hit and then counts the rest of them for the "3 of 47" report, and that
// count decodes every visible record's text over every visible column (invariant #1) —
// which is why it is bounded and reports a floor ("530+") on any log big enough to reach
// the bound (ARCHITECTURE.md §7.1.3). Run once per keystroke, the whole of it is latency
// the reader feels in the very box they are typing into, so the bound had to be small
// enough to hide inside a keypress and the total was a floor far sooner than it needed
// to be.
//
// So the query box coalesces, on FilterPane's rule and for FilterPane's reason: the
// debounce engages only once a search has MEASURED ITSELF slow, so a log small enough to
// search on the keystroke still does, and pays no added latency for a bound it was never
// going to hit. Nothing else in the bar is debounced — Enter, the two arrow buttons and
// the option checkboxes are each one deliberate gesture rather than a stream, and a
// gesture that answered 150 ms later would read as a dropped keypress.
//
// The threshold has to sit BELOW what a search on such a log costs or the coalescing
// never engages at all, which is the one way these two constants are coupled to
// MainWindow's kFindTallyMs: raise that bound without raising this one and every search
// measures fast by construction.
constexpr qint64 kSearchDebounceThresholdMs = 40;
constexpr int    kSearchDebounceMs = 150;

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

    // "Highlight": take what is in the box and make a highlight rule of it, on the
    // message-text axis, with this bar's own Regex and Case options (SPEC.md §5, §7).
    // Find and highlighting already share the query language — the two read the same
    // TextMatcher — so the gesture a reader makes after finding the thing they were
    // looking for a third time is one press rather than a trip to another pane to retype
    // it. Hidden unless somebody asks for it (setHighlightVisible): the config editor
    // shares this bar and has nothing to put a rule on.
    //
    // It sits with the two arrow buttons rather than after the checkboxes: those are
    // the row's ACTIONS and this is a third, while Regex and Case are options that all
    // three of them read. Not auto-raised, for the reason the Filters pane records — a
    // frameless button carrying only a word reads as a caption, and the frame is what
    // says "press this".
    m_highlight = new QToolButton(this);
    m_highlight->setObjectName(QStringLiteral("findHighlight"));
    m_highlight->setText(tr("Highlight"));
    m_highlight->setToolTip(tr("Add a highlight rule matching this search"));
    m_highlight->hide();
    row->addWidget(m_highlight);

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

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(kSearchDebounceMs);
    // fromStart, exactly as the immediate path is: what is owed here is a query EDIT, and
    // an edited query has to be searched for from the top or the first match above the
    // cursor is skipped.
    connect(m_searchTimer, &QTimer::timeout, this, [this] { emitSearch(true, true); });

    // Typing (or toggling an option) restarts the search from the top so the first
    // match is found without needing to press Enter twice. Typing goes through the
    // debounce and the rest do not — see the constants above.
    connect(m_edit, &QLineEdit::textChanged, this, [this](const QString &) { scheduleSearch(); });
    connect(m_edit, &QLineEdit::returnPressed, this, [this] { emitSearch(true, false); });
    connect(m_regex, &QCheckBox::toggled, this, [this](bool) { emitSearch(true, true); });
    connect(m_case, &QCheckBox::toggled, this, [this](bool) { emitSearch(true, true); });
    connect(next, &QToolButton::clicked, this, [this] { emitSearch(true, false); });
    connect(prev, &QToolButton::clicked, this, [this] { emitSearch(false, false); });
    connect(close, &QToolButton::clicked, this, [this] { hide(); emit closed(); });
    connect(m_highlight, &QToolButton::clicked, this, [this] { emit highlightRequested(); });

    hide();
}

void FindBar::scheduleSearch()
{
    if (m_lastSearchMs < kSearchDebounceThresholdMs) {
        emitSearch(true, true);
        return;
    }
    m_searchTimer->start(); // restarts the wait: the burst is not over yet
}

void FindBar::emitSearch(bool forward, bool fromStart)
{
    // This search supersedes whatever a pending edit was going to ask for: the query it
    // would search is the one in the box, which is the one being searched right now, and
    // letting it fire afterwards would restart a Find Next from the top. Every route out
    // of this bar goes through here, so no caller has to remember it.
    cancelPendingSearch();

    // Time the search itself, not the emit's own overhead: findRequested is a direct
    // connection in both owners (DocumentView forwards it to MainWindow::runFind,
    // ConfigView handles it), so this returns once the search has actually run.
    QElapsedTimer clock;
    clock.start();
    emit findRequested(forward, fromStart);
    m_lastSearchMs = clock.elapsed();
}

void FindBar::cancelPendingSearch()
{
    if (m_searchTimer)
        m_searchTimer->stop();
}

bool FindBar::searchPending() const
{
    return m_searchTimer && m_searchTimer->isActive();
}

void FindBar::hideEvent(QHideEvent *event)
{
    // A search owed to text in a box that is no longer on screen has nowhere to report
    // into and nothing to mark — see the header.
    cancelPendingSearch();
    QWidget::hideEvent(event);
}

QString FindBar::pattern() const { return m_edit->text(); }
// The report as it was given, not as the label happens to be rendering it — which is
// elided to whatever width the bar has (updateStatusText below).
void FindBar::setPlaceholderText(const QString &text)
{
    m_edit->setPlaceholderText(text);
}

void FindBar::setHighlightVisible(bool visible)
{
    m_highlight->setVisible(visible);
}

QString FindBar::describeMatch(int index, int total, bool complete, bool wrapped,
                               bool forward)
{
    QString status;
    if (index <= 0) {
        // Counting stopped short of this hit, so there is no position to give — but the
        // total that WAS reached still answers the question the reader asked the bar,
        // which is how much there is of what they searched for. Saying only "match"
        // threw that away on exactly the logs big enough to need it.
        status = complete ? tr("%1 matches").arg(total) : tr("%1+ matches").arg(total);
    } else if (complete) {
        status = tr("%1 of %2").arg(index).arg(total);
    } else {
        status = tr("%1 of %2+").arg(index).arg(total); // at least that many
    }
    if (wrapped) {
        status = forward ? tr("%1, wrapped to the top").arg(status)
                         : tr("%1, wrapped to the bottom").arg(status);
    }
    return status;
}

QString FindBar::describeNoMatch()
{
    // The same "x of y" the successful report speaks, with both numbers zero. Nothing
    // matched anywhere, so the total is not a bound and not an estimate — it is 0.
    return tr("%1 of %2").arg(0).arg(0);
}

QString FindBar::status() const { return m_statusText; }
bool FindBar::regex() const { return m_regex->isChecked(); }
bool FindBar::caseSensitive() const { return m_case->isChecked(); }

void FindBar::activate()
{
    // Whatever the last search reported is about a query that is about to be replaced,
    // and a stale "3 of 47" over a fresh empty box is a lie.
    setStatus(QString());
    // And any search still owed to the standing query, which is about to be selected for
    // replacement: a report arriving 150 ms into the reader's first keystroke would land
    // on top of the blank box this gesture just gave them.
    cancelPendingSearch();
    // And the red with it: the report and the cue are two halves of one answer about a
    // query that is now selected for replacement, so leaving one behind is a field that
    // says "this found nothing" over a bar that says nothing at all.
    setQueryFailed(false);
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

void FindBar::setQueryFailed(bool failed)
{
    if (m_queryFailed == failed)
        return; // this runs on every keystroke; a palette write relays the field out
    m_queryFailed = failed;
    // Start from the field's OWN palette rather than a default-constructed one, for the
    // reason AxisEditor's invalid-regex cue does: ensureReadablePlaceholder() has already
    // repaired the placeholder colour in it, and a fresh QPalette would silently drop
    // that repair every time a search failed.
    QPalette pal = m_edit->palette();
    pal.setColor(QPalette::Text, failed ? errorColor(pal)
                                        : qApp->palette(m_edit).color(QPalette::Text));
    m_edit->setPalette(pal);
}

bool FindBar::queryFailed() const { return m_queryFailed; }

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
                emitSearch(false, false);
                return true; // consumed, so no returnPressed() and no forward search
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace loftail
