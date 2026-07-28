#include <QtTest>

#include <QDateTime>
#include <QJsonObject>
#include <QTimeZone>

#include "Filter.h"
#include "LogFormat.h"
#include "MatchCriteria.h"
#include "Priority.h"
#include "Record.h"
#include "RecordIndex.h"

using namespace loftail;

// MatchCriteria is the portable form of the five match axes, shared by the Filters
// pane and by every highlight rule (SPEC.md §6, §7, ARCHITECTURE.md §7.2). It is the
// hinge that keeps filtering and highlighting matching on the same criteria, so what
// is tested here is precisely the places the two must agree — and the two arguments
// where they deliberately do not.
//
// Core-only and UI-free: no QApplication, no files.
class TestMatchCriteria : public QObject
{
    Q_OBJECT

private:
    static RecordIndex makeIndex();
    static LogFormat   makeFormat();
    static Record      rec(quint32 loggerId, Priority p, quint32 threadId = 1,
                           qint64 timestamp = 1000);

private slots:
    void jsonUsesTheOriginalFilterPaneKeys();
    void jsonRoundTrip();
    void jsonDefaultsMatchAFreshFilterPane();
    void resolveDropsNamesTheFileHasNotProduced();
    void resolveConvertsWallClockInTheDisplayZone();
    void collapseIsPolicyNotBehavior();
    void absentFieldPolicyInverts();
    void formatGatesThreadAndTimeAxes();
};

RecordIndex TestMatchCriteria::makeIndex()
{
    RecordIndex idx;
    idx.loggers.intern(QStringLiteral("net.io"));   // id 1
    idx.loggers.intern(QStringLiteral("db.pool"));  // id 2
    idx.threads.intern(QStringLiteral("main"));     // id 1
    idx.threads.intern(QStringLiteral("worker-2")); // id 2
    return idx;
}

LogFormat TestMatchCriteria::makeFormat()
{
    LogFormat f;
    f.dateGroup = 1;
    f.threadGroup = 2;
    f.msgGroup = 3;
    return f;
}

Record TestMatchCriteria::rec(quint32 loggerId, Priority p, quint32 threadId, qint64 timestamp)
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

void TestMatchCriteria::jsonUsesTheOriginalFilterPaneKeys()
{
    // These key names are FilterPane's originals, and they are load-bearing: filter
    // presets, exported preset files and stored sessions are full of them, and both
    // PresetStore and SessionStore gate on exact schema-version equality with no
    // migration path — so a renamed key would silently discard a user's saved state
    // just as surely as a version bump would.
    const QJsonObject o = MatchCriteria().toJson();
    const QStringList expected = {
        QStringLiteral("priorityEnabled"), QStringLiteral("minPriorityIndex"),
        QStringLiteral("loggerEnabled"),   QStringLiteral("loggerChecked"),
        QStringLiteral("threadEnabled"),   QStringLiteral("threadChecked"),
        QStringLiteral("textEnabled"),     QStringLiteral("text"),
        QStringLiteral("textRegex"),       QStringLiteral("textCase"),
        QStringLiteral("textNegate"),      QStringLiteral("timeEnabled"),
        QStringLiteral("timeStart"),       QStringLiteral("timeEnd"),
    };
    QStringList got = o.keys();
    QStringList want = expected;
    got.sort();
    want.sort();
    QCOMPARE(got, want);
}

void TestMatchCriteria::jsonRoundTrip()
{
    MatchCriteria in;
    in.priorityEnabled = true;
    in.minPriority = Priority::Error;
    in.loggerEnabled = true;
    in.loggerNames = {QStringLiteral("db.pool"), QStringLiteral("net.io")};
    in.threadEnabled = true;
    in.threadNames = {QStringLiteral("worker-2")};
    in.text.enabled = true;
    in.text.negate = true;
    in.text.matcher.set(QStringLiteral("time.*out"), /*regex=*/true, Qt::CaseSensitive);
    in.timeEnabled = true;
    in.start = QDateTime(QDate(2026, 7, 21), QTime(12, 0, 0));
    in.end = QDateTime(QDate(2026, 7, 21), QTime(13, 30, 0));

    const MatchCriteria out = MatchCriteria::fromJson(in.toJson());
    QCOMPARE(out.priorityEnabled, in.priorityEnabled);
    QCOMPARE(int(out.minPriority), int(in.minPriority));
    QCOMPARE(out.loggerEnabled, in.loggerEnabled);
    QCOMPARE(out.loggerNames, in.loggerNames);
    QCOMPARE(out.threadEnabled, in.threadEnabled);
    QCOMPARE(out.threadNames, in.threadNames);
    QCOMPARE(out.text.enabled, in.text.enabled);
    QCOMPARE(out.text.negate, in.text.negate);
    QCOMPARE(out.text.matcher.pattern(), in.text.matcher.pattern());
    QCOMPARE(out.text.matcher.isRegex(), in.text.matcher.isRegex());
    QCOMPARE(int(out.text.matcher.caseSensitivity()), int(in.text.matcher.caseSensitivity()));
    QCOMPARE(out.timeEnabled, in.timeEnabled);
    QCOMPARE(out.start, in.start);
    QCOMPARE(out.end, in.end);
}

void TestMatchCriteria::jsonDefaultsMatchAFreshFilterPane()
{
    // Reading an object with no keys at all must reproduce the Filters pane's opening
    // state (SPEC.md §6): the two metadata axes on, the three that need a typed value
    // off. Any other default would change what a pre-existing session restores to.
    const MatchCriteria c = MatchCriteria::fromJson(QJsonObject());
    QVERIFY(c.priorityEnabled);
    QCOMPARE(int(c.minPriority), int(Priority::Trace));
    QVERIFY(c.loggerEnabled);
    QVERIFY(!c.threadEnabled);
    QVERIFY(!c.timeEnabled);
    QVERIFY(!c.text.enabled);
}

void TestMatchCriteria::resolveDropsNamesTheFileHasNotProduced()
{
    const RecordIndex idx = makeIndex();
    MatchCriteria c;
    c.loggerEnabled = true;
    c.loggerCoversAll = false;
    c.loggerNames = {QStringLiteral("db.pool"), QStringLiteral("never.seen")};

    const FilterSet fs = c.resolve(idx, makeFormat(), QTimeZone::utc(),
                                   AbsentField::Matches, NoOpAxes::Collapse);
    // Only the name the file actually produced has an id; the other simply matches
    // nothing until it appears and a re-resolve binds it.
    QCOMPARE(fs.loggerIds.size(), 1);
    QVERIFY(fs.loggerIds.contains(2));
    QVERIFY(fs.acceptsIntegerAxes(rec(2, Priority::Info)));
    QVERIFY(!fs.acceptsIntegerAxes(rec(1, Priority::Info)));
}

void TestMatchCriteria::resolveConvertsWallClockInTheDisplayZone()
{
    const RecordIndex idx = makeIndex();
    MatchCriteria c;
    c.timeEnabled = true;
    // The same digits the user typed, read in two different display zones. This is
    // invariant #10's single "in" conversion: the wall clock is what is stored, and
    // the instant it denotes is derived — which is why moving the display zone
    // re-points a bound correctly instead of shifting the instant under it.
    c.start = QDateTime(QDate(1970, 1, 1), QTime(1, 0, 0));
    c.end = QDateTime(QDate(1970, 1, 1), QTime(2, 0, 0));

    const FilterSet utc = c.resolve(idx, makeFormat(), QTimeZone::utc(),
                                    AbsentField::Matches, NoOpAxes::Keep);
    QCOMPARE(utc.startMs, qint64(3'600'000));
    QCOMPARE(utc.endMs, qint64(7'200'000));

    const FilterSet plusOne = c.resolve(idx, makeFormat(), QTimeZone::fromSecondsAheadOfUtc(3600),
                                        AbsentField::Matches, NoOpAxes::Keep);
    QCOMPARE(plusOne.startMs, qint64(0));
    QCOMPARE(plusOne.endMs, qint64(3'600'000));
}

void TestMatchCriteria::collapseIsPolicyNotBehavior()
{
    const RecordIndex idx = makeIndex();
    MatchCriteria c;
    c.priorityEnabled = true;
    c.minPriority = Priority::Trace; // the lowest selectable minimum: narrows nothing
    c.loggerEnabled = true;
    c.loggerCoversAll = true;        // every offered subsystem ticked: narrows nothing
    c.loggerNames = {QStringLiteral("net.io"), QStringLiteral("db.pool")};

    // Filtering collapses both, so FilteredIndex stays on its identity path instead of
    // materializing a compact copy of every record (ARCHITECTURE.md §7.2). The
    // collapses are exact, not heuristic — nothing that would have been visible is
    // hidden by them.
    const FilterSet collapsed = c.resolve(idx, makeFormat(), QTimeZone::utc(),
                                          AbsentField::Matches, NoOpAxes::Collapse);
    QVERIFY(!collapsed.priorityEnabled);
    QVERIFY(!collapsed.loggerEnabled);
    QVERIFY(!collapsed.anyActive());

    // Highlighting keeps them: "≥TRACE" and "every subsystem" are a legitimate *color
    // every parsed record* rule, and there is no compact index to protect.
    const FilterSet kept = c.resolve(idx, makeFormat(), QTimeZone::utc(),
                                     AbsentField::DoesNotMatch, NoOpAxes::Keep);
    QVERIFY(kept.priorityEnabled);
    QVERIFY(kept.loggerEnabled);
    QVERIFY(kept.anyActive());
    QVERIFY(kept.acceptsIntegerAxes(rec(1, Priority::Trace)));
}

void TestMatchCriteria::absentFieldPolicyInverts()
{
    const RecordIndex idx = makeIndex();
    // An unparsed plain-text line: no subsystem, no thread, no priority, no timestamp
    // (interned id 0 is the "field absent" sentinel).
    const Record plain = rec(0, Priority::Unknown, 0, Record::kNoTimestamp);

    MatchCriteria c;
    c.loggerEnabled = true;
    c.loggerCoversAll = false;
    c.loggerNames = {QStringLiteral("db.pool")};

    // Filtering: the line is NOT hidden by an axis on a field it never carried
    // (SPEC.md §4 promises plain-text lines stay visible).
    const FilterSet filtering = c.resolve(idx, makeFormat(), QTimeZone::utc(),
                                          AbsentField::Matches, NoOpAxes::Keep);
    QVERIFY(filtering.acceptsIntegerAxes(plain));

    // Highlighting: the same line is NOT colored by that axis (SPEC.md §7), or a
    // subsystem rule would paint every plain-text line in the file.
    const FilterSet highlighting = c.resolve(idx, makeFormat(), QTimeZone::utc(),
                                             AbsentField::DoesNotMatch, NoOpAxes::Keep);
    QVERIFY(!highlighting.acceptsIntegerAxes(plain));

    // A record that DOES carry the field is judged identically either way.
    QVERIFY(filtering.acceptsIntegerAxes(rec(2, Priority::Info)));
    QVERIFY(highlighting.acceptsIntegerAxes(rec(2, Priority::Info)));
    QVERIFY(!filtering.acceptsIntegerAxes(rec(1, Priority::Info)));
    QVERIFY(!highlighting.acceptsIntegerAxes(rec(1, Priority::Info)));
}

void TestMatchCriteria::formatGatesThreadAndTimeAxes()
{
    const RecordIndex idx = makeIndex();
    MatchCriteria c;
    c.threadEnabled = true;
    c.threadCoversAll = false;
    c.threadNames = {QStringLiteral("main")};
    c.timeEnabled = true;
    c.start = QDateTime::fromMSecsSinceEpoch(0, QTimeZone::utc());
    c.end = QDateTime::fromMSecsSinceEpoch(1, QTimeZone::utc());

    // A pattern with no %t and no %d carries neither field, so both axes go inactive
    // rather than rejecting every record (SPEC.md §6).
    LogFormat bare;
    const FilterSet fs = c.resolve(idx, bare, QTimeZone::utc(),
                                   AbsentField::DoesNotMatch, NoOpAxes::Keep);
    QVERIFY(!fs.threadEnabled);
    QVERIFY(!fs.timeEnabled);
    QVERIFY(!fs.anyActive());
}

QTEST_APPLESS_MAIN(TestMatchCriteria)
#include "tst_matchcriteria.moc"
