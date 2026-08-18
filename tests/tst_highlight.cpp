#include <QtTest>

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimeZone>

#include <cmath>
#include <functional>

#include "Highlight.h"
#include "LogFormat.h"
#include "MatchCriteria.h"
#include "Palette.h"
#include "Priority.h"
#include "Record.h"
#include "RecordIndex.h"

using namespace loftail;

// M19 — every case written before actions existed asks "which rule COLOURS this
// record", because colour was then a rule's only effect. Phrasing them through one
// helper rather than rewriting thirty assertions is deliberate: it keeps them a
// regression statement about the Color action, and leaves the per-action cases below
// as the only place matchActions() is exercised directly.
static int colorRule(const HighlighterSet &set, const Record &r)
{
    return set.match(r, HighlightAction::Color);
}
template <class MessageFn>
static int colorRule(const HighlighterSet &set, const Record &r, MessageFn &&msg)
{
    return set.match(r, HighlightAction::Color, std::forward<MessageFn>(msg));
}

// M5 — the highlight core (SPEC.md §7, ARCHITECTURE.md §8, invariant #4). Pure,
// UI-free: rules persist palette INDICES (not RGB) and portable match criteria (not
// interned ids), match first-match-wins over the same five axes a filter offers, and
// supply a background and a foreground role with a *default* fallback. QColor and
// QRegularExpression are value types, so no QApplication is needed.
class TestHighlight : public QObject
{
    Q_OBJECT

private:
    // A small index: three subsystems, two threads, so match() has real interned ids
    // to compare (invariant #4).
    static RecordIndex makeIndex();
    // A format that carries every field, so no axis is gated off (SPEC.md §6).
    static LogFormat makeFormat();
    static Record rec(quint32 loggerId, Priority p, quint32 threadId = 1,
                      qint64 timestamp = 1000);
    // The one call every test needs; the zone only matters to the time axis.
    static void resolve(HighlighterSet &set, const RecordIndex &idx);

private slots:
    void ruleJsonRoundTripUsesIndicesNotRgb();
    void setJsonRoundTrip();
    void legacyFlatRuleJsonStillLoads();
    void firstMatchWins();
    void defaultRoleFallsBackToTheme();
    void priorityIsMinLevelAndExemptsUnknown();
    void loggerAxisMatchesInternedIds();
    void threadAxisMatchesInternedIds();
    void timeRangeAxisIsInclusive();
    void textAxisSubstringRegexCaseNegate();
    void axesCombineWithAnd();
    void absentFieldNeverMatches();
    void textDecodeIsLazyAndMemoized();
    void unconfiguredRuleIsInert();
    void disabledRuleIsSkipped();
    void unresolvedSetMatchesNothing();
    void paletteDualThemeResolves();
    void paletteEveryBackgroundHasReadableText();

    // M19 — a rule's effect is a SET of actions, and colour is one of them.
    void actionsRoundTripAsTokens();
    void absentActionsMeansColourOnly();
    void anEmptyActionsArrayIsNotColour();
    void unknownActionTokensAreIgnoredWithoutResurrectingColour();
    void aColourOnlyRuleSerializesWithoutTheActionsKey();
    void aDigestOnlyRuleDoesNotShadowAColouringRule();
    void firstMatchWinsIsDecidedPerAction();
    void matchActionsDecodesOnceAcrossEveryWantedAction();
    void anyEnabledFiltersByAction();
    void aRuleWithNoActionsIsNeverACandidate();

    // The rule list a log with nothing saved for it starts with (SPEC.md §7).
    void theDefaultRulesColourTheThreeLevelsWorthFindingAndNothingBelow();
    void everyDefaultRuleCarriesTheColourActionAndNothingElse();
    void everyDefaultRuleNamesTheTextColourThatReadsOnItsBackground();
    void theDefaultRulesAreInertWhereNoRecordCarriesALevel();

    // Rule equality, which the Highlighters tab's marker is decided by.
    void aFreshCopyOfTheSeededRulesComparesEqual();
    void aRuleDiffersWhenAnyOneFieldOfItDoes();
    void anUnsetTimeBoundEqualsAnotherUnsetOneAndNotAnySetOne();
    void reorderingARuleListMakesItADifferentList();
};

// Every field of a rule set to something other than its default, so a case below can
// move exactly one of them and be sure the move is what the comparison saw. Enumerated
// from the struct definitions in Highlight.h and MatchCriteria.h, which is the only way
// to write this: a field nobody thought of is a field nobody tests.
static HighlightRule fullyConfiguredRule()
{
    HighlightRule r;
    r.enabled = true;
    r.match.priorityEnabled = true;
    r.match.minPriority = Priority::Warn;
    r.match.loggerEnabled = true;
    r.match.loggerNames = QStringList{QStringLiteral("db.pool"), QStringLiteral("net.io")};
    r.match.threadEnabled = true;
    r.match.threadNames = QStringList{QStringLiteral("main")};
    r.match.loggerCoversAll = false;
    r.match.threadCoversAll = false;
    r.match.loggerRestrictive = true;
    r.match.threadRestrictive = true;
    r.match.timeEnabled = true;
    r.match.start = QDateTime(QDate(2026, 7, 21), QTime(10, 0, 0));
    r.match.end = QDateTime(QDate(2026, 7, 21), QTime(11, 0, 0));
    r.match.text.enabled = true;
    r.match.text.negate = true;
    r.match.text.matcher.set(QStringLiteral("boom"), /*regex=*/true, Qt::CaseSensitive);
    r.actions = HighlightAction::Color | HighlightAction::Digest;
    r.background = 3;
    r.foreground = 7;
    return r;
}

Record TestHighlight::rec(quint32 loggerId, Priority p, quint32 threadId, qint64 timestamp)
{
    Record r{};
    r.offset = 0;
    r.timestamp = timestamp;
    r.length = 1;
    r.loggerId = loggerId;
    r.threadId = threadId;
    r.lineCount = 1;
    r.priority = quint8(p);
    return r;
}

RecordIndex TestHighlight::makeIndex()
{
    RecordIndex idx;
    idx.loggers.intern(QStringLiteral("net.io"));    // id 1
    idx.loggers.intern(QStringLiteral("db.pool"));   // id 2
    idx.loggers.intern(QStringLiteral("ui.render")); // id 3
    idx.threads.intern(QStringLiteral("main"));      // id 1
    idx.threads.intern(QStringLiteral("worker-2"));  // id 2
    return idx;
}

LogFormat TestHighlight::makeFormat()
{
    LogFormat f;
    f.dateGroup = 1;
    f.threadGroup = 2;
    f.msgGroup = 3;
    return f;
}

void TestHighlight::resolve(HighlighterSet &set, const RecordIndex &idx)
{
    set.resolve(idx, makeFormat(), QTimeZone::utc());
}

void TestHighlight::ruleJsonRoundTripUsesIndicesNotRgb()
{
    HighlightRule in;
    in.enabled = true;
    in.match.loggerEnabled = true;
    in.match.loggerNames = {QStringLiteral("db.pool"), QStringLiteral("net.io")};
    in.match.priorityEnabled = true;
    in.match.minPriority = Priority::Error;
    in.match.text.enabled = true;
    in.match.text.matcher.set(QStringLiteral("timeout"), /*regex=*/false, Qt::CaseInsensitive);
    in.background = 0;                            // a palette slot (Red)
    in.foreground = HighlightPalette::kDefault;   // theme default

    const QJsonObject o = in.toJson();
    // The persisted roles are integer palette indices — never RGB values (§8).
    QVERIFY(o.value(QStringLiteral("background")).isDouble());
    QCOMPARE(o.value(QStringLiteral("background")).toInt(), 0);
    QCOMPARE(o.value(QStringLiteral("foreground")).toInt(), HighlightPalette::kDefault);
    // The axes live in a nested object carrying NAMES, portable across a re-index.
    const QJsonObject m = o.value(QStringLiteral("match")).toObject();
    QCOMPARE(m.value(QStringLiteral("loggerChecked")).toArray().size(), 2);
    QCOMPARE(m.value(QStringLiteral("text")).toString(), QStringLiteral("timeout"));

    const HighlightRule out = HighlightRule::fromJson(o);
    QCOMPARE(out.enabled, in.enabled);
    QCOMPARE(out.match.loggerEnabled, in.match.loggerEnabled);
    QCOMPARE(out.match.loggerNames, in.match.loggerNames);
    QCOMPARE(out.match.priorityEnabled, in.match.priorityEnabled);
    QCOMPARE(int(out.match.minPriority), int(in.match.minPriority));
    QCOMPARE(out.match.text.enabled, in.match.text.enabled);
    QCOMPARE(out.match.text.matcher.pattern(), in.match.text.matcher.pattern());
    QCOMPARE(out.background, in.background);
    QCOMPARE(out.foreground, in.foreground);
}

void TestHighlight::setJsonRoundTrip()
{
    HighlighterSet in;
    HighlightRule a;
    a.match.priorityEnabled = true;
    a.match.minPriority = Priority::Fatal;
    a.background = 0;
    HighlightRule b;
    b.match.loggerEnabled = true;
    b.match.loggerNames = {QStringLiteral("db.pool")};
    b.foreground = 3;
    in.rules = {a, b};

    const HighlighterSet out = HighlighterSet::fromJson(in.toJson());
    QCOMPARE(out.rules.size(), 2);
    QCOMPARE(int(out.rules.at(0).match.minPriority), int(Priority::Fatal));
    QCOMPARE(out.rules.at(0).background, 0);
    QCOMPARE(out.rules.at(1).match.loggerNames, QStringList{QStringLiteral("db.pool")});
    QCOMPARE(out.rules.at(1).foreground, 3);
}

void TestHighlight::legacyFlatRuleJsonStillLoads()
{
    // The two-axis rule shape that shipped before highlighting gained the full filter
    // axis set. Highlighter presets, exported preset files and stored sessions are
    // full of these, and PresetStore/SessionStore reject an unrecognised schema
    // version outright rather than migrating — so the READ has to stay compatible.
    QJsonObject legacy;
    legacy.insert(QStringLiteral("enabled"), true);
    legacy.insert(QStringLiteral("matchLogger"), true);
    legacy.insert(QStringLiteral("loggerNames"),
                  QJsonArray{QStringLiteral("db.pool")});
    legacy.insert(QStringLiteral("matchPriority"), true);
    legacy.insert(QStringLiteral("minPriority"), QStringLiteral("ERROR"));
    legacy.insert(QStringLiteral("background"), 0);
    legacy.insert(QStringLiteral("foreground"), HighlightPalette::kDefault);

    const HighlightRule r = HighlightRule::fromJson(legacy);
    QVERIFY(r.enabled);
    QVERIFY(r.match.loggerEnabled);
    QCOMPARE(r.match.loggerNames, QStringList{QStringLiteral("db.pool")});
    QVERIFY(r.match.priorityEnabled);
    QCOMPARE(int(r.match.minPriority), int(Priority::Error));
    QCOMPARE(r.background, 0);
    QCOMPARE(r.foreground, HighlightPalette::kDefault);
    // The axes the old shape could not express default OFF, not on: a highlight rule
    // has always had to opt into each axis explicitly.
    QVERIFY(!r.match.threadEnabled);
    QVERIFY(!r.match.timeEnabled);
    QVERIFY(!r.match.text.enabled);

    // And it still matches the way it used to.
    HighlighterSet set;
    set.rules = {r};
    const RecordIndex idx = makeIndex();
    resolve(set, idx);
    QCOMPARE(colorRule(set, rec(2, Priority::Error)), 0);
    QCOMPARE(colorRule(set, rec(2, Priority::Info)), -1);
    QCOMPARE(colorRule(set, rec(1, Priority::Error)), -1);
}

void TestHighlight::firstMatchWins()
{
    const RecordIndex idx = makeIndex();

    HighlighterSet set;
    // Rule 0 matches FATAL and up; rule 1 matches WARN and up. A FATAL record
    // matches BOTH, so first-match-wins must pick rule 0 (SPEC.md §7).
    HighlightRule r0;
    r0.match.priorityEnabled = true;
    r0.match.minPriority = Priority::Fatal;
    r0.background = 0; // Red
    HighlightRule r1;
    r1.match.priorityEnabled = true;
    r1.match.minPriority = Priority::Warn;
    r1.background = 2; // Amber
    set.rules = {r0, r1};
    resolve(set, idx);

    QCOMPARE(colorRule(set, rec(1, Priority::Fatal)), 0); // both match -> the earlier one
    QCOMPARE(colorRule(set, rec(1, Priority::Warn)), 1);  // only rule 1
    QCOMPARE(colorRule(set, rec(1, Priority::Info)), -1); // neither
}

void TestHighlight::defaultRoleFallsBackToTheme()
{
    HighlightRule r;
    r.match.priorityEnabled = true;
    r.match.minPriority = Priority::Info;
    r.background = 5;                           // Blue
    r.foreground = HighlightPalette::kDefault;  // leave text at the theme color

    HighlighterSet set;
    set.rules = {r};
    const RecordIndex idx = makeIndex();
    resolve(set, idx);

    const int m = colorRule(set, rec(1, Priority::Info));
    QCOMPARE(m, 0);
    const HighlightRule &hit = set.rules.at(m);

    // Background is a real slot -> a valid color; foreground is default -> invalid,
    // which the model returns as an empty variant so the theme color stands.
    QVERIFY(HighlightPalette::color(hit.background, /*dark=*/false).isValid());
    QVERIFY(!HighlightPalette::color(hit.foreground, /*dark=*/false).isValid());
}

void TestHighlight::priorityIsMinLevelAndExemptsUnknown()
{
    HighlightRule r;
    r.match.priorityEnabled = true;
    r.match.minPriority = Priority::Warn;
    HighlighterSet set;
    set.rules = {r};
    resolve(set, makeIndex());

    QCOMPARE(colorRule(set, rec(1, Priority::Warn)), 0);   // >= WARN
    QCOMPARE(colorRule(set, rec(1, Priority::Error)), 0);
    QCOMPARE(colorRule(set, rec(1, Priority::Info)), -1);  // below the minimum
    // Unparsed (Unknown) carries no priority, so a priority rule never colors it.
    QCOMPARE(colorRule(set, rec(1, Priority::Unknown)), -1);
}

void TestHighlight::loggerAxisMatchesInternedIds()
{
    const RecordIndex idx = makeIndex();
    HighlightRule r;
    r.match.loggerEnabled = true;
    r.match.loggerNames = {QStringLiteral("db.pool")}; // id 2
    HighlighterSet set;
    set.rules = {r};
    resolve(set, idx);

    QCOMPARE(colorRule(set, rec(2, Priority::Info)), 0);  // db.pool
    QCOMPARE(colorRule(set, rec(1, Priority::Info)), -1); // net.io — not selected

    // A rule naming a subsystem the file has not produced resolves to no id, so it
    // matches nothing until that subsystem appears (then a re-resolve binds it).
    HighlightRule ghost;
    ghost.match.loggerEnabled = true;
    ghost.match.loggerNames = {QStringLiteral("never.seen")};
    HighlighterSet g;
    g.rules = {ghost};
    resolve(g, idx);
    QCOMPARE(colorRule(g, rec(1, Priority::Info)), -1);
}

void TestHighlight::threadAxisMatchesInternedIds()
{
    const RecordIndex idx = makeIndex();
    HighlightRule r;
    r.match.threadEnabled = true;
    r.match.threadNames = {QStringLiteral("worker-2")}; // id 2
    HighlighterSet set;
    set.rules = {r};
    resolve(set, idx);

    QCOMPARE(colorRule(set, rec(1, Priority::Info, /*threadId=*/2)), 0);
    QCOMPARE(colorRule(set, rec(1, Priority::Info, /*threadId=*/1)), -1);

    // The axis is gated on the format carrying a thread field, exactly as the filter
    // axis is (SPEC.md §6): with no %t there is nothing to match on.
    LogFormat noThread = makeFormat();
    noThread.threadGroup = -1;
    HighlighterSet gated;
    gated.rules = {r};
    gated.resolve(idx, noThread, QTimeZone::utc());
    QCOMPARE(colorRule(gated, rec(1, Priority::Info, /*threadId=*/2)), -1);
}

void TestHighlight::timeRangeAxisIsInclusive()
{
    const RecordIndex idx = makeIndex();
    HighlightRule r;
    r.match.timeEnabled = true;
    // Wall clock, interpreted in the zone passed to resolve() — here UTC, so the
    // bounds are 10 000 ms and 20 000 ms after the epoch.
    r.match.start = QDateTime::fromMSecsSinceEpoch(10000, QTimeZone::utc());
    r.match.end = QDateTime::fromMSecsSinceEpoch(20000, QTimeZone::utc());
    HighlighterSet set;
    set.rules = {r};
    resolve(set, idx);

    QCOMPARE(colorRule(set, rec(1, Priority::Info, 1, /*timestamp=*/10000)), 0); // lower bound
    QCOMPARE(colorRule(set, rec(1, Priority::Info, 1, /*timestamp=*/15000)), 0);
    QCOMPARE(colorRule(set, rec(1, Priority::Info, 1, /*timestamp=*/20000)), 0); // upper bound
    QCOMPARE(colorRule(set, rec(1, Priority::Info, 1, /*timestamp=*/9999)), -1);
    QCOMPARE(colorRule(set, rec(1, Priority::Info, 1, /*timestamp=*/20001)), -1);
}

void TestHighlight::textAxisSubstringRegexCaseNegate()
{
    const RecordIndex idx = makeIndex();
    auto matchOn = [&idx](const HighlightRule &r, const QString &message) {
        HighlighterSet set;
        set.rules = {r};
        set.resolve(idx, makeFormat(), QTimeZone::utc());
        return colorRule(set, rec(1, Priority::Info), [&message] { return message; });
    };

    HighlightRule sub;
    sub.match.text.enabled = true;
    sub.match.text.matcher.set(QStringLiteral("TIMEOUT"), false, Qt::CaseInsensitive);
    QCOMPARE(matchOn(sub, QStringLiteral("socket timeout after 30s")), 0);

    HighlightRule cased = sub;
    cased.match.text.matcher.set(QStringLiteral("TIMEOUT"), false, Qt::CaseSensitive);
    QCOMPARE(matchOn(cased, QStringLiteral("socket timeout after 30s")), -1);

    HighlightRule re;
    re.match.text.enabled = true;
    re.match.text.matcher.set(QStringLiteral("timeout.*retry"), /*regex=*/true, Qt::CaseInsensitive);
    QCOMPARE(matchOn(re, QStringLiteral("timeout, will retry")), 0);
    QCOMPARE(matchOn(re, QStringLiteral("timeout, giving up")), -1);

    // Negation colors what does NOT match — "everything except the known noise".
    HighlightRule neg = sub;
    neg.match.text.negate = true;
    QCOMPARE(matchOn(neg, QStringLiteral("socket timeout after 30s")), -1);
    QCOMPARE(matchOn(neg, QStringLiteral("connection established")), 0);

    // A regex that will not compile matches nothing rather than throwing.
    HighlightRule bad;
    bad.match.text.enabled = true;
    bad.match.text.matcher.set(QStringLiteral("("), true, Qt::CaseInsensitive);
    QVERIFY(!bad.match.text.matcher.isValid());
    QCOMPARE(matchOn(bad, QStringLiteral("anything at all")), -1);
}

void TestHighlight::axesCombineWithAnd()
{
    const RecordIndex idx = makeIndex();
    HighlightRule r;
    r.match.priorityEnabled = true;
    r.match.minPriority = Priority::Warn;
    r.match.loggerEnabled = true;
    r.match.loggerNames = {QStringLiteral("db.pool")}; // id 2
    r.match.text.enabled = true;
    r.match.text.matcher.set(QStringLiteral("deadlock"), false, Qt::CaseInsensitive);
    HighlighterSet set;
    set.rules = {r};
    resolve(set, idx);

    auto hit = [&set](const Record &rr, const QString &msg) {
        return colorRule(set, rr, [&msg] { return msg; });
    };
    // Across axes it is AND (SPEC.md §6): all three must hold.
    QCOMPARE(hit(rec(2, Priority::Error), QStringLiteral("deadlock detected")), 0);
    QCOMPARE(hit(rec(2, Priority::Info), QStringLiteral("deadlock detected")), -1);  // priority
    QCOMPARE(hit(rec(1, Priority::Error), QStringLiteral("deadlock detected")), -1); // subsystem
    QCOMPARE(hit(rec(2, Priority::Error), QStringLiteral("all fine")), -1);          // text
}

void TestHighlight::absentFieldNeverMatches()
{
    // The deliberate inverse of filtering. A filter must not HIDE a record that never
    // carried the field it tests (SPEC.md §4/§6, tst_filter::absentLoggerOrThreadNever
    // Hidden); a highlight rule must not COLOR it (SPEC.md §7), or a subsystem rule
    // would paint every unparsed plain-text line.
    const RecordIndex idx = makeIndex();
    // Interned id 0 is the "field absent" sentinel; kNoTimestamp likewise.
    const Record plain = rec(/*loggerId=*/0, Priority::Unknown, /*threadId=*/0,
                             Record::kNoTimestamp);

    HighlightRule byLogger;
    byLogger.match.loggerEnabled = true;
    byLogger.match.loggerNames = {QStringLiteral("db.pool")};

    HighlightRule byThread;
    byThread.match.threadEnabled = true;
    byThread.match.threadNames = {QStringLiteral("main")};

    HighlightRule byTime;
    byTime.match.timeEnabled = true;
    byTime.match.start = QDateTime::fromMSecsSinceEpoch(0, QTimeZone::utc());
    byTime.match.end = QDateTime::fromMSecsSinceEpoch(1'000'000, QTimeZone::utc());

    HighlightRule byPriority;
    byPriority.match.priorityEnabled = true;
    byPriority.match.minPriority = Priority::Trace; // the widest possible minimum

    for (const HighlightRule &r : {byLogger, byThread, byTime, byPriority}) {
        HighlighterSet set;
        set.rules = {r};
        resolve(set, idx);
        QCOMPARE(colorRule(set, plain), -1);
    }

    // The text axis has no "absent" case: an unparsed record's whole line IS its
    // message (Document::messageText), so it is matched like any other.
    HighlightRule byText;
    byText.match.text.enabled = true;
    byText.match.text.matcher.set(QStringLiteral("segfault"), false, Qt::CaseInsensitive);
    HighlighterSet set;
    set.rules = {byText};
    resolve(set, idx);
    QCOMPARE(colorRule(set, plain, [] { return QStringLiteral("*** segfault ***"); }), 0);
}

void TestHighlight::textDecodeIsLazyAndMemoized()
{
    // The text axis is the only one without an integer fast path, and it runs on the
    // PAINT path (invariant #4, ARCHITECTURE.md §7.2). Two properties keep it cheap:
    // the decode never happens for a record the integer axes already rejected, and it
    // happens at most once no matter how many rules ask for it.
    const RecordIndex idx = makeIndex();

    HighlightRule a;
    a.match.loggerEnabled = true;
    a.match.loggerNames = {QStringLiteral("db.pool")}; // id 2
    a.match.text.enabled = true;
    a.match.text.matcher.set(QStringLiteral("zzz"), false, Qt::CaseInsensitive); // never hits
    HighlightRule b = a;
    b.match.text.matcher.set(QStringLiteral("yyy"), false, Qt::CaseInsensitive); // never hits
    HighlightRule c = a;
    c.match.text.matcher.set(QStringLiteral("boom"), false, Qt::CaseInsensitive);

    HighlighterSet set;
    set.rules = {a, b, c};
    resolve(set, idx);

    int decodes = 0;
    auto message = [&decodes] {
        ++decodes;
        return QStringLiteral("boom");
    };

    // net.io fails every rule's subsystem axis, so no rule ever reaches its text axis.
    QCOMPARE(colorRule(set, rec(1, Priority::Info), message), -1);
    QCOMPARE(decodes, 0);

    // db.pool passes all three subsystem axes; three text axes are consulted, but the
    // decode is memoized across the rule loop.
    decodes = 0;
    QCOMPARE(colorRule(set, rec(2, Priority::Info), message), 2);
    QCOMPARE(decodes, 1);
}

void TestHighlight::unconfiguredRuleIsInert()
{
    // A rule with no active axis must never match — a freshly-added rule is inert
    // until the user configures an axis.
    HighlightRule r;
    r.background = 0;
    HighlighterSet set;
    set.rules = {r};
    resolve(set, makeIndex());
    QCOMPARE(colorRule(set, rec(1, Priority::Fatal)), -1);
    QVERIFY(!set.anyEnabled());

    // An enabled text axis with an empty pattern is still inert: an empty query would
    // match everything, which is not a rule.
    HighlightRule empty;
    empty.match.text.enabled = true;
    HighlighterSet e;
    e.rules = {empty};
    resolve(e, makeIndex());
    QVERIFY(!e.anyEnabled());
    QCOMPARE(colorRule(e, rec(1, Priority::Fatal), [] { return QStringLiteral("x"); }), -1);
}

void TestHighlight::disabledRuleIsSkipped()
{
    HighlightRule r;
    r.enabled = false;
    r.match.priorityEnabled = true;
    r.match.minPriority = Priority::Trace; // would match everything if enabled
    HighlighterSet set;
    set.rules = {r};
    resolve(set, makeIndex());
    QCOMPARE(colorRule(set, rec(1, Priority::Fatal)), -1);
    QVERIFY(!set.anyEnabled());
}

void TestHighlight::unresolvedSetMatchesNothing()
{
    // resolve() is what binds names to ids, wall clock to UTC ms and the pattern to a
    // compiled regex. Until it has run there is nothing to compare, so a rule matches
    // nothing rather than matching on stale or half-built state.
    HighlightRule r;
    r.match.priorityEnabled = true;
    r.match.minPriority = Priority::Trace;
    HighlighterSet set;
    set.rules = {r};
    QCOMPARE(colorRule(set, rec(1, Priority::Fatal)), -1);

    resolve(set, makeIndex());
    QCOMPARE(colorRule(set, rec(1, Priority::Fatal)), 0);
}

void TestHighlight::paletteDualThemeResolves()
{
    // Three tone bands of eight hues plus a neutral (Palette.h).
    QCOMPARE(HighlightPalette::count(), 27);
    QCOMPARE(HighlightPalette::kSlotsPerBand * HighlightPalette::kBandCount, 27);
    for (int i = 0; i < HighlightPalette::count(); ++i) {
        QVERIFY(HighlightPalette::color(i, /*dark=*/false).isValid());
        QVERIFY(HighlightPalette::color(i, /*dark=*/true).isValid());
    }
    // The three neutrals close their bands, so Ink, Gray and Paper are exactly the
    // slots addRule() skips when it picks a background colour by itself.
    QVERIFY(HighlightPalette::isNeutral(HighlightPalette::kInk));
    QVERIFY(HighlightPalette::isNeutral(HighlightPalette::kPaper));
    QVERIFY(HighlightPalette::isNeutral(17));
    QVERIFY(!HighlightPalette::isNeutral(0));
    QVERIFY(!HighlightPalette::isNeutral(HighlightPalette::kDefault));
    // The default sentinel resolves to an invalid color in either theme.
    QVERIFY(!HighlightPalette::color(HighlightPalette::kDefault, false).isValid());
    QVERIFY(!HighlightPalette::color(HighlightPalette::kDefault, true).isValid());
}

void TestHighlight::paletteEveryBackgroundHasReadableText()
{
    // The property the palette exists for, and the one it did NOT have before: for
    // every slot used as a background there is a palette colour that reads on it, in
    // BOTH themes, and readableTextSlot() names it. The old palette gave each theme a
    // single tone, so on a dark theme the best of all 144 slot-on-slot pairs measured
    // 1.85:1 — no readable combination existed at all.
    //
    // WCAG relative luminance and contrast, inline: tst_highlight links loftail_core
    // only, and UiColors (which has the same two functions) is UI-side chrome.
    const auto luminance = [](const QColor &c) {
        const auto chan = [](double v) {
            v /= 255.0;
            return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * chan(c.red()) + 0.7152 * chan(c.green()) + 0.0722 * chan(c.blue());
    };
    const auto contrast = [&](const QColor &a, const QColor &b) {
        const double la = luminance(a), lb = luminance(b);
        return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
    };

    for (int i = 0; i < HighlightPalette::count(); ++i) {
        const int fg = HighlightPalette::readableTextSlot(i);
        // Only ever one of the two extremes, so the answer is stable across themes.
        QVERIFY(fg == HighlightPalette::kInk || fg == HighlightPalette::kPaper);
        for (bool dark : {false, true}) {
            const double ratio = contrast(HighlightPalette::color(i, dark),
                                          HighlightPalette::color(fg, dark));
            QVERIFY2(ratio >= 4.5,
                     qPrintable(QStringLiteral("%1 on %2 (%3 theme) is only %4:1")
                                    .arg(QString(HighlightPalette::slot(fg).name),
                                         QString(HighlightPalette::slot(i).name),
                                         dark ? QStringLiteral("dark") : QStringLiteral("light"))
                                    .arg(ratio, 0, 'f', 2)));
        }
    }
    // An out-of-range or default background still answers, rather than reading past
    // the table: a corrupt persisted index must not crash the paint path.
    QCOMPARE(HighlightPalette::readableTextSlot(HighlightPalette::kDefault),
             HighlightPalette::kPaper);
    QCOMPARE(HighlightPalette::readableTextSlot(9999), HighlightPalette::kPaper);
}

// --- M19: a rule's effect is a set of actions -------------------------------

void TestHighlight::actionsRoundTripAsTokens()
{
    HighlightRule r;
    r.actions = HighlightAction::Color | HighlightAction::Digest | HighlightAction::Notify;
    r.match.priorityEnabled = true;
    r.match.minPriority = Priority::Warn;

    const QJsonObject o = r.toJson();
    // TOKENS, never the flags integer: a token still means what it meant after a
    // version has learned a fifth action, and a bitfield does not.
    QVERIFY(o.value(QStringLiteral("actions")).isArray());
    const QJsonArray a = o.value(QStringLiteral("actions")).toArray();
    QCOMPARE(a.size(), 3);
    QCOMPARE(a.at(0).toString(), QStringLiteral("color"));
    QCOMPARE(a.at(1).toString(), QStringLiteral("digest"));
    QCOMPARE(a.at(2).toString(), QStringLiteral("notify"));

    QCOMPARE(HighlightRule::fromJson(o).actions, r.actions);
}

void TestHighlight::absentActionsMeansColourOnly()
{
    // Every rule ever written before M19. Colour was a rule's only effect, so an absent
    // key means exactly that — and this is why no preset or session schema had to move.
    QJsonObject o;
    o.insert(QStringLiteral("enabled"), true);
    o.insert(QStringLiteral("background"), 3);

    const HighlightRule r = HighlightRule::fromJson(o);
    QCOMPARE(r.actions, HighlightActions(HighlightAction::Color));
    QCOMPARE(r.background, 3);
}

void TestHighlight::anEmptyActionsArrayIsNotColour()
{
    // The distinction the read is easy to get wrong on. A PRESENT but empty array is
    // "this rule matches and does nothing" — which is one click away, the moment the
    // user unticks Colour — and reading it as "nothing saved" would silently re-colour
    // every parked rule on the next launch. Hence contains(), never isEmpty().
    QJsonObject o;
    o.insert(QStringLiteral("enabled"), true);
    o.insert(QStringLiteral("actions"), QJsonArray());

    QCOMPARE(HighlightRule::fromJson(o).actions, HighlightActions());
}

void TestHighlight::unknownActionTokensAreIgnoredWithoutResurrectingColour()
{
    // A rule from a later version naming an action this build has never heard of.
    QJsonObject o;
    o.insert(QStringLiteral("actions"),
             QJsonArray({QStringLiteral("digest"), QStringLiteral("teleport")}));
    QCOMPARE(HighlightRule::fromJson(o).actions, HighlightActions(HighlightAction::Digest));

    // And the corner that must not collapse into the absent case: unknown tokens ONLY.
    // The key was there and the user's intent was recorded, just not in a vocabulary
    // this build has — so the answer is no actions, not "colour, as of old".
    QJsonObject onlyUnknown;
    onlyUnknown.insert(QStringLiteral("actions"), QJsonArray({QStringLiteral("teleport")}));
    QCOMPARE(HighlightRule::fromJson(onlyUnknown).actions, HighlightActions());
}

void TestHighlight::aColourOnlyRuleSerializesWithoutTheActionsKey()
{
    // The whole reason neither store's schema version moved: a colour-only rule — which
    // is every rule that predates M19 and most that follow it — writes byte-identically
    // to what it wrote before. PresetStore gates on EXACT version equality with no
    // migration, so a bump discards every preset a user has.
    HighlightRule plain;
    plain.match.priorityEnabled = true;
    QVERIFY(!plain.toJson().contains(QStringLiteral("actions")));

    // ...and anything else says so explicitly, including "no actions at all".
    HighlightRule parked = plain;
    parked.actions = HighlightActions();
    QVERIFY(parked.toJson().contains(QStringLiteral("actions")));
    QVERIFY(parked.toJson().value(QStringLiteral("actions")).toArray().isEmpty());
}

void TestHighlight::aDigestOnlyRuleDoesNotShadowAColouringRule()
{
    const RecordIndex idx = makeIndex();

    HighlightRule digestOnly;
    digestOnly.actions = HighlightAction::Digest;
    digestOnly.match.priorityEnabled = true;
    digestOnly.match.minPriority = Priority::Info;

    HighlightRule colouring;
    colouring.actions = HighlightAction::Color;
    colouring.match.priorityEnabled = true;
    colouring.match.minPriority = Priority::Info;

    HighlighterSet set;
    set.rules = {digestOnly, colouring}; // the digest rule sits FIRST
    resolve(set, idx);

    // First-match-wins is per ACTION, so the rule above was never a candidate for
    // Color at all. Getting this wrong would mean adding a digest rule silently
    // switched off the colouring of everything below it.
    QCOMPARE(set.match(rec(1, Priority::Error), HighlightAction::Color), 1);
    QCOMPARE(set.match(rec(1, Priority::Error), HighlightAction::Digest), 0);
}

void TestHighlight::firstMatchWinsIsDecidedPerAction()
{
    const RecordIndex idx = makeIndex();

    HighlightRule first;
    first.actions = HighlightAction::Color | HighlightAction::Tab;
    first.match.priorityEnabled = true;
    first.match.minPriority = Priority::Error;

    HighlightRule second;
    second.actions = HighlightAction::Color | HighlightAction::Digest;
    second.match.priorityEnabled = true;
    second.match.minPriority = Priority::Info;

    HighlighterSet set;
    set.rules = {first, second};
    resolve(set, idx);

    // An ERROR: both rules match. Color and Tab go to the first, Digest to the second
    // — the first does not carry it — and Notify to nobody.
    const ActionMatch m = set.matchActions(
        rec(1, Priority::Error),
        HighlightAction::Color | HighlightAction::Digest | HighlightAction::Tab
            | HighlightAction::Notify,
        [] { return QString(); });
    QCOMPARE(m.color, 0);
    QCOMPARE(m.tab, 0);
    QCOMPARE(m.digest, 1);
    QCOMPARE(m.notify, -1);

    // An INFO: only the second rule matches, and it answers what it carries.
    const ActionMatch info = set.matchActions(
        rec(1, Priority::Info),
        HighlightAction::Color | HighlightAction::Digest | HighlightAction::Tab,
        [] { return QString(); });
    QCOMPARE(info.color, 1);
    QCOMPARE(info.digest, 1);
    QCOMPARE(info.tab, -1);
}

void TestHighlight::matchActionsDecodesOnceAcrossEveryWantedAction()
{
    // The reason matchActions() is ONE pass over a wanted-set rather than one call per
    // action. The live path wants Digest, Tab and Notify for the same record; three
    // calls would be three decodes of the same message, which is exactly the cost
    // invariant #4 exists to stop.
    const RecordIndex idx = makeIndex();

    HighlightRule a;
    a.actions = HighlightAction::Digest;
    a.match.text.enabled = true;
    a.match.text.matcher.set(QStringLiteral("zzz"), false, Qt::CaseInsensitive);
    HighlightRule b = a;
    b.actions = HighlightAction::Tab;
    b.match.text.matcher.set(QStringLiteral("yyy"), false, Qt::CaseInsensitive);
    HighlightRule c = a;
    c.actions = HighlightAction::Notify;
    c.match.text.matcher.set(QStringLiteral("boom"), false, Qt::CaseInsensitive);

    HighlighterSet set;
    set.rules = {a, b, c};
    resolve(set, idx);

    int decodes = 0;
    auto message = [&decodes] {
        ++decodes;
        return QStringLiteral("boom");
    };

    const ActionMatch m = set.matchActions(
        rec(1, Priority::Info),
        HighlightAction::Digest | HighlightAction::Tab | HighlightAction::Notify, message);
    QCOMPARE(decodes, 1);
    QCOMPARE(m.digest, -1);
    QCOMPARE(m.tab, -1);
    QCOMPARE(m.notify, 2);

    // And a record no candidate rule's axes admit still costs nothing: asking for an
    // action no rule carries never reaches a text axis at all.
    decodes = 0;
    QCOMPARE(set.match(rec(1, Priority::Info), HighlightAction::Color, message), -1);
    QCOMPARE(decodes, 0);
}

void TestHighlight::anyEnabledFiltersByAction()
{
    const RecordIndex idx = makeIndex();

    HighlightRule colourOnly;
    colourOnly.actions = HighlightAction::Color;
    colourOnly.match.priorityEnabled = true;

    HighlighterSet set;
    set.rules = {colourOnly};
    resolve(set, idx);

    // This is the live path's whole cost for a document whose rules only colour: one
    // walk of the rule list per watch tick, touching no record and decoding nothing.
    QVERIFY(set.anyEnabled());
    QVERIFY(set.anyEnabled(HighlightAction::Color));
    QVERIFY(!set.anyEnabled(HighlightAction::Digest | HighlightAction::Tab
                            | HighlightAction::Notify));

    set.rules[0].actions |= HighlightAction::Tab;
    resolve(set, idx);
    QVERIFY(set.anyEnabled(HighlightAction::Digest | HighlightAction::Tab
                           | HighlightAction::Notify));

    // A disabled rule counts for nothing, whatever it carries.
    set.rules[0].enabled = false;
    resolve(set, idx);
    QVERIFY(!set.anyEnabled(HighlightAction::Tab));
}

void TestHighlight::aRuleWithNoActionsIsNeverACandidate()
{
    const RecordIndex idx = makeIndex();

    HighlightRule parked;
    parked.actions = HighlightActions(); // matches, and does nothing
    parked.match.priorityEnabled = true;
    parked.match.minPriority = Priority::Trace;

    HighlightRule colouring;
    colouring.actions = HighlightAction::Color;
    colouring.match.priorityEnabled = true;
    colouring.match.minPriority = Priority::Trace;

    HighlighterSet set;
    set.rules = {parked, colouring};
    resolve(set, idx);

    // Parking a rule is how it is kept without being deleted, so it must not shadow
    // what follows it — the same rule as a digest-only rule above, in its limit case.
    QCOMPARE(set.match(rec(1, Priority::Info), HighlightAction::Color), 1);
    QVERIFY(!set.anyEnabled(HighlightAction::Digest));
}

void TestHighlight::theDefaultRulesColourTheThreeLevelsWorthFindingAndNothingBelow()
{
    HighlighterSet set = HighlighterSet::defaults();
    resolve(set, makeIndex());

    // ORDER IS THE WHOLE THING. The priority axis is a minimum, so the ERROR rule
    // matches FATAL too; only the FATAL rule sitting above it stops first-match-wins
    // handing a FATAL record the ERROR colour. Swap the two and this is the one
    // assertion that fails.
    QCOMPARE(colorRule(set, rec(1, Priority::Fatal)), 0);
    QCOMPARE(colorRule(set, rec(1, Priority::Error)), 1);
    QCOMPARE(colorRule(set, rec(1, Priority::Warn)), 2);
    QVERIFY(set.rules.at(0).background != set.rules.at(1).background);
    QVERIFY(set.rules.at(1).background != set.rules.at(2).background);

    // Nothing below WARN is coloured: colouring the noise spends the reader's attention
    // on the records they were skipping.
    QCOMPARE(colorRule(set, rec(1, Priority::Info)), -1);
    QCOMPARE(colorRule(set, rec(1, Priority::Debug)), -1);
    QCOMPARE(colorRule(set, rec(1, Priority::Trace)), -1);
}

void TestHighlight::everyDefaultRuleCarriesTheColourActionAndNothingElse()
{
    const HighlighterSet set = HighlighterSet::defaults();
    QVERIFY(!set.rules.isEmpty());
    for (const HighlightRule &r : set.rules) {
        QVERIFY(r.enabled);
        // Colour ALONE. A default that marked tabs or raised desktop notifications
        // would be loftail deciding, before the user has opened the pane, that every
        // ERROR in every log is worth interrupting them for.
        QCOMPARE(r.actions, HighlightActions(HighlightAction::Color));
        // And the only axis is priority, so a default rule says nothing about a
        // subsystem, a thread, a window of time or a word in the message.
        QVERIFY(r.match.priorityEnabled);
        QVERIFY(!r.match.loggerEnabled);
        QVERIFY(!r.match.threadEnabled);
        QVERIFY(!r.match.timeEnabled);
        QVERIFY(!r.match.text.active());
    }
}

void TestHighlight::everyDefaultRuleNamesTheTextColourThatReadsOnItsBackground()
{
    for (const HighlightRule &r : HighlighterSet::defaults().rules) {
        // Palette slots, never RGB and never *default* on the background's partner:
        // readableTextSlot() is the pairing paletteEveryBackgroundHasReadableText()
        // measures at 4.5:1 in BOTH themes, so a default rule is legible by
        // construction rather than by inspection.
        QVERIFY(HighlightPalette::isSlot(r.background));
        QCOMPARE(r.foreground, HighlightPalette::readableTextSlot(r.background));
    }
}

void TestHighlight::theDefaultRulesAreInertWhereNoRecordCarriesALevel()
{
    HighlighterSet set = HighlighterSet::defaults();
    resolve(set, makeIndex());
    // A pattern with no %p leaves every record at Priority::Unknown, which
    // AbsentField::DoesNotMatch already refuses to colour. So the defaults need no gate
    // for a log whose format cannot fill the axis they key on — they simply match
    // nothing, exactly as a hand-made priority rule does on such a log.
    QCOMPARE(colorRule(set, rec(1, Priority::Unknown)), -1);
    QCOMPARE(colorRule(set, rec(0, Priority::Unknown, 0, Record::kNoTimestamp)), -1);
}

void TestHighlight::aFreshCopyOfTheSeededRulesComparesEqual()
{
    // The baseline the marker rests on: the seed is a value, so a log that was handed
    // it and never touched compares equal to a seed built a second later. If this ever
    // fails, every log wears the marker again and for a subtler reason than before.
    QVERIFY(HighlighterSet::defaults().rules == HighlighterSet::defaults().rules);
    const HighlightRule r = fullyConfiguredRule();
    QVERIFY(r == fullyConfiguredRule());
    QVERIFY(!(r != fullyConfiguredRule()));
}

void TestHighlight::aRuleDiffersWhenAnyOneFieldOfItDoes()
{
    // One case per field a user can move, HighlightRule's five and MatchCriteria's
    // fourteen alike. A field missing from operator== is a field the user can edit with
    // the Highlighters tab going on saying nothing about it (ARCHITECTURE.md §7.5) —
    // silent, and in the direction of reporting less than the truth. So the list here is
    // the point, and it grows with the struct.
    struct Case
    {
        const char                      *field;
        std::function<void(HighlightRule &)> edit;
    };
    const Case cases[] = {
        { "enabled",           [](HighlightRule &r) { r.enabled = false; } },
        { "priorityEnabled",   [](HighlightRule &r) { r.match.priorityEnabled = false; } },
        { "minPriority",       [](HighlightRule &r) { r.match.minPriority = Priority::Error; } },
        { "loggerEnabled",     [](HighlightRule &r) { r.match.loggerEnabled = false; } },
        { "loggerNames",       [](HighlightRule &r) { r.match.loggerNames.removeLast(); } },
        { "threadEnabled",     [](HighlightRule &r) { r.match.threadEnabled = false; } },
        { "threadNames",       [](HighlightRule &r) { r.match.threadNames = QStringList{QStringLiteral("worker")}; } },
        { "loggerCoversAll",   [](HighlightRule &r) { r.match.loggerCoversAll = true; } },
        { "threadCoversAll",   [](HighlightRule &r) { r.match.threadCoversAll = true; } },
        { "loggerRestrictive", [](HighlightRule &r) { r.match.loggerRestrictive = false; } },
        { "threadRestrictive", [](HighlightRule &r) { r.match.threadRestrictive = false; } },
        { "timeEnabled",       [](HighlightRule &r) { r.match.timeEnabled = false; } },
        { "start",             [](HighlightRule &r) { r.match.start = r.match.start.addSecs(60); } },
        { "end",               [](HighlightRule &r) { r.match.end = r.match.end.addSecs(-60); } },
        { "start unset",       [](HighlightRule &r) { r.match.start = QDateTime(); } },
        { "end unset",         [](HighlightRule &r) { r.match.end = QDateTime(); } },
        { "text.enabled",      [](HighlightRule &r) { r.match.text.enabled = false; } },
        { "text.negate",       [](HighlightRule &r) { r.match.text.negate = false; } },
        { "text pattern",      [](HighlightRule &r) { r.match.text.matcher.set(QStringLiteral("bang"), true, Qt::CaseSensitive); } },
        { "text regex flag",   [](HighlightRule &r) { r.match.text.matcher.set(QStringLiteral("boom"), false, Qt::CaseSensitive); } },
        { "text case option",  [](HighlightRule &r) { r.match.text.matcher.set(QStringLiteral("boom"), true, Qt::CaseInsensitive); } },
        // An action ADDED, and every action taken away — the parked rule, one click from
        // unticking Colour, which fromJson already refuses to read as "nothing saved".
        { "actions gained",    [](HighlightRule &r) { r.actions |= HighlightAction::Tab; } },
        { "actions emptied",   [](HighlightRule &r) { r.actions = HighlightActions(); } },
        { "background",        [](HighlightRule &r) { r.background = 4; } },
        { "foreground",        [](HighlightRule &r) { r.foreground = 8; } },
    };

    for (const Case &c : cases) {
        HighlightRule edited = fullyConfiguredRule();
        c.edit(edited);
        QVERIFY2(edited != fullyConfiguredRule(), c.field);
        QVERIFY2(!(edited == fullyConfiguredRule()), c.field);
        // ...and the edit is the ONLY reason: put it back and the two agree again, so a
        // case cannot pass by way of some other field the helper left unstable.
        HighlightRule again = edited;
        QVERIFY2(again == edited, c.field);
    }
}

void TestHighlight::anUnsetTimeBoundEqualsAnotherUnsetOneAndNotAnySetOne()
{
    // A default rule holds two invalid QDateTimes and so does one read back from a
    // stored empty ISO string, so "neither names a bound" has to compare equal or every
    // log with the seeded rules reports itself edited. Spelled out in MatchCriteria
    // rather than left to QDateTime, whose answer for two invalid values is a property
    // of the Qt in front of you and not a promise (the floor is 6.4).
    HighlightRule a;
    HighlightRule b;
    QVERIFY(a == b);
    QVERIFY(!a.match.start.isValid() && !a.match.end.isValid());

    b.match.start = QDateTime(QDate(2026, 7, 21), QTime(10, 0, 0));
    QVERIFY(a != b);
    a.match.start = b.match.start;
    QVERIFY(a == b);
}

void TestHighlight::reorderingARuleListMakesItADifferentList()
{
    // Order is meaning, not presentation: first-match-wins is per action (§7.5), so the
    // seeded rules with FATAL and ERROR swapped paint a FATAL record the ERROR colour.
    // The marker therefore compares the whole list IN ORDER, never a count or a set.
    QVector<HighlightRule> seeded = HighlighterSet::defaults().rules;
    QVERIFY(seeded.size() >= 2);
    QVector<HighlightRule> swapped = seeded;
    swapped.swapItemsAt(0, 1);
    QVERIFY(swapped != seeded);

    swapped.swapItemsAt(0, 1);
    QVERIFY(swapped == seeded);

    // A shorter list is a different list too — the case an "every rule I have matches
    // one of theirs" comparison would miss.
    QVector<HighlightRule> shorter = seeded;
    shorter.removeLast();
    QVERIFY(shorter != seeded);
}

QTEST_APPLESS_MAIN(TestHighlight)
#include "tst_highlight.moc"
