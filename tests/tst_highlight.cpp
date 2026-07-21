#include <QtTest>

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>

#include "Highlight.h"
#include "Palette.h"
#include "Priority.h"
#include "Record.h"
#include "RecordIndex.h"

using namespace loftail;

// M5 — the highlight core (SPEC.md §7, ARCHITECTURE.md §8, invariant #4). Pure,
// UI-free: rules persist palette INDICES (not RGB), match on integer axes
// first-match-wins, and supply a background and a foreground role with a *default*
// fallback. QColor is a value type, so no QApplication is needed.
class TestHighlight : public QObject
{
    Q_OBJECT

private:
    // Build a small index: three subsystems and a handful of records so match() has
    // real interned ids and priorities to compare (invariant #4).
    static RecordIndex makeIndex();
    static Record rec(quint32 loggerId, Priority p);

private slots:
    void ruleJsonRoundTripUsesIndicesNotRgb();
    void setJsonRoundTrip();
    void firstMatchWins();
    void defaultRoleFallsBackToTheme();
    void priorityIsMinLevelAndExemptsUnknown();
    void loggerAxisMatchesInternedIds();
    void unconfiguredRuleIsInert();
    void disabledRuleIsSkipped();
    void paletteDualThemeResolves();
};

Record TestHighlight::rec(quint32 loggerId, Priority p)
{
    Record r{};
    r.offset = 0;
    r.timestamp = Record::kNoTimestamp;
    r.length = 1;
    r.loggerId = loggerId;
    r.threadId = 0;
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
    return idx;
}

void TestHighlight::ruleJsonRoundTripUsesIndicesNotRgb()
{
    HighlightRule in;
    in.enabled = true;
    in.matchLogger = true;
    in.loggerNames = {QStringLiteral("net.io"), QStringLiteral("db.pool")};
    in.matchPriority = true;
    in.minPriority = Priority::Error;
    in.background = 0;                            // a palette slot (Red)
    in.foreground = HighlightPalette::kDefault;   // theme default

    const QJsonObject o = in.toJson();
    // The persisted roles are integer palette indices — never RGB values (§8).
    QVERIFY(o.value(QStringLiteral("background")).isDouble());
    QCOMPARE(o.value(QStringLiteral("background")).toInt(), 0);
    QCOMPARE(o.value(QStringLiteral("foreground")).toInt(), HighlightPalette::kDefault);
    // Subsystems persist as NAMES, portable across a re-index (no interned ids).
    QCOMPARE(o.value(QStringLiteral("loggerNames")).toArray().size(), 2);
    QCOMPARE(o.value(QStringLiteral("minPriority")).toString(), QStringLiteral("ERROR"));

    const HighlightRule out = HighlightRule::fromJson(o);
    QCOMPARE(out.enabled, in.enabled);
    QCOMPARE(out.matchLogger, in.matchLogger);
    QCOMPARE(out.loggerNames, in.loggerNames);
    QCOMPARE(out.matchPriority, in.matchPriority);
    QCOMPARE(int(out.minPriority), int(in.minPriority));
    QCOMPARE(out.background, in.background);
    QCOMPARE(out.foreground, in.foreground);
}

void TestHighlight::setJsonRoundTrip()
{
    HighlighterSet in;
    HighlightRule a;
    a.matchPriority = true;
    a.minPriority = Priority::Fatal;
    a.background = 0;
    HighlightRule b;
    b.matchLogger = true;
    b.loggerNames = {QStringLiteral("db.pool")};
    b.foreground = 3;
    in.rules = {a, b};

    const HighlighterSet out = HighlighterSet::fromJson(in.toJson());
    QCOMPARE(out.rules.size(), 2);
    QCOMPARE(int(out.rules.at(0).minPriority), int(Priority::Fatal));
    QCOMPARE(out.rules.at(0).background, 0);
    QCOMPARE(out.rules.at(1).loggerNames, QStringList{QStringLiteral("db.pool")});
    QCOMPARE(out.rules.at(1).foreground, 3);
}

void TestHighlight::firstMatchWins()
{
    const RecordIndex idx = makeIndex();

    HighlighterSet set;
    // Rule 0 matches FATAL and up; rule 1 matches WARN and up. A FATAL record
    // matches BOTH, so first-match-wins must pick rule 0 (SPEC.md §7).
    HighlightRule r0;
    r0.matchPriority = true;
    r0.minPriority = Priority::Fatal;
    r0.background = 0; // Red
    HighlightRule r1;
    r1.matchPriority = true;
    r1.minPriority = Priority::Warn;
    r1.background = 2; // Amber
    set.rules = {r0, r1};
    set.resolve(idx);

    QCOMPARE(set.match(rec(1, Priority::Fatal)), 0); // both match -> the earlier one
    QCOMPARE(set.match(rec(1, Priority::Warn)), 1);  // only rule 1
    QCOMPARE(set.match(rec(1, Priority::Info)), -1); // neither
}

void TestHighlight::defaultRoleFallsBackToTheme()
{
    HighlightRule r;
    r.matchPriority = true;
    r.minPriority = Priority::Info;
    r.background = 5;                           // Blue
    r.foreground = HighlightPalette::kDefault;  // leave text at the theme color

    HighlighterSet set;
    set.rules = {r};
    const RecordIndex idx = makeIndex();
    set.resolve(idx);

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
    r.matchPriority = true;
    r.minPriority = Priority::Warn;
    HighlighterSet set;
    set.rules = {r};
    set.resolve(makeIndex());

    QCOMPARE(set.match(rec(1, Priority::Warn)), 0);   // >= WARN
    QCOMPARE(set.match(rec(1, Priority::Error)), 0);
    QCOMPARE(set.match(rec(1, Priority::Info)), -1);  // below the minimum
    // Unparsed (Unknown) is exempt, exactly like the priority filter (§7.2).
    QCOMPARE(set.match(rec(1, Priority::Unknown)), -1);
}

void TestHighlight::loggerAxisMatchesInternedIds()
{
    const RecordIndex idx = makeIndex();
    HighlightRule r;
    r.matchLogger = true;
    r.loggerNames = {QStringLiteral("db.pool")}; // id 2
    HighlighterSet set;
    set.rules = {r};
    set.resolve(idx);

    QCOMPARE(set.match(rec(2, Priority::Info)), 0);  // db.pool
    QCOMPARE(set.match(rec(1, Priority::Info)), -1); // net.io — not selected

    // A rule naming a subsystem the file has not produced resolves to no id, so it
    // matches nothing until that subsystem appears (then a re-resolve binds it).
    HighlightRule ghost;
    ghost.matchLogger = true;
    ghost.loggerNames = {QStringLiteral("never.seen")};
    HighlighterSet g;
    g.rules = {ghost};
    g.resolve(idx);
    QCOMPARE(g.match(rec(1, Priority::Info)), -1);
}

void TestHighlight::unconfiguredRuleIsInert()
{
    // A rule with no active axis must never match — a freshly-added rule is inert
    // until the user configures an axis.
    HighlightRule r;
    r.background = 0;
    HighlighterSet set;
    set.rules = {r};
    set.resolve(makeIndex());
    QCOMPARE(set.match(rec(1, Priority::Fatal)), -1);
    QVERIFY(!set.anyEnabled());
}

void TestHighlight::disabledRuleIsSkipped()
{
    HighlightRule r;
    r.enabled = false;
    r.matchPriority = true;
    r.minPriority = Priority::Trace; // would match everything if enabled
    HighlighterSet set;
    set.rules = {r};
    set.resolve(makeIndex());
    QCOMPARE(set.match(rec(1, Priority::Fatal)), -1);
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
