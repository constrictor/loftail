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
//   %d{fmt} / %D{fmt}   date; %d implies local time, %D implies UTC (§5.1).
//                       Braces optional; a default format is used when omitted.
//   %p  priority   %c  logger (subsystem)   %t  thread
//   %m  message    %F  file name   %L  line number   %M  method   %n  newline
//   %%  a literal percent sign
//
// Supported modifiers, in log4cplus order  %[-][minWidth][.maxWidth]X :
//   -         left-justify (default is right-justify)
//   minWidth  pad with spaces to at least this width
//   .maxWidth truncate to at most this many characters
//
// Unknown specifiers, and strftime codes outside the supported subset, produce a
// CompileError with an offset into the pattern rather than a silent mismatch.
class PatternCompiler
{
public:
    static Expected<LogFormat, CompileError> compile(QStringView pattern);

private:
    PatternCompiler() = delete;
};

} // namespace loftail
