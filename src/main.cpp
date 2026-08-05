#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>

#include "AppStyle.h"
#include "MainWindow.h"
#include "Version.h"

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <cstdio>
#endif

int main(int argc, char *argv[])
{
#if defined(Q_OS_WIN)
    // loftail is a GUI-subsystem executable, so it has no console of its own. On
    // Windows QCommandLineParser prints --version/--help (and parse errors) by
    // popping a modal MessageBox when it sees no console — which blocks forever in
    // an unattended run (CI, scripts). Attach to the launching terminal, if there
    // is one, and point the CRT streams at it so that output goes to the console
    // and process() can print-and-exit like it does on Linux/macOS. A GUI launch
    // (double-click, no parent console) hits neither branch and is unaffected.
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
#endif

    QApplication app(argc, argv);

    // The desktop's own style, minus the themed icons Qt hangs on standard dialog
    // buttons (AppStyle.h). Installed before any window exists so nothing is polished
    // twice; the palette is untouched, so a dark theme stays dark.
    QApplication::setStyle(new loftail::AppStyle);

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

    parser.addPositionalArgument(
        QStringLiteral("file"),
        QStringLiteral("Log file to open (optional). Either a local path or a remote "
                       "log over SSH, spelled ssh://user@host/path/to/file.log. A "
                       "compressed log (app.log.gz) opens directly; a log inside an "
                       "archive is named by continuing the path through it, as "
                       "bundle.tar.gz/var/log/app.log. The two combine, so "
                       "ssh://host/var/log/app.log.1.gz works."),
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
