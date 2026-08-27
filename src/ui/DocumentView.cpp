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

#include "DocumentView.h"

#include "DocumentContext.h"
#include "Document.h"
#include "FindBar.h"
#include "LogModel.h"
#include "UiColors.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

namespace loftail {

DocumentView::DocumentView(DocumentContext *context, QWidget *parent)
    : QWidget(parent), m_context(context)
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    // The disconnected strip, above the table. Built here rather than in the window
    // because a log may have SEVERAL views and each needs its own — the same reason the
    // Find bar is a view's and not the window's (invariant #7).
    m_staleBar = new QFrame(this);
    m_staleBar->setObjectName(QStringLiteral("staleBar")); // test contract, never translated
    m_staleBar->setFrameShape(QFrame::StyledPanel);
    auto *staleLayout = new QHBoxLayout(m_staleBar);
    staleLayout->setContentsMargins(8, 4, 4, 4);
    staleLayout->setSpacing(6);
    // A MessageLabel: the sentence carries the transport's own words and a record count,
    // and wraps at any ordinary window width — a plain wrapped QLabel is sized from a
    // hint its own text does not fit in (MessageLabel.h).
    m_staleText = new MessageLabel(m_staleBar);
    m_staleText->setObjectName(QStringLiteral("staleBarText")); // test contract
    // Selectable, exactly as the window's open-refusal strip is: the transport's wording
    // is what a reader takes to a colleague or a search box.
    m_staleText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *reconnect = new QPushButton(tr("Reconnect"), m_staleBar);
    reconnect->setObjectName(QStringLiteral("staleBarReconnect")); // test contract
    // The one gesture worth putting here rather than leaving to the File menu: the strip
    // is on screen precisely because the link went, and "try now" is what the reader
    // looking at it wants. It is the SAME command — the window's, which is the only
    // thing that can reach the fetcher (SPEC.md §3).
    connect(reconnect, &QPushButton::clicked, this, &DocumentView::reconnectRequested);
    staleLayout->addWidget(m_staleText, 1);
    staleLayout->addWidget(reconnect, 0, Qt::AlignTop);
    m_staleBar->setVisible(false);
    // Stretch 0: it takes the one row it asks for and the table keeps the rest.
    m_layout->addWidget(m_staleBar, 0);

    m_logView = new LogView(context->doc.get(), context->model, this);
    m_logView->setObjectName(QStringLiteral("logView")); // test contract, never translated
    m_layout->addWidget(m_logView, 1);

    // The strip is NAMED (SPEC.md §7). A second table under the first explains nothing
    // on its own, and its rows are copies of rows in the log above it, so without a
    // caption the honest reading is a rendering fault. A body-less SectionBox: the same
    // title-row-and-hairline the five filter axes wear, minus the checkbox, so the one
    // caption in the document area is drawn by the same class as every caption in a pane
    // rather than by a hand-styled label that would drift from them.
    m_digestTitle = new SectionBox(tr("Digest"), this);
    m_digestTitle->setObjectName(QStringLiteral("digestTitle")); // test contract
    m_digestTitle->setFlat(true);
    m_digestTitle->setTitleDivider(true);
    m_digestTitle->hide(); // with the strip it names
    m_layout->addWidget(m_digestTitle, 0);

    // The digest strip (M19, SPEC.md §7): under the table, above the Find bar, at
    // stretch 0 so it takes exactly the height it asks for and the table keeps the rest.
    //
    // A sibling of its caption, NOT a child of it, however much a titled box wants to
    // hold the thing it titles: LogView::digestContentLines() caps the strip at a third
    // of parentWidget()'s height, so parenting it to a box whose own height comes from
    // the strip's size hint would make the cap a third of the strip itself and feed the
    // layout its own output.
    m_digestView = new LogView(context->doc.get(), context->digestModel, this,
                               LogView::Role::Digest);
    m_digestView->setObjectName(QStringLiteral("digestView"));
    m_digestView->hide(); // until a rule asks for one
    m_layout->addWidget(m_digestView, 0);

    m_findBar = new FindBar(this);
    // A log has somewhere to put a highlight rule, so this bar offers the button; the
    // config editor's bar, built from the same class, does not (FindBar::
    // setHighlightVisible).
    m_findBar->setHighlightVisible(true);
    m_layout->addWidget(m_findBar);
    connect(m_findBar, &FindBar::findRequested, this, &DocumentView::findRequested);
    connect(m_findBar, &FindBar::highlightRequested, this, &DocumentView::highlightRequested);
    // Closing the bar takes the marks with it (SPEC.md §5): the query is gone from the
    // screen, so what is still marked in the table would be a claim about a search the
    // reader can no longer see. Here rather than in MainWindow because the bar and the
    // table it searched are both this view's (invariant #7).
    connect(m_findBar, &FindBar::closed, m_logView, &LogView::clearFindMatcher);

    // The strip appears and disappears with its content, so the model's own reset is
    // the signal — not a separate flag the window would have to remember to set.
    connect(context->digestModel, &QAbstractItemModel::modelReset,
            this, &DocumentView::refreshDigestVisibility);
    connect(context->digestModel, &QAbstractItemModel::rowsInserted,
            this, &DocumentView::refreshDigestVisibility);
    connect(context->digestModel, &QAbstractItemModel::rowsRemoved,
            this, &DocumentView::refreshDigestVisibility);

    // Columns and horizontal scroll are MIRRORED from the table, one way only — the
    // strip's claim is that a row is "rendered exactly as it is in the log", and column
    // state alone makes that true only at horizontal offset zero. One way, so the two
    // views cannot chase each other.
    connect(m_logView, &LogView::columnLayoutChanged, this, [this] {
        m_digestView->restoreColumnState(m_logView->saveColumnState());
    });
    connect(m_logView, &LogView::horizontalOffsetChanged,
            m_digestView, &LogView::setHorizontalOffset);
    m_digestView->restoreColumnState(m_logView->saveColumnState());

    // Deliberately NOT connected: LogView::recordMenuRequested. The strip's view rows
    // are a different ordinal space — MainWindow::showRecordMenu resolves them against
    // the document's own FilteredIndex — so wiring it up would make the menu act
    // silently on the wrong record. Avoided by omission, which is exactly the kind of
    // thing a later "connect everything" pass restores.

    // Settle it once now. A view is often built AFTER its document's rules are in place
    // — session restore, and a second view of a log that already has a digest — and in
    // that case no model reset will ever arrive to reveal the strip.
    refreshDigestVisibility();

    // Focusing the page focuses the table, so raising a tab puts the caret where the
    // user is about to read rather than on the Find bar.
    setFocusProxy(m_logView);
}

void DocumentView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // The strip's height cap is a fraction of THIS widget's height, so it has to be
    // re-decided when the window changes size — the strip's own resize cannot see it.
    m_digestView->refreshDigestCap();
}

DocumentView::~DocumentView() = default;

void DocumentView::activateFind()
{
    m_findBar->activate();
}

void DocumentView::setStaleNotice(const QString &reason)
{
    if (reason.isEmpty()) {
        m_staleText->clear();
        m_staleBar->setVisible(false);
        return;
    }

    // Two sentences, and the second is the one that is not obvious: a reader who sees
    // "lost the connection" over a full table has no way of knowing whether those
    // records are still arriving. Saying how many there are also dates them — the count
    // stops moving, which is the thing a followed log otherwise shows by scrolling.
    const int records = m_context && m_context->doc
        ? int(m_context->doc->index().records.size())
        : 0;
    m_staleText->setText(
        tr("%1 — showing the %n record(s) fetched before it went.", nullptr, records)
            .arg(reason));
    // Taken from the CURRENT palette every time rather than at construction, so the
    // colour follows a theme changed under a running window (UiColors.h). A WARNING and
    // not an error: nothing has failed and nothing is lost — what is on screen is true,
    // it has simply stopped being added to.
    QPalette notice = m_staleText->palette();
    notice.setColor(QPalette::WindowText, warningColor(palette()));
    m_staleText->setPalette(notice);
    m_staleBar->setVisible(true);
}

void DocumentView::refreshDigestVisibility()
{
    const bool show = m_context->digestModel && m_context->digestModel->rowCount() > 0;
    // Both, explicitly. The caption is a sibling rather than a parent (see the
    // constructor), so hiding one does not hide the other — and a "Digest" heading over
    // nothing is worse than no heading. Setting the view's own visibility is also what
    // keeps digestView()->isHidden() meaning "the strip is not there".
    m_digestTitle->setVisible(show);
    m_digestView->setVisible(show);
    if (show)
        m_digestView->refreshDigestCap();
}

} // namespace loftail
