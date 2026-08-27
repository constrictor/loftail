#pragma once

#include <QRegularExpression>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace loftail {

// The role a field plays. This — not the pattern string — is what filters,
// highlighters, and the view consume (ARCHITECTURE.md §3: "no component
// downstream of the parser ever sees the pattern string").
enum class FieldRole {
    Date,        // %d / %D
    Priority,    // %p
    Logger,      // %c   (the "subsystem" in SPEC terms)
    Thread,      // %t
    Message,     // %m
    FileName,    // %F, and %b (the basename form of the same thing)
    LineNumber,  // %L
    Method,      // %M

    // The remaining log4cplus conversion specifiers. These are display-only
    // columns: nothing is stored for them on Record (invariant #1 keeps it at 32
    // bytes), so they are re-extracted from the record's first line inside
    // data(), exactly like %F/%L/%M already are.
    Location,    // %l   ("%F:%L")
    ThreadName,  // %T
    ProcessId,   // %i
    Hostname,    // %h / %H
    Elapsed,     // %r   (milliseconds since program start)
    Ndc,         // %x   (nested diagnostic context)
    Mdc,         // %X   (mapped diagnostic context; whole map, or %X{key})
    EnvVar,      // %E{VAR}
};

// One extracted column, in pattern order. Drives the column headers in the view.
struct Field
{
    FieldRole role;
    QString name;   // display header, e.g. "Time", "Priority", "Subsystem"
    int group = -1; // 1-based capture-group index into LogFormat::recordRe
};

// One field of a translated %d{...} format, in the order the text presents them.
// PatternCompiler emits these beside the regex it generated; TimestampParser is
// the only consumer.
//
// The token list exists because DateFormat::qtFormat CANNOT express what
// log4cplus can produce. A space-padded day (%e), epoch seconds (%s), a UTC
// offset (%z), %Q's fractional milliseconds and every "matched but meaningless"
// code (%j, %U, %Z, …) have no Qt spelling at all — so a parser that re-derived
// its own tokens from qtFormat, as TimestampParser did until the strftime set
// was completed, could not read back the very text the compiler had just agreed
// to match. qtFormat is now for DISPLAY only (LogModel's As Written mode).
enum class DateTokenKind {
    Literal,      // one literal character, matched verbatim
    Year4,        // %Y %G
    Year2,        // %y %g
    MonthNumber,  // %m
    MonthName,    // %b %B %h
    Day,          // %d %e   (a space pad is tolerated on the way in)
    Hour24,       // %H %k
    Hour12,       // %I %l   (needs an AmPm token to be unambiguous)
    Minute,       // %M
    Second,       // %S
    Millis,       // %q
    MillisFrac,   // %Q      "123.456" — the microsecond remainder is read and dropped
    AmPm,         // %p %P
    EpochSeconds, // %s      an absolute instant; the source zone does not apply
    UtcOffset,    // %z      overrides the source zone for this record
    SkipDigits,   // %j %u %w %U %V %W %C  — matched, contributes nothing
    SkipWord,     // %a %A %Z              — likewise
};

struct DateToken
{
    DateTokenKind kind = DateTokenKind::Literal;
    int   width = 0;   // maximum digits a numeric token consumes
    QChar literal;     // Literal only
};

// How to read the text a %d / %D specifier produces. PatternCompiler translates
// the log4cplus strftime-style inner format into both a sub-regex (already baked
// into recordRe) and a Qt date format the indexer will hand to QDateTime when it
// parses timestamps in M2. Zone lives on LogFormat, not here (§5.1).
struct DateFormat
{
    QString strftime;  // the inner format as written, or the default when braces were omitted
    QString qtFormat;  // translated to QDateTime::fromString() form, e.g. "yyyy-MM-dd HH:mm:ss"
    bool isValid = false;

    // The %d carries milliseconds (log4cplus %q, translated to "zzz"). DECLARED by
    // the compiler rather than sniffed back out of qtFormat: the compiler knows
    // exactly when it emitted a ms field, whereas a string search re-derives that at
    // a distance and would start lying the moment another code maps to "zzz".
    // Consumed by the seconds display modes, which render s.mmm only when it is set
    // — ".000" on every row of a file whose format has no ms invents precision.
    bool hasMillis = false;

    // The parse program: see DateTokenKind. Empty only for a format that produced
    // no fields at all, where parse() falls back to QDateTime::fromString().
    QVector<DateToken> tokens;
};

// The compiled form of a ConversionPattern. String in (the pattern),
// LogFormat out (ARCHITECTURE.md §3). Everything downstream binds to this.
struct LogFormat
{
    QRegularExpression recordRe;       // matches a full record's start line
    QRegularExpression recordStartRe;  // the prefix up to the message field; identifies record boundaries (§4)

    QVector<Field> fields;             // ordered; drives column headers

    // Role indices: 1-based capture-group numbers into recordRe, or -1 when the
    // pattern omits that field. Missing %p or %c is not an error — the UI simply
    // warns that filtering on that axis is unavailable (SPEC.md §4).
    int dateGroup = -1;
    int prioGroup = -1;
    int loggerGroup = -1;
    int threadGroup = -1;
    int msgGroup = -1;

    DateFormat impliedDateFormat;      // how to parse %d text; isValid=false when the pattern has no date
    Qt::TimeSpec impliedZone = Qt::LocalTime;  // %d implies UTC, %D implies local (§5.1); meaningful only when dateGroup != -1
};

} // namespace loftail
