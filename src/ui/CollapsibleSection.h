#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
class QToolButton;
QT_END_NAMESPACE

namespace loftail {

// A disclosure triangle over a block of controls, collapsed by default.
//
// For settings that HAVE a good default and are rarely changed. The point is not to save
// space — it is that a form where everything is equally visible says everything is
// equally important, and the Open Remote dialog was saying that about a network poll
// cadence and about which machine to connect to. Collapsing the first is what lets the
// second read as the question the dialog is asking.
//
// Only for options that are genuinely optional: anything the user must decide, or must
// be told, belongs where they cannot miss it. A plain-text-password warning behind a
// disclosure triangle would be a lie of omission with a UI affordance in front of it.
//
// The content widget is reparented and owned. Toggling changes the size hint, so a
// dialog holding one should not have a fixed size; expanding grows it, which is what the
// user just asked for by clicking. Collapsing shrinks it back, which QDialog does not do
// on its own — the section asks its window to adjustSize() after collapsing.
class CollapsibleSection : public QWidget
{
    Q_OBJECT

public:
    explicit CollapsibleSection(const QString &title, QWidget *parent = nullptr);

    // Takes ownership; replaces any previous content.
    void setContentWidget(QWidget *content);
    QWidget *contentWidget() const { return m_content; }

    bool isExpanded() const;
    void setExpanded(bool expanded);

    // The button carries the title, so tests and accelerators reach it here.
    QToolButton *toggleButton() const { return m_toggle; }

private:
    void applyState(bool expanded);

    QToolButton *m_toggle = nullptr;
    QWidget     *m_content = nullptr;
};

} // namespace loftail
