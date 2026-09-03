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

// What a row stands for. Three plain strings rather than the Entry itself in a
// QVariant: these are the only fields an activation needs, and a row carrying a
// registered metatype is a metatype to keep registered.
constexpr int kAddressRole = Qt::UserRole;
constexpr int kHostRole    = Qt::UserRole + 1;
constexpr int kPathRole    = Qt::UserRole + 2;

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

    const int rowHeight = fontMetrics().height() + kEmptyInset;

    // --- Recent logs ---------------------------------------------------------------

    auto *recentHead = new QHBoxLayout;
    auto *recentTitle = new QLabel(tr("Recent logs"), column);
    {
        QFont f = recentTitle->font();
        f.setBold(true);
        recentTitle->setFont(f);
    }
    recentHead->addWidget(recentTitle);
    recentHead->addStretch(1);
    m_clearRecent = new QPushButton(tr("Clear"), column);
    m_clearRecent->setObjectName(QStringLiteral("welcomeClearRecent")); // findChild, for tests
    m_clearRecent->setToolTip(tr("Forget every remembered log"));
    connect(m_clearRecent, &QPushButton::clicked, this, &WelcomeView::clearRecentRequested);
    recentHead->addWidget(m_clearRecent);
    layout->addLayout(recentHead);

    m_recent = new QListWidget(column);
    m_recent->setObjectName(QStringLiteral("welcomeRecentList")); // findChild, for tests
    m_recent->setMinimumHeight(rowHeight * kVisibleRows);
    // ACTIVATED, never doubleClicked: one signal is both the double-click and Return on
    // the selected row, and a list reachable only by double-click is not reachable from
    // a keyboard at all — which nothing on screen would say.
    connect(m_recent, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (item)
            emit recentActivated(item->data(kAddressRole).toString());
    });
    m_recent->viewport()->installEventFilter(this);
    layout->addWidget(m_recent, 1);

    m_recentEmpty = new QLabel(tr("No logs opened yet."), m_recent->viewport());
    m_recentEmpty->setObjectName(QStringLiteral("welcomeRecentEmpty")); // findChild, for tests
    m_recentEmpty->setAlignment(Qt::AlignCenter);
    m_recentEmpty->setWordWrap(true);
    // Transparent to the mouse, or the empty message swallows a click on the list it is
    // lying over. HighlighterPane's rule-table placeholder, verbatim.
    m_recentEmpty->setAttribute(Qt::WA_TransparentForMouseEvents);

    auto *recentButtons = new QHBoxLayout;
    recentButtons->addStretch(1);
    auto *open = new QPushButton(tr("Open Log..."), column);
    open->setObjectName(QStringLiteral("welcomeOpen")); // findChild, for tests
    connect(open, &QPushButton::clicked, this, &WelcomeView::browseRequested);
    recentButtons->addWidget(open);
    layout->addLayout(recentButtons);

    layout->addSpacing(fontMetrics().height());

    // --- Remote hosts --------------------------------------------------------------

    // A widget of its own, so a build with no SSH hides the section as a unit rather
    // than hiding four things and leaving their spacing behind.
    m_remoteSection = new QWidget(column);
    m_remoteSection->setObjectName(QStringLiteral("welcomeRemoteSection")); // findChild, for tests
    auto *remoteLayout = new QVBoxLayout(m_remoteSection);
    remoteLayout->setContentsMargins(0, 0, 0, 0);

    auto *remoteTitle = new QLabel(tr("Remote hosts"), m_remoteSection);
    {
        QFont f = remoteTitle->font();
        f.setBold(true);
        remoteTitle->setFont(f);
    }
    remoteLayout->addWidget(remoteTitle);

    m_remotes = new QListWidget(m_remoteSection);
    m_remotes->setObjectName(QStringLiteral("welcomeRemoteList")); // findChild, for tests
    m_remotes->setMinimumHeight(rowHeight * kVisibleRows);
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
    m_remotes->viewport()->installEventFilter(this);
    remoteLayout->addWidget(m_remotes, 1);

    m_remotesEmpty = new QLabel(tr("No saved hosts yet."), m_remotes->viewport());
    m_remotesEmpty->setObjectName(QStringLiteral("welcomeRemoteEmpty")); // findChild, for tests
    m_remotesEmpty->setAlignment(Qt::AlignCenter);
    m_remotesEmpty->setWordWrap(true);
    m_remotesEmpty->setAttribute(Qt::WA_TransparentForMouseEvents);

    auto *remoteButtons = new QHBoxLayout;
    remoteButtons->addStretch(1);
    auto *openRemote = new QPushButton(tr("Open Remote..."), m_remoteSection);
    openRemote->setObjectName(QStringLiteral("welcomeOpenRemote")); // findChild, for tests
    connect(openRemote, &QPushButton::clicked, this, &WelcomeView::openRemoteRequested);
    remoteButtons->addWidget(openRemote);
    remoteLayout->addLayout(remoteButtons);

    layout->addWidget(m_remoteSection, 1);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);
    scroll->setWidget(page);

    applyMutedColours();
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


void WelcomeView::applyMutedColours()
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
    // The three muted colours were taken from the OLD palette, so they have to be
    // re-taken or they stay legible only on the theme they were built under.
    if (event->type() == QEvent::PaletteChange)
        applyMutedColours();
}

} // namespace loftail
