#pragma once

#include <QString>
#include <QtGlobal>

namespace loftail {

// How the timestamp column renders (SPEC.md §4). Chosen per FILE from the Date
// column's header context menu, which is the SOLE control: the first three kinds
// are what the format editor's old "Display time zone" combo used to be, so that
// combo is gone and this enum replaced FormatSettings::displayZone.
//
// AsWritten/LocalTime/Utc render the file's OWN date format
// (LogFormat::impliedDateFormat.qtFormat) in a zone derived from the kind.
// EpochSeconds/RunSeconds/SincePrevious render a plain number and involve no zone at
// all: Record::timestamp is already UTC epoch ms (invariant #10), so seconds are a
// subtraction and a divide. Document::displayZone() derives to the source zone
// for those three, where it simply goes unused by the Date column.
//
// SincePrevious is the one mode whose cell depends on ANOTHER ROW rather than on this
// record plus a partition: it is the gap to the record on the row above IN THE TABLE
// ASKING, which is what makes it compose with filters (filter to one subsystem and the
// column reads that subsystem's cadence) and what keeps it meaningful in the digest
// strip, a second view over a different subset (ARCHITECTURE.md §5.1, §7.5.1).
enum class TimeDisplay : quint8 {
    AsWritten     = 0,  // the file's own format, no conversion (the default)
    LocalTime     = 1,
    Utc           = 2,
    EpochSeconds  = 3,  // seconds since the epoch; s.mmm when the format has %q
    RunSeconds    = 4,  // seconds since this record's run started; s.mmm likewise
    SincePrevious = 5,  // seconds since the previous VISIBLE record; s.mmm likewise
};

// Round-trip through a plain string for QSettings persistence. The vocabulary
// deliberately EXTENDS the legacy ZoneChoice one rather than replacing it —
// "local" and "utc" keep their old spelling — so a store written before this
// setting existed reads correctly through the legacy-key fallback in LogSettingsStore and
// SessionStore, with no mapping table.
inline QString timeDisplayToString(TimeDisplay d)
{
    switch (d) {
    case TimeDisplay::LocalTime:    return QStringLiteral("local");
    case TimeDisplay::Utc:          return QStringLiteral("utc");
    case TimeDisplay::EpochSeconds: return QStringLiteral("epochSeconds");
    case TimeDisplay::RunSeconds:   return QStringLiteral("runSeconds");
    case TimeDisplay::SincePrevious: return QStringLiteral("sincePrevious");
    case TimeDisplay::AsWritten:    break;
    }
    return QStringLiteral("asWritten");
}

// Anything unrecognized maps to AsWritten. That covers the legacy display-zone
// spellings "default" and "offset:N", which both meant "as written" for display —
// and it is also what a binary older than a mode makes of that mode's spelling, which
// is why a new value costs no schema version: an older loftail reading a store that
// names "sincePrevious" shows the column as written rather than refusing the file.
inline TimeDisplay timeDisplayFromString(const QString &s)
{
    if (s == QLatin1String("local"))
        return TimeDisplay::LocalTime;
    if (s == QLatin1String("utc"))
        return TimeDisplay::Utc;
    if (s == QLatin1String("epochSeconds"))
        return TimeDisplay::EpochSeconds;
    if (s == QLatin1String("runSeconds"))
        return TimeDisplay::RunSeconds;
    if (s == QLatin1String("sincePrevious"))
        return TimeDisplay::SincePrevious;
    return TimeDisplay::AsWritten;
}

} // namespace loftail
