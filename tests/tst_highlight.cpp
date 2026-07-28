#include <QtTest>

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimeZone>

#include "Highlight.h"
#include "LogFormat.h"
#include "MatchCriteria.h"
#include "Palette.h"
#include "Priority.h"
#include "Record.h"
#include "RecordIndex.h"

using namespace loftail;

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
};

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
    QCOMPARE(set.match(rec(2, Priority::Error)), 0);
    QCOMPARE(set.match(rec(2, Priority::Info)), -1);
    QCOMPARE(set.match(rec(1, Priority::Error)), -1);
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

    QCOMPARE(set.match(rec(1, Priority::Fatal)), 0); // both match -> the earlier one
    QCOMPARE(set.match(rec(1, Priority::Warn)), 1);  // only rule 1
    QCOMPARE(set.match(rec(1, Priority::Info)), -1); // neither
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

    const int m = set.match(rec(1, Priority::Info));
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

    QCOMPARE(set.match(rec(1, Priority::Warn)), 0);   // >= WARN
    QCOMPARE(set.match(rec(1, Priority::Error)), 0);
    QCOMPARE(set.match(rec(1, Priority::Info)), -1);  // below the minimum
    // Unparsed (Unknown) carries no priority, so a priority rule never colors it.
    QCOMPARE(set.match(rec(1, Priority::Unknown)), -1);
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

    QCOMPARE(set.match(rec(2, Priority::Info)), 0);  // db.pool
    QCOMPARE(set.match(rec(1, Priority::Info)), -1); // net.io — not selected

    // A rule naming a subsystem the file has not produced resolves to no id, so it
    // matches nothing until that subsystem appears (then a re-resolve binds it).
    HighlightRule ghost;
    ghost.match.loggerEnabled = true;
    ghost.match.loggerNames = {QStringLiteral("never.seen")};
    HighlighterSet g;
    g.rules = {ghost};
    resolve(g, idx);
    QCOMPARE(g.match(rec(1, Priority::Info)), -1);
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

    QCOMPARE(set.match(rec(1, Priority::Info, /*threadId=*/2)), 0);
    QCOMPARE(set.match(rec(1, Priority::Info, /*threadId=*/1)), -1);

    // The axis is gated on the format carrying a thread field, exactly as the filter
    // axis is (SPEC.md §6): with no %t there is nothing to match on.
    LogFormat noThread = makeFormat();
    noThread.threadGroup = -1;
    HighlighterSet gated;
    gated.rules = {r};
    gated.resolve(idx, noThread, QTimeZone::utc());
    QCOMPARE(gated.match(rec(1, Priority::Info, /*threadId=*/2)), -1);
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

    QCOMPARE(set.match(rec(1, Priority::Info, 1, /*timestamp=*/10000)), 0); // lower bound
    QCOMPARE(set.match(rec(1, Priority::Info, 1, /*timestamp=*/15000)), 0);
    QCOMPARE(set.match(rec(1, Priority::Info, 1, /*timestamp=*/20000)), 0); // upper bound
    QCOMPARE(set.match(rec(1, Priority::Info, 1, /*timestamp=*/9999)), -1);
    QCOMPARE(set.match(rec(1, Priority::Info, 1, /*timestamp=*/20001)), -1);
}

void TestHighlight::textAxisSubstringRegexCaseNegate()
{
    const RecordIndex idx = makeIndex();
    auto matchOn = [&idx](const HighlightRule &r, const QString &message) {
        HighlighterSet set;
        set.rules = {r};
        set.resolve(idx, makeFormat(), QTimeZone::utc());
        return set.match(rec(1, Priority::Info), [&message] { return message; });
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
        return set.match(rr, [&msg] { return msg; });
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
        QCOMPARE(set.match(plain), -1);
    }

    // The text axis has no "absent" case: an unparsed record's whole line IS its
    // message (Document::messageText), so it is matched like any other.
    HighlightRule byText;
    byText.match.text.enabled = true;
    byText.match.text.matcher.set(QStringLiteral("segfault"), false, Qt::CaseInsensitive);
    HighlighterSet set;
    set.rules = {byText};
    resolve(set, idx);
    QCOMPARE(set.match(plain, [] { return QStringLiteral("*** segfault ***"); }), 0);
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
    QCOMPARE(set.match(rec(1, Priority::Info), message), -1);
    QCOMPARE(decodes, 0);

    // db.pool passes all three subsystem axes; three text axes are consulted, but the
    // decode is memoized across the rule loop.
    decodes = 0;
    QCOMPARE(set.match(rec(2, Priority::Info), message), 2);
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
    QCOMPARE(set.match(rec(1, Priority::Fatal)), -1);
    QVERIFY(!set.anyEnabled());

    // An enabled text axis with an empty pattern is still inert: an empty query would
    // match everything, which is not a rule.
    HighlightRule empty;
    empty.match.text.enabled = true;
    HighlighterSet e;
    e.rules = {empty};
    resolve(e, makeIndex());
    QVERIFY(!e.anyEnabled());
    QCOMPARE(e.match(rec(1, Priority::Fatal), [] { return QStringLiteral("x"); }), -1);
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
    QCOMPARE(set.match(rec(1, Priority::Fatal)), -1);
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
    QCOMPARE(set.match(rec(1, Priority::Fatal)), -1);

    resolve(set, makeIndex());
    QCOMPARE(set.match(rec(1, Priority::Fatal)), 0);
}

void TestHighlight::paletteDualThemeResolves()
{
    QCOMPARE(HighlightPalette::count(), 12);
    for (int i = 0; i < HighlightPalette::count(); ++i) {
        const QColor light = HighlightPalette::color(i, /*dark=*/false);
        const QColor dark = HighlightPalette::color(i, /*dark=*/true);
        QVERIFY(light.isValid());
        QVERIFY(dark.isValid());
        // The two theme variants of a slot differ — that is the point of a
        // dual-theme palette (SPEC.md §7).
        QVERIFY(light != dark);
    }
    // The default sentinel resolves to an invalid color in either theme.
    QVERIFY(!HighlightPalette::color(HighlightPalette::kDefault, false).isValid());
    QVERIFY(!HighlightPalette::color(HighlightPalette::kDefault, true).isValid());
}

QTEST_APPLESS_MAIN(TestHighlight)
#include "tst_highlight.moc"
