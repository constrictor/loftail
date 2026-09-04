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

#include "WelcomeView.h"

#include "MessageLabel.h"
#include "UiColors.h"

#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>
#include <QVariant>

namespace loftail {

namespace {

// The title, relative to the interface font. Derived rather than a point size written
// down: the reference desktop, a HiDPI screen and a user who has enlarged their desktop
// font all resolve a different base, and a constant is right for exactly one of them.
constexpr qreal kTitleScale = 1.8;

// How wide the column of content is allowed to grow, in characters of the interface
// font. A welcome screen stretched across the whole of a wide window reads as a layout
// fault rather than as a page; Kate's own is a centred column for the same reason.
constexpr int kContentChars = 84;

// The column's share of the width against one of the two spacers either side of it. It
// is what the maximum above is reached BY; the spacers only take over once there is more
// width than the column is allowed to use.
constexpr int kColumnStretch = 10;

// How many rows of either list are on screen before it scrolls. Both take the same
// number so neither reads as the more important one.
constexpr int kVisibleRows = 6;

// Between the empty-state message and the edge of the viewport it is centred in.
constexpr int kEmptyInset = 8;

// Between a section's button column and its list.
constexpr int kSectionGap = 12;

// What a row stands for. Three plain strings rather than the Entry itself in a
// QVariant: these are the only fields an activation needs, and a row carrying a
// registered metatype is a metatype to keep registered.
constexpr int kAddressRole = Qt::UserRole;
constexpr int kHostRole    = Qt::UserRole + 1;
constexpr int kPathRole    = Qt::UserRole + 2;

// How tall a row of either list actually is, under this style and this font. MEASURED
// from a prototype rather than derived from the font's height: an item view spends the
// style's own item margin above and below the text, which is 2 px a side under Fusion and
// more under Breeze, so a written-down guess shows five and a half rows on one desktop
// and six and a bit on the next.
int measuredRowHeight(QListWidget *list)
{
    list->addItem(QStringLiteral("Ag"));
    const int height = list->sizeHintForRow(0);
    delete list->takeItem(0);
    return qMax(1, height);
}

// Put the empty-state message over the list it belongs to. Called on every refresh as
// well as on every resize, because the list may never have been laid out when the first
// refresh runs — which for this widget is inside the MainWindow constructor.
void layOutEmptyLabel(QListWidget *list, QLabel *empty)
{
    empty->setGeometry(list->viewport()->rect().adjusted(kEmptyInset, kEmptyInset,
                                                         -kEmptyInset, -kEmptyInset));
}

// Rebuild one list from scratch. A row carries its address (or its host and path) in
// item data; nothing ever reads a row back by its visible text.
void fillList(QListWidget *list, QLabel *empty, const QVector<WelcomeView::Entry> &entries)
{
    list->clear();
    for (const WelcomeView::Entry &e : entries) {
        auto *item = new QListWidgetItem(e.label, list);
        // The whole address, which is what makes the short label safe — the same bargain
        // the recent-files menu strikes (SPEC.md §3). NOT re-spelled: this is what
        // activating the row opens.
        item->setToolTip(e.tooltip);
        item->setData(kAddressRole, e.address);
        item->setData(kHostRole, e.hostName);
        item->setData(kPathRole, e.path);
    }
    // A LABEL over the viewport and never a row, which is HighlighterPane's rule for its
    // rule table and is here for the same reason: a row is an entry to everything that
    // walks rows, and nothing in a list has a way of being uncountable.
    layOutEmptyLabel(list, empty);
    empty->setVisible(entries.isEmpty());
}

} // namespace

QLabel *WelcomeView::makeEmptyLabel(QListWidget *list, const QString &text,
                                   const QString &objectName)
{
    // A LABEL over the viewport and never a row, which is HighlighterPane's rule for its
    // rule table and is here for the same reason: a row is an entry to everything that
    // walks rows, and nothing in a list has a way of being uncountable.
    auto *empty = new QLabel(text, list->viewport());
    empty->setObjectName(objectName); // findChild, for tests
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    // Transparent to the mouse, or the empty message swallows a click on the list it is
    // lying over. HighlighterPane's rule-table placeholder, verbatim.
    empty->setAttribute(Qt::WA_TransparentForMouseEvents);
    return empty;
}

QLayout *WelcomeView::buildSection(QWidget *parent, const QString &heading,
                                   QPushButton *action, QPushButton *listAction,
                                   QListWidget *list)
{
    // BUTTONS TO THE LEFT OF THE LIST, top-aligned with it, which is Kate's arrangement
    // and the reason it reads as a page rather than as a form: the thing you do sits
    // beside the thing you do it to, and the two sections' buttons line up in a column
    // of their own down the left. Under the list they read as a footer belonging to
    // nothing in particular, and each one pushed the next section a button's height
    // further down.
    auto *row = new QHBoxLayout;

    auto *buttons = new QVBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->addWidget(action);
    buttons->addStretch(1); // top-aligned against the LIST, not centred on it
    row->addLayout(buttons);
    row->addSpacing(kSectionGap);

    auto *right = new QVBoxLayout;
    right->setContentsMargins(0, 0, 0, 0);
    // SET rather than read back: QBoxLayout::spacing() answers the style's metric only
    // once the layout chain reaches a parent WIDGET to ask, and this one does not until
    // the caller adds the row it is in — so asking here answers -1, and the offset below
    // would be measured against a gap seven pixels narrower than the one the layout
    // actually lays out with. Stating the number binds the measurement and the layout to
    // one value by construction.
    right->setSpacing(parent->style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing,
                                                   nullptr, parent));

    auto *headRow = new QHBoxLayout;
    auto *title = new QLabel(heading, parent);
    {
        QFont f = title->font();
        f.setBold(true);
        title->setFont(f);
    }
    headRow->addWidget(title);
    headRow->addStretch(1);
    // The per-list action, where there is one: it acts on what is BELOW it and not on
    // the application, which is why it is not down in the button column with the opens.
    if (listAction)
        headRow->addWidget(listAction);
    right->addLayout(headRow);

    // A FIXED height, not a minimum: this is what stops the two lists absorbing the whole
    // window between them and leaves the page something to centre. A list with more rows
    // than fit scrolls, which is what a list is for.
    list->setFixedHeight(measuredRowHeight(list) * kVisibleRows + 2 * list->frameWidth());
    // Zebra, so a long address is read across without losing the row. Qt's own
    // AlternatingRowColors alone is not enough — see applyThemeColours(), which supplies
    // the colour, because nothing obliges a theme to make AlternateBase differ from Base.
    list->setAlternatingRowColors(true);
    right->addWidget(list);

    row->addLayout(right, 1);
    // The button column keeps its own width and the LIST takes the slack, which is what
    // makes the two lists the same width as each other whatever their buttons say.
    row->setStretch(0, 0);

    // The button sits level with the LIST and not with the heading over it, which is
    // where Kate puts it and what makes the two sections' buttons read as a column of
    // their own: level with the heading they read as part of it, and the taller of the
    // two heading rows then drags its button out of line with the other section's. The
    // offset is MEASURED from the heading row that was just built, because that row is a
    // caption alone in one section and a caption and a button in the other.
    buttons->insertSpacing(0, headRow->sizeHint().height() + right->spacing());
    return row;
}

WelcomeView::WelcomeView(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("welcomeView")); // findChild, for tests

    // A scroll area, because the content does not reflow: on a short window the sections
    // stay the shape they are and the page scrolls, which is what Kate does and what
    // keeps a 1366x768 screen from clipping the second list away with nothing to say so.
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("welcomeScroll")); // findChild, for tests
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);

    auto *page = new QWidget(scroll);
    // Centred by a stretch either side rather than by an alignment on the scroll area's
    // widget: setWidgetResizable(true) hands the widget the whole viewport, so an
    // alignment there is ignored and the column would fill the window.
    auto *pageLayout = new QHBoxLayout(page);
    auto *column = new QWidget(page);
    column->setObjectName(QStringLiteral("welcomeColumn")); // findChild, for tests
    column->setMaximumWidth(QFontMetrics(font()).horizontalAdvance(QString(kContentChars, u'0')));
    // The column takes most of the width and the two spacers share what is left, which
    // is what keeps it centred AND wide. Stretch 0 here would size it to its own hint —
    // the widest of a button and a heading, since a list asks for nothing in particular —
    // and the page would be a narrow ribbon of controls with the cap never reached.
    pageLayout->addStretch(1);
    pageLayout->addWidget(column, kColumnStretch);
    pageLayout->addStretch(1);

    auto *layout = new QVBoxLayout(column);
    // VERTICALLY CENTRED, which is what the bounded list heights below buy: with the
    // lists stretching there was nothing to centre — the content was the viewport — and
    // the page read as two tall empty boxes with a caption over them. The stretches
    // collapse to nothing on a window too short for the content, at which point the
    // scroll area takes over, so this costs the short case nothing.
    layout->addStretch(1);

    m_title = new QLabel(QStringLiteral("loftail"), column);
    // NOT tr(): the name of the application is a name (CLAUDE.md, Conventions).
    m_title->setObjectName(QStringLiteral("welcomeTitle")); // findChild, for tests
    m_title->setAlignment(Qt::AlignCenter);
    {
        QFont f = m_title->font();
        f.setBold(true);
        // A font is sized in points OR in pixels and answers -1 to the other, so both
        // have to be handled: a platform theme that specifies pixels would otherwise
        // give the title a size of -1 and no text at all.
        if (f.pointSizeF() > 0)
            f.setPointSizeF(f.pointSizeF() * kTitleScale);
        else if (f.pixelSize() > 0)
            f.setPixelSize(qRound(f.pixelSize() * kTitleScale));
        m_title->setFont(f);
    }
    layout->addWidget(m_title);

    m_tagline = new QLabel(tr("A viewer for log4cplus logs."), column);
    m_tagline->setObjectName(QStringLiteral("welcomeTagline")); // findChild, for tests
    m_tagline->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_tagline);

    layout->addSpacing(m_title->fontMetrics().height());

    // What a session restore that could not reopen its logs has to say (SPEC.md §10).
    // A MessageLabel and not a wrapped QLabel, for the reason that class exists: this is
    // a list of addresses and wraps at any ordinary width, and a layout sizing a
    // word-wrapped QLabel from its hint reserves a row the text does not fit in.
    m_message = new MessageLabel(column);
    m_message->setObjectName(QStringLiteral("welcomeMessage")); // findChild, for tests
    // Selectable for the same reason the refusal strip's text is: an address is what a
    // reader takes to a colleague or a search box, and it is otherwise only retypable.
    m_message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_message->setVisible(false);
    layout->addWidget(m_message);

    // --- Recent logs ---------------------------------------------------------------

    auto *open = new QPushButton(tr("Open Log..."), column);
    open->setObjectName(QStringLiteral("welcomeOpen")); // findChild, for tests
    connect(open, &QPushButton::clicked, this, &WelcomeView::browseRequested);

    m_clearRecent = new QPushButton(tr("Clear"), column);
    m_clearRecent->setObjectName(QStringLiteral("welcomeClearRecent")); // findChild, for tests
    m_clearRecent->setToolTip(tr("Forget every remembered log"));
    connect(m_clearRecent, &QPushButton::clicked, this, &WelcomeView::clearRecentRequested);

    m_recent = new QListWidget(column);
    m_recent->setObjectName(QStringLiteral("welcomeRecentList")); // findChild, for tests
    // ACTIVATED, never doubleClicked: one signal is both the double-click and Return on
    // the selected row, and a list reachable only by double-click is not reachable from
    // a keyboard at all — which nothing on screen would say.
    connect(m_recent, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (item)
            emit recentActivated(item->data(kAddressRole).toString());
    });
    m_recentEmpty = makeEmptyLabel(m_recent, tr("No logs opened yet."),
                                   QStringLiteral("welcomeRecentEmpty"));
    m_recent->viewport()->installEventFilter(this);

    layout->addLayout(buildSection(column, tr("Recent logs"), open, m_clearRecent, m_recent));
    layout->addSpacing(fontMetrics().height());

    // --- Remote hosts --------------------------------------------------------------

    // A widget of its own, so a build with no SSH hides the section as a unit rather
    // than hiding four things and leaving their spacing behind.
    m_remoteSection = new QWidget(column);
    m_remoteSection->setObjectName(QStringLiteral("welcomeRemoteSection")); // findChild, for tests
    auto *remoteOuter = new QVBoxLayout(m_remoteSection);
    remoteOuter->setContentsMargins(0, 0, 0, 0);

    auto *openRemote = new QPushButton(tr("Open Remote..."), m_remoteSection);
    openRemote->setObjectName(QStringLiteral("welcomeOpenRemote")); // findChild, for tests
    connect(openRemote, &QPushButton::clicked, this, &WelcomeView::openRemoteRequested);

    m_remotes = new QListWidget(m_remoteSection);
    m_remotes->setObjectName(QStringLiteral("welcomeRemoteList")); // findChild, for tests
    connect(m_remotes, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (!item)
            return;
        // The HOST and the path, never the ssh:// address the tooltip shows: opening a
        // remembered remote log also has to carry that host's poll cadence, tail-start
        // and compression into the fetcher, and only the bookmark knows those. An empty
        // path is a host with no remembered log and asks for the dialog instead, which
        // the receiver decides — this row cannot tell the two apart any other way.
        emit remoteActivated(item->data(kHostRole).toString(),
                             item->data(kPathRole).toString());
    });
    m_remotesEmpty = makeEmptyLabel(m_remotes, tr("No saved hosts yet."),
                                    QStringLiteral("welcomeRemoteEmpty"));
    m_remotes->viewport()->installEventFilter(this);

    remoteOuter->addLayout(buildSection(m_remoteSection, tr("Remote hosts"), openRemote,
                                        nullptr, m_remotes));
    layout->addWidget(m_remoteSection);

    layout->addStretch(1);

    // The two button columns are widened to the SAME measurement, which is what makes
    // both lists start at one x — the whole point of putting the buttons beside the
    // lists rather than under them. Done after both exist, since the number is the wider
    // of the two hints.
    const int buttonWidth = qMax(open->sizeHint().width(), openRemote->sizeHint().width());
    open->setMinimumWidth(buttonWidth);
    openRemote->setMinimumWidth(buttonWidth);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);
    scroll->setWidget(page);

    applyThemeColours();
    // The two lists start empty, so the two messages start on screen.
    fillList(m_recent, m_recentEmpty, {});
    fillList(m_remotes, m_remotesEmpty, {});
}

void WelcomeView::setRecent(const QVector<Entry> &entries)
{
    fillList(m_recent, m_recentEmpty, entries);
    // Disabled while there is nothing to forget, exactly as the menu item is.
    m_clearRecent->setEnabled(!entries.isEmpty());
}

void WelcomeView::setRemotes(const QVector<Entry> &entries)
{
    fillList(m_remotes, m_remotesEmpty, entries);
}

void WelcomeView::setRemotesVisible(bool on)
{
    m_remoteSection->setVisible(on);
}

void WelcomeView::setMessage(const QString &text)
{
    m_message->setText(text);
    m_message->setVisible(!text.isEmpty());
}


void WelcomeView::applyThemeColours()
{
    // From the palette, never a constant: this page is read on a light theme and a dark
    // one, and a grey chosen for either is invisible on the other.
    //
    // The tagline sits on Window and the two empty messages sit on a list's viewport,
    // which is Base — so they take DIFFERENT functions, mutedColor() deriving from
    // WindowText/Window and placeholderColor() from Text/Base. Swapping them is invisible
    // on a theme whose two surfaces are close and wrong on one where they are not.
    QPalette tag = m_tagline->palette();
    tag.setColor(QPalette::WindowText, mutedColor(palette()));
    m_tagline->setPalette(tag);

    const QColor onList = placeholderColor(m_recent->palette());
    for (QLabel *label : {m_recentEmpty, m_remotesEmpty}) {
        QPalette pal = label->palette();
        pal.setColor(QPalette::WindowText, onList);
        label->setPalette(pal);
    }

    // THE ZEBRA IS SUPPLIED, not left to QPalette::AlternateBase, which is the log
    // table's own rule (UiColors::alternateRowColor, ARCHITECTURE.md §8.3): nothing
    // obliges a theme to make that role differ from Base, and a theme that leaves the
    // two equal takes the banding away in silence — which is worse than never having had
    // it, since the rows then look banded on the developer's desktop and flat on the
    // user's. alternateRowColor() is a fixed step from Base toward Text and lands on
    // either theme.
    for (QListWidget *list : {m_recent, m_remotes}) {
        QPalette pal = list->palette();
        pal.setColor(QPalette::AlternateBase, alternateRowColor(list->palette()));
        list->setPalette(pal);
    }
}

bool WelcomeView::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Resize) {
        if (watched == m_recent->viewport())
            layOutEmptyLabel(m_recent, m_recentEmpty);
        else if (watched == m_remotes->viewport())
            layOutEmptyLabel(m_remotes, m_remotesEmpty);
    }
    return QWidget::eventFilter(watched, event);
}

void WelcomeView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // Every colour here was derived from the OLD palette, so they have to be re-taken
    // or the muted text stays legible only on the theme it was built under and the zebra
    // keeps a band mixed from the wrong two ends.
    if (event->type() == QEvent::PaletteChange)
        applyThemeColours();
}

} // namespace loftail
