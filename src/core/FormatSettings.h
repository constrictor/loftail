#pragma once

#include "Encoding.h"

#include <QString>
#include <QTimeZone>
#include <QtGlobal>

namespace loftail {

// How a source or display time zone is chosen (SPEC.md §4). The stored value is
// the user's CHOICE, including the sentinel — never the concrete zone the
// inference resolved to (mirrors the encoding rule, §6.1: persisting the resolved
// value would freeze a guess). toZone() turns a choice into the QTimeZone the
// Document consumes: an INVALID QTimeZone is the sentinel Document reads as
// "infer from the pattern" (source) or "as written / follow source" (display).
struct ZoneChoice
{
    enum class Kind : quint8 {
        Default,      // source: Infer from pattern; display: As written in the file
        Local,        // the system local zone
        Utc,
        FixedOffset,  // offsetSeconds east of UTC (offered for the source zone only)
    };

    Kind kind = Kind::Default;
    int  offsetSeconds = 0;

    // The concrete zone, or an INVALID QTimeZone for Default (the sentinel).
    QTimeZone toZone() const
    {
        switch (kind) {
        case Kind::Local:       return QTimeZone::systemTimeZone();
        case Kind::Utc:         return QTimeZone::utc();
        case Kind::FixedOffset: return QTimeZone(offsetSeconds);
        case Kind::Default:     break;
        }
        return QTimeZone();
    }

    // Round-trip through a plain string for QSettings persistence.
    QString toString() const
    {
        switch (kind) {
        case Kind::Local:       return QStringLiteral("local");
        case Kind::Utc:         return QStringLiteral("utc");
        case Kind::FixedOffset: return QStringLiteral("offset:%1").arg(offsetSeconds);
        case Kind::Default:     break;
        }
        return QStringLiteral("default");
    }

    static ZoneChoice fromString(const QString &s)
    {
        ZoneChoice z;
        if (s == QLatin1String("local"))
            z.kind = Kind::Local;
        else if (s == QLatin1String("utc"))
            z.kind = Kind::Utc;
        else if (s.startsWith(QLatin1String("offset:"))) {
            z.kind = Kind::FixedOffset;
            z.offsetSeconds = QStringView(s).mid(7).toInt();
        } else {
            z.kind = Kind::Default;
        }
        return z;
    }

    bool operator==(const ZoneChoice &o) const
    {
        return kind == o.kind && (kind != Kind::FixedOffset || offsetSeconds == o.offsetSeconds);
    }
    bool operator!=(const ZoneChoice &o) const { return !(*this == o); }
};

// The complete per-file format choice (SPEC.md §4): the ConversionPattern, the
// encoding, and the source/display time-zone choices. This is what the Log Format
// dialog edits, the FormatCache persists per file path, and MainWindow diffs to
// pick the change-cost (encoding rescan / source-zone reparse / display reformat).
//
// The pattern STRING lives here and in ManualFormatProvider only — nothing
// downstream of PatternCompiler ever sees it (invariant #3).
struct FormatSettings
{
    QString    pattern;
    Encoding   encoding = Encoding::Auto;
    ZoneChoice sourceZone;   // Default == infer from the pattern's date specifier
    ZoneChoice displayZone;  // Default == as written in the file (follows source)

    bool operator==(const FormatSettings &o) const
    {
        return pattern == o.pattern && encoding == o.encoding
            && sourceZone == o.sourceZone && displayZone == o.displayZone;
    }
    bool operator!=(const FormatSettings &o) const { return !(*this == o); }
};

} // namespace loftail
