#pragma once

#include "Filter.h"
#include "MatchCriteria.h"
#include "Palette.h"
#include "Priority.h"
#include "Record.h"

#include <QFlags>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimeZone>
#include <QVector>

#include <optional>
#include <utility>

namespace loftail {

struct LogFormat;
class RecordIndex;

// M19 — what a matching rule DOES (SPEC.md §7, ARCHITECTURE.md §7.5). Colouring used
// to be the only effect and is now one action among four; a rule may match and not
// colour, and a rule may carry no action at all, which is how one is parked without
// being deleted.
//
// Values are explicit bits because they are serialized — as STABLE TOKENS, never as
// this number (see HighlightRule::toJson). Q_DECLARE_FLAGS is a plain typedef and
// needs no Q_OBJECT/Q_GADGET; nothing here wants QMetaEnum reflection.
enum class HighlightAction : quint8 {
    Color  = 0x1, // recolour the record in the log (what a rule has always done)
    Digest = 0x2, // list this rule's newest match in the digest strip under the log
    Tab    = 0x4, // mark the tab when a match arrives and the log is not on screen
    Notify = 0x8, // raise a desktop notification on the same trigger
};
Q_DECLARE_FLAGS(HighlightActions, HighlightAction)

// M5 — Highlighting (SPEC.md §7, ARCHITECTURE.md §8, invariant #4). A highlight
// rule colors matching records in place — it never removes them. Rules are an
// ORDERED list evaluated first-match-wins; the matched rule supplies a background
// AND a foreground role, each either a palette INDEX (0..11) or `kDefault` meaning
// "leave that role at the theme's normal color".
//
// A rule matches on the SAME five axes as a filter — subsystem, thread, priority,
// time range and message text — by embedding a `MatchCriteria` (SPEC.md §6, §7).
// The criteria carry NAMES, levels and wall clock, so a rule survives export/import,
// a re-index and a time-zone change; HighlighterSet::resolve() turns each rule's copy
// into a `FilterSet` whose integer axes the paint path compares directly.
//
// Two things differ from filtering, both expressed as arguments to that resolve
// rather than as a second predicate chain:
//   - AbsentField::DoesNotMatch — a record lacking the field a rule keys on is not
//     colored, the deliberate inverse of §6's promise that it is not hidden.
//   - NoOpAxes::Keep — "≥TRACE" or "every subsystem" is a legitimate *color
//     everything parsed* rule, so no axis is collapsed away.
//
// Priority match semantics: MINIMUM level (`>=`), consistent with the filter axis
// (§7.2). A rule matching "at least WARN" colors WARN, ERROR and FATAL; order the
// list high-severity-first for a per-level look.
struct HighlightRule
{
    bool enabled = true;

    // The five match axes. A rule with no active axis is inert (see match()), so a
    // freshly added, unconfigured rule colors nothing until an axis is set.
    MatchCriteria match;

    // What this rule DOES when it matches (M19, SPEC.md §7). Defaults to Color alone,
    // which is what every rule written before actions existed meant.
    HighlightActions actions = HighlightAction::Color;

    // The two roles, and the configuration of the Color action specifically. Each is a
    // palette index (0..kSlotCount-1) or HighlightPalette::kDefault.
    int background = HighlightPalette::kDefault;
    int foreground = HighlightPalette::kDefault;

    // Portable JSON: names, palette INDICES and action TOKENS only — never ids, never
    // RGB, never a flags integer — so an exported rule imports on any theme, after a
    // re-index, and across a version that has learned a new action.
    // The axes go under a nested "match" object; fromJson() still reads the original
    // flat two-axis keys when that object is absent, so highlighter presets, exported
    // files and sessions written before the axis set grew keep loading. That backward
    // read, and "actions" being omitted for a colour-only rule, are what make a
    // PresetStore/SessionStore schema bump unnecessary — and it must stay unnecessary,
    // for two different reasons rather than the one this comment used to give:
    // PresetStore gates on EXACT version equality with no migration (PresetStore.cpp),
    // so a bump silently discards every preset a user already has; SessionStore does
    // migrate v1 and v2 upward (SessionStore.cpp), but a bumped session file is then
    // unreadable by any already-shipped binary, which is a downgrade hazard.
    //
    // Two rules about "actions" that are easy to undo (ARCHITECTURE.md §7.5):
    //   - fromJson tests contains("actions"), NEVER the array's emptiness. Absent means
    //     {Color}; present-but-empty means NO actions, which is a rule the user has
    //     deliberately parked — exactly what a rule looks like the moment Colour is
    //     unticked. Reading empty as absent re-colours every such rule on next launch.
    //   - toJson omits the key entirely for a colour-only rule, so everything written
    //     before M19 serializes byte-identically.
    QJsonObject toJson() const;
    static HighlightRule fromJson(const QJsonObject &o);

    // Every field of the rule: its tick, its five axes, what it does and the two
    // colours it does it in. There is nothing else on a rule — no name, no label — so
    // this IS the rule, and anything added to the struct belongs here in the same
    // commit (MatchCriteria::operator== says why at length).
    //
    // What asks: the Highlighters tab's marker, which compares this log's whole rule
    // list IN ORDER against HighlighterSet::defaults() (HighlighterPane::
    // hasCustomRules, ARCHITECTURE.md §7.5). Order matters to it because order is
    // meaning here — first-match-wins is per action (§7.5), so two rules swapped is a
    // different set of colours on screen, not the same set differently listed.
    bool operator==(const HighlightRule &o) const
    {
        return enabled == o.enabled && match == o.match && actions == o.actions
            && background == o.background && foreground == o.foreground;
    }
    bool operator!=(const HighlightRule &o) const { return !(*this == o); }
};

Q_DECLARE_OPERATORS_FOR_FLAGS(HighlightActions)

// The first-match-wins rule index PER ACTION (M19). -1 where no enabled rule carrying
// that action matched. Resolving all the wanted actions in one pass is not a
// convenience: the live path wants Digest, Tab and Notify for the same record, and
// three separate match() calls would decode that record's message three times —
// exactly the cost invariant #4 exists to stop.
struct ActionMatch
{
    int color = -1;
    int digest = -1;
    int tab = -1;
    int notify = -1;

    int forAction(HighlightAction a) const
    {
        switch (a) {
        case HighlightAction::Color:  return color;
        case HighlightAction::Digest: return digest;
        case HighlightAction::Tab:    return tab;
        case HighlightAction::Notify: return notify;
        }
        return -1;
    }
};

// The complete highlight state for one Document (per-file, invariant #7). Holds the
// ordered rule list plus, per rule, its criteria resolved to the integer-comparing
// `FilterSet` the paint path evaluates (invariant #4).
class HighlighterSet
{
public:
    QVector<HighlightRule> rules;

    // The rule list a log starts with when nothing has ever been saved for it
    // (SPEC.md §7, ARCHITECTURE.md §7.5.4). ERROR and FATAL are what a reader opens a
    // log to find, and out of the box they rendered exactly like TRACE; three rules is
    // what that costs, and the whole of what shipping them needs.
    //
    // A SEED, never a floor: the caller applies it only where nothing was stored, so a
    // user who deletes every rule keeps an empty list. The distinction is the caller's
    // — MainWindow asks whether the session said anything about this file's rules at
    // all, not whether what it said was empty (the contains()-not-isEmpty() rule
    // fromJson records above, one level up).
    //
    // Ordered high-severity-first, because the priority axis is a MINIMUM: the ERROR
    // rule also matches FATAL, so the FATAL rule has to sit above it or first-match-wins
    // hands a FATAL record the ERROR colour. Nothing below WARN is coloured — colouring
    // the noise spends the reader's attention on the records they were skipping — and
    // every rule carries the COLOUR ACTION ALONE: a default that marked tabs or raised
    // desktop notifications would be loftail deciding, before the user has opened the
    // pane, that every ERROR in every log is worth interrupting them for.
    //
    // A format with no %p leaves every record at Priority::Unknown, which
    // AbsentField::DoesNotMatch already refuses to colour, so these rules are inert on
    // a log that carries no level with no gate anywhere.
    static HighlighterSet defaults();

    // Resolve every rule's criteria against `idx` (which owns the intern tables),
    // `format` (which gates the thread and time axes) and `displayZone` (which
    // interprets the typed time bounds). Must be re-run whenever the intern tables
    // grow, the rule list changes, or the display zone moves. Cheap — a hash lookup
    // per name and one regex compile per text axis, all of it kept OFF the paint
    // path. After this, match() compares integers and runs an already-compiled regex.
    void resolve(const RecordIndex &idx, const LogFormat &format,
                 const QTimeZone &displayZone);

    // First-match-wins over the ENABLED rules, PER ACTION (M19). `wanted` names the
    // actions the caller cares about; only rules carrying one of them are candidates,
    // and each wanted action is answered by the first such rule that matches. So a
    // digest-only rule placed above a colouring rule cannot shadow it — it was never a
    // candidate for Color at all.
    //
    // `msg` is any callable returning the record's decoded message text; it is invoked
    // ONLY when a rule's integer axes have already passed AND that rule has an active
    // text axis, and its result is memoized across rules — so N text rules still cost at
    // most ONE decode, and a record no candidate rule's integer axes admit costs none
    // (invariant #4, §7.2, exactly the ordering FilterSet::accepts uses). The memo spans
    // every wanted action, which is the reason this is one pass and not one call each.
    //
    // A rule whose criteria have no active axis never matches, so an unconfigured rule
    // is inert; so is a rule carrying no action, which is never a candidate for anything.
    template <class MessageFn>
    ActionMatch matchActions(const Record &r, HighlightActions wanted, MessageFn &&msg) const
    {
        static const FilterSet kInert; // no active axis => never matches

        ActionMatch out;
        HighlightActions remaining = wanted;
        if (!remaining)
            return out;

        std::optional<QString> message;
        for (int i = 0; i < rules.size() && remaining; ++i) {
            const HighlightRule &rule = rules.at(i);
            if (!rule.enabled)
                continue;
            // Only what this rule could still answer. A rule carrying nothing still
            // wanted, or nothing at all, is skipped before its axes are even read.
            const HighlightActions offers = rule.actions & remaining;
            if (!offers)
                continue;
            const FilterSet &fs = (i < m_resolved.size()) ? m_resolved.at(i) : kInert;
            if (!fs.anyActive() || !fs.acceptsIntegerAxes(r))
                continue;
            if (fs.text.active()) {
                if (!message)
                    message = msg();
                if (!fs.acceptsText(*message))
                    continue;
            }
            if (offers.testFlag(HighlightAction::Color))
                out.color = i;
            if (offers.testFlag(HighlightAction::Digest))
                out.digest = i;
            if (offers.testFlag(HighlightAction::Tab))
                out.tab = i;
            if (offers.testFlag(HighlightAction::Notify))
                out.notify = i;
            remaining &= ~offers; // answered; later rules cannot win these
        }
        return out;
    }

    // Does rule `i` match this record, on its own terms? The question the DIGEST asks
    // (M19), and the one first-match-wins cannot answer: a digest is one row PER RULE,
    // so "which rule wins" is not what is being asked — each rule needs its own newest
    // match, including rules a higher rule would have shadowed.
    //
    // `message` is an in/out memo so a caller testing several rules against ONE record
    // decodes it at most once, exactly as matchActions() does across rules.
    template <class MessageFn>
    bool ruleMatches(int i, const Record &r, std::optional<QString> &message,
                     MessageFn &&msg) const
    {
        static const FilterSet kInert;

        if (i < 0 || i >= rules.size() || !rules.at(i).enabled)
            return false;
        const FilterSet &fs = (i < m_resolved.size()) ? m_resolved.at(i) : kInert;
        if (!fs.anyActive() || !fs.acceptsIntegerAxes(r))
            return false;
        if (fs.text.active()) {
            if (!message)
                message = msg();
            if (!fs.acceptsText(*message))
                return false;
        }
        return true;
    }

    // The same with no shared memo, for a caller testing one rule against one record.
    template <class MessageFn>
    bool ruleMatches(int i, const Record &r, MessageFn &&msg) const
    {
        std::optional<QString> message;
        return ruleMatches(i, r, message, std::forward<MessageFn>(msg));
    }

    // The single-action form: the index of the first enabled rule carrying `action`
    // that matches, or -1 when none does (the record keeps its un-highlighted
    // appearance). This is what the paint path asks, with action == Color.
    template <class MessageFn>
    int match(const Record &r, HighlightAction action, MessageFn &&msg) const
    {
        return matchActions(r, action, std::forward<MessageFn>(msg)).forAction(action);
    }

    // Convenience for callers with no decode available (the integer axes are all
    // they can evaluate). A rule with an active text axis sees an EMPTY message, so
    // it matches nothing — use the MessageFn overload wherever the text is reachable.
    int match(const Record &r, HighlightAction action) const
    {
        return match(r, action, [] { return QString(); });
    }

    // True when some enabled rule is actually configured to match something. The
    // paint path's early-out: with no such rule there is nothing to evaluate and no
    // decode to risk.
    bool anyEnabled() const;

    // The same question restricted to rules carrying one of `actions` — the live
    // path's early-out (ARCHITECTURE.md §7.5). A document whose rules only colour
    // therefore pays exactly one walk of this list per watch tick, touching no record
    // and decoding nothing.
    bool anyEnabled(HighlightActions actions) const;

    // Portable JSON round-trip for presets, export/import, and per-file session
    // persistence (all name/index based — theme- and reindex-portable, §8).
    QJsonArray toJson() const;
    static HighlighterSet fromJson(const QJsonArray &a);

private:
    // Parallel to `rules`: rules[i]'s criteria resolved to the integer predicate.
    // Sized to rules.size() by resolve(); match() tolerates a shorter/absent vector
    // by treating the rule as inert until resolve() runs.
    QVector<FilterSet> m_resolved;
};

} // namespace loftail
