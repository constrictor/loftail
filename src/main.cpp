#include <QApplication>
#include <QCommandLineParser>

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

    // Minimal command-line handling so a file can be opened at launch (used by the
    // headless smoke check). The full `loftail <file> --pattern <p>` contract is
    // M7; only the file and pattern are honored here.
    QCommandLineParser parser;
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("Log file to open."));
    QCommandLineOption patternOpt(QStringLiteral("pattern"),
                                  QStringLiteral("log4cplus ConversionPattern."),
                                  QStringLiteral("pattern"));
    parser.addOption(patternOpt);
    parser.addHelpOption();
    parser.process(app);

    loftail::MainWindow window;
    window.show();

    const QStringList positional = parser.positionalArguments();
    if (!positional.isEmpty())
        window.openFile(positional.first(), parser.value(patternOpt));

    return app.exec();
}
