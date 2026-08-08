#pragma once

#include "FormatSettings.h"

QT_BEGIN_NAMESPACE
class QSettings;
QT_END_NAMESPACE

namespace loftail {

// The DEFAULT log format (SPEC.md §4 "Default log format", ARCHITECTURE.md §8): the
// format a never-seen file is first tried against, so a user whose logs all share one
// house pattern is not asked about every one of them.
//
// This is the second of the two levels, and the split is the whole design. FormatCache
// answers "what did the user choose for THIS file" and wins outright. This answers "what
// should a file nobody has configured be tried with", and it is consulted only when the
// cache has nothing — never as a fallback for a file that has an entry.
//
// It is not applied silently either way: the value here is fed into the ordinary open
// path, where MainWindow::offerFormat() shows the Log Format dialog whenever the format
// does not actually parse the file. A wrong default costs a dialog, never a mis-split
// table.
//
// Persisted as three scalar keys in their own QSettings group — NOT a row in the format
// cache's array, which is keyed by path and would need a sentinel key to hold a
// pathless entry. Unversioned, because there is no structure to migrate: a key that is
// not there reads as the built-in, which is exactly what an older store should do. The
// key spellings are the ones FormatCache and SessionStore already use for the same three
// fields, so the three stores share one vocabulary.
class DefaultFormatStore
{
public:
    // What loftail ships with, used when the user has saved nothing. log4cplus's own
    // documentation uses this shape, so it is the best guess available before anyone
    // has said otherwise.
    static FormatSettings builtIn();

    // The saved default, or builtIn() when unset. Never fails and never returns nullopt:
    // an open always has something to try.
    static FormatSettings load(QSettings &settings);

    // Remember `s` as the default. Writes the PATTERN, the ENCODING and the SOURCE ZONE
    // only — exactly what the Log Format dialog edits.
    //
    // `timeDisplay` and the runStart* fields are deliberately dropped: the first belongs
    // to the timestamp column's header menu and the second to the Run pane, both of them
    // per-file choices about a particular log rather than statements about a format.
    // Widening this to the whole struct would make every newly opened file inherit
    // another file's timestamp mode and run splitting.
    static void save(QSettings &settings, const FormatSettings &s);

private:
    DefaultFormatStore() = delete;
};

} // namespace loftail
