#pragma once

#include "FormatSettings.h"
#include "WrapMode.h"

#include <QJsonObject>

namespace loftail {

// Everything one node of the settings tree says about a log (SPEC.md §4): the format
// (pattern, encoding, source zone, timestamp display, run splitting) and the wrap mode
// a new view of it starts in.
//
// WHOLE-NODE VALUES, no per-field merging. The deepest matching node wins outright and
// the levels never mix — the rule the two-store arrangement this replaced already had,
// kept deliberately. A file node that differs from its pattern in one field therefore
// carries a complete copy of the rest; "Promote to Parent Pattern" in Preferences is what
// puts that copy back where it belongs.
//
// `wrapMode` sits BESIDE FormatSettings rather than inside it because MainWindow diffs
// FormatSettings to pick the cost of a change (rescan / reparse / repaint). A wrap
// change costs nothing and must not be mistaken for a format change.
struct LogProfile
{
    FormatSettings format;
    WrapMode       wrapMode = WrapMode::Off;

    // What a log nobody has said anything about gets, before any node exists: the
    // conventional log4cplus layout, auto-detected encoding, zone inferred from the
    // pattern, no wrapping. This is the value the ROOT node is created with.
    static LogProfile builtIn();

    bool operator==(const LogProfile &o) const
    {
        return format == o.format && wrapMode == o.wrapMode;
    }
    bool operator!=(const LogProfile &o) const { return !(*this == o); }
};

// The profile's JSON form, shared by the two stores that hold one: logsettings.json's
// defaults and pattern nodes (LogSettingsStore) and the per-log records (LogFileStore).
// One spelling in one place, because a profile moves between those two levels every time
// somebody presses Promote to Parent Pattern — two serializers would have to agree
// key for key, and would drift the first time a field was added to only one of them.
//
// The keys are the ones M3's FormatCache and the QSettings session already used for the
// same fields, kept verbatim, so a value still moves between stores with no mapping
// table (ARCHITECTURE.md §8). They are JSON keys and are NEVER translated (§9.1).
QJsonObject logProfileToJson(const LogProfile &p);

// PRESENCE, NOT EMPTINESS, for `pattern`. An empty saved pattern is a real answer — it
// parses nothing, so every log it applies to reaches the format dialog, which is how a
// user asks to be consulted about each one. Reading empty as "nothing saved" would
// silently reinstate the built-in and make that setting unreachable. Every other key
// falls back to its struct default, so a missing one is benign.
LogProfile logProfileFromJson(const QJsonObject &o);

} // namespace loftail
