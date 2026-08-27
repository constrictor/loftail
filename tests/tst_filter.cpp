// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

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
    void theMatcherSaysWhereItMatchedAndNotOnlyThatItDid();
    void integersEvaluatedBeforeText();
    void filteredIndexGeometry();
    void filteredIndexIdentityWhenInactive();
    void viewRowOfInvertsSourceRow();
    void viewRowAtOrAfterFindsTheNearestSurvivor();
    void theReverseMapIsTheIdentityWhenNothingIsFiltered();
    void aSubsetPublishedOutOfOrderStillAnswersExactly();
    void documentApplyFiltersEndToEnd();
    void findWalksAndWraps();
    void theMatchTallyCountsWithinItsBoundAndSaysWhenItStoppedShort();
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

// --- where it matched, not only that it did (SPEC.md §5) --------------------
//
// The Find bar marks the matched run on screen, and it must mark exactly what the
// search decided (ARCHITECTURE.md §7.1.4) — so the positions come off the same
// TextMatcher rather than out of a second implementation beside it.

void TestFilter::theMatcherSaysWhereItMatchedAndNotOnlyThatItDid()
{
    // Every occurrence, in order, and the same case rule matches() applies.
    {
        TextMatcher m;
        m.set(QStringLiteral("needle"), false, Qt::CaseInsensitive);
        const QVector<TextMatcher::Span> spans =
            m.spans(QStringLiteral("a NEEDLE and a needle"));
        QCOMPARE(spans.size(), 2);
        QCOMPARE(spans.at(0).start, 2);
        QCOMPARE(spans.at(0).length, 6);
        QCOMPARE(spans.at(1).start, 15);
        QCOMPARE(spans.at(1).length, 6);
    }
    {
        TextMatcher m;
        m.set(QStringLiteral("needle"), false, Qt::CaseSensitive);
        QCOMPARE(m.spans(QStringLiteral("a NEEDLE and a needle")).size(), 1);
    }
    // Overlapping occurrences are not double-counted: a mark is a run of glyphs, and
    // two spans over the same glyphs would paint one another out.
    {
        TextMatcher m;
        m.set(QStringLiteral("aa"), false, Qt::CaseSensitive);
        const QVector<TextMatcher::Span> spans = m.spans(QStringLiteral("aaaa"));
        QCOMPARE(spans.size(), 2);
        QCOMPARE(spans.at(0).start, 0);
        QCOMPARE(spans.at(1).start, 2);
    }
    // A regex reports what it actually captured, not the query's length.
    {
        TextMatcher m;
        m.set(QStringLiteral("err(or)?"), true, Qt::CaseInsensitive);
        const QVector<TextMatcher::Span> spans = m.spans(QStringLiteral("an err and an ERROR"));
        QCOMPARE(spans.size(), 2);
        QCOMPARE(spans.at(0).length, 3);
        QCOMPARE(spans.at(1).length, 5);
    }
    // A pattern that can match nothing at all marks nothing: there are no glyphs under
    // a zero-width match, and a span per character position would fill the line.
    {
        TextMatcher m;
        m.set(QStringLiteral("x*"), true, Qt::CaseSensitive);
        QVERIFY(m.matches(QStringLiteral("abc")));
        QVERIFY(m.spans(QStringLiteral("abc")).isEmpty());
        QCOMPARE(m.spans(QStringLiteral("axxb")).size(), 1);
    }
    // The bound is the caller's, because the caller is a paint path.
    {
        TextMatcher m;
        m.set(QStringLiteral("a"), false, Qt::CaseSensitive);
        QCOMPARE(m.spans(QStringLiteral("aaaaaaaaaa"), 4).size(), 4);
        QCOMPARE(m.spans(QStringLiteral("aaaaaaaaaa"), -1).size(), 10);
    }
    // An empty query matches everything and marks nothing; a broken regex matches
    // nothing and marks nothing — both exactly as matches() answers for them.
    {
        TextMatcher m;
        QVERIFY(m.matches(QStringLiteral("anything")));
        QVERIFY(m.spans(QStringLiteral("anything")).isEmpty());
        m.set(QStringLiteral("("), true, Qt::CaseSensitive);
        QVERIFY(m.spans(QStringLiteral("(")).isEmpty());
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

// --- the reverse map (the filter anchor, SPEC.md §6) -------------------------
//
// A source ordinal is the one coordinate a filter change does not move, so a view
// anchors its selection and its scroll position to one across a re-apply. That needs
// sourceRow()'s inverse, which is what these four pin.

namespace {

RecordIndex plainIndex(int n)
{
    RecordIndex src;
    for (int i = 0; i < n; ++i) {
        Record r{};
        r.lineCount = quint16((i % 3) + 1);
        src.records.append(r);
    }
    src.rebuildBlockSums();
    return src;
}

} // namespace

void TestFilter::viewRowOfInvertsSourceRow()
{
    const RecordIndex src = plainIndex(RecordIndex::kBlockSize + 50); // spans two blocks
    QVector<qint32> visible;
    for (int i = 0; i < src.records.size(); ++i)
        if (i % 3 == 0)
            visible.append(i);

    FilteredIndex fi;
    fi.setSource(&src);
    fi.setVisible(visible);
    QVERIFY(fi.isAscending());

    for (int v = 0; v < fi.recordCount(); ++v)
        QCOMPARE(fi.viewRowOf(fi.sourceRow(v)), v);

    // Every hidden ordinal answers "not visible" rather than a neighbour's row.
    for (int s = 0; s < src.records.size(); ++s)
        if (s % 3 != 0)
            QCOMPARE(fi.viewRowOf(s), -1);

    QCOMPARE(fi.viewRowOf(-1), -1);
    QCOMPARE(fi.viewRowOf(src.records.size() + 100), -1);
}

void TestFilter::viewRowAtOrAfterFindsTheNearestSurvivor()
{
    const RecordIndex src = plainIndex(20);
    FilteredIndex fi;
    fi.setSource(&src);
    fi.setVisible({3, 8, 9, 14});

    QCOMPARE(fi.viewRowAtOrAfter(3), 0);   // exactly on a survivor
    QCOMPARE(fi.viewRowAtOrAfter(0), 0);   // before every survivor
    QCOMPARE(fi.viewRowAtOrAfter(4), 1);   // hidden -> the next one down the file
    QCOMPARE(fi.viewRowAtOrAfter(9), 2);
    QCOMPARE(fi.viewRowAtOrAfter(10), 3);
    // Past the last survivor: recordCount(), which the caller reads as "the whole
    // visible set is above where you were" and answers with the end of the file.
    QCOMPARE(fi.viewRowAtOrAfter(15), fi.recordCount());
}

void TestFilter::theReverseMapIsTheIdentityWhenNothingIsFiltered()
{
    const RecordIndex src = plainIndex(10);
    FilteredIndex fi;
    fi.setSource(&src);
    QVERIFY(!fi.active());

    QCOMPARE(fi.viewRowOf(4), 4);
    QCOMPARE(fi.viewRowOf(-1), -1);
    QCOMPARE(fi.viewRowOf(10), -1);
    QCOMPARE(fi.viewRowAtOrAfter(4), 4);
    QCOMPARE(fi.viewRowAtOrAfter(-3), 0);
    QCOMPARE(fi.viewRowAtOrAfter(99), 10); // clamped to recordCount()
}

void TestFilter::aSubsetPublishedOutOfOrderStillAnswersExactly()
{
    // The digest's shape: Document::publishDigest() reorders its ordinals by timestamp,
    // so this subset is NOT ascending — and a bare binary search over it would answer
    // confidently and wrongly. viewRowOf() must still be exact.
    const RecordIndex src = plainIndex(12);
    FilteredIndex fi;
    fi.setSource(&src);
    fi.setVisible({5, 2, 9});

    QVERIFY(!fi.isAscending());
    QCOMPARE(fi.viewRowOf(5), 0);
    QCOMPARE(fi.viewRowOf(2), 1);
    QCOMPARE(fi.viewRowOf(9), 2);
    QCOMPARE(fi.viewRowOf(7), -1);

    // And it goes back to ascending when an ordinary filter subset is published over it.
    fi.setVisible({1, 4, 6});
    QVERIFY(fi.isAscending());
    QCOMPARE(fi.viewRowAtOrAfter(5), 2);
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

void TestFilter::theMatchTallyCountsWithinItsBoundAndSaysWhenItStoppedShort()
{
    // Rows 2, 5, 8 match, as above.
    const QVector<bool> hit = {false, false, true, false, false, true, false, false, true, false};
    int asked = 0;
    auto match = [&hit, &asked](int r) { ++asked; return hit.at(r); };
    const int n = hit.size();

    // Unbounded: the whole view is counted, so the total is a fact and the position of
    // each match is its ordinal among them (SPEC.md §5).
    Find::Tally t = Find::tally(n, 5, 0, 0, match);
    QCOMPARE(t.total, 3);
    QCOMPARE(t.index, 2); // row 5 is the second match
    QVERIFY(t.complete);
    QCOMPARE(asked, n); // every row, exactly once

    QCOMPARE(Find::tally(n, 2, 0, 0, match).index, 1);
    QCOMPARE(Find::tally(n, 8, 0, 0, match).index, 3);

    // Bounded by rows, with the match inside the counted part: the position is still
    // exact and the total is a floor.
    t = Find::tally(n, 2, 6, 0, match);
    QCOMPARE(t.total, 2); // rows 2 and 5
    QCOMPARE(t.index, 1);
    QVERIFY(!t.complete);

    // Bounded by rows, with the match BEYOND them: there is no position to report.
    t = Find::tally(n, 8, 6, 0, match);
    QCOMPARE(t.total, 2);
    QCOMPARE(t.index, 0);
    QVERIFY(!t.complete);

    // A bound at or past the end is no bound at all.
    QVERIFY(Find::tally(n, 8, n, 0, match).complete);
    QVERIFY(Find::tally(n, 8, n + 100, 0, match).complete);

    // A row limit never makes the scan read past the view, and an empty view is answered
    // without asking anything.
    asked = 0;
    t = Find::tally(0, -1, 0, 0, match);
    QCOMPARE(asked, 0);
    QCOMPARE(t.total, 0);
    QVERIFY(!t.complete);

    // The time bound stops a scan the row bound would have let run — which is the bound
    // that actually holds when a record is expensive to decode rather than merely
    // numerous. The clock is read every 256th row, so the view has to be longer than
    // that for it to be read at all.
    const int wide = 2000;
    auto slow = [](int r) { QTest::qSleep(1); return r % 3 == 0; };
    t = Find::tally(wide, wide - 1, 0, 1, slow);
    QVERIFY(!t.complete);       // a millisecond does not buy two thousand slow rows
    QVERIFY(t.total > 0);       // but what it did count, it counted
    QVERIFY(t.total < wide / 3);
    QCOMPARE(t.index, 0);       // the last row was never reached, so it has no position
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
