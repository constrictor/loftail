#pragma once

#include "FormatSettings.h"
#include "WrapMode.h"

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

} // namespace loftail
