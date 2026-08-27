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

// A month or weekday name. strftime renders these in the process's locale, so the
// run is any letters rather than three ASCII ones — \p{L} and not \w, which
// depends on how PCRE2 was built. A trailing '.' is allowed for the locales whose
// abbreviations carry one.
constexpr QLatin1String kNameRun("\\p{L}+\\.?");

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

// Translate a log4cplus strftime-style date format into a matching sub-regex, a
// Qt date format for display and a token list for parsing. `baseOffset` is the
// position in the original pattern of the first character of `fmt`, so error
// offsets are exact.
struct DateTranslation
{
    QString regex;   // matches the produced date text; no capturing groups
    DateFormat format;
};

// The strftime set log4cplus can produce. It handles %q, %Q and %s itself
// (src/timehelper.cxx) and hands EVERYTHING else to the platform's strftime — so
// the supported vocabulary is C/POSIX strftime plus those three, not the shorter
// list the doxygen page prints. Each code contributes a regex fragment (no
// capturing groups: the whole %d field is one group), a Qt fragment for display,
// and zero or more parse tokens.
//
// Names (%a %A %b %B %h) are matched as \p{L}+ rather than a fixed three letters
// because strftime renders them in the process's locale; the parser reads English
// and the system locale back, which is what a log written in the C locale — every
// syslog line on the machine — needs.
Expected<DateTranslation, CompileError> translateDateFormat(QStringView fmt, int baseOffset);

// A code that strftime defines as a shorthand for other codes. Expanding rather
// than hand-writing each one is what keeps %T and %H:%M:%S provably identical.
QStringView expansionOf(QChar code)
{
    switch (code.unicode()) {
    case u'c': return u"%a %b %e %H:%M:%S %Y"; // C locale's date-and-time
    case u'D': return u"%m/%d/%y";
    case u'F': return u"%Y-%m-%d";
    case u'r': return u"%I:%M:%S %p";
    case u'R': return u"%H:%M";
    case u'T': return u"%H:%M:%S";
    case u'x': return u"%m/%d/%y";             // C locale's date
    case u'X': return u"%H:%M:%S";             // C locale's time
    default:   return {};
    }
}

Expected<DateTranslation, CompileError> translateDateFormat(QStringView fmt, int baseOffset)
{
    DateTranslation result;
    result.format.strftime = fmt.toString();

    QString &re = result.regex;
    QString &qt = result.format.qtFormat;
    QVector<DateToken> &tokens = result.format.tokens;

    // A numeric field: `digits` wide, optionally space-padded on the way in (%e,
    // %k and %l pad with a space rather than a zero).
    const auto numeric = [&](DateTokenKind kind, int digits, bool spacePadded,
                             QLatin1String qtSpelling) {
        re += spacePadded ? QStringLiteral("[ \\d]") + QStringLiteral("\\d{%1}").arg(digits - 1)
                          : QStringLiteral("\\d{%1}").arg(digits);
        qt += qtSpelling;
        tokens.append(DateToken{kind, digits, QChar()});
    };

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
            tokens.append(DateToken{DateTokenKind::Literal, 0, c});
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

        // A composite code is translated by translating what it stands for. Its
        // expansion contains no composites, so the recursion is one deep, and every
        // sub-code is supported, so it cannot fail — but the error is still
        // propagated rather than asserted away.
        if (const QStringView sub = expansionOf(code); !sub.isEmpty()) {
            auto expanded = translateDateFormat(sub, baseOffset + i);
            if (!expanded)
                return Expected<DateTranslation, CompileError>::makeError(expanded.error());
            re += expanded.value().regex;
            qt += expanded.value().format.qtFormat;
            tokens += expanded.value().format.tokens;
            if (expanded.value().format.hasMillis)
                result.format.hasMillis = true;
            i += 2;
            continue;
        }

        switch (code.unicode()) {
        // --- the date ------------------------------------------------------
        case u'Y': numeric(DateTokenKind::Year4, 4, false, QLatin1String("yyyy")); break;
        case u'y': numeric(DateTokenKind::Year2, 2, false, QLatin1String("yy")); break;
        case u'm': numeric(DateTokenKind::MonthNumber, 2, false, QLatin1String("MM")); break;
        case u'd': numeric(DateTokenKind::Day, 2, false, QLatin1String("dd")); break;
        case u'e': numeric(DateTokenKind::Day, 2, true, QLatin1String("d")); break;
        case u'b':
        case u'h': re += kNameRun; qt += QStringLiteral("MMM");
                   tokens.append(DateToken{DateTokenKind::MonthName, 0, QChar()}); break;
        case u'B': re += kNameRun; qt += QStringLiteral("MMMM");
                   tokens.append(DateToken{DateTokenKind::MonthName, 0, QChar()}); break;
        case u'a': re += kNameRun; qt += QStringLiteral("ddd");
                   tokens.append(DateToken{DateTokenKind::SkipWord, 0, QChar()}); break;
        case u'A': re += kNameRun; qt += QStringLiteral("dddd");
                   tokens.append(DateToken{DateTokenKind::SkipWord, 0, QChar()}); break;

        // --- the time ------------------------------------------------------
        case u'H': numeric(DateTokenKind::Hour24, 2, false, QLatin1String("HH")); break;
        case u'k': numeric(DateTokenKind::Hour24, 2, true,  QLatin1String("H")); break;
        case u'I': numeric(DateTokenKind::Hour12, 2, false, QLatin1String("hh")); break;
        case u'l': numeric(DateTokenKind::Hour12, 2, true,  QLatin1String("h")); break;
        case u'M': numeric(DateTokenKind::Minute, 2, false, QLatin1String("mm")); break;
        case u'S': numeric(DateTokenKind::Second, 2, false, QLatin1String("ss")); break;
        case u'p': re += QStringLiteral("[AP]M"); qt += QStringLiteral("AP");
                   tokens.append(DateToken{DateTokenKind::AmPm, 0, QChar()}); break;
        case u'P': re += QStringLiteral("[ap]m"); qt += QStringLiteral("ap");
                   tokens.append(DateToken{DateTokenKind::AmPm, 0, QChar()}); break;

        // --- log4cplus's own three (src/timehelper.cxx) ---------------------
        case u'q': re += QStringLiteral("\\d{3}"); qt += QStringLiteral("zzz");
                   tokens.append(DateToken{DateTokenKind::Millis, 3, QChar()});
                   result.format.hasMillis = true; break;
        case u'Q': // Milliseconds and a sub-millisecond remainder. log4cplus writes
                   // "123.456"; rsyslog's RFC3339 stamp — the shape /var/log/syslog
                   // has carried since Debian 12 and Ubuntu 24.04 — writes the same
                   // information as "123456", with no separator and any width. Both
                   // are accepted, and the remainder is dropped either way: Record
                   // has no room below a millisecond (invariant #1).
                   re += QStringLiteral("\\d{3}(?:\\.\\d{1,6}|\\d{1,6})?");
                   qt += QStringLiteral("zzz");
                   tokens.append(DateToken{DateTokenKind::MillisFrac, 3, QChar()});
                   result.format.hasMillis = true; break;
        case u's': // Seconds since the epoch: an instant, so no Qt spelling and no
                   // zone. qtFormat is filled in below when this was the whole format.
                   re += QStringLiteral("\\d{1,12}");
                   tokens.append(DateToken{DateTokenKind::EpochSeconds, 12, QChar()}); break;

        // --- the zone ------------------------------------------------------
        case u'z': // +hhmm, and the +hh:mm and Z spellings a log may carry instead.
                   re += QStringLiteral("(?:[+-]\\d{2}:?\\d{2}|Z)"); qt += QLatin1Char('t');
                   tokens.append(DateToken{DateTokenKind::UtcOffset, 0, QChar()}); break;
        case u'Z': re += QStringLiteral("[A-Za-z][A-Za-z0-9_/+-]*"); qt += QLatin1Char('t');
                   tokens.append(DateToken{DateTokenKind::SkipWord, 0, QChar()}); break;

        // --- matched, but carrying nothing this parser can use ---------------
        case u'j': re += QStringLiteral("\\d{3}");
                   tokens.append(DateToken{DateTokenKind::SkipDigits, 3, QChar()}); break;
        case u'C':
        case u'g':
        case u'U':
        case u'V':
        case u'W': re += QStringLiteral("\\d{2}");
                   tokens.append(DateToken{DateTokenKind::SkipDigits, 2, QChar()}); break;
        // An ISO week-numbering year is not the calendar year — it differs across a
        // year boundary — so it is matched and dropped rather than read as %Y.
        case u'G': re += QStringLiteral("\\d{4}");
                   tokens.append(DateToken{DateTokenKind::SkipDigits, 4, QChar()}); break;
        case u'u':
        case u'w': re += QStringLiteral("\\d");
                   tokens.append(DateToken{DateTokenKind::SkipDigits, 1, QChar()}); break;

        // --- literals -------------------------------------------------------
        case u't': re += QStringLiteral("\\t"); qt += QStringLiteral("'\t'");
                   tokens.append(DateToken{DateTokenKind::Literal, 0, QLatin1Char('\t')}); break;
        case u'%': re += QRegularExpression::escape(QStringLiteral("%")); qt += QLatin1Char('%');
                   tokens.append(DateToken{DateTokenKind::Literal, 0, QLatin1Char('%')}); break;

        case u'n':
            // strftime's newline. A record's start line is one line by definition
            // (invariant #2), so a timestamp containing one can never be matched —
            // say so rather than compiling a regex that silently matches nothing.
            return Expected<DateTranslation, CompileError>::makeError(
                makeError(CompileError::Code::UnsupportedDateCode,
                          // Through specText() rather than written in: a literal "%n"
                          // in a tr() source string is Qt's plural-count placeholder.
                          Tr::tr("A newline ('%1') inside a timestamp cannot appear "
                                 "within one log line").arg(specText(code)),
                          baseOffset + i));
        default:
            return Expected<DateTranslation, CompileError>::makeError(
                makeError(CompileError::Code::UnsupportedDateCode,
                          Tr::tr("Unsupported date code '%1' inside %d{...}").arg(specText(code)),
                          baseOffset + i));
        }
        i += 2;
    }

    // A format made only of codes Qt cannot spell (%s on its own is the realistic
    // one) would leave the As Written column blank. Give it something readable.
    if (qt.isEmpty())
        qt = QString(kDefaultDateFormat);

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
            pieces.append(Piece{compileLiteral(literalBuf), false, FieldRole::Message, false, {}});
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
