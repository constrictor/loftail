#pragma once

#include "LogSettings.h"

#include <QString>

#include <utility>

QT_BEGIN_NAMESPACE
class QSettings;
QT_END_NAMESPACE

namespace loftail {

// Persistence for the settings tree (ARCHITECTURE.md §8): ONE schema-versioned JSON
// file, `logsettings.json`, under QStandardPaths::AppConfigLocation, written atomically
// through AtomicJson for the multi-instance case (§8.1) exactly as the preset and host
// stores are.
//
// One file rather than one file per node because the pattern list is ORDERED: order is
// free in a JSON array and would otherwise need either an index file or an order field
// in every node, and the whole tree is small enough to read once at startup.
//
// The store is constructed against a directory so tests can isolate it; production
// passes defaultDir(). It touches no QApplication.
class LogSettingsStore
{
public:
    static constexpr int kSchemaVersion = 1;

    explicit LogSettingsStore(QString dir) : m_dir(std::move(dir)) {}

    // The AppConfigLocation-based directory used in production. Empty if the location
    // cannot be resolved.
    static QString defaultDir();

    QString filePath() const;

    // The stored tree, or a tree holding nothing but the built-in defaults when the
    // file is absent, unparseable, or written by a version this build cannot read.
    LogSettingsTree load();

    // The per-log profiles this store has seen but no longer keeps: the `files[]` array
    // read by load(), and M18's `formatCache` drained by migrateLegacy(). Handed to
    // LogFileStore::adoptLegacy() on the first launch after M21 moved the file level into
    // its own pool. EMPTY for anything this build wrote, because save() no longer emits
    // the key — which is what closes the migration with nothing to remember it by.
    //
    // A TAKE, because it is a drain: the caller has to be able to say the entries have
    // been adopted, and load() is called again by showPreferences() every time the dialog
    // is opened. Both sources ACCUMULATE here rather than replacing each other, so the
    // order migrateLegacy() and load() run in does not have to be remembered.
    //
    // No schema bump came with the removal: a removed key is exactly what a backward read
    // handles, which is the rule SessionStore states for its own file and M20 already used
    // when it dropped the format group from the session. A bump would be destructive here
    // — this file is the only copy of a user's whole format configuration.
    QVector<LegacyFileNode> takeLegacyFiles() { return std::exchange(m_legacyFiles, {}); }

    // Replace the file. Returns false on a write failure, or when the file on disk was
    // written by a LATER schema version — see readOnly().
    bool save(const LogSettingsTree &tree, QString *error = nullptr) const;

    // Whether the file on disk is from a schema version this build does not understand,
    // as last seen by load(). Such a file is never overwritten: running an older build
    // for one session must not discard a newer one's configuration. The dialog says so.
    bool readOnly() const { return m_readOnly; }

    // The generation of the built-in pattern seed this build ships. Bumped when a
    // pattern is ADDED to the list below it; see seedBuiltInPatterns().
    static constexpr int kSeedVersion = 1;

    // THE ONE-TIME SEED of the file patterns loftail ships with (SPEC.md §4). Today
    // that is one: `messages*`, carrying the traditional syslog layout every
    // /var/log/messages line on a Red Hat-family box is written in, so the file opens
    // split into columns instead of raising the format dialog.
    //
    // A SEED IS NOT A DEFAULT. It is written into the user's pattern list once and is
    // theirs from then on: editing it sticks, and DELETING IT STICKS — which is the
    // whole reason there is a flag rather than a "is it missing?" test. A pattern that
    // came back on the next launch would be one nobody could get rid of, the shape
    // HighlighterSet::defaults() records for the seeded highlight rules.
    //
    // The flag is a VERSION and not a bool so a later build can add a second pattern
    // without re-adding the first. It lives in QSettings beside the other application-
    // level flags rather than in logsettings.json, because it describes what this
    // installation has already done and not what any log gets — and because a tree
    // written by a newer build is read-only here, so a flag inside it could never be
    // written at the one moment it is most needed.
    //
    // A seed whose example address is ALREADY CLAIMED by a pattern is skipped: the user
    // got there first, and first-match-wins would make ours dead weight anyway. The flag
    // is still recorded in that case, so this asks the question once either way.
    //
    // Returns whether the tree changed. It SAVES on its own when it did — and records
    // the flag only once that write succeeded, so a read-only or unwritable store is
    // retried on the next launch rather than silently skipped for ever.
    bool seedBuiltInPatterns(LogSettingsTree &tree, QSettings &settings);

    // The one-time move off the two QSettings stores this replaced — `defaultFormat`
    // (three scalar keys) and `formatCache` (a path-keyed array). Does nothing when
    // logsettings.json already exists. On success the two QSettings groups are REMOVED:
    // one home for a setting, not two that can disagree.
    //
    // Returns whether anything was migrated.
    bool migrateLegacy(QSettings &settings);

private:
    QString                 m_dir;
    bool                    m_readOnly = false;
    QVector<LegacyFileNode> m_legacyFiles;
};

} // namespace loftail
