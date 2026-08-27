#pragma once

#include <QStringView>

#include "CompileError.h"
#include "Expected.h"
#include "LogFormat.h"

namespace loftail {

// Compiles a log4cplus ConversionPattern into a LogFormat (a regex plus a field
// map). Pure: string in, LogFormat or a structured CompileError out. No I/O, no
// Qt GUI types, no QApplication — the most testable unit in the project, built
// and covered first (ARCHITECTURE.md §3, PLAN.md M1).
//
// Supported specifiers:
//   %d{fmt} / %D{fmt}   date; %d implies UTC, %D implies local time (§5.1) — that
//                       is log4cplus's mapping, not a typo. Braces optional; a
//                       default format is used when omitted.
//   %p  priority   %c{N}  logger (subsystem)   %t  thread id   %T  thread name
//   %m  message    %F  file name   %b  file basename   %L  line number
//   %l  file:line  %M  method   %i  process id   %r  ms since program start
//   %h  hostname   %H  fully-qualified hostname   %n  newline
//   %x  NDC        %X / %X{key}  MDC      %E{VAR}  environment variable
//   %%  a literal percent sign
//
// That is the whole set log4cplus's PatternLayout defines; anything else is a
// CompileError rather than a silently dropped column.
//
// Supported modifiers, in log4cplus order  %[-][minWidth][.maxWidth]X :
//   -         left-justify (default is right-justify)
//   minWidth  pad with spaces to at least this width
//   .maxWidth truncate to at most this many characters
//
// Inside %d{...} / %D{...}: log4cplus renders %q (milliseconds), %Q (milliseconds
// with a microsecond remainder, "123.456") and %s (seconds since the epoch) itself
// and hands everything else to the platform's strftime — so the whole C/POSIX
// strftime vocabulary is accepted, not the shorter list the doxygen page prints:
//
//   %a %A %b %B %h    weekday and month names          %C %g %G  century, ISO year
//   %c %D %F %r %R    composites, expanded to what     %j %u %U %V %w %W  ordinals
//   %T %x %X          they stand for                   %z %Z     zone
//   %d %e %H %I %k %l %m %M %p %P %S %y %Y             %t %%     literals
//
// Only %n is refused: a record's start line is one line (invariant #2), so a
// timestamp carrying a newline could never match. Codes that name nothing this
// reader can use — a week number, a day of the year, a zone abbreviation — are
// matched and dropped rather than rejected, so the format still compiles.
//
// Unknown specifiers, and strftime codes outside the supported subset, produce a
// CompileError with an offset into the pattern rather than a silent mismatch.
class PatternCompiler
{
public:
    static Expected<LogFormat, CompileError> compile(QStringView pattern);

    // Public: see FormatDetector.h — a deleted function says so more clearly than an
    // inaccessible one, and this class is a namespace of static functions.
    PatternCompiler() = delete;
};

} // namespace loftail
