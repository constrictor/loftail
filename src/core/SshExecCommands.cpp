#include "SshExecCommands.h"

#include <QList>

namespace loftail {

QString shellQuote(const QString &path)
{
    QString out;
    out.reserve(path.size() + 2);
    out += QLatin1Char('\'');
    for (const QChar c : path) {
        if (c == QLatin1Char('\'')) {
            // Close the quoted run, emit a backslash-escaped quote outside it, reopen.
            // The only way to get a literal ' into a single-quoted POSIX word.
            out += QLatin1String("'\\''");
        } else {
            out += c;
        }
    }
    out += QLatin1Char('\'');
    return out;
}

QString statCommand(const QString &path)
{
    const QString quoted = shellQuote(path);
    // GNU coreutils first because it is what Linux servers have; BSD/macOS second. The
    // redirect keeps a failed first attempt from mixing its complaint into the output
    // that the second one's answer has to be parsed out of.
    return QStringLiteral("stat -c '%s %Y' %1 2>/dev/null || stat -f '%z %m' %1")
        .arg(quoted);
}

QString readCommand(const QString &path, qint64 offset, qint64 length)
{
    const QString quoted = shellQuote(path);
    // +1 because `tail -c +N` is 1-BASED: +1 is the whole file, not "skip one byte".
    const qint64 from = qMax<qint64>(0, offset) + 1;
    return QStringLiteral("tail -c +%1 %2 | head -c %3")
        .arg(from)
        .arg(quoted)
        .arg(qMax<qint64>(0, length));
}

QByteArray probeMarker()
{
    return QByteArrayLiteral("loftail-exec-ok");
}

QString probeCommand()
{
    // Checks the two utilities the transport actually needs rather than merely that a
    // shell answered: an account confined to a restricted shell can exit 0 having run
    // nothing at all, and finding that out here is much cheaper than finding it out
    // once a log is supposedly open.
    return QStringLiteral("command -v tail >/dev/null 2>&1 && command -v stat >/dev/null 2>&1 "
                          "&& echo %1")
        .arg(QString::fromLatin1(probeMarker()));
}

ExecAttrs parseStatOutput(const QByteArray &output)
{
    ExecAttrs out;
    // Both stat flavours print one line, but a server may prepend a banner or a warning
    // on stdout. Take the LAST non-empty line: the answer is what the command printed
    // last, and anything before it is somebody else's noise.
    const QList<QByteArray> lines = output.split('\n');
    QByteArray line;
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QByteArray trimmed = it->trimmed();
        if (!trimmed.isEmpty()) {
            line = trimmed;
            break;
        }
    }
    if (line.isEmpty())
        return out;

    const QList<QByteArray> fields = line.simplified().split(' ');
    if (fields.size() != 2)
        return out;

    bool sizeOk = false;
    bool mtimeOk = false;
    const qint64 size = fields.at(0).toLongLong(&sizeOk);
    const qint64 mtime = fields.at(1).toLongLong(&mtimeOk);
    if (!sizeOk || !mtimeOk || size < 0 || mtime < 0)
        return out;

    out.ok = true;
    out.size = size;
    out.mtime = mtime;
    return out;
}

} // namespace loftail
