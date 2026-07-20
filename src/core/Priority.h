#pragma once

#include <QLatin1StringView>
#include <QStringView>
#include <QtGlobal>

namespace loftail {

// log4cplus severity levels, DECLARED IN SEVERITY ORDER so that a minimum-level
// priority filter is a single integer `>=` test (ARCHITECTURE.md §7.2,
// invariant #4). `Unknown` sorts BELOW `Trace` on purpose: unparsed records
// carry no priority and must never be hidden by a minimum-level filter (§7.2).
//
// Stored in Record::priority as a quint8 — see Record.h.
enum class Priority : quint8 {
    Unknown = 0,  // unparsed line / no %p in the pattern
    Trace   = 1,
    Debug   = 2,
    Info    = 3,
    Warn    = 4,
    Error   = 5,
    Fatal   = 6,
};

// Map a priority token (the %p capture) to the enum. Case-sensitive on the
// canonical upper-case log4cplus spellings, which is what the framework emits;
// anything else is Unknown. Uses QStringView to avoid allocating on the parse
// path (CLAUDE.md conventions).
inline Priority parsePriority(QStringView token)
{
    token = token.trimmed();
    switch (token.size()) {
    case 4:
        if (token == QLatin1StringView("INFO")) return Priority::Info;
        if (token == QLatin1StringView("WARN")) return Priority::Warn;
        break;
    case 5:
        if (token == QLatin1StringView("TRACE")) return Priority::Trace;
        if (token == QLatin1StringView("DEBUG")) return Priority::Debug;
        if (token == QLatin1StringView("ERROR")) return Priority::Error;
        if (token == QLatin1StringView("FATAL")) return Priority::Fatal;
        break;
    default:
        break;
    }
    return Priority::Unknown;
}

// Display name for a priority. Empty for Unknown so an unparsed record shows a
// blank priority cell rather than the literal word "Unknown".
inline QLatin1StringView priorityName(Priority p)
{
    switch (p) {
    case Priority::Trace: return QLatin1StringView("TRACE");
    case Priority::Debug: return QLatin1StringView("DEBUG");
    case Priority::Info:  return QLatin1StringView("INFO");
    case Priority::Warn:  return QLatin1StringView("WARN");
    case Priority::Error: return QLatin1StringView("ERROR");
    case Priority::Fatal: return QLatin1StringView("FATAL");
    case Priority::Unknown:
        break;
    }
    return QLatin1StringView("");
}

} // namespace loftail
