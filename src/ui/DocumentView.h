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
// The widget reports focus so the window can track which view is active and
// rebind the side panes to its Document (invariant #7).
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

    // The objectName of the QDockWidget hosting this view. Generated once per view
    // and persisted with the session, because restoreState()/restoreDockWidget()
    // match saved layout slots to docks by object name.
    const QString &dockName() const { return m_dockName; }
    void setDockName(const QString &name) { m_dockName = name; }
    // A fresh, never-reused dock object name.
    static QString makeDockName();

    // Show the Find bar and focus its text field (Ctrl+F).
    void activateFind();

signals:
    // Forwarded from the Find bar so the window can run the search over this view.
    void findRequested(bool forward, bool fromStart);

private:
    DocumentContext *m_context = nullptr;
    LogView         *m_logView = nullptr;
    FindBar         *m_findBar = nullptr;
    QVBoxLayout     *m_layout = nullptr;
    QString          m_dockName;
};

} // namespace loftail
