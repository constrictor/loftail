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

#pragma once

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QCoreApplication;
QT_END_NAMESPACE

namespace loftail {

// The command-line contract (SPEC.md §3):
//
//   loftail [options] [file...]
//     --pattern <p>   log4cplus ConversionPattern for the files named, overriding
//                     what is remembered for them — and judged against each
//     --help, --version
//
// Split out of main() so it can be driven without launching a process: what is worth
// pinning is that EVERY file named opens, not just the first — the whole point of an
// application whose logs are tabs — and main() itself is not reachable from a test.
//
// There is deliberately no --follow: every file opens at its end and follows,
// unconditionally (SPEC.md §3, §11). Following is not a mode, so it is not a flag.
//
// Not a QObject (nothing here has signals), so the help text — which is user-visible
// prose — goes through the Q_DECLARE_TR_FUNCTIONS shim rather than a hand-rolled tr(),
// which lupdate would not understand (ARCHITECTURE.md §9.1). What must NOT be
// translated is the argument SYNTAX: "pattern" is the literal spelling of --pattern,
// and "file"/"[file...]" are the placeholder names in the usage line. Translating
// those would rename the option and break every script that passes it.
class CommandLine
{
    Q_DECLARE_TR_FUNCTIONS(CommandLine)

public:
    CommandLine();

    // main()'s route: handles --help and --version itself (printing and exiting) and
    // reports an unknown option. Never throws, so a malformed invocation degrades to a
    // usage message rather than a crash.
    void process(const QCoreApplication &app);

    // A test's route: the same parse without the exiting. `arguments` starts with the
    // program name, as QCoreApplication::arguments() does. Returns false on a
    // malformed invocation.
    bool parse(const QStringList &arguments);

    // Every file named, in the order given; empty for a bare launch. Each is a local
    // path, an ssh:// address or an in-archive address, in any accepted spelling —
    // this layer does not tell them apart, MainWindow::openFile() normalizes them.
    QStringList files() const;

    // --pattern, or an empty string. ONE pattern covers every file named: it says how
    // a log is written, and someone passing a set of files on one command line is
    // passing a set written alike.
    //
    // An EMPTY string is what `--pattern "$FMT"` yields for an unset variable, and it
    // names no format: the switch is then the bare launch and each log's own settings
    // stand. Deliberately not an error — there is nothing to refuse, and a supplied
    // pattern is judged against the log now rather than believed (SPEC.md §3), so an
    // empty one cannot damage anything by being ignored.
    QString pattern() const;

private:
    QCommandLineParser m_parser;
    QCommandLineOption m_patternOption;
};

} // namespace loftail
