#include <QtTest>

#include <QTemporaryFile>

#include "Document.h"
#include "Filter.h"
#include "FilteredIndex.h"
#include "Priority.h"
#include "Record.h"
#include "RecordIndex.h"

using namespace loftail;

// M4 — filtering core (SPEC.md §6, invariant #4/#6). All of this is UI-free: the
// predicate chain, the visible-subset FilteredIndex geometry, the end-to-end
// Document::applyFilters, and the Find walk. Runs GUILESS (Document maps a temp
// file; no QApplication needed).
class TestFilter : public QObject
{
    Q_OBJECT

private:
    static Record rec(Priority p, quint32 logger, quint32 thread, qint64 ts,
                      quint16 lines = 1)
    {
        Record r{};
        r.priority = quint8(p);
        r.loggerId = logger;
        r.threadId = thread;
        r.timestamp = ts;
        r.lineCount = lines;
        return r;
    }

    static bool writeLog(QTemporaryFile &f, const QByteArray &bytes)
    {
        if (!f.open())
            return false;
        f.write(bytes);
        f.flush();
        return true;
    }

private slots:
    void priorityMinLevel();
    void unknownNeverHiddenByPriority();
    void loggerAndThreadSets();
    void absentLoggerOrThreadNeverHidden();
    void timeRange();
    void textSubstringRegexCaseNegate();
    void integersEvaluatedBeforeText();
    void filteredIndexGeometry();
    void filteredIndexIdentityWhenInactive();
    void documentApplyFiltersEndToEnd();
    void findWalksAndWraps();
    void findChangesNoFilterState();
};

// --- priority ---------------------------------------------------------------

void TestFilter::priorityMinLevel()
{
    FilterSet fs;
    fs.priorityEnabled = true;
    fs.minPriority = Priority::Warn;

    QVERIFY(!fs.acceptsIntegerAxes(rec(Priority::Info, 1, 1, 0)));  // below WARN
    QVERIFY(!fs.acceptsIntegerAxes(rec(Priority::Debug, 1, 1, 0)));
    QVERIFY(fs.acceptsIntegerAxes(rec(Priority::Warn, 1, 1, 0)));   // == WARN
    QVERIFY(fs.acceptsIntegerAxes(rec(Priority::Error, 1, 1, 0)));  // above
    QVERIFY(fs.acceptsIntegerAxes(rec(Priority::Fatal, 1, 1, 0)));
}

void TestFilter::unknownNeverHiddenByPriority()
{
    FilterSet fs;
    fs.priorityEnabled = true;
    fs.minPriority = Priority::Error; // strict
    // Unparsed records carry Priority::Unknown and must stay visible (§7.2).
    QVERIFY(fs.acceptsIntegerAxes(rec(Priority::Unknown, 1, 1, 0)));
}

// --- logger / thread sets ---------------------------------------------------

void TestFilter::loggerAndThreadSets()
{
    FilterSet fs;
    fs.loggerEnabled = true;
    fs.loggerIds = {2, 5};   // OR within the axis
    fs.threadEnabled = true;
    fs.threadIds = {7};

    QVERIFY(fs.acceptsIntegerAxes(rec(Priority::Info, 2, 7, 0)));
    QVERIFY(fs.acceptsIntegerAxes(rec(Priority::Info, 5, 7, 0)));
    QVERIFY(!fs.acceptsIntegerAxes(rec(Priority::Info, 3, 7, 0))); // logger not in set
    QVERIFY(!fs.acceptsIntegerAxes(rec(Priority::Info, 2, 8, 0))); // thread not in set (AND across axes)
}

// --- time range -------------------------------------------------------------

// Id 0 is InternTable's "field absent" sentinel: an unparsed plain-text line, or a
// pattern with no %c/%t. Such a record must survive a filter on that field, exactly
// as Priority::Unknown survives a minimum level — otherwise enabling the subsystem
// axis (on by default, SPEC.md §6) hides every plain-text line, contradicting §4's
// promise that they stay visible.
void TestFilter::absentLoggerOrThreadNeverHidden()
{
    FilterSet fs;
    fs.loggerEnabled = true;
    fs.loggerIds = {2, 5};
    fs.threadEnabled = true;
    fs.threadIds = {7};

    QVERIFY(fs.acceptsIntegerAxes(rec(Priority::Info, 0, 7, 0)));  // no subsystem
    QVERIFY(fs.acceptsIntegerAxes(rec(Priority::Info, 2, 0, 0)));  // no thread
    QVERIFY(fs.acceptsIntegerAxes(rec(Priority::Unknown, 0, 0, 0))); // wholly unparsed

    // An id that IS present and simply unselected is still hidden.
    QVERIFY(!fs.acceptsIntegerAxes(rec(Priority::Info, 3, 7, 0)));
}

void TestFilter::timeRange()
{
    FilterSet fs;
    fs.timeEnabled = true;
    fs.startMs = 1000;
    fs.endMs = 2000;

    QVERIFY(!fs.acceptsIntegerAxes(rec(Priority::Info, 1, 1, 999)));
    QVERIFY(fs.acceptsIntegerAxes(rec(Priority::Info, 1, 1, 1000)));  // inclusive
    QVERIFY(fs.acceptsIntegerAxes(rec(Priority::Info, 1, 1, 1500)));
    QVERIFY(fs.acceptsIntegerAxes(rec(Priority::Info, 1, 1, 2000)));  // inclusive
    QVERIFY(!fs.acceptsIntegerAxes(rec(Priority::Info, 1, 1, 2001)));
    // A record with no timestamp is never hidden by a time bound.
    QVERIFY(fs.acceptsIntegerAxes(rec(Priority::Info, 1, 1, Record::kNoTimestamp)));
}

// --- message text -----------------------------------------------------------

void TestFilter::textSubstringRegexCaseNegate()
{
    // Substring, case-insensitive (default).
    {
        TextMatcher m;
        m.set(QStringLiteral("timeout"), false, Qt::CaseInsensitive);
        QVERIFY(m.matches(QStringLiteral("Connection TIMEOUT after 30s")));
        QVERIFY(!m.matches(QStringLiteral("all good")));
    }
    // Substring, case-sensitive.
    {
        TextMatcher m;
        m.set(QStringLiteral("Timeout"), false, Qt::CaseSensitive);
        QVERIFY(m.matches(QStringLiteral("Timeout!")));
        QVERIFY(!m.matches(QStringLiteral("timeout!")));
    }
    // Regex.
    {
        TextMatcher m;
        m.set(QStringLiteral("err(or)?\\b"), true, Qt::CaseInsensitive);
        QVERIFY(m.isValid());
        QVERIFY(m.matches(QStringLiteral("fatal ERROR here")));
        QVERIFY(m.matches(QStringLiteral("an err today")));
        QVERIFY(!m.matches(QStringLiteral("nothing wrong")));
    }
    // Invalid regex matches nothing but does not throw.
    {
        TextMatcher m;
        m.set(QStringLiteral("("), true, Qt::CaseInsensitive);
        QVERIFY(!m.isValid());
        QVERIFY(!m.matches(QStringLiteral("(")));
    }
    // Negation via the FilterSet: hide matching records.
    {
        FilterSet fs;
        fs.text.enabled = true;
        fs.text.negate = true;
        fs.text.matcher.set(QStringLiteral("noise"), false, Qt::CaseInsensitive);
        QVERIFY(!fs.acceptsText(QStringLiteral("this is noise")));  // matches -> hidden
        QVERIFY(fs.acceptsText(QStringLiteral("signal")));          // no match -> kept
    }
}

// --- integers before text (invariant #4) ------------------------------------

void TestFilter::integersEvaluatedBeforeText()
{
    FilterSet fs;
    fs.priorityEnabled = true;
    fs.minPriority = Priority::Warn;
    fs.text.enabled = true;
    fs.text.matcher.set(QStringLiteral("x"), false, Qt::CaseInsensitive);

    int decodes = 0;
    auto msg = [&decodes] { ++decodes; return QStringLiteral("x"); };

    // Below the priority minimum: rejected by the integer axis, so the message
    // decode is NEVER invoked.
    QVERIFY(!fs.accepts(rec(Priority::Info, 1, 1, 0), msg));
    QCOMPARE(decodes, 0);

    // Passes the integer axis: only now is the message decoded (exactly once).
    QVERIFY(fs.accepts(rec(Priority::Error, 1, 1, 0), msg));
    QCOMPARE(decodes, 1);
}

// --- FilteredIndex geometry -------------------------------------------------

void TestFilter::filteredIndexGeometry()
{
    RecordIndex src;
    const int n = RecordIndex::kBlockSize + 50; // spans two blocks
    for (int i = 0; i < n; ++i) {
        Record r{};
        r.lineCount = quint16((i % 3) + 1); // 1..3 lines
        src.records.append(r);
    }
    src.rebuildBlockSums();

    // Keep every 3rd record.
    QVector<qint32> visible;
    for (int i = 0; i < n; ++i)
        if (i % 3 == 0)
            visible.append(i);

    FilteredIndex fi;
    fi.setSource(&src);
    fi.setVisible(visible);
    QVERIFY(fi.active());
    QCOMPARE(fi.recordCount(), visible.size());

    const RecordIndex &geo = fi.geometry();

    // sourceRow maps each view row back to the source ordinal.
    for (int v = 0; v < visible.size(); ++v)
        QCOMPARE(fi.sourceRow(v), int(visible.at(v)));

    // Total display lines == sum over the visible records only.
    qint64 expectTotal = 0;
    for (qint32 s : visible)
        expectTotal += RecordIndex::displayLines(src.records.at(s));
    QCOMPARE(geo.totalLines(), expectTotal);

    // Line<->record round-trip over the FILTERED set (invariant #6): each view
    // row's first line resolves back to that view row, as does an interior line.
    qint64 acc = 0;
    for (int v = 0; v < visible.size(); ++v) {
        QCOMPARE(geo.firstLineOfRecord(v), acc);
        QCOMPARE(geo.recordAtLine(acc), v);
        const qint64 mid = acc + RecordIndex::displayLines(geo.records.at(v)) - 1;
        QCOMPARE(geo.recordAtLine(mid), v);
        acc += RecordIndex::displayLines(geo.records.at(v));
    }
    QCOMPARE(acc, expectTotal);
}

void TestFilter::filteredIndexIdentityWhenInactive()
{
    RecordIndex src;
    for (int i = 0; i < 10; ++i) {
        Record r{};
        r.lineCount = quint16(i + 1);
        src.records.append(r);
    }
    src.rebuildBlockSums();

    FilteredIndex fi;
    fi.setSource(&src);
    QVERIFY(!fi.active());
    QCOMPARE(fi.recordCount(), 10);
    QCOMPARE(fi.sourceRow(4), 4);              // identity mapping
    QCOMPARE(&fi.geometry(), &src);            // geometry IS the source index
    QCOMPARE(fi.geometry().totalLines(), src.totalLines());

    fi.setVisible({1, 3, 5});
    QVERIFY(fi.active());
    fi.clear();
    QVERIFY(!fi.active());
    QCOMPARE(fi.recordCount(), 10);            // back to identity
}

// --- Document end-to-end ----------------------------------------------------

void TestFilter::documentApplyFiltersEndToEnd()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 10:00:00,000 [main] INFO  net.socket - alpha connected\n"
        "2026-07-21 10:00:01,000 [worker] WARN  db.pool - beta exhausted\n"
        "2026-07-21 10:00:02,000 [main] ERROR net.socket - gamma reset\n"
        "2026-07-21 10:00:03,000 [worker] DEBUG db.pool - delta idle\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.size(), 4);

    // Inactive by default: identity view.
    QVERIFY(!doc.filtered().active());
    doc.applyFilters();
    QVERIFY(!doc.filtered().active());
    QCOMPARE(doc.filtered().recordCount(), 4);

    // Priority >= WARN hides the INFO (0) and DEBUG (3) records.
    doc.filters() = FilterSet{};
    doc.filters().priorityEnabled = true;
    doc.filters().minPriority = Priority::Warn;
    doc.applyFilters();
    QVERIFY(doc.filtered().active());
    QCOMPARE(doc.filtered().recordCount(), 2);
    QCOMPARE(doc.filtered().sourceRow(0), 1); // WARN
    QCOMPARE(doc.filtered().sourceRow(1), 2); // ERROR

    // Subsystem filter: only net.socket (records 0 and 2).
    bool found = false;
    const quint32 netId = doc.index().loggers.idOf(QStringLiteral("net.socket"), &found);
    QVERIFY(found);
    doc.filters() = FilterSet{};
    doc.filters().loggerEnabled = true;
    doc.filters().loggerIds = {netId};
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 2);
    QCOMPARE(doc.filtered().sourceRow(0), 0);
    QCOMPARE(doc.filtered().sourceRow(1), 2);

    // Message-text filter (decode path, invariant #1/#8): substring "beta".
    doc.filters() = FilterSet{};
    doc.filters().text.enabled = true;
    doc.filters().text.matcher.set(QStringLiteral("beta"), false, Qt::CaseInsensitive);
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 1);
    QCOMPARE(doc.filtered().sourceRow(0), 1);

    // Negated text: hide "beta" -> the other three remain.
    doc.filters().text.negate = true;
    doc.applyFilters();
    QCOMPARE(doc.filtered().recordCount(), 3);

    // Disabling everything returns to identity.
    doc.filters() = FilterSet{};
    doc.applyFilters();
    QVERIFY(!doc.filtered().active());
    QCOMPARE(doc.filtered().recordCount(), 4);
}

// --- Find -------------------------------------------------------------------

void TestFilter::findWalksAndWraps()
{
    // Rows 2, 5, 8 match.
    const QVector<bool> hit = {false, false, true, false, false, true, false, false, true, false};
    auto match = [&hit](int r) { return hit.at(r); };
    const int n = hit.size();

    // Forward from the top finds the first match.
    QCOMPARE(Find::search(n, -1, true, true, match), 2);
    // Forward Find Next from a match advances to the next.
    QCOMPARE(Find::search(n, 2, true, true, match), 5);
    QCOMPARE(Find::search(n, 5, true, true, match), 8);
    // Wrap-around: past the last match, forward wraps to the first (SPEC.md §5).
    QCOMPARE(Find::search(n, 8, true, true, match), 2);
    // With no wrap, past the last match returns nothing.
    QCOMPARE(Find::search(n, 8, true, false, match), -1);

    // Backward navigation.
    QCOMPARE(Find::search(n, 8, false, true, match), 5);
    QCOMPARE(Find::search(n, 5, false, true, match), 2);
    QCOMPARE(Find::search(n, 2, false, true, match), 8); // wraps to the last

    // No match anywhere.
    auto none = [](int) { return false; };
    QCOMPARE(Find::search(n, -1, true, true, none), -1);
}

void TestFilter::findChangesNoFilterState()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 10:00:00,000 [main] INFO  net.socket - alpha\n"
        "2026-07-21 10:00:01,000 [main] INFO  net.socket - bravo\n"
        "2026-07-21 10:00:02,000 [main] INFO  net.socket - charlie\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    // A filter is active and must be untouched by a Find walk.
    doc.filters().text.enabled = true;
    doc.filters().text.matcher.set(QStringLiteral("nomatch-keeps-all-hidden-except"), false,
                                   Qt::CaseInsensitive);
    const FilterSet before = doc.filters();

    // Find over the (unfiltered here) records: locate "charlie".
    TextMatcher m;
    m.set(QStringLiteral("charlie"), false, Qt::CaseInsensitive);
    auto match = [&](int r) { return m.matches(doc.messageText(doc.index().records.at(r))); };
    const int hit = Find::search(doc.index().records.size(), -1, true, true, match);
    QCOMPARE(hit, 2);

    // The filter set is byte-for-byte unchanged: Find touched no filter state.
    QCOMPARE(doc.filters().text.enabled, before.text.enabled);
    QCOMPARE(doc.filters().text.matcher.pattern(), before.text.matcher.pattern());
    QCOMPARE(doc.filters().anyActive(), before.anyActive());
}

QTEST_GUILESS_MAIN(TestFilter)
#include "tst_filter.moc"
