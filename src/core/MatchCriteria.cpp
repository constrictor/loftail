#include "MatchCriteria.h"

#include "LogFormat.h"
#include "RecordIndex.h"

#include <QJsonArray>

namespace loftail {

namespace {

// Severity order (§7.2) — the same order the Priority enum is declared in, so the
// combo index is the severity index and index 0 is the minimum that narrows nothing.
const Priority kPriorityByIndex[] = {
    Priority::Trace, Priority::Debug, Priority::Info,
    Priority::Warn,  Priority::Error, Priority::Fatal,
};
constexpr int kPriorityCount = int(sizeof(kPriorityByIndex) / sizeof(kPriorityByIndex[0]));

QJsonArray namesToArray(const QStringList &names)
{
    QStringList sorted = names;
    sorted.sort(Qt::CaseInsensitive); // stable, diffable preset files
    QJsonArray a;
    for (const QString &n : sorted)
        a.append(n);
    return a;
}

QStringList arrayToNames(const QJsonArray &a)
{
    QStringList out;
    for (const QJsonValue &v : a) {
        const QString s = v.toString();
        if (!s.isEmpty() && !out.contains(s))
            out.append(s);
    }
    return out;
}

// Resolve a name list against one intern table, dropping names the file has not
// emitted (InternTable::idOf's `found` out-param) so a manually entered value simply
// matches nothing until it appears.
QSet<quint32> resolveIds(const InternTable &table, const QStringList &names)
{
    QSet<quint32> ids;
    for (const QString &name : names) {
        bool found = false;
        const quint32 id = table.idOf(name, &found);
        if (found)
            ids.insert(id);
    }
    return ids;
}

} // namespace

int      PriorityChoice::count() { return kPriorityCount; }
Priority PriorityChoice::at(int index) { return kPriorityByIndex[qBound(0, index, kPriorityCount - 1)]; }

int PriorityChoice::indexOf(Priority p)
{
    for (int i = 0; i < kPriorityCount; ++i)
        if (kPriorityByIndex[i] == p)
            return i;
    return 0; // Unknown has no selector entry; fall back to the widest minimum
}

bool MatchCriteria::anyActive() const
{
    return priorityEnabled || loggerEnabled || threadEnabled || timeEnabled || text.active();
}

FilterSet MatchCriteria::resolve(const RecordIndex &idx, const LogFormat &format,
                                 const QTimeZone &displayZone, AbsentField absent,
                                 NoOpAxes noOps) const
{
    FilterSet fs;
    fs.absentFieldMatches = (absent == AbsentField::Matches);
    const bool collapse = (noOps == NoOpAxes::Collapse);

    // Priority: one `>=` against the severity-ordered enum. TRACE is the lowest
    // selectable minimum, so "enabled at TRACE" excludes nothing and collapses.
    fs.minPriority = minPriority;
    fs.priorityEnabled = priorityEnabled
                         && !(collapse && minPriority == kPriorityByIndex[0]);

    // Subsystem / thread: names -> interned ids (invariant #4). An all-inclusive
    // selection collapses only when asked; see loggerCoversAll's comment for why the
    // coverage answer comes from the editor rather than from the intern table.
    fs.loggerEnabled = loggerEnabled && !(collapse && loggerCoversAll);
    fs.loggerIds = resolveIds(idx.loggers, loggerNames);

    fs.threadEnabled = threadEnabled && !(collapse && threadCoversAll)
                       && format.threadGroup > 0;
    fs.threadIds = resolveIds(idx.threads, threadNames);

    // Message text — carried across whole; TextMatcher compiled its regex once when
    // the pattern was set, so nothing is compiled on the paint path.
    fs.text = text;

    // Time range: the editors hold display-zone wall clock; this is the single "in"
    // conversion to UTC ms (§5.1, invariant #10). setTimeZone reinterprets the same
    // digits in the display zone rather than shifting them.
    fs.timeEnabled = timeEnabled && format.dateGroup > 0;
    if (fs.timeEnabled) {
        QDateTime s = start;
        QDateTime e = end;
        s.setTimeZone(displayZone);
        e.setTimeZone(displayZone);
        if (s.isValid())
            fs.startMs = s.toMSecsSinceEpoch();
        if (e.isValid())
            fs.endMs = e.toMSecsSinceEpoch();
    }

    return fs;
}

QJsonObject MatchCriteria::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("priorityEnabled"), priorityEnabled);
    o.insert(QStringLiteral("minPriorityIndex"), PriorityChoice::indexOf(minPriority));

    o.insert(QStringLiteral("loggerEnabled"), loggerEnabled);
    o.insert(QStringLiteral("loggerChecked"), namesToArray(loggerNames));

    o.insert(QStringLiteral("threadEnabled"), threadEnabled);
    o.insert(QStringLiteral("threadChecked"), namesToArray(threadNames));

    // Written only when set, so a state that does not restrict serializes exactly as
    // it did before the key existed — which is what keeps existing presets, exported
    // files and sessions loading without a PresetStore/SessionStore version bump
    // (both compare versions for exact equality and have no migration path).
    if (loggerRestrictive)
        o.insert(QStringLiteral("loggerRestrictive"), true);
    if (threadRestrictive)
        o.insert(QStringLiteral("threadRestrictive"), true);

    // Coverage, written only where the name list alone would be read wrongly. A list
    // already implies an answer — nothing listed excludes nothing, something listed is
    // a narrowing — so the key goes in exactly when the truth differs from that
    // implication, i.e. for an empty selection somebody chose (the None button) and
    // for a full one that has to keep growing with the file. Every other state,
    // including every default and every rule loftail seeds, serializes byte-for-byte
    // as it did before the key existed, so neither store's version has to move. The
    // same habit as loggerRestrictive above, on a field whose default is true.
    if (loggerCoversAll != loggerNames.isEmpty())
        o.insert(QStringLiteral("loggerCoversAll"), loggerCoversAll);
    if (threadCoversAll != threadNames.isEmpty())
        o.insert(QStringLiteral("threadCoversAll"), threadCoversAll);

    o.insert(QStringLiteral("textEnabled"), text.enabled);
    o.insert(QStringLiteral("text"), text.matcher.pattern());
    o.insert(QStringLiteral("textRegex"), text.matcher.isRegex());
    o.insert(QStringLiteral("textCase"),
             text.matcher.caseSensitivity() == Qt::CaseSensitive);
    o.insert(QStringLiteral("textNegate"), text.negate);

    o.insert(QStringLiteral("timeEnabled"), timeEnabled);
    o.insert(QStringLiteral("timeStart"), start.toString(Qt::ISODate));
    o.insert(QStringLiteral("timeEnd"), end.toString(Qt::ISODate));
    return o;
}

MatchCriteria MatchCriteria::fromJson(const QJsonObject &o)
{
    MatchCriteria c;

    // Per-key defaults reproduce FilterPane's original restore behavior exactly: the
    // two metadata axes default on (SPEC.md §6), the three that need a typed value
    // default off. A state written by this version always carries every key.
    c.priorityEnabled = o.value(QStringLiteral("priorityEnabled")).toBool(true);
    c.minPriority = PriorityChoice::at(o.value(QStringLiteral("minPriorityIndex")).toInt(0));

    c.loggerEnabled = o.value(QStringLiteral("loggerEnabled")).toBool(true);
    c.loggerNames = arrayToNames(o.value(QStringLiteral("loggerChecked")).toArray());

    c.threadEnabled = o.value(QStringLiteral("threadEnabled")).toBool(false);
    c.threadNames = arrayToNames(o.value(QStringLiteral("threadChecked")).toArray());

    // Absent means "not a restriction", which is what every state written before the
    // key existed meant: a hand-ticked list that widens with the file.
    c.loggerRestrictive = o.value(QStringLiteral("loggerRestrictive")).toBool(false);
    c.threadRestrictive = o.value(QStringLiteral("threadRestrictive")).toBool(false);

    // contains(), never the value alone: an absent key is a state whose coverage the
    // name list already implies — or one written before the key existed, where the two
    // cannot be told apart at all. Falling back on the implication is what makes an
    // older build's empty selection read as "nothing had been offered", which heals a
    // stashed-too-early state that would otherwise come back empty for ever; an empty
    // selection the user DID choose has carried the key since this version, so only
    // states written before it lose that distinction, and only once.
    c.loggerCoversAll = o.contains(QStringLiteral("loggerCoversAll"))
                            ? o.value(QStringLiteral("loggerCoversAll")).toBool(true)
                            : c.loggerNames.isEmpty();
    c.threadCoversAll = o.contains(QStringLiteral("threadCoversAll"))
                            ? o.value(QStringLiteral("threadCoversAll")).toBool(true)
                            : c.threadNames.isEmpty();

    c.text.enabled = o.value(QStringLiteral("textEnabled")).toBool(false);
    c.text.negate = o.value(QStringLiteral("textNegate")).toBool(false);
    c.text.matcher.set(o.value(QStringLiteral("text")).toString(),
                       o.value(QStringLiteral("textRegex")).toBool(false),
                       o.value(QStringLiteral("textCase")).toBool(false) ? Qt::CaseSensitive
                                                                        : Qt::CaseInsensitive);

    c.timeEnabled = o.value(QStringLiteral("timeEnabled")).toBool(false);
    c.start = QDateTime::fromString(o.value(QStringLiteral("timeStart")).toString(), Qt::ISODate);
    c.end = QDateTime::fromString(o.value(QStringLiteral("timeEnd")).toString(), Qt::ISODate);

    return c;
}

} // namespace loftail
