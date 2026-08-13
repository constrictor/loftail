#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>

#include "AppStyle.h"
#include "MainWindow.h"
#include "UiLanguage.h"
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
    // The version string, NOT the bare version: this is what --version prints, and a
    // binary downloaded from a workflow run has to be able to say which run built it
    // (Version.h). The packages keep naming the plain release in their filenames.
    QApplication::setApplicationVersion(loftail::applicationVersionString());
    // No setApplicationDisplayName(): QPlatformWindow::formatWindowTitle() appends it to
    // every window title that does not already END with it, so "loftail — app.log" reached
    // the window manager as "loftail — app.log - loftail". MainWindow writes the whole
    // title itself, leading name included. An empty title still reads "loftail", because
    // the same function falls back to applicationName() for one.

    // Which language the interface speaks, and Qt's own strings put into the same one
    // (UiLanguage.h). Before the parser below, whose help text is user-visible, and
    // before MainWindow, which builds its entire menu bar in its constructor.
    loftail::installUiLanguage();

    // Command-line contract (SPEC.md §3, PLAN.md M7):
    //   loftail [options] [file]
    //   --pattern <p>   log4cplus ConversionPattern for a never-seen file
    //   --help, --version
    //
    // There is deliberately no --follow: every file opens at its end and follows,
    // unconditionally (SPEC.md §3, §11). Following is not a mode, so it is not a flag.
    //
    // --help is user-facing prose, so it goes through translate() — with an explicit
    // context, main() being a free function. What does NOT is the argument SYNTAX:
    // "pattern" is the literal spelling of --pattern, and "file"/"[file]" are the
    // placeholder names in the usage line. Translating those would rename the option
    // and break every script that passes it.
    QCommandLineParser parser;
    parser.setApplicationDescription(QCoreApplication::translate(
        "main",
        "loftail — a desktop viewer for log4cplus logs.\n\n"
        "Opens the given file at its end and follows it as it grows, "
        "like tail -f. Filters and highlights by subsystem and priority."));
    const QCommandLineOption helpOpt = parser.addHelpOption();
    const QCommandLineOption versionOpt = parser.addVersionOption();

    parser.addPositionalArgument(
        QStringLiteral("file"),
        QCoreApplication::translate(
            "main",
            "Log file to open (optional). Either a local path or a remote "
            "log over SSH, spelled ssh://user@host/path/to/file.log. A "
            "compressed log (app.log.gz) opens directly; a log inside an "
            "archive is named by continuing the path through it, as "
            "bundle.tar.gz/var/log/app.log. The two combine, so "
            "ssh://host/var/log/app.log.1.gz works."),
        QStringLiteral("[file]"));

    QCommandLineOption patternOpt(
        QStringLiteral("pattern"),
        QCoreApplication::translate(
            "main",
            "log4cplus ConversionPattern for the format of <file>. "
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
