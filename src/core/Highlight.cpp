#include "Highlight.h"

#include "LogFormat.h"
#include "RecordIndex.h"

namespace loftail {

namespace {

// Priority <-> canonical token, so the legacy flat rule format stays readable and
// stable across enum-value changes (it stored the NAME, not the numeric enum).
Priority priorityFromToken(const QString &s, Priority fallback)
{
    if (s.isEmpty())
        return fallback;
    const Priority p = parsePriority(s);
    return p == Priority::Unknown ? fallback : p;
}

// Read the two-axis rule shape that shipped before highlighting gained the full
// filter axis set: matchLogger/loggerNames/matchPriority/minPriority at the top
// level, with no "match" object. Keeps existing presets, exported preset files and
// stored sessions loading unchanged (see HighlightRule::toJson's comment).
MatchCriteria legacyCriteriaFromJson(const QJsonObject &o)
{
    MatchCriteria c;
    c.loggerEnabled = o.value(QStringLiteral("matchLogger")).toBool(false);
    for (const QJsonValue &v : o.value(QStringLiteral("loggerNames")).toArray()) {
        const QString n = v.toString();
        if (!n.isEmpty() && !c.loggerNames.contains(n))
            c.loggerNames.append(n);
    }
    c.priorityEnabled = o.value(QStringLiteral("matchPriority")).toBool(false);
    c.minPriority = priorityFromToken(o.value(QStringLiteral("minPriority")).toString(),
                                      Priority::Warn);
    return c;
}

} // namespace

QJsonObject HighlightRule::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("enabled"), enabled);
    o.insert(QStringLiteral("match"), match.toJson());
    // Palette INDICES, never RGB (ARCHITECTURE.md §8): -1 == default.
    o.insert(QStringLiteral("background"), background);
    o.insert(QStringLiteral("foreground"), foreground);
    return o;
}

HighlightRule HighlightRule::fromJson(const QJsonObject &o)
{
    HighlightRule r;
    r.enabled = o.value(QStringLiteral("enabled")).toBool(true);

    const QJsonValue m = o.value(QStringLiteral("match"));
    if (m.isObject()) {
        r.match = MatchCriteria::fromJson(m.toObject());
    } else {
        // Pre-axis-set rule. Note the defaults differ from MatchCriteria::fromJson's:
        // there, an absent key means the Filters pane's enabled-by-default axes; here
        // it means the axis was simply off, since a highlight rule has always had to
        // opt into each axis explicitly.
        r.match = legacyCriteriaFromJson(o);
    }

    // Clamp a corrupt index back to the default sentinel rather than out of range.
    auto readSlot = [](const QJsonValue &v) {
        const int i = v.toInt(HighlightPalette::kDefault);
        return HighlightPalette::isSlot(i) ? i : HighlightPalette::kDefault;
    };
    r.background = readSlot(o.value(QStringLiteral("background")));
    r.foreground = readSlot(o.value(QStringLiteral("foreground")));
    return r;
}

void HighlighterSet::resolve(const RecordIndex &idx, const LogFormat &format,
                             const QTimeZone &displayZone)
{
    m_resolved.clear();
    m_resolved.reserve(rules.size());
    for (const HighlightRule &rule : rules) {
        // AbsentField::DoesNotMatch — a record lacking the field a rule keys on must
        // not be colored (SPEC.md §7), the inverse of filtering's exemption.
        // NoOpAxes::Keep — nothing to collapse: "≥TRACE" colors every parsed record
        // on purpose, and highlighting materializes no compact index to protect.
        m_resolved.append(rule.match.resolve(idx, format, displayZone,
                                             AbsentField::DoesNotMatch, NoOpAxes::Keep));
    }
}

bool HighlighterSet::anyEnabled() const
{
    for (const HighlightRule &r : rules)
        if (r.enabled && r.match.anyActive())
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
