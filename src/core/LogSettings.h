#pragma once

#include "LogProfile.h"

#include <QString>
#include <QVector>

namespace loftail {

// THE SETTINGS TREE (SPEC.md §4, ARCHITECTURE.md §8). Three levels answer one question —
// what settings does THIS log open with — and the deepest one that has an answer wins,
// whole:
//
//   root      the defaults, for a log nothing else matches
//   pattern   an ORDERED list of file patterns; the FIRST match wins
//   file      one concrete log, local or remote, keyed by its normalized address
//
// A file that matches no pattern is shown in Preferences directly under the root, which
// is the level it inherits from. Nothing here represents "no parent" and nothing needs
// to: it is the absence of a match, not a thing to store.
//
// THE FILE LEVEL IS NOT IN THIS FILE, and that is M21's whole shape. One log's settings
// are per-file state, so they live one per file — LogFileStore.h, a bounded pool under
// <config>/fileSettings — while the two INHERITED levels stay here, in one document, read
// once at startup. What decided the split is that the pattern list is ORDERED: order is
// free in a JSON array and would otherwise need an index file or an order field in every
// node, whereas a per-log record has no order at all and every reason to be reached
// without loading every other log's.
//
// A FILE RECORD STORES NO PARENT LINK. Its parent is derived by running the matcher, so
// deleting or reordering a pattern re-homes its logs automatically and there is no stored
// reference that can go stale. That is also why the pool has to be swept when a pattern
// moves (LogFileStore::pruneAgainst): nothing writes a record when the level above it
// changes underneath.

// One file-pattern node: a wildcard or a regular expression, matched against either the
// log's file name or its whole address.
struct LogPatternNode
{
    enum class Kind : quint8 {
        Wildcard, // `*` and `?`, everything else literal
        Regex,    // a QRegularExpression, matched UNANCHORED
    };

    // Stable identity, generated on creation. Preferences reselects by this after a
    // reorder, and it is what makes reordering not renumber anything the user can see.
    QString    id;
    Kind       kind = Kind::Wildcard;
    QString    match;
    bool       caseSensitive = false;
    // False (the default) matches the log's file name and extension only; true matches
    // the whole normalized address, `ssh://` scheme and all. See logMatchTarget().
    bool       matchFullPath = false;
    LogProfile profile;

    // Whether this pattern claims `address`. An invalid regular expression NEVER
    // matches — a half-typed pattern in the dialog must not start claiming files.
    bool matches(const QString &address) const;
};

// One concrete log's stored profile, as read from a `files[]` array written before M21
// moved the file level into its own pool. Exists ONLY to be drained by
// LogFileStore::adoptLegacy() on the first launch after the upgrade — see
// LogSettingsStore::legacyFiles(). Nothing writes one.
struct LegacyFileNode
{
    QString    path;
    LogProfile profile;
};

class LogSettingsTree
{
public:
    // What `address` inherits: the first pattern that matches it, or the defaults. Taken
    // WHOLE — the levels are never merged field by field (ARCHITECTURE.md §8).
    //
    // This used to be one of a pair, with resolve() answering the same question and then
    // letting the log's own node override. With the file level in its own store the two
    // collapsed into this one, and the name that survived is the one that says what it
    // means from the log's point of view: the value a record has to differ from in order
    // to be worth storing at all.
    LogProfile inherited(const QString &address) const;

    // Which pattern claims `address`, or -1 when none does. Preferences needs it to know
    // where to hang the log's row and whether Promote to Parent Pattern has a parent to
    // promote into; nothing else asks.
    int matchingPattern(const QString &address) const;

    // Remove the pattern at `index`. The logs under it are NOT touched: they re-home under
    // whichever pattern now matches them, or under the root defaults.
    void removePattern(int index);

    // Move the pattern at `index` one place earlier or later, changing which of two
    // overlapping patterns wins. No-op at the ends.
    void movePattern(int index, int delta);

    // Append a pattern, giving it an id no existing pattern has.
    int addPattern(LogPatternNode node);

    int indexOfPatternId(const QString &id) const;

    // Whether two trees would give every log the same answer. The caller's gate for the
    // pool sweep: re-reading every stored record is worth doing when a pattern has moved
    // and is pure waste when the visit only changed one log.
    bool operator==(const LogSettingsTree &o) const;
    bool operator!=(const LogSettingsTree &o) const { return !(*this == o); }

    const LogProfile &defaults() const { return m_defaults; }
    void setDefaults(const LogProfile &p) { m_defaults = p; }

    const QVector<LogPatternNode> &patterns() const { return m_patterns; }
    LogPatternNode &patternAt(int index) { return m_patterns[index]; }

private:
    LogProfile              m_defaults = LogProfile::builtIn();
    QVector<LogPatternNode> m_patterns;
};

// The glob dialect a Wildcard pattern speaks, as an anchored regular expression:
// `*` is any run of characters INCLUDING `/`, `?` is exactly one, everything else is
// literal. Written out rather than taken from QRegularExpression::wildcardToRegular-
// Expression(), whose behaviour is path-aware (a `*` stops at a separator) and whose
// WildcardConversionOptions overload is Qt 6.6 — above this project's 6.4 floor.
QString wildcardToRegex(const QString &wildcard);

} // namespace loftail
