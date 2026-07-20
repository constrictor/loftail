#include <QApplication>

#include "MainWindow.h"
#include "Version.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Organization/application names must be set before any QSettings is
    // constructed so settings resolve to the right location (CLAUDE.md: no
    // hardcoded paths; QSettings derives its path from these).
    QApplication::setOrganizationName(QString::fromLatin1(loftail::organizationName));
    QApplication::setApplicationName(QString::fromLatin1(loftail::applicationName));
    QApplication::setApplicationVersion(loftail::applicationVersion());

    loftail::MainWindow window;
    window.show();

    return app.exec();
}
