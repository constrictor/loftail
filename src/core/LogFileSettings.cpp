#include "LogFileSettings.h"

#include "Highlight.h"
#include "MatchCriteria.h"

namespace loftail {

namespace {

// JSON keys. Never translated (ARCHITECTURE.md §9.1).
constexpr auto kSchemaVersionKey = "schemaVersion";
constexpr auto kAddressKey       = "address";
constexpr auto kProfileKey       = "profile";
constexpr auto kFiltersKey       = "filters";
constexpr auto kHighlightersKey  = "highlighters";
constexpr auto kRunKey           = "run";

constexpr auto kRunAllKey        = "all";
constexpr auto kRunOffsetKey     = "startOffset";
constexpr auto kRunTimestampKey  = "startTimestamp";

// The `inherited` mark. A STRING where an object would otherwise be, rather than an
// absent key: a record that states filters and nothing else then still says something
// deliberate about its format instead of reading as a file that was half written.
constexpr auto kInherited = "inherited";

// The two context spinners FilterPane::saveState() appends beside the criteria, written
// only when non-zero — so an untouched pane serializes exactly as it did before context
// existed, and these lookups default to 0 for every state that predates it.
constexpr auto kContextBeforeKey = "contextBefore";
constexpr auto kContextAfterKey  = "contextAfter";

} // namespace

bool filterStateSaysNothing(const QJsonObject &state)
{
    if (state.isEmpty())
        return true;
    if (state.value(QLatin1String(kContextBeforeKey)).toInt(0) != 0)
        return false;
    if (state.value(QLatin1String(kContextAfterKey)).toInt(0) != 0)
        return false;

    const MatchCriteria c = MatchCriteria::fromJson(state);

    // AXIS BY AXIS, and this is deliberately NOT `c == MatchCriteria{}`. Two of the six
    // axes read back as something other than their default from a pane nobody has
    // touched:
    //
    //   * every discovered subsystem starts TICKED (SPEC.md §6), so an untouched pane
    //     over an indexed log lists every name in the file — and comparing values would
    //     make every log that has finished scanning look filtered, which is every log;
    //   * a QDateTimeEdit always holds a datetime, so an untouched time axis reads back
    //     as a valid 2000-01-01 bound (the AxisEditor::criteria() non-inverse that used
    //     to rewrite this log's seeded highlight rules on a bare run click).
    //
    // What is asked instead is whether any axis NARROWS anything, which is exactly
    // MatchCriteria::resolve()'s NoOpAxes::Collapse rule with the intern-table lookup
    // left out — so an axis switched off, an all-inclusive value selection and a TRACE
    // floor all say nothing, and the two traps above cannot arise. An axis added to
    // resolve()'s collapse logic belongs here in the same commit.
    if (c.priorityEnabled && c.minPriority != PriorityChoice::at(0))
        return false;
    if (c.loggerEnabled && !c.loggerCoversAll)
        return false;
    if (c.threadEnabled && !c.threadCoversAll)
        return false;
    if (c.timeEnabled)
        return false;
    if (c.text.active())
        return false;
    return true;
}

void LogFileSettings::reduce(const LogProfile &inheritedProfile)
{
    if (profile && *profile == inheritedProfile)
        profile.reset();

    if (filterStateSaysNothing(filters))
        filters = QJsonObject();

    // The whole list IN ORDER against the seed, through HighlightRule::operator==. Order
    // is meaning here — first-match-wins is per action — so a reorder is a difference and
    // is kept. An EMPTY stored list is not the seed and therefore survives, which is what
    // makes "I deleted every rule" stick across a relaunch.
    if (highlighters
        && HighlighterSet::fromJson(*highlighters).rules == HighlighterSet::defaults().rules)
        highlighters.reset();

    if (run.saysNothing())
        run = RunSelection();
}

bool LogFileSettings::saysSomething() const
{
    return profile.has_value() || !filters.isEmpty() || highlighters.has_value()
        || !run.saysNothing();
}

QJsonObject LogFileSettings::toJson() const
{
    QJsonObject o;
    o.insert(QLatin1String(kSchemaVersionKey), 1);
    o.insert(QLatin1String(kAddressKey), address);
    o.insert(QLatin1String(kProfileKey),
             profile ? QJsonValue(logProfileToJson(*profile))
                     : QJsonValue(QLatin1String(kInherited)));

    // The three optional sections are omitted when they say nothing, so a record kept for
    // one of them alone stays small and readable — and so a section that has fallen back
    // into line leaves no trace to be misread as an answer.
    if (!filters.isEmpty())
        o.insert(QLatin1String(kFiltersKey), filters);
    if (highlighters)
        o.insert(QLatin1String(kHighlightersKey), *highlighters);
    if (!run.saysNothing()) {
        QJsonObject r;
        r.insert(QLatin1String(kRunAllKey), run.all);
        r.insert(QLatin1String(kRunOffsetKey), double(run.startOffset));
        // Omitted rather than written when there is no timestamp. JSON has one numeric
        // type and it is `double`, and kNoTimestamp is qint64's MINIMUM — writing it
        // would round-trip a value sitting exactly on the edge of what a double can hold
        // and what a narrowing conversion back is allowed to do with it. An absent key
        // reads back as kNoTimestamp below, which is the same answer with none of that.
        if (run.startTimestamp != Record::kNoTimestamp)
            r.insert(QLatin1String(kRunTimestampKey), double(run.startTimestamp));
        o.insert(QLatin1String(kRunKey), r);
    }
    return o;
}

LogFileSettings LogFileSettings::fromJson(const QJsonObject &o)
{
    LogFileSettings s;
    s.address = o.value(QLatin1String(kAddressKey)).toString();

    // An object is a profile of this log's own; anything else — the `inherited` mark, an
    // absent key, or a spelling a later version introduced — means the level above
    // answers. Falling back to inheritance is the safe direction: it shows the log what
    // its pattern says rather than a value this build guessed at.
    if (const QJsonValue p = o.value(QLatin1String(kProfileKey)); p.isObject())
        s.profile = logProfileFromJson(p.toObject());

    s.filters = o.value(QLatin1String(kFiltersKey)).toObject();

    // PRESENCE, NEVER EMPTINESS. contains(), so a stored empty list comes back as an
    // empty list rather than as silence — see the header.
    if (o.contains(QLatin1String(kHighlightersKey)))
        s.highlighters = o.value(QLatin1String(kHighlightersKey)).toArray();

    const QJsonObject r = o.value(QLatin1String(kRunKey)).toObject();
    s.run.all = r.value(QLatin1String(kRunAllKey)).toBool();
    s.run.startOffset = qint64(r.value(QLatin1String(kRunOffsetKey)).toDouble(-1));
    s.run.startTimestamp = r.contains(QLatin1String(kRunTimestampKey))
        ? qint64(r.value(QLatin1String(kRunTimestampKey)).toDouble())
        : Record::kNoTimestamp;
    return s;
}

bool LogFileSettings::operator==(const LogFileSettings &o) const
{
    return address == o.address && profile == o.profile && filters == o.filters
        && highlighters == o.highlighters && run == o.run;
}

} // namespace loftail
