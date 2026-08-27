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

#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLineEdit;
class QLabel;
class QToolButton;
QT_END_NAMESPACE

namespace loftail {

// M4 — the Find bar (SPEC.md §5). Distinct from filtering: Find moves the cursor
// and leaves every record visible; it walks whatever is CURRENTLY visible (the
// filtered subset when a filter is active). It shares the message-matching code
// with the filter (loftail::TextMatcher) but changes no filter state.
//
// The bar is pure UI: it emits findRequested() with the query and direction, and
// MainWindow does the actual walk over the model's visible rows via Find::search().
class FindBar : public QWidget
{
    Q_OBJECT

public:
    explicit FindBar(QWidget *parent = nullptr);

    QString pattern() const;
    // The last report given to setStatus(), in full. The label itself shows an elided
    // rendering of it, so this — not the label's text — is what the report IS.
    QString status() const;
    bool regex() const;
    bool caseSensitive() const;

    // Show the bar and focus the text field (Ctrl+F). Clears the last report and
    // selects the standing query for replacement — the reader asked for a box to type
    // into, so what the previous query found is about to stop being true.
    void activate();
    // Put the bar on screen and focus it, touching NOTHING else — not the status, not
    // the query, not the selection. Every report runFind() makes goes into this bar's
    // own label, so a search asked for from the table (F3) has to reveal it BEFORE it
    // writes, or the answer lands where nobody can read it (SPEC.md §5). The focus is
    // not decoration: Escape is handled in FindBar::keyPressEvent and the bar is a
    // SIBLING of the table under DocumentView, not an ancestor, so a bar revealed with
    // the caret left in the table could be closed only with the ✕ button.
    void reveal();
    // Say, in the query field itself, that the standing text found nothing — the text
    // goes red (SPEC.md §5). It is the field and not the label because that is where the
    // reader is looking when they type: a failing search is a property of what is in the
    // box, so the box is what wears it, which leaves the label free to do nothing but
    // count. Cleared by every search that does not fail, so a query edited into one that
    // matches loses the red on the same keystroke.
    void setQueryFailed(bool failed);
    // Whether the last search failed, as set above. The palette is where it SHOWS, and a
    // test reading a QPalette colour back is asserting on the theme rather than on this;
    // this is the state itself.
    bool queryFailed() const;

    // Report the outcome of the last search in the bar's own status label (SPEC.md §5):
    // which match of how many, whether the search wrapped, or why there was nothing to
    // go to. The bar's label rather than the window's status bar, because that one is
    // rewritten on every ingest tick and tab switch. An empty string clears it.
    void setStatus(const QString &text);

    // The bar searches records in a log and text in a config file, and the two say
    // "Search visible records..." and something else. Default unchanged, so every
    // existing caller and every existing test is untouched.
    void setPlaceholderText(const QString &text);

    // Whether the bar offers "Highlight" — turning the standing query into a highlight
    // rule on the message-text axis (SPEC.md §5, §7).
    //
    // OFF by default, and the default is the load-bearing half: this bar is shared with
    // the config-file editor, which has no highlight rules and nothing to add one to, so
    // a button that shipped visible would be a dead control there — and so would it be
    // for whatever third thing grows a Find bar next. The one place with somewhere to
    // put a rule (DocumentView) asks for it by name.
    void setHighlightVisible(bool visible);

    // The four sentences a search can end in, in one place.
    //
    // SHARED so the two things that use a Find bar cannot come to word the same outcome
    // differently — a reader stepping between a log tab and a config tab would otherwise
    // meet two vocabularies for one gesture. `index` <= 0 means the count never reached
    // the hit, which is the ordinary case on a large log and has no position to give;
    // `complete` false renders the total as a floor ("47+"), never as a fact.
    static QString describeMatch(int index, int total, bool complete, bool wrapped,
                                 bool forward);

    // What the label says when nothing matched: `0 of 0`, in the same vocabulary the
    // successful report uses, rather than a sentence of its own. The failure is told by
    // the query field going red, so the label's one job in every branch is the count —
    // and a search that matched nothing found exactly none, which is a fact and not an
    // estimate.
    static QString describeNoMatch();

signals:
    // forward=true for Find Next, false for Find Previous. `fromStart` restarts the
    // search from the top/bottom rather than the current cursor (used when the query
    // text itself changes).
    void findRequested(bool forward, bool fromStart);
    // The Highlight button. Carries nothing: the query, the regex flag and the case
    // option are all read back off this bar by whoever handles it, exactly as a search
    // reads them, so a rule can never come to disagree with the search that produced it.
    void highlightRequested();
    void closed();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    // Re-elides the status into the cell the bar's width gives it — see setStatus().
    void resizeEvent(QResizeEvent *event) override;
    // Watches the query field so Shift+Enter can mean "search backwards" (SPEC.md §5).
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Cuts m_statusText into the label's own width, and puts the full text on the
    // tooltip only when it did not fit. The label's width never depends on the text:
    // it is a stretch share of the bar, so the controls beside it cannot move.
    void updateStatusText();

    QString    m_statusText;
    bool       m_queryFailed = false;
    QLineEdit  *m_edit = nullptr;
    QToolButton *m_highlight = nullptr;
    QCheckBox *m_regex = nullptr;
    QCheckBox *m_case = nullptr;
    QLabel    *m_status = nullptr;
};

} // namespace loftail
