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
// A file that matches no pattern is shown in Preferences under a virtual "No pattern"
// node. That node exists only in the tree widget: nothing here represents it, because
// "no parent" is the absence of a match and not a thing to store.
//
// A FILE NODE STORES NO PARENT LINK. Its parent is derived by running the matcher, so
// deleting or reordering a pattern re-homes its files automatically and there is no
// stored reference that can go stale.

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

// One concrete log. `path` is a normalized address run through logSettingsKey().
struct LogFileNode
{
    QString    path;
    LogProfile profile;
};

class LogSettingsTree
{
public:
    // Which node answered for an address, and what it said.
    struct Resolution
    {
        int        patternIndex = -1; // -1: no pattern matched
        int        fileIndex    = -1; // -1: no node for this file
        LogProfile profile;

        // Whether anything below the root claimed this log.
        bool fromNode() const { return fileIndex >= 0 || patternIndex >= 0; }
    };

    // The winning node's profile, taken whole.
    Resolution resolve(const QString &address) const;

    // What `address` would resolve to if it had no file node of its own — the value a
    // file node has to differ from in order to be worth storing.
    LogProfile inherited(const QString &address) const;

    // Store `p` for `address`, or REMOVE its file node when `p` is exactly what the
    // address already inherits. That is the whole of "a per-file node is not created
    // unless the user changes something", and it is also what leaves a clean tree after
    // a profile is promoted to its parent pattern.
    //
    // Returns whether the tree actually changed. The caller writes the file only then:
    // every resume of a remote or archived log persists what it used, and without the
    // gate that is one atomic rewrite per poll of an identical tree.
    bool setFileProfile(const QString &address, const LogProfile &p);

    // Store `p` for `address` unconditionally, keeping the node even when it matches
    // what the address inherits. For LOADING only: a file node written before the
    // pattern that now covers it is exactly such a node, and dropping it on load would
    // be a change the user never made.
    void insertFileProfile(const QString &address, const LogProfile &p);

    // Drop the file node for `address`, if there is one. Returns whether there was.
    bool removeFile(const QString &address);

    // Drop EVERY file node that says exactly what its address already inherits — the
    // same rule setFileProfile() applies to one node, applied to the whole list.
    // setFileProfile() can only notice a node that is being written; a node stops saying
    // something of its own just as surely when the PATTERN above it is edited, added,
    // reordered or deleted, and nothing writes that node. Without this, a pattern taught
    // to say what a hundred logs' own entries said leaves those hundred entries behind,
    // shadowing it for ever: the pattern is then editable and they no longer follow it.
    //
    // `except` names one address to leave alone whatever it says, for the scratch node
    // Preferences creates so that a log HAS a row to be selected and edited in.
    //
    // Returns whether anything went, so a caller can write the file only when it did.
    bool pruneRedundantFiles(const QString &except = QString());

    // Drop every per-log node, so each log falls back to its pattern or the defaults.
    void clearFiles() { m_files.clear(); }

    // Remove the pattern at `index`. Its files are NOT touched: they re-home under
    // whichever pattern now matches them, or under "No pattern".
    void removePattern(int index);

    // Move the pattern at `index` one place earlier or later, changing which of two
    // overlapping patterns wins. No-op at the ends.
    void movePattern(int index, int delta);

    // Append a pattern, giving it an id no existing pattern has.
    int addPattern(LogPatternNode node);

    int indexOfPatternId(const QString &id) const;
    int indexOfFile(const QString &address) const;

    const LogProfile &defaults() const { return m_defaults; }
    void setDefaults(const LogProfile &p) { m_defaults = p; }

    const QVector<LogPatternNode> &patterns() const { return m_patterns; }
    LogPatternNode &patternAt(int index) { return m_patterns[index]; }

    const QVector<LogFileNode> &files() const { return m_files; }
    LogFileNode &fileAt(int index) { return m_files[index]; }

private:
    LogProfile              m_defaults = LogProfile::builtIn();
    QVector<LogPatternNode> m_patterns;
    QVector<LogFileNode>    m_files;
};

// The glob dialect a Wildcard pattern speaks, as an anchored regular expression:
// `*` is any run of characters INCLUDING `/`, `?` is exactly one, everything else is
// literal. Written out rather than taken from QRegularExpression::wildcardToRegular-
// Expression(), whose behaviour is path-aware (a `*` stops at a separator) and whose
// WildcardConversionOptions overload is Qt 6.6 — above this project's 6.4 floor.
QString wildcardToRegex(const QString &wildcard);

} // namespace loftail
