#include "CommandLine.h"

namespace loftail {

CommandLine::CommandLine()
    : m_patternOption(
          QStringLiteral("pattern"),
          tr("log4cplus ConversionPattern for the format of the files named. "
             "Used only for a file loftail has not seen before; a file "
             "with a remembered format ignores it. A pattern that does "
             "not match opens the file as plain text."),
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
