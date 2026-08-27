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

#include "CommandLine.h"

namespace loftail {

CommandLine::CommandLine()
    : m_patternOption(
          QStringLiteral("pattern"),
          tr("log4cplus ConversionPattern for the format of the files named. It "
             "overrides whatever loftail has remembered for them, and is then "
             "checked against each one: where it fits it is remembered for that "
             "log; where it does not, Preferences opens to correct it, and "
             "dismissing that leaves the log unopened and nothing saved."),
          QStringLiteral("pattern"))
{
    m_parser.setApplicationDescription(
        tr("loftail — a desktop viewer for log4cplus logs.\n\n"
           "Opens each file given at its end and follows it as it grows, "
           "like tail -f. Several files open at once, each in its own tab. "
           "Filters and highlights by subsystem and priority."));
    m_parser.addHelpOption();
    m_parser.addVersionOption();

    m_parser.addPositionalArgument(
        QStringLiteral("file"),
        tr("Log file to open (optional). Several may be given, each opening "
           "in its own tab. Either a local path or a remote "
           "log over SSH, spelled ssh://user@host/path/to/file.log. A "
           "compressed log (app.log.gz) opens directly; a log inside an "
           "archive is named by continuing the path through it, as "
           "bundle.tar.gz/var/log/app.log. The two combine, so "
           "ssh://host/var/log/app.log.1.gz works."),
        QStringLiteral("[file...]"));

    m_parser.addOption(m_patternOption);
}

void CommandLine::process(const QCoreApplication &app)
{
    m_parser.process(app);
}

bool CommandLine::parse(const QStringList &arguments)
{
    return m_parser.parse(arguments);
}

QStringList CommandLine::files() const
{
    return m_parser.positionalArguments();
}

QString CommandLine::pattern() const
{
    return m_parser.value(m_patternOption);
}

} // namespace loftail
