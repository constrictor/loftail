#include "Highlight.h"

#include "RecordIndex.h"

namespace loftail {

namespace {
// Priority <-> canonical token, so persisted rules are readable and stable across
// enum-value changes (we store the NAME, not the numeric enum).
QString priorityToken(Priority p)
{
    return QString(priorityName(p)); // "" for Unknown
}

Priority priorityFromToken(const QString &s, Priority fallback)
{
    const Priority p = parsePriority(s);
    return (p == Priority::Unknown && s != QLatin1String("")) ? fallback
         : (s.isEmpty() ? fallback : p);
}
} // namespace

QJsonObject HighlightRule::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("enabled"), enabled);
    o.insert(QStringLiteral("matchLogger"), matchLogger);
    QJsonArray names;
    for (const QString &n : loggerNames)
        names.append(n);
    o.insert(QStringLiteral("loggerNames"), names);
    o.insert(QStringLiteral("matchPriority"), matchPriority);
    o.insert(QStringLiteral("minPriority"), priorityToken(minPriority));
    // Palette INDICES, never RGB (ARCHITECTURE.md §8): -1 == default.
    o.insert(QStringLiteral("background"), background);
    o.insert(QStringLiteral("foreground"), foreground);
    return o;
}

HighlightRule HighlightRule::fromJson(const QJsonObject &o)
{
    HighlightRule r;
    r.enabled = o.value(QStringLiteral("enabled")).toBool(true);
    r.matchLogger = o.value(QStringLiteral("matchLogger")).toBool(false);
    for (const QJsonValue &v : o.value(QStringLiteral("loggerNames")).toArray())
        r.loggerNames.append(v.toString());
    r.matchPriority = o.value(QStringLiteral("matchPriority")).toBool(false);
    r.minPriority = priorityFromToken(o.value(QStringLiteral("minPriority")).toString(),
                                      Priority::Warn);
    // Clamp a corrupt index back to the default sentinel rather than out of range.
    auto readSlot = [](const QJsonValue &v) {
        const int i = v.toInt(HighlightPalette::kDefault);
        return HighlightPalette::isSlot(i) ? i : HighlightPalette::kDefault;
    };
    r.background = readSlot(o.value(QStringLiteral("background")));
    r.foreground = readSlot(o.value(QStringLiteral("foreground")));
    return r;
}

void HighlighterSet::resolve(const RecordIndex &idx)
{
    m_resolvedLoggerIds.clear();
    m_resolvedLoggerIds.reserve(rules.size());
    for (const HighlightRule &rule : rules) {
        QSet<quint32> ids;
        if (rule.matchLogger) {
            for (const QString &name : rule.loggerNames) {
                bool found = false;
                const quint32 id = idx.loggers.idOf(name, &found);
                if (found)
                    ids.insert(id);
            }
        }
        m_resolvedLoggerIds.append(std::move(ids));
    }
}

int HighlighterSet::match(const Record &r) const
{
    static const QSet<quint32> kEmpty;
    for (int i = 0; i < rules.size(); ++i) {
        const HighlightRule &rule = rules.at(i);
        if (!rule.enabled)
            continue;

        bool anyAxis = false;
        bool ok = true;

        if (rule.matchPriority) {
            anyAxis = true;
            // Min-level `>=` (invariant #4, §7.2); Unknown (unparsed) never matches
            // a priority axis, mirroring how the priority filter exempts it.
            const Priority p = r.priorityEnum();
            if (p == Priority::Unknown || p < rule.minPriority)
                ok = false;
        }

        if (ok && rule.matchLogger) {
            anyAxis = true;
            const QSet<quint32> &ids =
                (i < m_resolvedLoggerIds.size()) ? m_resolvedLoggerIds.at(i) : kEmpty;
            if (!ids.contains(r.loggerId))
                ok = false;
        }

        // A rule with no active axis never matches (an unconfigured rule is inert).
        if (anyAxis && ok)
            return i;
    }
    return -1;
}

bool HighlighterSet::anyEnabled() const
{
    for (const HighlightRule &r : rules)
        if (r.enabled && (r.matchLogger || r.matchPriority))
            return true;
    return false;
}

QJsonArray HighlighterSet::toJson() const
{
    QJsonArray a;
    for (const HighlightRule &r : rules)
        a.append(r.toJson());
    return a;
}

HighlighterSet HighlighterSet::fromJson(const QJsonArray &a)
{
    HighlighterSet set;
    for (const QJsonValue &v : a)
        set.rules.append(HighlightRule::fromJson(v.toObject()));
    return set;
}

} // namespace loftail
