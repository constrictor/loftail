#include "PatternCompiler.h"

#include <QCoreApplication>
#include <QChar>

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr(); Q_DECLARE_TR_FUNCTIONS is what lets lupdate file these under a
// name that means something. Only the PROSE goes through it — every regex fragment,
// Qt date code and log4cplus conversion character in this file stays a literal, and
// mixing the two up would produce a compiler that cannot parse its own output.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::PatternCompiler)
};
} // namespace


namespace {

// The format log4cplus uses for %d / %D when no braces are supplied.
constexpr QLatin1String kDefaultDateFormat("%Y-%m-%d %H:%M:%S");

CompileError makeError(CompileError::Code code, QString message, int offset)
{
    return CompileError{code, std::move(message), offset};
}

// A specifier as the user wrote it, for error messages: "%x", "%d". QString::arg()
// has no "%%" escape — it substitutes the lowest-numbered place marker and leaves
// every other percent alone — so a format string like "'%%%1'" renders BOTH literal
// percents and shows the user '%%x'. Build the '%' in rather than escaping it.
QString specText(QChar spec)
{
    return QLatin1Char('%') + QString(spec);
}

// Compile a run of literal pattern text into a regex fragment. Whitespace runs
// become `\s+` so that field padding (%-5p leaves a trailing space) is absorbed
// by the separators around it; everything else is matched exactly via
// QRegularExpression::escape. Produces no capturing groups, which keeps the
// group numbering equal to the field order.
QString compileLiteral(QStringView text)
{
    QString out;
    int i = 0;
    const int n = int(text.size());
    while (i < n) {
        if (text[i].isSpace()) {
            while (i < n && text[i].isSpace())
                ++i;
            out += QStringLiteral("\\s+");
        } else {
            const int start = i;
            while (i < n && !text[i].isSpace())
                ++i;
            out += QRegularExpression::escape(text.sliced(start, i - start).toString());
        }
    }
    return out;
}

// Translate a log4cplus strftime-style date format into a matching sub-regex and
// a Qt date format. Handles the common numeric subset and rejects the rest with
// a clear error (PLAN.md M1 risk note). `baseOffset` is the position in the
// original pattern of the first character of `fmt`, so error offsets are exact.
struct DateTranslation
{
    QString regex;   // matches the produced date text; no capturing groups
    DateFormat format;
};

Expected<DateTranslation, CompileError> translateDateFormat(QStringView fmt, int baseOffset)
{
    DateTranslation result;
    result.format.strftime = fmt.toString();

    QString &re = result.regex;
    QString &qt = result.format.qtFormat;

    int i = 0;
    const int n = int(fmt.size());
    while (i < n) {
        const QChar c = fmt[i];
        if (c != QLatin1Char('%')) {
            // A literal character inside the date format.
            re += QRegularExpression::escape(QString(c));
            if (c.isLetter())
                qt += QLatin1Char('\'') + QString(c) + QLatin1Char('\''); // quote so Qt treats it as literal
            else
                qt += c;
            ++i;
            continue;
        }

        if (i + 1 >= n) {
            return Expected<DateTranslation, CompileError>::makeError(
                makeError(CompileError::Code::DanglingPercentInDate,
                          Tr::tr("Date format ends with a stray '%'"),
                          baseOffset + i));
        }

        const QChar code = fmt[i + 1];
        switch (code.unicode()) {
        case u'Y': re += QStringLiteral("\\d{4}"); qt += QStringLiteral("yyyy"); break;
        case u'y': re += QStringLiteral("\\d{2}"); qt += QStringLiteral("yy"); break;
        case u'm': re += QStringLiteral("\\d{2}"); qt += QStringLiteral("MM"); break;
        case u'd': re += QStringLiteral("\\d{2}"); qt += QStringLiteral("dd"); break;
        case u'H': re += QStringLiteral("\\d{2}"); qt += QStringLiteral("HH"); break;
        case u'I': re += QStringLiteral("\\d{2}"); qt += QStringLiteral("hh"); break;
        case u'M': re += QStringLiteral("\\d{2}"); qt += QStringLiteral("mm"); break;
        case u'S': re += QStringLiteral("\\d{2}"); qt += QStringLiteral("ss"); break;
        case u'p': re += QStringLiteral("[AP]M"); qt += QStringLiteral("AP"); break;
        case u'q': re += QStringLiteral("\\d{3}"); qt += QStringLiteral("zzz");
                   result.format.hasMillis = true; break; // log4cplus milliseconds
        case u'%': re += QRegularExpression::escape(QStringLiteral("%")); qt += QLatin1Char('%'); break;
        default:
            return Expected<DateTranslation, CompileError>::makeError(
                makeError(CompileError::Code::UnsupportedDateCode,
                          Tr::tr("Unsupported date code '%1' inside %d{...}").arg(specText(code)),
                          baseOffset + i));
        }
        i += 2;
    }

    result.format.isValid = true;
    return result;
}

QString fieldName(FieldRole role)
{
    switch (role) {
    case FieldRole::Date:       return Tr::tr("Time");
    case FieldRole::Priority:   return Tr::tr("Priority");
    case FieldRole::Logger:     return Tr::tr("Subsystem");
    case FieldRole::Thread:     return Tr::tr("Thread");
    case FieldRole::Message:    return Tr::tr("Message");
    case FieldRole::FileName:   return Tr::tr("File");
    case FieldRole::LineNumber: return Tr::tr("Line");
    case FieldRole::Method:     return Tr::tr("Method");
    case FieldRole::Location:   return Tr::tr("Location");
    case FieldRole::ThreadName: return Tr::tr("Thread name");
    case FieldRole::ProcessId:  return Tr::tr("PID");
    case FieldRole::Hostname:   return Tr::tr("Host");
    case FieldRole::Elapsed:    return Tr::tr("Elapsed");
    case FieldRole::Ndc:        return Tr::tr("NDC");
    case FieldRole::Mdc:        return Tr::tr("MDC");
    case FieldRole::EnvVar:     return Tr::tr("Env");
    }
    return {};
}

// The three context specifiers (%x, %X, %E) produce arbitrary application text:
// it can contain spaces and, when no context is set, can be empty — so `\S+`,
// which every other field uses, is wrong on both counts. A lazy `.*?` bounded by
// whatever literal follows it is the best available: with a separator after the
// field it matches exactly the context text, and with none it degrades to the
// shortest match rather than swallowing the fields that follow.
constexpr QLatin1String kFreeText(".*?");

// One compiled chunk of the pattern: either an inert literal, or a field that
// contributes exactly one capturing group to recordRe.
struct Piece
{
    QString regex;            // fragment appended to recordRe
    bool isField = false;
    FieldRole role = FieldRole::Message;
    bool isMessage = false;
    QString name;             // overrides fieldName(role) when the specifier carried
                              // an argument worth showing, e.g. %X{key} -> "MDC[key]"
};

// Wrap a field's base pattern in a capture group, allowing surrounding space to
// be absorbed when the field carries a min-width (which pads with spaces). The
// padding-absorb matters for bracketed padded fields like "[%5t]" where there is
// no whitespace separator to soak up the pad.
Piece makeFieldPiece(const QString &base, FieldRole role, bool hasMinWidth, bool isMessage)
{
    Piece p;
    p.isField = true;
    p.role = role;
    p.isMessage = isMessage;
    p.regex = QLatin1Char('(') + base + QLatin1Char(')');
    if (hasMinWidth)
        p.regex = QStringLiteral("\\s*") + p.regex + QStringLiteral("\\s*");
    return p;
}

} // namespace

Expected<LogFormat, CompileError> PatternCompiler::compile(QStringView pattern)
{
    if (pattern.isEmpty()) {
        return Expected<LogFormat, CompileError>::makeError(
            makeError(CompileError::Code::EmptyPattern, Tr::tr("Pattern is empty"), 0));
    }

    QVector<Piece> pieces;
    QString literalBuf;
    LogFormat format;

    auto flushLiteral = [&]() {
        if (!literalBuf.isEmpty()) {
            pieces.append(Piece{compileLiteral(literalBuf), false, FieldRole::Message, false});
            literalBuf.clear();
        }
    };

    const int len = int(pattern.size());
    int i = 0;
    int groupCounter = 0;

    while (i < len) {
        const QChar ch = pattern[i];
        if (ch != QLatin1Char('%')) {
            literalBuf += ch;
            ++i;
            continue;
        }

        // At a '%'. Parse optional modifier  [-][digits][.digits]  then the specifier.
        const int percentPos = i;
        int j = i + 1;
        if (j >= len) {
            return Expected<LogFormat, CompileError>::makeError(
                makeError(CompileError::Code::DanglingPercent,
                          Tr::tr("Pattern ends with a stray '%'"), percentPos));
        }

        bool hasMinWidth = false;
        if (pattern[j] == QLatin1Char('-'))
            ++j; // left-justify flag
        while (j < len && pattern[j].isDigit()) {
            hasMinWidth = true;
            ++j;
        }
        if (j < len && pattern[j] == QLatin1Char('.')) {
            ++j;
            while (j < len && pattern[j].isDigit())
                ++j; // maxWidth (truncation) — parsed, but does not change the regex
        }

        if (j >= len) {
            return Expected<LogFormat, CompileError>::makeError(
                makeError(CompileError::Code::DanglingPercent,
                          Tr::tr("Modifier is not followed by a specifier"), percentPos));
        }

        const QChar spec = pattern[j];

        // %% is a literal percent; modifiers (if any) are meaningless before it.
        if (spec == QLatin1Char('%')) {
            literalBuf += QLatin1Char('%');
            i = j + 1;
            continue;
        }

        // Any real field or newline terminates the pending literal run.
        auto emitSimpleField = [&](const QString &base, FieldRole role, int &groupOut) {
            flushLiteral();
            const bool isMsg = (role == FieldRole::Message);
            pieces.append(makeFieldPiece(base, role, hasMinWidth, isMsg));
            groupOut = ++groupCounter;
        };

        // Consume an optional "{argument}" after the specifier — %c{2} (rightmost
        // logger components), %X{key} (one MDC entry), %E{VAR} (one environment
        // variable). Returns the index just past it, `k` when there is no brace,
        // or -1 when a '{' is never closed.
        auto scanBraceArg = [&](int k, QString *argOut) -> int {
            if (k >= len || pattern[k] != QLatin1Char('{'))
                return k;
            int end = k + 1;
            while (end < len && pattern[end] != QLatin1Char('}'))
                ++end;
            if (end >= len)
                return -1;
            if (argOut)
                *argOut = pattern.sliced(k + 1, end - k - 1).toString();
            return end + 1;
        };
        auto unterminatedBrace = [&](int at) {
            return Expected<LogFormat, CompileError>::makeError(
                makeError(CompileError::Code::UnterminatedBrace,
                          Tr::tr("'%1{' has no closing '}'").arg(specText(spec)), at));
        };

        switch (spec.unicode()) {
        case u'd':
        case u'D': {
            flushLiteral();
            // Optional {inner-format}.
            QString defaultHolder;        // keeps the default format alive for innerFmt
            QStringView innerFmt;
            int fmtOffset = j; // used only for error reporting
            int k = j + 1;
            if (k < len && pattern[k] == QLatin1Char('{')) {
                const int braceContentStart = k + 1;
                int end = braceContentStart;
                while (end < len && pattern[end] != QLatin1Char('}'))
                    ++end;
                if (end >= len) {
                    return Expected<LogFormat, CompileError>::makeError(
                        makeError(CompileError::Code::UnterminatedDateBrace,
                                  Tr::tr("'%1{' has no closing '}'").arg(specText(spec)),
                                  k));
                }
                innerFmt = pattern.sliced(braceContentStart, end - braceContentStart);
                fmtOffset = braceContentStart;
                k = end + 1;
            } else {
                defaultHolder = QString(kDefaultDateFormat);
                innerFmt = defaultHolder;
                // fmtOffset stays at j: with no {inner-format} the whole specifier is
                // what an error points at.
            }

            auto translated = translateDateFormat(innerFmt, fmtOffset);
            if (!translated)
                return Expected<LogFormat, CompileError>::makeError(translated.error());

            pieces.append(makeFieldPiece(translated.value().regex, FieldRole::Date, hasMinWidth, false));
            format.dateGroup = ++groupCounter;
            format.impliedDateFormat = translated.value().format;
            // log4cplus's layout.h is explicit and counter-intuitive: %d is the UTC
            // specifier and %D is the local-time one. Do not "correct" this to the
            // reading that looks natural — it was wrong that way until 2026-07-28.
            format.impliedZone = (spec == QLatin1Char('d')) ? Qt::UTC : Qt::LocalTime;
            i = k;
            continue;
        }
        case u'p': emitSimpleField(QStringLiteral("\\S+"), FieldRole::Priority, format.prioGroup); i = j + 1; continue;
        case u'c': {
            // %c{N} prints only the N rightmost components of the logger name. The
            // text is still a single whitespace-free token either way, so the
            // argument changes no regex — but it must be consumed, or "{2}" would
            // be compiled as literal text the log line does not contain.
            const int k = scanBraceArg(j + 1, nullptr);
            if (k < 0)
                return unterminatedBrace(j + 1);
            emitSimpleField(QStringLiteral("\\S+"), FieldRole::Logger, format.loggerGroup);
            i = k;
            continue;
        }
        case u't': emitSimpleField(QStringLiteral("\\S+"), FieldRole::Thread, format.threadGroup); i = j + 1; continue;
        case u'T': { int dummy = -1; emitSimpleField(QStringLiteral("\\S+"), FieldRole::ThreadName, dummy); i = j + 1; continue; }
        case u'F': { int dummy = -1; emitSimpleField(QStringLiteral("\\S+"), FieldRole::FileName, dummy); i = j + 1; continue; }
        case u'b': { int dummy = -1; emitSimpleField(QStringLiteral("\\S+"), FieldRole::FileName, dummy); i = j + 1; continue; }
        case u'L': { int dummy = -1; emitSimpleField(QStringLiteral("\\d+"), FieldRole::LineNumber, dummy); i = j + 1; continue; }
        case u'l': { int dummy = -1; emitSimpleField(QStringLiteral("\\S+"), FieldRole::Location, dummy); i = j + 1; continue; }
        case u'M': { int dummy = -1; emitSimpleField(QStringLiteral("\\S+"), FieldRole::Method, dummy); i = j + 1; continue; }
        case u'i': { int dummy = -1; emitSimpleField(QStringLiteral("\\d+"), FieldRole::ProcessId, dummy); i = j + 1; continue; }
        case u'r': { int dummy = -1; emitSimpleField(QStringLiteral("\\d+"), FieldRole::Elapsed, dummy); i = j + 1; continue; }
        case u'h':
        case u'H': { int dummy = -1; emitSimpleField(QStringLiteral("\\S+"), FieldRole::Hostname, dummy); i = j + 1; continue; }
        case u'x': { int dummy = -1; emitSimpleField(kFreeText, FieldRole::Ndc, dummy); i = j + 1; continue; }
        case u'X': {
            // %X is the whole MDC map; %X{key} is one entry. The key is worth
            // showing, so it names the column.
            QString key;
            const int k = scanBraceArg(j + 1, &key);
            if (k < 0)
                return unterminatedBrace(j + 1);
            int dummy = -1;
            emitSimpleField(kFreeText, FieldRole::Mdc, dummy);
            if (!key.isEmpty())
                pieces.last().name = Tr::tr("MDC[%1]").arg(key);
            i = k;
            continue;
        }
        case u'E': {
            QString var;
            const int k = scanBraceArg(j + 1, &var);
            if (k < 0)
                return unterminatedBrace(j + 1);
            int dummy = -1;
            emitSimpleField(kFreeText, FieldRole::EnvVar, dummy);
            if (!var.isEmpty())
                pieces.last().name = Tr::tr("Env[%1]").arg(var);
            i = k;
            continue;
        }
        case u'm': emitSimpleField(QStringLiteral(".*"), FieldRole::Message, format.msgGroup); i = j + 1; continue;
        case u'n':
            // Platform newline: not part of a decoded line's content, so it emits
            // nothing. It still flushes the pending literal.
            flushLiteral();
            i = j + 1;
            continue;
        default:
            return Expected<LogFormat, CompileError>::makeError(
                makeError(CompileError::Code::UnknownSpecifier,
                          Tr::tr("Unknown conversion specifier '%1'").arg(specText(spec)), j));
        }
    }

    flushLiteral();

    // Assign group numbers to Field entries and build recordRe / recordStartRe.
    // Group numbering follows field order because only field pieces contribute a
    // capturing group; literals and the date sub-regex introduce none.
    QString recordPattern = QStringLiteral("^");
    QString startPattern = QStringLiteral("^");
    int fieldCounter = 0;
    int messagePieceIndex = -1;
    for (int p = 0; p < pieces.size(); ++p) {
        if (pieces[p].isMessage) {
            messagePieceIndex = p;
            break;
        }
    }

    for (int p = 0; p < pieces.size(); ++p) {
        const Piece &piece = pieces[p];
        recordPattern += piece.regex;
        if (messagePieceIndex < 0 || p < messagePieceIndex)
            startPattern += piece.regex;

        if (piece.isField) {
            ++fieldCounter;
            const QString name = piece.name.isEmpty() ? fieldName(piece.role) : piece.name;
            format.fields.append(Field{piece.role, name, fieldCounter});
        }
    }
    recordPattern += QLatin1Char('$');

    format.recordRe = QRegularExpression(recordPattern);
    format.recordStartRe = QRegularExpression(startPattern);

    if (!format.recordRe.isValid() || !format.recordStartRe.isValid()) {
        return Expected<LogFormat, CompileError>::makeError(
            makeError(CompileError::Code::InvalidRegex,
                      Tr::tr("Internal error: generated an invalid regular expression"), -1));
    }

    return format;
}

} // namespace loftail
