#pragma once

#include "LogFileSettings.h"
#include "LogSettings.h"

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace loftail {

// ONE JSON FILE PER LOG, IN A BOUNDED POOL OF SLOTS (ARCHITECTURE.md §8.2).
//
//   <AppConfigLocation>/fileSettings/map    address -> slot, plus an MRU tick per entry
//   <AppConfigLocation>/fileSettings/<n>    n in [0, kSlots), one log, NO extension
//
// This is the settings tree's FILE level (LogSettings.h keeps the two above it) together
// with the per-file half of what the QSettings session used to carry. Both moved here for
// the same reason: a log's own settings are per-file state, and `logsettings.json` was the
// one config file that was global-shaped — every open rewrote a document holding every log
// ever configured, and two instances reading different logs contended over it (§8.1).
//
// WHY A SLOT NUMBER RATHER THAN THE ADDRESS AS THE FILE NAME. An address is a path, a URL
// or an archive address: it carries `/`, `:` and `*`, it outruns NAME_MAX on a deep tree,
// and its case folding differs per platform. It would have to be hashed — and a hash needs
// the address stored inside the file anyway, to tell a hit from a collision. The slot is
// that indirection made explicit, and made BOUNDED: nothing ever deleted a per-log entry
// except the redundancy rule, so the old `files[]` grew for the life of the installation.
// kSlots is what turns "remembered for ever" into a promise loftail can keep for the logs
// that matter, rather than a directory nobody prunes.
//
// ---- CRASH SAFETY: EVERY CRASH ENDS AT AN ORPHAN --------------------------------------
//
// Both files go through AtomicJson (temp file + rename), so neither is ever torn. What is
// not atomic is the PAIR, and the order is chosen so the surviving inconsistency is always
// the harmless one:
//
//     a WRITE  goes slot file FIRST, map SECOND
//     a REMOVE goes map FIRST, slot file SECOND
//
// Either way a crash in the gap leaves a slot file no map entry points at. That costs one
// small file, is invisible to every read, and is overwritten whole the moment its number
// is reallocated — which is why there is no reclamation pass here and does not need to be.
// The FORBIDDEN direction is a map entry naming a slot whose contents belong to a
// DIFFERENT log: that is one log opening with another's format, filters and highlight
// rules, which is very much worse than losing the settings outright.
//
// ---- A STALE SLOT IS SETTLED BY THE FILE, NEVER BY THE MAP ----------------------------
//
// Every record names its own address, in the slot file as well as in the map. read(A)
// takes the slot the map offers and then checks that name:
//
//     file missing        -> the map lies; the entry is dropped and A has no settings
//     file names B not A  -> THE FILE WINS; A's entry is dropped, B keeps the slot
//
// So the map is a hint that is always verified and never trusted. That is what makes the
// multi-instance case safe: SPEC.md §3 allows several instances at once, the map is
// last-writer-wins like every other global (§8.1), and two of them racing on an allocation
// can only ever produce a map entry the reader refuses — never a wrong answer. It is also
// why a lost or unparseable map is REBUILT from the slot files rather than surrendered.
class LogFileStore
{
public:
    static constexpr int kSchemaVersion = 1;

    // The pool size. A cap rather than a promise of unlimited memory: past it, the least
    // recently OPENED log's record is dropped to make room. Logs open in a tab are pinned
    // and never chosen (setPinned).
    static constexpr int kSlots = 500;

    // `configDir` is the AppConfigLocation directory — the SAME one LogSettingsStore is
    // built against, so one temporary directory isolates both in a test. The subdirectory
    // is appended here, and nothing outside this class spells "fileSettings".
    explicit LogFileStore(const QString &configDir);

    // The AppConfigLocation-based directory used in production. Empty if it cannot be
    // resolved. Identical to LogSettingsStore::defaultDir() by construction.
    static QString defaultDir();

    QString directory() const;
    QString mapPath() const;
    QString slotPath(int slot) const;

    // Read the MAP, and nothing else. One small file, so this is affordable in the
    // MainWindow constructor beside LogSettingsStore::load(). Rebuilds the map from the
    // slot files when it is missing or unparseable while slot files exist.
    void load();

    // The map was written by a schema version this build does not understand: nothing is
    // read and nothing will ever be written. Running an older build for one session must
    // not discard a newer one's configuration — the rule LogSettingsStore::readOnly()
    // states for the tree, applied to the pool.
    bool readOnly() const { return m_readOnly; }

    // One log's record, or an empty one carrying just the normalized address. Costs one
    // file read. Heals a stale or missing slot as described above; the map write that
    // records the healing rides out with the next save(), remove() or flush().
    LogFileSettings read(const QString &address);

    // THE ONE WRITER. reduce()s `s` against `inherited` and then either stores it or —
    // when nothing is left to say — deletes the record and frees its slot. That is the
    // whole of "a log gets an entry of its own only when you change something"
    // (SPEC.md §4), and it is why merely opening a log leaves nothing behind.
    //
    // Returns whether the DISK changed, so a caller with no change gate of its own still
    // gets a cheap answer. Fails only when every slot is taken by a pinned log, which is
    // more open tabs than there are slots; refusing to store is the only safe answer,
    // since evicting a pinned log takes settings out from under a tab that is still using
    // them.
    bool save(LogFileSettings s, const LogProfile &inherited, QString *error = nullptr);

    // Drop the record and free its slot. Returns whether there was one.
    bool remove(const QString &address);

    // Least-recently-OPENED, not least-recently-written: the tick moves on an open and on
    // nothing else, so a log tailed for a week in a background tab does not outrank one
    // its reader opens daily. IN MEMORY ONLY — the map rides out with the next save(),
    // remove() or flush(), because a restored session of twenty tabs must not be twenty
    // atomic map writes. A lost tick costs eviction ORDER and never a record.
    void touch(const QString &address);

    // Logs that may never be evicted: the ones open in a tab.
    void setPinned(QSet<QString> addresses);

    // Every remembered address, most recently opened first.
    QStringList addresses() const;

    // Write the map if anything has moved it. Cheap and idempotent when nothing has.
    bool flush(QString *error = nullptr);

    // THE PATTERN-CHANGE SWEEP, and the reason it cannot live where its predecessor did.
    // A record stops saying something of its own just as surely when the PATTERN above it
    // is edited, added, reordered or deleted — and nothing writes that record, so nothing
    // re-tests it. Left alone, a pattern taught what a hundred logs' own entries said
    // leaves those hundred shadowing it for ever: the pattern is editable and its logs no
    // longer follow it, which is precisely the state the middle level exists to remove.
    //
    // Re-reads every record, reduce()s it against `tree`, and rewrites or deletes what has
    // fallen into line. Returns how many records changed. One read per record and ONE map
    // write, so the caller must run it on a deliberate gesture — pressing OK on Preferences
    // — and only when the tree actually moved. NEVER on an open and never on a watch tick.
    int pruneAgainst(const LogSettingsTree &tree);

    // THE ONE-TIME DRAIN of the per-log profiles that used to live elsewhere: M20's
    // `files[]` inside logsettings.json and, through it, M18's `formatCache`
    // (LogSettingsStore::takeLegacyFiles). Returns how many were adopted.
    //
    // Three rules. An EXISTING RECORD WINS, so a user who downgraded, ran a build that
    // rewrites `files[]`, and upgraded again does not have their newer per-log record
    // rolled back. Each node goes through save(), so the redundancy rule fires on the way
    // in and a node that already agreed with its pattern is never given a slot. And with
    // more nodes than slots the LAST kSlots are kept: the old array is insertion order, so
    // its tail is the nearest thing to an MRU the source has, and a silent drop beats a
    // dialog on the first launch after upgrade about entries nobody can see.
    int adoptLegacy(const QVector<LegacyFileNode> &nodes, const LogSettingsTree &tree);

private:
    struct Entry
    {
        QString address; // logSettingsKey() form
        int     slot = -1;
        qint64  used = 0; // MRU tick; higher is more recent
    };

    int  indexOf(const QString &key) const;
    int  allocateSlot(QString *error);
    bool writeSlot(int slot, const LogFileSettings &s, QString *error) const;
    void rebuildFromSlots();

    QString        m_dir;
    QVector<Entry> m_entries;
    QSet<QString>  m_pinned;
    qint64         m_tick = 0;
    bool           m_mapDirty = false;
    bool           m_readOnly = false;
};

} // namespace loftail
