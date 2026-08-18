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

// Action <-> canonical token (M19). Never translated and never a number: the token is
// what makes a rule portable across a version that has learned a new action, and the
// American spelling matches the code side of the repo's split (rowColors,
// HighlightPalette::color) rather than the prose side.
struct ActionToken
{
    HighlightAction action;
    const char     *token;
};
constexpr ActionToken kActionTokens[] = {
    { HighlightAction::Color,  "color"  },
    { HighlightAction::Digest, "digest" },
    { HighlightAction::Tab,    "tab"    },
    { HighlightAction::Notify, "notify" },
};

QJsonArray actionsToJson(HighlightActions actions)
{
    QJsonArray a;
    for (const ActionToken &t : kActionTokens)
        if (actions.testFlag(t.action))
            a.append(QLatin1String(t.token));
    return a;
}

// Unknown tokens are IGNORED rather than preserved: round-tripping them would need a
// string list on a struct whose whole point is that it is small and portable, and it
// would buy a forward compatibility nothing here promises (PresetStore already refuses
// a schema it does not know). But ignoring must not collapse into the absent case —
// see fromJson: a rule whose only token is unknown has NO actions, because the key was
// there and the user's intent was recorded, just not in a vocabulary this build has.
HighlightActions actionsFromJson(const QJsonArray &a)
{
    HighlightActions actions;
    for (const QJsonValue &v : a) {
        const QString s = v.toString();
        for (const ActionToken &t : kActionTokens)
            if (s == QLatin1String(t.token))
                actions |= t.action;
    }
    return actions;
}

} // namespace

QJsonObject HighlightRule::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("enabled"), enabled);
    o.insert(QStringLiteral("match"), match.toJson());
    // Written ONLY when the rule does something other than exactly colour, so a
    // colour-only rule — every rule that predates M19 — serializes byte-identically to
    // what it did before and neither store's schema version has to move. The same habit
    // as MatchCriteria's loggerRestrictive.
    if (actions != HighlightActions(HighlightAction::Color))
        o.insert(QStringLiteral("actions"), actionsToJson(actions));
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

    // contains(), NEVER the array's emptiness. An absent key is a rule written before
    // actions existed, which means {Color}; a PRESENT but empty array is the answer
    // "this rule matches and does nothing", which is a state the user can reach in one
    // click by unticking Colour. Reading the second as the first would silently
    // re-colour every parked rule on the next launch, invisibly until someone wondered
    // why the setting would not stick — the same trap LogSettingsStore's
    // contains("pattern") records for an empty saved pattern.
    if (o.contains(QStringLiteral("actions")))
        r.actions = actionsFromJson(o.value(QStringLiteral("actions")).toArray());

    // Clamp a corrupt index back to the default sentinel rather than out of range.
    auto readSlot = [](const QJsonValue &v) {
        const int i = v.toInt(HighlightPalette::kDefault);
        return HighlightPalette::isSlot(i) ? i : HighlightPalette::kDefault;
    };
    r.background = readSlot(o.value(QStringLiteral("background")));
    r.foreground = readSlot(o.value(QStringLiteral("foreground")));
    return r;
}

HighlighterSet HighlighterSet::defaults()
{
    // Palette SLOTS, never RGB (ARCHITECTURE.md §8), so a default rule follows the
    // theme exactly as a hand-made one does. A slot is `band * kSlotsPerBand + hue`
    // over Palette.cpp's table, where hue 0 is Red and hue 2 Amber — spelled out that
    // way rather than as 0/9/20, because the number means nothing and the band does.
    // One rule per band, in the order the bands are loud: FATAL screams (Vivid), ERROR
    // is a strong fill (Deep), WARN is a quiet tint (Soft).
    constexpr int kDeepRed = 0 * HighlightPalette::kSlotsPerBand + 0;
    constexpr int kVividRed = 1 * HighlightPalette::kSlotsPerBand + 0;
    constexpr int kSoftAmber = 2 * HighlightPalette::kSlotsPerBand + 2;

    HighlighterSet set;
    auto level = [&set](Priority minimum, int background) {
        HighlightRule r;
        r.match.priorityEnabled = true;
        r.match.minPriority = minimum;
        // Explicit although it is also the member's default: what a default rule does
        // is the half of it that has to be read at a glance, and Colour ALONE is the
        // claim being made.
        r.actions = HighlightAction::Color;
        r.background = background;
        // The partner the palette names, never a second choice: that pairing is the one
        // thing guaranteed to clear 4.5:1 in BOTH themes (Palette.h), so a default rule
        // is readable by construction rather than by inspection.
        r.foreground = HighlightPalette::readableTextSlot(background);
        set.rules.append(r);
    };
    level(Priority::Fatal, kVividRed);
    level(Priority::Error, kDeepRed);
    level(Priority::Warn, kSoftAmber);
    return set;
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

bool HighlighterSet::anyEnabled(HighlightActions actions) const
{
    for (const HighlightRule &r : rules)
        if (r.enabled && (r.actions & actions) && r.match.anyActive())
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
