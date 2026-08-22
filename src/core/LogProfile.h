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

    // Where this log's CONFIG FILE is (SPEC.md §4) — the file that says which subsystems
    // the writing application logs at which priority. Relative to the log's OWN
    // directory, or absolute; either way it names a file on the same machine as the log.
    // Empty means "not configured", which is what makes the menu item ask for one.
    // ConfigLocation.h turns this plus the log's address into something openable.
    //
    // BESIDE FormatSettings for the same reason wrapMode is, and the reason is worth
    // restating because it is the whole of why this field is here and not one struct
    // deeper: MainWindow diffs FormatSettings to pick what a change COSTS (an encoding
    // rescan, a timestamp reparse, a repaint). Nothing about a config path changes how a
    // single record is read, so putting it inside would make editing the path reindex
    // the log — a wrong answer that would look like a performance bug rather than a
    // misplaced field.
    QString        configPath;

    // What a log nobody has said anything about gets, before any node exists: the
    // conventional log4cplus layout, auto-detected encoding, zone inferred from the
    // pattern, no wrapping. This is the value the ROOT node is created with.
    static LogProfile builtIn();

    // EVERY FIELD, and a field added above without a clause here is silent data loss
    // rather than a missing feature. LogFileSettings::reduce() drops a per-log profile
    // that compares equal to what the log inherits, so a field this does not look at is
    // a field two profiles never differ in — the log's own entry is deleted on the next
    // write and the setting is gone, with nothing on screen to say so. Pinned per field
    // by tst_logsettings::aProfileDiffersWhenAnyOneFieldOfItDoes.
    bool operator==(const LogProfile &o) const
    {
        return format == o.format && wrapMode == o.wrapMode && configPath == o.configPath;
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
