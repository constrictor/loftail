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

#include "RunPane.h"

#include "MessageLabel.h"
#include "UiColors.h"

#include "Document.h"
#include "Highlight.h"
#include "Palette.h"
#include "Priority.h"
#include "Record.h"

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QStringList>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimeZone>
#include <QVBoxLayout>

namespace loftail {

namespace {

// Geometry of a run row, all in device-independent pixels round text that is measured.
constexpr int kHMargin    = 4;   // row edge to text
constexpr int kVMargin    = 3;
constexpr int kLineGap    = 1;   // between the three lines
constexpr int kChipPadH   = 5;   // inside a count chip, either side of its text
constexpr int kChipGap    = 4;   // between two chips
constexpr int kChipRadius = 3;

// The two colours a level's chip is drawn in: the very pair HighlighterSet::defaults()
// gives that level in the log itself (SPEC.md §7). Read out of defaults() rather than
// written down again, so the pane cannot come to disagree with the colours the records
// are actually wearing — and taken as a PAIR, background and its paired foreground,
// which is what makes a chip readable in both themes by construction (Palette.h) where
// a background used as text colour would be a pale Soft Amber on a light theme.
//
// Deliberately the DEFAULTS and not this document's rules: a count is a property of the
// run, and a reader who has recoloured ERROR green has not thereby made these numbers
// mean something else.
struct ChipColours
{
    QColor bg;
    QColor fg;
};

ChipColours chipColoursFor(Priority level, bool dark)
{
    static const HighlighterSet kDefaults = HighlighterSet::defaults();
    for (const HighlightRule &r : kDefaults.rules) {
        if (r.match.priorityEnabled && r.match.minPriority == level)
            return { HighlightPalette::color(r.background, dark),
                     HighlightPalette::color(r.foreground, dark) };
    }
    return {};
}

// The three levels a run reports, loudest first — the order the default rules are in,
// for the reason they are in it (the ERROR rule matches FATAL too).
struct LevelColumn
{
    Priority level;
    int      role;
};
constexpr LevelColumn kLevels[] = {
    { Priority::Fatal, RunPane::kRunFatalRole },
    { Priority::Error, RunPane::kRunErrorRole },
    { Priority::Warn,  RunPane::kRunWarnRole },
};

// "3 ERROR". The level word is priorityName(), which is NOT translated — it must
// round-trip against the log text (invariant #4) and is the same token the Priority
// column shows — so the whole chip is a number and a token and needs no tr().
QString chipText(Priority level, int count)
{
    return QStringLiteral("%1 %2").arg(count).arg(QString(priorityName(level)));
}

// The span of instants a run covers, as the pane's second line. The end is dropped to
// a bare time when it falls on the run's own start date, which is the ordinary case and
// what keeps this line inside a dock's width without eliding.
QString runTimesText(const Document::RunStats &stats, const QTimeZone &zone)
{
    // Qt date codes are format, not prose — never translated (ARCHITECTURE.md §9.1).
    static const QString kStamp = QStringLiteral("yyyy-MM-dd HH:mm:ss");
    static const QString kTime  = QStringLiteral("HH:mm:ss");

    // "here" and not "in this run": the same line speaks for "All runs" too, where the
    // range is the whole file.
    if (stats.firstTimestamp == Record::kNoTimestamp)
        return RunPane::tr("no timestamps here");

    const QDateTime from = QDateTime::fromMSecsSinceEpoch(stats.firstTimestamp, zone);
    const QDateTime to   = QDateTime::fromMSecsSinceEpoch(stats.lastTimestamp, zone);
    if (from == to)
        return from.toString(kStamp);
    return from.toString(kStamp) + QStringLiteral(" ") + QChar(0x2013) + QStringLiteral(" ")
        + to.toString(from.date() == to.date() ? kTime : kStamp);
}

// Put one row's three lines on an item: the roles the delegate composes, the one-line
// DisplayRole fallback under them, and the tooltip that carries what the row cannot fit.
// EVERY row goes through here — "All runs" and the runs alike — because the three lines
// mean the same thing on both and a second copy is how they start disagreeing about what
// a span or a count is. `extraTip` is what only that kind of row can say.
void fillRow(QListWidgetItem *item, const QString &title, const Document::RunStats &stats,
             const QTimeZone &zone, const QStringList &extraTip)
{
    const QString times = runTimesText(stats, zone);
    item->setData(RunPane::kRunTitleRole, title);
    item->setData(RunPane::kRunTimesRole, times);
    item->setData(RunPane::kRunFatalRole, stats.fatal);
    item->setData(RunPane::kRunErrorRole, stats.error);
    item->setData(RunPane::kRunWarnRole, stats.warn);
    // A plain one-line rendering under Qt::DisplayRole as well. The delegate clears it
    // before the style sees it, so nothing draws twice — but it is what keyboard
    // type-ahead searches, what an accessibility client reads out and what a test
    // asserting on this list can get at.
    item->setText(QStringLiteral("%1 - %2").arg(title, times));

    QStringList tip;
    tip << title << times;
    QStringList counts;
    if (stats.fatal > 0)
        counts << chipText(Priority::Fatal, stats.fatal);
    if (stats.error > 0)
        counts << chipText(Priority::Error, stats.error);
    if (stats.warn > 0)
        counts << chipText(Priority::Warn, stats.warn);
    tip << (counts.isEmpty() ? RunPane::tr("nothing at WARN or above")
                             : counts.join(QStringLiteral(", ")));
    tip += extraTip;
    item->setToolTip(tip.join(QChar('\n')));
}

// Draws a row as three lines — its name, the span it covers, what is outstanding in it.
// Every row a document is behind gets them, "All runs" included: it answers the same
// question about a wider target, and a mode row that said only its own name was the one
// row in the list a reader could learn nothing from. What still falls through to the base
// class is a row with NO title role, which is "All runs" with no document bound — there
// is nothing there to report a span or a count for. The separator is neither, and is
// drawn by this delegate as the one thing it is: a line.
class RunItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.data(RunPane::kSeparatorRole).isValid())
            return {0, separatorHeight(option)};
        if (!index.data(RunPane::kRunTitleRole).isValid())
            return QStyledItemDelegate::sizeHint(option, index);
        QStyleOptionViewItem o(option);
        initStyleOption(&o, index);
        const int lh = lineHeight(o);
        return {naturalWidth(o, index), 3 * lh + 2 * kLineGap + 2 * kVMargin};
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (index.data(RunPane::kSeparatorRole).isValid()) {
            paintSeparator(p, option);
            return;
        }

        const QVariant title = index.data(RunPane::kRunTitleRole);
        if (!title.isValid()) {
            QStyledItemDelegate::paint(p, option, index);
            return;
        }

        QStyleOptionViewItem o(option);
        initStyleOption(&o, index);
        // The style still draws the row — its selection fill, its hover, its focus
        // rect — and only the TEXT is taken off it, because everything below this is
        // three lines the style has no way to lay out. Painting the background by hand
        // instead is how a list stops matching the platform it is in.
        o.text.clear();
        const QWidget *w = o.widget;
        QStyle *style = w ? w->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &o, p, w);

        const bool selected = (o.state & QStyle::State_Selected) != 0;
        const QColor fg = selected ? o.palette.color(QPalette::HighlightedText)
                                   : o.palette.color(QPalette::Text);
        // Muted secondary text says "this is about the row, not the row's name" — but
        // not over a selection fill, where a colour derived from the unselected palette
        // is the one thing guaranteed to be unreadable.
        const QColor secondary = selected ? fg : mutedColor(o.palette);

        const QRect body = o.rect.adjusted(kHMargin, kVMargin, -kHMargin, -kVMargin);
        const int lh = lineHeight(o);

        QFont titleFont = o.font;
        titleFont.setBold(true);

        p->save();
        p->setFont(titleFont);
        p->setPen(fg);
        const QRect r1(body.left(), body.top(), body.width(), lh);
        p->drawText(r1, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(titleFont).elidedText(title.toString(), Qt::ElideRight,
                                                       body.width()));

        p->setFont(o.font);
        p->setPen(secondary);
        const QFontMetrics fm(o.font);
        const QRect r2(body.left(), r1.bottom() + 1 + kLineGap, body.width(), lh);
        p->drawText(r2, Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(index.data(RunPane::kRunTimesRole).toString(),
                                  Qt::ElideRight, body.width()));

        drawCounts(p, QRect(body.left(), r2.bottom() + 1 + kLineGap, body.width(), lh),
                   index, o, secondary);
        p->restore();
    }

private:
    // The separator's whole height, and it is derived from the font rather than written
    // down: everything else in this list is measured that way, so a written-down gap is
    // the one part of the row that would not grow with the interface font.
    static int separatorHeight(const QStyleOptionViewItem &o)
    {
        return qMax(5, QFontMetrics(o.font).height() / 2);
    }

    // A hairline across the row, inset by the same margin the text keeps, in a colour
    // taken from the palette — mid between Base and Text — so it lands on a dark theme
    // as well as a light one and cannot be the invisible line QPalette::Mid is free to
    // be. No zebra band and no selection fill: the separator is not a row a reader can
    // reach, so drawing it as one would be a fourth kind of entry in a list of three.
    static void paintSeparator(QPainter *p, const QStyleOptionViewItem &o)
    {
        const QRect r = o.rect;
        const int y = r.top() + r.height() / 2;
        p->save();
        p->setPen(QPen(mutedColor(o.palette), 1));
        p->drawLine(r.left() + kHMargin, y, r.right() - kHMargin, y);
        p->restore();
    }

    // One pitch for all three lines, taken from the taller of the two faces so the
    // bold title and the plain lines under it cannot disagree — the same rule the log
    // view's own line pitch keeps, one widget over.
    static int lineHeight(const QStyleOptionViewItem &o)
    {
        QFont bold = o.font;
        bold.setBold(true);
        return qMax(QFontMetrics(o.font).height(), QFontMetrics(bold).height());
    }

    static int naturalWidth(const QStyleOptionViewItem &o, const QModelIndex &index)
    {
        QFont bold = o.font;
        bold.setBold(true);
        int w = QFontMetrics(bold).horizontalAdvance(index.data(RunPane::kRunTitleRole).toString());
        const QFontMetrics fm(o.font);
        w = qMax(w, fm.horizontalAdvance(index.data(RunPane::kRunTimesRole).toString()));
        int chips = 0;
        for (const LevelColumn &l : kLevels) {
            const int n = index.data(l.role).toInt();
            if (n > 0)
                chips += fm.horizontalAdvance(chipText(l.level, n)) + 2 * kChipPadH + kChipGap;
        }
        return qMax(w, chips) + 2 * kHMargin;
    }

    static void drawCounts(QPainter *p, const QRect &row, const QModelIndex &index,
                           const QStyleOptionViewItem &o, const QColor &secondary)
    {
        const bool dark = isDarkPalette(o.palette);
        const QFontMetrics fm(o.font);
        int x = row.left();
        bool any = false;
        for (const LevelColumn &l : kLevels) {
            const int n = index.data(l.role).toInt();
            if (n <= 0)
                continue;
            const QString text = chipText(l.level, n);
            const QRect chip(x, row.top(), fm.horizontalAdvance(text) + 2 * kChipPadH,
                             row.height());
            // A chip that would not fit whole is not drawn at all: half a count is a
            // wrong count, and the tooltip carries every one of them regardless.
            if (chip.right() > row.right())
                break;
            any = true;
            const ChipColours c = chipColoursFor(l.level, dark);
            p->setRenderHint(QPainter::Antialiasing, true);
            p->setPen(Qt::NoPen);
            p->setBrush(c.bg);
            p->drawRoundedRect(chip, kChipRadius, kChipRadius);
            p->setRenderHint(QPainter::Antialiasing, false);
            p->setBrush(Qt::NoBrush);
            p->setPen(c.fg);
            p->drawText(chip, Qt::AlignCenter, text);
            x = chip.right() + 1 + kChipGap;
        }
        if (!any) {
            p->setPen(secondary);
            p->drawText(row, Qt::AlignLeft | Qt::AlignVCenter,
                        RunPane::tr("nothing at WARN or above"));
        }
    }
};

} // namespace

RunPane::RunPane(QWidget *parent) : QWidget(parent)
{
    buildUi();
    setDocument(nullptr);
}

void RunPane::buildUi()
{
    auto *root = new QVBoxLayout(this);

    auto *box = new QGroupBox(tr("Run start"), this);
    auto *v = new QVBoxLayout(box);

    v->addWidget(new QLabel(
        tr("Regexp marking where each run begins (matched against the\n"
                       "whole log line). Leave empty to view the entire file."),
        box));

    m_patternEdit = new QLineEdit(box);
    m_patternEdit->setObjectName(QStringLiteral("runStartPattern")); // test contract, never translated
    m_patternEdit->setPlaceholderText(tr("e.g. Application starting"));
    ensureReadablePlaceholder(m_patternEdit);
    m_patternEdit->setClearButtonEnabled(true);
    v->addWidget(m_patternEdit);

    auto *opts = new QHBoxLayout;
    m_regex = new QCheckBox(tr("Regex"), box);
    m_regex->setObjectName(QStringLiteral("runStartRegex")); // test contract, never translated
    m_case = new QCheckBox(tr("Case sensitive"), box);
    m_case->setObjectName(QStringLiteral("runStartCase")); // test contract, never translated
    opts->addWidget(m_regex);
    opts->addWidget(m_case);
    opts->addStretch(1);
    m_apply = new QPushButton(tr("Apply"), box);
    m_apply->setObjectName(QStringLiteral("runApply")); // test contract, never translated
    opts->addWidget(m_apply);
    v->addLayout(opts);

    // This pane is the only one with an Apply button — the Filters and Highlighters panes
    // act as the user types — so it says why, next to the button that is the difference.
    // Splitting a log into runs re-partitions and re-applies the whole view, which is not
    // something to do per keystroke; but by the time a reader reaches this pane they have
    // learned everywhere else that an edit takes effect as it is made, so an unpressed
    // Apply otherwise reads as a pane that has stopped responding.
    m_applyNote = new MessageLabel(box);
    m_applyNote->setObjectName(QStringLiteral("runApplyNote")); // test contract, never translated
    v->addWidget(m_applyNote);

    m_info = new QLabel(box);
    m_info->setObjectName(QStringLiteral("runInfo")); // test contract, never translated
    m_info->setWordWrap(true);
    v->addWidget(m_info);

    root->addWidget(box);

    auto *runBox = new QGroupBox(tr("Run"), this);
    auto *rv = new QVBoxLayout(runBox);
    m_runList = new QListWidget(runBox);
    m_runList->setObjectName(QStringLiteral("runList"));
    // A run row is three lines and neither of the two rows above it is, so it will not
    // fit a dock's width either. Eliding is the right answer rather than a horizontal
    // scrollbar — the head of every line is the part that identifies the run, and the
    // whole of it is one hover away — and switching the scrollbar off is what makes the
    // view elide instead of widening past the pane (the width trap the Filters pane
    // records). The delegate elides its own three lines; this setting is what covers
    // the two one-liners above them, which the base class still draws.
    m_runList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_runList->setTextElideMode(Qt::ElideRight);
    // NOT uniform: with uniform sizes Qt measures the FIRST item and gives every other
    // row that height, and the first item can be a single-line mode row — which
    // would clip every run row to a third of itself.
    m_runList->setUniformItemSizes(false);
    // A run row is three lines tall and the rows carry no separator, so with a single
    // fill behind all of them a list of runs reads as one block of text in which it is
    // the READER who has to work out where one run's three lines stop and the next
    // one's begin. The band says it instead, and it is the same answer the log table
    // gives one widget over for the same reason (a record there is several lines too).
    // The delegate needs nothing: it hands the row's fill to the style, which is what
    // reads the Alternate feature the view sets here.
    m_runList->setAlternatingRowColors(true);
    applyZebraColour();
    m_runList->setItemDelegate(new RunItemDelegate(m_runList));
    rv->addWidget(m_runList, 1);
    // The run box is the one thing here that takes the spare height, and there is
    // deliberately NO trailing addStretch: a stretch below would compete for the same
    // pixels and pin the list to its floor with an empty gap under it.
    root->addWidget(runBox, 1);

    // APPLY AND RETURN ARE THE ONLY ROUTE OUT OF THIS PANE. Both boxes were once wired
    // to emitPattern as well, which meant ticking Regex applied whatever half-typed
    // pattern was standing in the field — re-splitting and re-reading the whole log, and
    // persisting that pattern into the log's settings node — for a gesture the user had
    // not pressed Apply for, and which threw away a pinned run on the way (SPEC.md §3a).
    // Nothing here may emit runStartChanged on an edit; a new control belongs on the
    // note below, not on this list.
    connect(m_apply, &QPushButton::clicked, this, &RunPane::emitPattern);
    connect(m_patternEdit, &QLineEdit::returnPressed, this, &RunPane::emitPattern);
    // NOTHING in this pane is applied as it is edited, so every editable control has to
    // say so — which is the whole of what these three connections do.
    connect(m_patternEdit, &QLineEdit::textChanged, this, &RunPane::updateApplyNote);
    connect(m_regex, &QCheckBox::toggled, this, &RunPane::updateApplyNote);
    connect(m_case, &QCheckBox::toggled, this, &RunPane::updateApplyNote);
    // currentRowChanged, not itemClicked: a list is walked with the arrow keys as well
    // as clicked, and only the current-row signal covers both. It fires on programmatic
    // changes too — including the clear() in rebuildRunList(), which emits row -1 — so
    // the m_populating guard is what keeps a repopulation from looking like a choice.
    connect(m_runList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_populating || row < 0)
            return;
        // The separator is not selectable, so it never arrives here — but a row that is
        // not a run must not be read as one, and this is the one place that arithmetic
        // is written down. Picking the BOTTOM run is not a case of its own: what makes
        // it follow is Document::selectRun(), which is also what makes the record menu
        // and a restored session mean the same thing by it.
        if (row == kAllRunsRow)
            emit runSelected(kAllRuns);
        else if (row >= kFirstRunRow)
            emit runSelected(row - kFirstRunRow);
    });
}

void RunPane::applyZebraColour()
{
    if (!m_runList)
        return;
    QPalette p = m_runList->palette();
    // Derived from Base and Text, neither of which this function ever writes, so
    // re-deriving it on a theme change reads the new theme rather than the last band.
    p.setColor(QPalette::AlternateBase, alternateRowColor(p));
    m_runList->setPalette(p);
}

void RunPane::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        applyZebraColour();
}

void RunPane::setDocument(Document *document)
{
    m_document = document;

    m_populating = true;
    if (m_document) {
        const TextMatcher &m = m_document->runStartMatcher();
        m_patternEdit->setText(m.pattern());
        m_regex->setChecked(m.isRegex());
        m_case->setChecked(m.caseSensitivity() == Qt::CaseSensitive);
    } else {
        m_patternEdit->clear();
        m_regex->setChecked(false);
        m_case->setChecked(false);
    }
    m_populating = false;

    const bool enabled = m_document != nullptr;
    m_patternEdit->setEnabled(enabled);
    m_regex->setEnabled(enabled);
    m_case->setEnabled(enabled);
    m_apply->setEnabled(enabled);
    m_runList->setEnabled(enabled);

    rebuildRunList();
}

void RunPane::refresh()
{
    rebuildRunList();
}

void RunPane::emitPattern()
{
    if (m_populating || !m_document)
        return;
    emit runStartChanged(m_patternEdit->text(), m_regex->isChecked(), m_case->isChecked());
}

void RunPane::rebuildRunList()
{
    m_populating = true;
    // refresh() runs on every live append, so the whole list is rebuilt while the user
    // is looking at it; keeping the scroll offset is what stops it jumping back to the
    // top each time a run's record count ticks over.
    const int scroll = m_runList->verticalScrollBar()->value();
    m_runList->clear();

    // "All runs" is not a run, so it is italic for the same reason AxisEditor's "Others"
    // is: it is the entry that says something ABOUT the list rather than naming a member
    // of it, and a log may well start a run with a line that reads like it.
    QFont modeFont = m_runList->font();
    modeFont.setItalic(true);

    // The list reads in FILE order: the whole file, then its runs oldest to newest. So
    // "All runs" opens the list and the runs follow it, with a rule drawn between them
    // where there are any — the whole file is a different kind of answer from any one
    // run, and a run row is three lines tall, so without the rule the top entry reads as
    // the first of a list it is not a member of.
    //
    // THE BOTTOM ROW IS BOTH THE NEWEST RUN AND "keep showing whichever run is newest":
    // there is no separate follow entry, because selecting the last run IS what asking
    // to follow means (SPEC.md §3a).
    auto *allRuns = new QListWidgetItem(tr("All runs"), m_runList);
    allRuns->setFont(modeFont);

    // "All runs" says what it will SHOW, in the three lines a run row uses, because it
    // resolves to a stretch of this log and a reader picking it is asking the same
    // question they ask of a run: what does it cover and is there anything wrong in it.
    // With no document there is nothing to report, so it stays the one-line italic entry
    // the pane opens with rather than growing a span and a count made of nothing.
    const QTimeZone zone = m_document ? m_document->displayZone() : QTimeZone::utc();
    const Document::RunStats fileStats =
        m_document ? m_document->fileStats() : Document::RunStats();

    if (m_document) {
        fillRow(allRuns, tr("All runs"), fileStats, zone,
                { tr("Show the whole file, with no run restriction."),
                  tr("%n record(s)", nullptr, int(m_document->index().records.size())) });

        const QVector<Document::Run> &runs = m_document->runs();
        if (!runs.isEmpty()) {
            // Not selectable and not enabled: a rule is not somewhere to be, and the
            // flags are what keep the arrow keys stepping straight over it as well as
            // the pointer — so kFirstRunRow is reachable from row 0 with one key press,
            // exactly as it was when the rows were adjacent.
            auto *rule = new QListWidgetItem(m_runList);
            rule->setData(kSeparatorRole, true);
            rule->setFlags(Qt::NoItemFlags);
        }
        for (int i = 0; i < runs.size(); ++i) {
            const Document::Run &r = runs.at(i);
            const Document::RunStats stats = m_document->runStats(i);
            // The ordinal, not a 1-based count: it is the run's identity everywhere else
            // in the application — what runSelected() carries, what the session stores —
            // and a list that numbered its rows differently from the thing they select
            // would be one more mapping for a reader to hold.
            const QString title = r.isPreamble ? tr("Before first run") : tr("Run %1").arg(i);

            QString snippet = r.firstLine.simplified();
            if (snippet.size() > 60)
                snippet = snippet.left(59) + QChar(0x2026); // ellipsis
            // The tooltip carries the whole of it — including the two things the three
            // lines have no room for, the run's record count and its first line — and,
            // on the last run alone, what picking it additionally means.
            QStringList tip;
            tip << tr("%n record(s)", nullptr, m_document->runRecordCount(i));
            if (!r.isPreamble && !snippet.isEmpty())
                tip << snippet;
            if (i == runs.size() - 1)
                tip << tr("The newest run. Selecting it keeps the view on whichever run "
                          "is newest, switching to each new one as it starts.");

            auto *item = new QListWidgetItem(m_runList);
            fillRow(item, title, stats, zone, tip);
        }
    } else {
        allRuns->setToolTip(tr("Show the whole file, with no run restriction."));
    }

    if (m_document) {
        const QVector<Document::Run> &runs = m_document->runs();
        // The selection is the document's ORDINAL and nothing else. Following is no
        // longer a row of its own, so there is nothing here to tell apart: a followed
        // document has already re-pointed its ordinal at the run that is last now, and
        // that run's row is the one to light.
        const int sel = m_document->selectedRun();
        m_runList->setCurrentRow(sel >= 0 ? sel + kFirstRunRow : kAllRunsRow);
        m_runList->verticalScrollBar()->setValue(scroll);

        // Status line under the pattern field.
        const TextMatcher &m = m_document->runStartMatcher();
        if (m.pattern().isEmpty())
            m_info->setText(tr("No run-start pattern — viewing the whole file."));
        else if (!m.isValid())
            m_info->setText(tr("Invalid regex — nothing matched."));
        else if (runs.isEmpty())
            m_info->setText(tr("Pattern matched no run starts."));
        else
            m_info->setText(tr("%1 run(s) detected.").arg(runs.size()));
    } else {
        // "All runs" is the only row there is with no log behind it, and it is what a
        // document with no runs detected resolves to as well, so the pane opens on the
        // same row either way.
        m_runList->setCurrentRow(kAllRunsRow);
        // NOT cleared. The list below still shows its one mode row — an entry that reads
        // as something to click — so the one widget here built to explain the list must
        // not fall silent exactly when there is nothing behind it to explain.
        m_info->setText(tr("No log is open. Open one to split it into runs."));
    }
    m_populating = false;
    updateApplyNote();
}

void RunPane::updateApplyNote()
{
    if (m_populating)
        return;

    // "Unapplied" is measured against the matcher that is actually in force, never
    // against a remembered copy of the field: the pattern can change from under this
    // pane — a rebind to another log, a session restore, a settings node edited in
    // Preferences — and every one of those routes ends at rebuildRunList().
    //
    // All THREE clauses are load-bearing, and the two box clauses most of all: a tick is
    // reported by nothing but this note, so dropping either one leaves a control whose
    // change the pane never mentions and Apply then silently acts on.
    bool pending = false;
    if (m_document) {
        const TextMatcher &m = m_document->runStartMatcher();
        pending = m_patternEdit->text() != m.pattern()
            || m_regex->isChecked() != m.isRegex()
            || m_case->isChecked() != (m.caseSensitivity() == Qt::CaseSensitive);
    }

    const int state = pending ? 1 : 0;
    if (state == m_noteState)
        return;
    m_noteState = state;

    m_applyNote->setText(pending
        ? tr("Edited — press Apply to re-read the runs.")
        : tr("The run-start pattern takes effect when you press Apply."));
    // Both colours come from the palette rather than being picked, so the line lands on
    // a dark theme as well as a light one: muted while it is only an explanation, and
    // the caution amber once it is describing a difference the user has to resolve —
    // which is what a caution is, something that still works and is not what was asked
    // for. It is a colour change and not a bold, because the line does not move.
    m_applyNote->setStyleSheet(QStringLiteral("color: %1;")
        .arg((pending ? warningColor(palette()) : mutedColor(palette())).name()));
}

} // namespace loftail
