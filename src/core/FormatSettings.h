#pragma once

#include "Encoding.h"
#include "TimeDisplay.h"

#include <QString>
#include <QTimeZone>
#include <QtGlobal>

namespace loftail {

// How the SOURCE time zone is chosen (SPEC.md §4). The stored value is the user's
// CHOICE, including the sentinel — never the concrete zone the inference resolved
// to (mirrors the encoding rule, §6.1: persisting the resolved value would freeze
// a guess). toZone() turns a choice into the QTimeZone the Document consumes: an
// INVALID QTimeZone is the sentinel Document reads as "infer from the pattern".
//
// The display side no longer uses this type: it is one axis of TimeDisplay now,
// chosen from the timestamp column's header menu (TimeDisplay.h).
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
// encoding, the source time-zone choice, and how timestamps are displayed. This is
// what MainWindow diffs to pick the change-cost (encoding rescan / source-zone
// reparse / display reformat) and what the FormatCache persists per file path.
//
// Note the two editors: the Log Format dialog owns everything here EXCEPT
// `timeDisplay`, whose sole control is the timestamp column's header context menu.
// Both routes funnel through MainWindow::applySettings, so persistence is shared.
//
// The pattern STRING lives here and in ManualFormatProvider only — nothing
// downstream of PatternCompiler ever sees it (invariant #3).
struct FormatSettings
{
    QString    pattern;
    Encoding   encoding = Encoding::Auto;
    ZoneChoice sourceZone;   // Default == infer from the pattern's date specifier

    // How the timestamp column renders (TimeDisplay.h). Free to change — a repaint,
    // never a rescan or a reparse — and it subsumes what used to be a separate
    // display-zone choice.
    TimeDisplay timeDisplay = TimeDisplay::AsWritten;

    // Run splitting (SPEC.md §3a): the run-start regexp that divides the file into
    // app runs. Not part of the log FORMAT (nothing in PatternCompiler reads it), but
    // persisted with the same per-file lifecycle as the format, so it rides in here
    // through the FormatCache and the session. Empty pattern == no run splitting.
    QString    runStartPattern;
    bool       runStartIsRegex = false;
    bool       runStartCaseSensitive = false;

    bool operator==(const FormatSettings &o) const
    {
        return pattern == o.pattern && encoding == o.encoding
            && sourceZone == o.sourceZone && timeDisplay == o.timeDisplay
            && runStartPattern == o.runStartPattern
            && runStartIsRegex == o.runStartIsRegex
            && runStartCaseSensitive == o.runStartCaseSensitive;
    }
    bool operator!=(const FormatSettings &o) const { return !(*this == o); }

    // The run-start axis alone changed (pattern/format identical). MainWindow uses
    // this to pick the CHEAP change-cost path: re-detect runs + re-apply the view,
    // never an encoding rescan or a timestamp reparse.
    bool sameFormatDifferentRun(const FormatSettings &o) const
    {
        return pattern == o.pattern && encoding == o.encoding
            && sourceZone == o.sourceZone && timeDisplay == o.timeDisplay
            && (runStartPattern != o.runStartPattern
                || runStartIsRegex != o.runStartIsRegex
                || runStartCaseSensitive != o.runStartCaseSensitive);
    }
};

} // namespace loftail
