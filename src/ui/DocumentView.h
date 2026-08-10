#pragma once

#include "LogView.h"

#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
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

    // Show the Find bar and focus its text field (Ctrl+F).
    void activateFind();

    // Re-read the digest model's row count and show or hide the strip accordingly.
    // One rule covers both "no rule asked for a digest" and "rules asked but nothing
    // has matched yet", so neither needs a case of its own.
    void refreshDigestVisibility();

signals:
    // Forwarded from the Find bar so the window can run the search over this view.
    void findRequested(bool forward, bool fromStart);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    DocumentContext *m_context = nullptr;
    LogView         *m_logView = nullptr;
    LogView         *m_digestView = nullptr;
    FindBar         *m_findBar = nullptr;
    QVBoxLayout     *m_layout = nullptr;
};

} // namespace loftail
