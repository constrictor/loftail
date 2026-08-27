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

#include "LogView.h"
#include "MessageLabel.h"
#include "SectionBox.h"

#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QFrame;
class QVBoxLayout;
QT_END_NAMESPACE

namespace loftail {

class DocumentContext;
class FindBar;

// One view onto one open file: the record table above its own Find bar, so Find
// docks at the bottom of the view rather than opening a modal dialog.
//
// A file may have SEVERAL of these. Everything that differs between two views of
// the same log — scroll position, selection, wrap mode, column layout, follow
// state — lives in the LogView; everything shared lives in the DocumentContext.
//
// One of these is a page in the window's document well; the window tracks which
// tab is current and rebinds the side panes to its Document (invariant #7).
//
// A view now holds TWO LogViews — the table and the digest strip (M19) — so
// `findChildren<LogView *>()` no longer counts views. Every test that counts or finds
// one names it: findChildren<LogView *>("logView"). Object names are the test contract
// precisely because they are not the visible text (ARCHITECTURE.md §9.1), and this is
// the case that cashed that in.
class DocumentView : public QWidget
{
    Q_OBJECT

public:
    // `context` must outlive the view. The view does NOT own it.
    DocumentView(DocumentContext *context, QWidget *parent = nullptr);
    ~DocumentView() override;

    DocumentContext *context() const { return m_context; }
    LogView *logView() const { return m_logView; }
    FindBar *findBar() const { return m_findBar; }

    // The highlight digest strip (M19, SPEC.md §7): the newest match of each rule that
    // asked for one, between the table and the Find bar. Never null — it is created
    // with the view and hides itself when there is nothing to show, so no caller has to
    // ask whether the feature is in use.
    //
    // Inside the view rather than a dock, because SPEC.md §8 promises panes attach left
    // or right and never as a strip above or below the log, and §5a keeps the document
    // area free of them.
    LogView *digestView() const { return m_digestView; }

    // The caption over that strip, appearing and disappearing with it. A second table
    // under the first is not self-explanatory — nothing on screen said why those rows
    // were repeated there, and a row that also appears in the log above looks like a
    // rendering fault until it is named.
    SectionBox *digestTitle() const { return m_digestTitle; }

    // Show the Find bar and focus its text field (Ctrl+F).
    void activateFind();

    // The disconnected strip: a row across the TOP of the view saying that the records
    // below it are what was fetched before the far end went, with a Reconnect button
    // (SPEC.md §3). Hidden when `reason` is empty.
    //
    // At the top and not the bottom, and not a placeholder. A placeholder is what a view
    // with NO records shows, and this state's whole point is that there are records — so
    // the sentence has to sit BESIDE them rather than in place of them, and above them
    // rather than below, because the reader of a followed log is looking at the bottom
    // and a strip that appears there would push the newest record out from under their
    // eye at the exact moment it stopped being the newest.
    void setStaleNotice(const QString &reason);

    // Never null; hidden unless the log is disconnected. Named `staleBar`, for tests.
    QFrame *staleBar() const { return m_staleBar; }

    // Re-read the digest model's row count and show or hide the strip accordingly.
    // One rule covers both "no rule asked for a digest" and "rules asked but nothing
    // has matched yet", so neither needs a case of its own.
    void refreshDigestVisibility();

signals:
    // Forwarded from the Find bar so the window can run the search over this view.
    void findRequested(bool forward, bool fromStart);
    // Forwarded likewise: "make a highlight rule of what is in the Find box" (SPEC.md
    // §5). The window handles it because the rules belong to the DOCUMENT and are edited
    // in a pane the window owns (invariant #7) — this view knows only that its bar asked.
    void highlightRequested();

    // The Reconnect button in the disconnected strip was pressed. Forwarded rather than
    // acted on for the same reason: the fetcher is reached through the window's own
    // File ▸ Reconnect path, and there is exactly one of those.
    void reconnectRequested();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    DocumentContext *m_context = nullptr;
    QFrame          *m_staleBar = nullptr;
    MessageLabel    *m_staleText = nullptr;
    LogView         *m_logView = nullptr;
    SectionBox      *m_digestTitle = nullptr;
    LogView         *m_digestView = nullptr;
    FindBar         *m_findBar = nullptr;
    QVBoxLayout     *m_layout = nullptr;
};

} // namespace loftail
