#include "MainWindow.h"

namespace loftail {

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("loftail"));
    resize(1024, 768);
}

} // namespace loftail
