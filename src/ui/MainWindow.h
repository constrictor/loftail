#pragma once

#include <QMainWindow>

namespace loftail {

// The application's top-level window. Empty in M0 — a placeholder that proves
// the Widgets stack builds and runs. The record view, panes, and menus arrive
// in later milestones.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
};

} // namespace loftail
