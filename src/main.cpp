#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>

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
    QApplication::setApplicationDisplayName(QStringLiteral("loftail"));
    QApplication::setApplicationVersion(loftail::applicationVersion());

    // Command-line contract (SPEC.md §3, PLAN.md M7):
    //   loftail [options] [file]
    //   --pattern <p>   log4cplus ConversionPattern for a never-seen file
    //   --help, --version
    //
    // There is deliberately no --follow: every file opens at its end and follows,
    // unconditionally (SPEC.md §3, §11). Following is not a mode, so it is not a flag.
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("loftail — a desktop viewer for log4cplus logs.\n\n"
                       "Opens the given file at its end and follows it as it grows, "
                       "like tail -f. Filters and highlights by subsystem and priority."));
    const QCommandLineOption helpOpt = parser.addHelpOption();
    const QCommandLineOption versionOpt = parser.addVersionOption();

    parser.addPositionalArgument(QStringLiteral("file"),
                                 QStringLiteral("Log file to open (optional)."),
                                 QStringLiteral("[file]"));

    QCommandLineOption patternOpt(
        QStringLiteral("pattern"),
        QStringLiteral("log4cplus ConversionPattern for the format of <file>. "
                       "Used only for a file loftail has not seen before; a file "
                       "with a remembered format ignores it. A pattern that does "
                       "not match opens the file as plain text."),
        QStringLiteral("pattern"));
    parser.addOption(patternOpt);

    // process() handles --help and --version itself (printing and exiting) and
    // reports unknown options; it never throws, so a malformed invocation degrades
    // to a usage message rather than a crash.
    parser.process(app);

    loftail::MainWindow window;
    window.show();

    // No file argument -> an empty window (SPEC.md §3; session restore may still
    // reopen the last file inside MainWindow). A bad --pattern is taken as intent
    // and opens the file as plain text without a blocking prompt (PLAN.md M3).
    const QStringList positional = parser.positionalArguments();
    if (!positional.isEmpty())
        window.openFile(positional.first(), parser.value(patternOpt));

    return app.exec();
}
