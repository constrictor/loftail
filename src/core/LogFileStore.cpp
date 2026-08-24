#include "LogFileStore.h"

#include "AtomicJson.h"
#include "RemoteLocation.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>

namespace loftail {

namespace {

constexpr auto kSubDir   = "fileSettings";
constexpr auto kMapName  = "map";

// JSON keys. Never translated (ARCHITECTURE.md §9.1).
constexpr auto kSchemaVersionKey = "schemaVersion";
constexpr auto kTickKey          = "tick";
constexpr auto kEntriesKey       = "entries";
constexpr auto kAddressKey       = "address";
constexpr auto kSlotKey          = "slot";
constexpr auto kUsedKey          = "used";

// A directory entry that is a slot, i.e. a decimal integer in range and nothing else.
//
// The strictness is load-bearing rather than tidy. AtomicJson writes through QSaveFile,
// whose temporary file lives in the SAME directory under a name like `7.aBc123` — so a
// scan that accepted anything starting with a digit would adopt, and a reclamation pass
// would delete, another instance's write while it is still in flight.
bool slotNumberOf(const QString &name, int *out)
{
    bool ok = false;
    const int n = name.toInt(&ok);
    // toInt() accepts leading `+`, a sign and surrounding oddities that would let two
    // names denote one slot; requiring the round trip to be identical admits exactly the
    // canonical spelling slotPath() produces.
    if (!ok || n < 0 || n >= LogFileStore::kSlots || QString::number(n) != name)
        return false;
    *out = n;
    return true;
}

} // namespace

LogFileStore::LogFileStore(const QString &configDir)
    : m_dir(QDir(configDir).filePath(QLatin1String(kSubDir)))
{
}

QString LogFileStore::defaultDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString LogFileStore::directory() const
{
    return m_dir;
}

QString LogFileStore::mapPath() const
{
    return QDir(m_dir).filePath(QLatin1String(kMapName));
}

QString LogFileStore::slotPath(int slot) const
{
    return QDir(m_dir).filePath(QString::number(slot));
}

int LogFileStore::indexOf(const QString &key) const
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).address == key)
            return i;
    }
    return -1;
}

void LogFileStore::load()
{
    m_entries.clear();
    m_tick = 0;
    m_mapDirty = false;
    m_readOnly = false;

    bool ok = false;
    const QJsonDocument doc = AtomicJson::read(mapPath(), &ok);
    if (!ok || !doc.isObject()) {
        // No map, or one that will not parse. Every record names its own address, so the
        // index can be reconstructed from the records themselves rather than the pool
        // being written off.
        rebuildFromSlots();
        return;
    }

    const QJsonObject root = doc.object();
    if (root.value(QLatin1String(kSchemaVersionKey)).toInt() > kSchemaVersion) {
        // Read nothing, write nothing. There is no migration downwards, so leaving the
        // pool alone is the only answer that cannot destroy a newer build's data.
        m_readOnly = true;
        return;
    }

    m_tick = qint64(root.value(QLatin1String(kTickKey)).toDouble());

    // An ARRAY rather than an address-keyed object, so that a duplicate — of an address
    // or of a slot — is something the loader can SEE and settle, instead of something
    // JSON silently collapses to whichever came last. Two instances racing on an
    // allocation is exactly how one arises.
    const QJsonArray entries = root.value(QLatin1String(kEntriesKey)).toArray();
    for (const auto &v : entries) {
        const QJsonObject o = v.toObject();
        Entry e;
        e.address = o.value(QLatin1String(kAddressKey)).toString();
        e.slot = o.value(QLatin1String(kSlotKey)).toInt(-1);
        e.used = qint64(o.value(QLatin1String(kUsedKey)).toDouble());
        if (e.address.isEmpty() || e.slot < 0 || e.slot >= kSlots) {
            m_mapDirty = true;
            continue;
        }
        // The more recently used claim wins either contest, and the loser is dropped
        // rather than re-homed: a record whose slot was taken from it is unreadable
        // anyway, since read() checks the address in the file.
        bool dropped = false;
        for (auto &x : m_entries) {
            if (x.address != e.address && x.slot != e.slot)
                continue;
            m_mapDirty = true;
            if (e.used > x.used)
                x = e;
            dropped = true;
            break;
        }
        if (!dropped)
            m_entries.push_back(e);
        m_tick = qMax(m_tick, e.used);
    }
}

void LogFileStore::rebuildFromSlots()
{
    QDir dir(m_dir);
    if (!dir.exists())
        return;

    const QStringList names = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QString &name : names) {
        int slot = 0;
        if (!slotNumberOf(name, &slot))
            continue;
        bool ok = false;
        const QJsonDocument doc = AtomicJson::read(dir.filePath(name), &ok);
        if (!ok || !doc.isObject())
            continue;
        const QString address = doc.object().value(QLatin1String(kAddressKey)).toString();
        if (address.isEmpty() || indexOf(address) >= 0)
            continue;
        // Nothing records how recently these were opened — the map was what held that —
        // so they all arrive equally stale and the first genuine open re-orders them.
        // Losing the ORDER is the recovery's whole cost; no record is lost.
        m_entries.push_back(Entry{address, slot, 0});
    }
    m_mapDirty = !m_entries.isEmpty();
}

LogFileSettings LogFileStore::read(const QString &address)
{
    LogFileSettings empty;
    empty.address = logSettingsKey(address);
    if (m_readOnly)
        return empty;

    const int i = indexOf(empty.address);
    if (i < 0)
        return empty;

    bool ok = false;
    const QJsonDocument doc = AtomicJson::read(slotPath(m_entries.at(i).slot), &ok);
    if (ok && doc.isObject()) {
        const LogFileSettings s = LogFileSettings::fromJson(doc.object());
        // THE FILE WINS. A map entry whose slot holds another log's record is a map that
        // is out of date, not a record that belongs to this address — see the header.
        if (s.address == empty.address)
            return s;
    }

    m_entries.remove(i);
    m_mapDirty = true;
    return empty;
}

int LogFileStore::allocateSlot(QString *error)
{
    QVector<bool> taken(kSlots, false);
    for (const Entry &e : m_entries)
        taken[e.slot] = true;

    for (int n = 0; n < kSlots; ++n) {
        if (!taken.at(n))
            return n;
    }

    // Full. Evict the least recently opened record that is not open in a tab.
    int victim = -1;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_pinned.contains(m_entries.at(i).address))
            continue;
        if (victim < 0 || m_entries.at(i).used < m_entries.at(victim).used)
            victim = i;
    }
    if (victim < 0) {
        if (error) {
            *error = QStringLiteral(
                "every one of the %1 per-log settings slots belongs to a log that is open")
                         .arg(kSlots);
        }
        return -1;
    }

    const int slot = m_entries.at(victim).slot;
    m_entries.remove(victim);
    m_mapDirty = true;
    return slot;
}

bool LogFileStore::writeSlot(int slot, const LogFileSettings &s, QString *error) const
{
    return AtomicJson::write(slotPath(slot), QJsonDocument(s.toJson()), error);
}

bool LogFileStore::save(LogFileSettings s, const LogProfile &inherited, QString *error)
{
    if (m_readOnly) {
        if (error)
            *error = QStringLiteral("the per-log settings were written by a newer version");
        return false;
    }

    s.address = logSettingsKey(s.address);
    s.reduce(inherited);

    // Nothing left that its parents do not already say, so there is no record. This is
    // the same rule LogSettingsTree::setFileProfile() applied to a profile alone, and it
    // is why opening a log and changing nothing leaves nothing behind.
    if (!s.saysSomething())
        return remove(s.address);

    int i = indexOf(s.address);
    if (i < 0) {
        const int slot = allocateSlot(error);
        if (slot < 0)
            return false;
        m_entries.push_back(Entry{s.address, slot, ++m_tick});
        i = int(m_entries.size()) - 1;
        m_mapDirty = true;
    }

    // SLOT FILE FIRST, MAP SECOND — see the header. A crash between them costs one
    // unreferenced file; the other order would point the map at another log's record.
    if (!writeSlot(m_entries.at(i).slot, s, error))
        return false;

    // NO MRU BUMP HERE. The tick means least recently OPENED, and a save is not an open:
    // bumping it would let a log that merely resumes in a background tab outrank one its
    // reader opens daily, which is the ordering eviction is decided by. A brand-new entry
    // gets a tick above because it has to have one, and being the newest is right.
    //
    // Flushed rather than deferred, unlike touch(): save() runs on a gesture that has
    // already written a file, and a record nobody can find until the next flush is a
    // record the very next launch would miss.
    return flush(error);
}

bool LogFileStore::remove(const QString &address)
{
    if (m_readOnly)
        return false;
    const int i = indexOf(logSettingsKey(address));
    if (i < 0)
        return false;

    const int slot = m_entries.at(i).slot;
    m_entries.remove(i);
    m_mapDirty = true;
    // MAP FIRST, SLOT FILE SECOND — the mirror of save()'s order, and for the same
    // reason: a crash here leaves an unreferenced file, never a live entry naming a slot
    // that has been deleted or handed to another log.
    flush();
    QFile::remove(slotPath(slot));
    return true;
}

void LogFileStore::touch(const QString &address)
{
    const int i = indexOf(logSettingsKey(address));
    if (i < 0)
        return; // Nothing is remembered about this log, so there is no order to move.
    m_entries[i].used = ++m_tick;
    m_mapDirty = true;
}

void LogFileStore::setPinned(const QSet<QString> &addresses)
{
    m_pinned.clear();
    m_pinned.reserve(addresses.size());
    for (const QString &a : addresses)
        m_pinned.insert(logSettingsKey(a));
}

QStringList LogFileStore::addresses() const
{
    QVector<Entry> sorted = m_entries;
    std::ranges::stable_sort(sorted,
                     [](const Entry &a, const Entry &b) { return a.used > b.used; });
    QStringList out;
    out.reserve(sorted.size());
    for (const Entry &e : sorted)
        out.append(e.address);
    return out;
}

bool LogFileStore::flush(QString *error)
{
    if (m_readOnly || !m_mapDirty)
        return true;

    QJsonObject root;
    root.insert(QLatin1String(kSchemaVersionKey), kSchemaVersion);
    root.insert(QLatin1String(kTickKey), double(m_tick));

    QJsonArray entries;
    for (const Entry &e : m_entries) {
        QJsonObject o;
        o.insert(QLatin1String(kAddressKey), e.address);
        o.insert(QLatin1String(kSlotKey), e.slot);
        o.insert(QLatin1String(kUsedKey), double(e.used));
        entries.append(o);
    }
    root.insert(QLatin1String(kEntriesKey), entries);

    if (!AtomicJson::write(mapPath(), QJsonDocument(root), error))
        return false;
    m_mapDirty = false;
    return true;
}

int LogFileStore::pruneAgainst(const LogSettingsTree &tree)
{
    if (m_readOnly)
        return 0;

    int changed = 0;
    // Backwards, so removing one entry does not step over the next.
    for (int i = int(m_entries.size()) - 1; i >= 0; --i) {
        const QString address = m_entries.at(i).address;
        const int slot = m_entries.at(i).slot;

        bool ok = false;
        const QJsonDocument doc = AtomicJson::read(slotPath(slot), &ok);
        if (!ok || !doc.isObject()) {
            m_entries.remove(i);
            m_mapDirty = true;
            ++changed;
            continue;
        }

        LogFileSettings s = LogFileSettings::fromJson(doc.object());
        if (s.address != address) {
            // The map was stale; the file's own name settles it, exactly as read() does.
            m_entries.remove(i);
            m_mapDirty = true;
            ++changed;
            continue;
        }

        const LogFileSettings before = s;
        s.reduce(tree.inherited(address));
        if (s == before)
            continue;

        ++changed;
        if (!s.saysSomething()) {
            m_entries.remove(i);
            m_mapDirty = true;
            QFile::remove(slotPath(slot));
            continue;
        }
        writeSlot(slot, s, nullptr);
    }

    // ONE map write for the whole sweep, whatever it touched.
    flush();
    return changed;
}

int LogFileStore::adoptLegacy(const QVector<LegacyFileNode> &nodes, const LogSettingsTree &tree)
{
    if (m_readOnly)
        return 0;

    // The tail, for the reason the header gives: insertion order is the closest thing to
    // an MRU the old array carries, so if there are more nodes than slots the ones kept
    // are the ones most recently given settings.
    const int first = qMax(0, int(nodes.size()) - kSlots);

    int adopted = 0;
    for (int i = first; i < nodes.size(); ++i) {
        const LegacyFileNode &n = nodes.at(i);
        if (n.path.isEmpty())
            continue;
        // AN EXISTING RECORD WINS. It is newer by construction — nothing writes `files[]`
        // any more — so overwriting it would roll a downgrade-then-upgrade back.
        if (read(n.path).saysSomething())
            continue;

        LogFileSettings s;
        s.address = n.path;
        s.profile = n.profile;
        if (save(s, tree.inherited(n.path)))
            ++adopted;
    }
    return adopted;
}

} // namespace loftail
