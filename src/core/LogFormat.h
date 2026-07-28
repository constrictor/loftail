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
