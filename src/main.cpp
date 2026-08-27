// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QCoreApplication>

#include "AppStyle.h"
#include "CommandLine.h"
#include "DiagnosticLog.h"
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
    // setStyle() takes ownership of the style and deletes the one it replaces, so
    // there is no handle to hold and nothing here to free. The analyzer sees only the
    // bare `new`, and anchors the report not at it but at whichever later statement it
    // stops tracking the pointer at — hence a REGION rather than a NOLINTNEXTLINE,
    // which would have to be moved every time a line is added below.
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
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
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
    // No setApplicationDisplayName(): QPlatformWindow::formatWindowTitle() appends it to
    // every window title that does not already END with it, so "loftail — app.log" reached
    // the window manager as "loftail — app.log - loftail". MainWindow writes the whole
    // title itself, leading name included. An empty title still reads "loftail", because
    // the same function falls back to applicationName() for one.

    // Which language the interface speaks, and Qt's own strings put into the same one
    // (UiLanguage.h). Before the parser below, whose help text is user-visible, and
    // before MainWindow, which builds its entire menu bar in its constructor.
    loftail::installUiLanguage();

    // Command-line contract (SPEC.md §3, PLAN.md M7), and its help text — both live
    // in CommandLine.h, which is where the rule that EVERY file named opens is pinned
    // by a test. main() is not reachable from one.
    loftail::CommandLine cmdLine;
    cmdLine.process(app);

    // The first line of loftail's own log, and the reason it is here rather than lazily
    // on the first interesting event: a diagnostic file whose top says which binary wrote
    // it is one that can be pasted into a bug report as-is. AFTER process(), so --help and
    // --version stay side-effect-free, and after the application name is set, since that
    // is what resolves the file's location (DiagnosticLog.h).
    loftail::diagLogSessionStart();

    loftail::MainWindow window;
    window.show();

    // EVERY file named opens, each in its own tab, in the order given — the same thing
    // dropping several files on the window does (SPEC.md §3). No file argument -> an
    // empty window (session restore may still reopen the last files inside MainWindow);
    // the files named are ADDED to whatever the session brought back, never a
    // replacement for it. --pattern overrides what is remembered for each of them and is
    // then judged against it: a pattern that does not fit raises Preferences, and
    // dismissing that opens neither the file nor a settings node (SPEC.md §3). It used
    // to be taken as intent and open the file as plain text, which also saved it
    // (bugs.md 15).
    //
    // None of this blocks: openFiles() is a loop over openFile(), which returns with a
    // tab that says it is connecting rather than connecting (ARCHITECTURE.md §6.3.3),
    // so ten unreachable hosts on one command line still show ten tabs at once.
    window.openFiles(cmdLine.files(), cmdLine.pattern());

    return QApplication::exec();
}
