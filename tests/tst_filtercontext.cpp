#include <QtTest>

#include <QByteArray>
#include <QFile>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "ContextEmitter.h"
#include "Document.h"
#include "FilteredIndex.h"
#include "LiveController.h"
#include "LogModel.h"
#include "Priority.h"
#include "RecordIndex.h"

using namespace loftail;

// M15 — filter with context (SPEC.md §6): grep's -B/-A over the filtered view, so
// narrowing to the ERRORs keeps what led to them instead of destroying it.
//
// Three layers, all core-only and GUILESS (a temp file + LiveController::checkNow(),
// no event loop and no QApplication):
//
//   (a) the pure emitter (ContextEmitter.h) — the whole rule is one forward pass, so
//       most of the feature is testable as a string-in/string-out function;
//   (b) Document::applyFilters() over real bytes, including the composition with a
//       selected run (context must not reach across a run boundary);
//   (c) the LIVE path, which is where this could actually go wrong. Every case there
//       is a named regression for one way the incremental emitter can disagree with a
//       one-shot scan, and liveConvergesWithAOneShotScan() is the general statement
//       the rest of them localize.
//
// UNGATED, unlike tst_tail: nothing here needs mmap-sees-appends, an unlink or a
// rename, and the emission rule must behave identically on Windows.
class TestFilterContext : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
    static constexpr auto kMarker = "RUN START";

    // --- the pure emitter, driven by masks ----------------------------------
    //
    // A case is three equal-length strings, which makes an expectation readable as a
    // picture of the view: "..X..X.." matches, "MMMMMMMM" bounds, and a result where
    // '.' is hidden, 'M' is a match and 'c' is a context row.
    static QString emitted(const QString &matches, int before, int after,
                           const QString &inBound = QString())
    {
        QString out(matches.size(), QLatin1Char('.'));
        ContextState st;
        emitWithContext(
            0, int(matches.size()) - 1, before, after, st,
            [&](int r) { return inBound.isEmpty() || inBound.at(r) != QLatin1Char('.'); },
            [&](int r) { return matches.at(r) == QLatin1Char('X'); },
            [&](int r, bool isContext) {
                out[r] = isContext ? QLatin1Char('c') : QLatin1Char('M');
            });
        return out;
    }

    // --- log fixtures --------------------------------------------------------

    static QByteArray rec(int sec, const char *prio, const QByteArray &msg)
    {
        QByteArray out = "2026-07-21 00:00:";
        out += QByteArray::number(sec).rightJustified(2, '0');
        out += ",000 [t1] ";
        out += prio;
        out += "  svc - ";
        out += msg;
        out += '\n';
        return out;
    }

    // A continuation line: does not match recordStartRe, so it attaches to the
    // preceding record (invariant #2) and makes that record's TEXT grow — which is
    // the only way a record already in the index can change what a filter thinks of it.
    static QByteArray cont(const QByteArray &text) { return text + "\n"; }

    static bool writeWhole(const QString &path, const QByteArray &bytes)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        f.write(bytes);
        f.close();
        return true;
    }

    static bool append(const QString &path, const QByteArray &bytes)
    {
        QFile f(path);
        if (!f.open(QIODevice::Append))
            return false;
        f.write(bytes);
        f.close();
        return true;
    }

    static bool openDoc(Document &doc, const QString &path)
    {
        return doc.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc());
    }

    // Filter on message text — the axis a growing record can flip, unlike priority,
    // which is fixed by the record's first line the moment it parses.
    static void filterOnText(Document &doc, const char *needle, bool negate = false)
    {
        doc.filters().text.enabled = true;
        doc.filters().text.negate = negate;
        doc.filters().text.matcher.set(QString::fromLatin1(needle), /*regex=*/false,
                                       Qt::CaseInsensitive);
    }

    // The visible subset as the same picture the emitter cases use, over `total`
    // source records: '.' hidden, 'M' match, 'c' context.
    static QString picture(const Document &doc, int total)
    {
        QString out(total, QLatin1Char('.'));
        const FilteredIndex &f = doc.filtered();
        for (int row = 0; row < f.recordCount(); ++row) {
            const int src = f.sourceRow(row);
            if (src >= 0 && src < total)
                out[src] = f.isContext(row) ? QLatin1Char('c') : QLatin1Char('M');
        }
        return out;
    }

private slots:
    // (a) the emitter
    void emitterWindows_data();
    void emitterWindows();
    void emitterSkipsOutOfBoundContext();
    void emitterCountsInBoundRowsNotOrdinals();
    void emitterSuffixInvariantHolds();

    // (b) Document
    void applyFiltersWithContext();
    void contextDoesNotActivateAnIdentityView();
    void contextIsClampedAndPerFile();
    void contextRespectsTheRunBound();
    // The M-plus-one rule: context widens the MESSAGE axis and only it.
    void contextIsInertWithoutAMessageFilter();
    void contextNeighboursPassTheOtherAxes();
    void theTwoHalvesOfTheViewPredicateComposeBack();

    // (c) the live path
    void liveConvergesWithAOneShotScan_data();
    void liveConvergesWithAOneShotScan();
    void liveLeadingContextIsAlwaysATailAppend();
    void liveAfterWindowFillsAcrossTicks();
    void liveProvisionalStartsMatchingPullsItsContext();
    void liveProvisionalFlipDoesNotDropAnEarlierMatch();
    void liveNoContextPopsExactlyOneRow();
    void liveNoFlipDoesNotChurnTheTail();
    void liveFlipReachesBackOverRejectedRecords();
};

// ---------------------------------------------------------------------------
// (a) The emitter
// ---------------------------------------------------------------------------

void TestFilterContext::emitterWindows_data()
{
    QTest::addColumn<QString>("matches");
    QTest::addColumn<int>("before");
    QTest::addColumn<int>("after");
    QTest::addColumn<QString>("expected");

    QTest::newRow("no context is plain filtering")
        << "..X...X..." << 0 << 0 << "..M...M...";
    QTest::newRow("before only")
        << "..X...X..." << 2 << 0 << "ccM.ccM...";
    QTest::newRow("after only")
        << "..X...X..." << 0 << 2 << "..Mcc.Mcc.";
    QTest::newRow("both")
        << "..X...X..." << 1 << 1 << ".cMc.cMc..";
    QTest::newRow("windows overlap without duplicating")
        << "..X..X...." << 3 << 3 << "ccMccMccc.";
    QTest::newRow("a match inside an after-window is still a match")
        << "X.X......." << 0 << 3 << "McMccc....";
    QTest::newRow("leading window clamps at row zero")
        << ".X........" << 5 << 0 << "cM........";
    QTest::newRow("no matches emits nothing, after-window or not")
        << ".........." << 3 << 3 << "..........";
    QTest::newRow("context never reaches past the end")
        << "........X." << 0 << 5 << "........Mc";
    QTest::newRow("everything matches")
        << "XXXX" << 2 << 2 << "MMMM";
}

void TestFilterContext::emitterWindows()
{
    QFETCH(QString, matches);
    QFETCH(int, before);
    QFETCH(int, after);
    QFETCH(QString, expected);

    QCOMPARE(emitted(matches, before, after), expected);
}

void TestFilterContext::emitterSkipsOutOfBoundContext()
{
    // The bound is the selected run (SPEC.md §3a). A match early in a run must not
    // pull neighbours in from the run before it — the whole point of selecting one.
    QCOMPARE(emitted("...X......", 3, 3, "...MMMMMMM"), "...Mccc...");
    // Nor may a trailing window walk out of the run at the other end.
    QCOMPARE(emitted("..X.......", 0, 3, "MMMM......"), "..Mc......");
}

void TestFilterContext::emitterCountsInBoundRowsNotOrdinals()
{
    // The bound is no longer only the run: it also carries every non-text filter axis
    // (SPEC.md §6), so its holes are interior and the window has to STEP OVER them
    // rather than spend itself on them. "-B 2" means the two nearest records the
    // other filters admit, however far back they are.
    QCOMPARE(emitted(".....X....", 2, 0, "M.M.MMMMMM"), "..c.cM....");
    // The same at the trailing end.
    QCOMPARE(emitted("X.........", 0, 2, "M.M.M....."), "M.c.c.....");
    // A window that runs out of in-bound rows simply stops; it does not keep walking
    // to make up the count from rows the other axes rejected.
    QCOMPARE(emitted("........X.", 5, 0, "..M.....MM"), "..c.....M.");
    // Reaching the start of the file is the same non-event it always was.
    QCOMPARE(emitted("..X.......", 4, 0, ".MMMMMMMMM"), ".cM.......");
}

void TestFilterContext::emitterSuffixInvariantHolds()
{
    // The property the live path rests on (ContextEmitter.h): after every step, every
    // in-bound row in [max(0, lastMatch - before), lastEmitted] has been emitted.
    // Consequence — leading context is only ever written ABOVE lastEmitted, i.e. the
    // visible list can be grown by tail-appending and never needs a mid-list insert.
    //
    // Deterministically seeded so a failure is reproducible.
    QRandomGenerator rng(0x10Fda11u);
    constexpr int kRows = 500;

    for (int trial = 0; trial < 200; ++trial) {
        const int before = int(rng.bounded(0, 8));
        const int after = int(rng.bounded(0, 8));
        const int density = int(rng.bounded(2, 20));

        QString matches(kRows, QLatin1Char('.'));
        QString bounds(kRows, QLatin1Char('M'));
        for (int i = 0; i < kRows; ++i) {
            if (rng.bounded(density) == 0)
                matches[i] = QLatin1Char('X');
            if (rng.bounded(30) == 0)
                bounds[i] = QLatin1Char('.'); // a hole, to prove nothing assumes density
        }

        const auto inBound = [&](int r) { return bounds.at(r) != QLatin1Char('.'); };
        const auto isMatch = [&](int r) { return matches.at(r) == QLatin1Char('X'); };

        QSet<int> emitted;
        int lastEmitted = -1;
        QString got(kRows, QLatin1Char('.'));
        ContextState st;
        emitWithContext(0, kRows - 1, before, after, st, inBound, isMatch,
                        [&](int r, bool isContext) {
                            // Ascending, every time. This is the tail-append claim in
                            // its most direct form: nothing is ever written behind
                            // something already written.
                            QVERIFY2(r > lastEmitted, "emissions must be strictly ascending");
                            lastEmitted = r;
                            emitted.insert(r);
                            got[r] = isContext ? QLatin1Char('c') : QLatin1Char('M');
                        });

        // (i) Against the naive definition of grep -C, written the obvious O(n·C)
        // way: a row is shown if it is an in-bound match, or one of the `before`
        // in-bound rows before one, or one of the `after` in-bound rows after one.
        // The counting is over IN-BOUND ROWS, not ordinals — the holes above are
        // records the non-text axes reject, and a window must neither include one
        // nor spend part of itself on one (ContextEmitter.h). The emitter's single
        // pass with its clamps must agree with this exactly, tags included.
        QString want(kRows, QLatin1Char('.'));
        for (int m = 0; m < kRows; ++m) {
            if (!inBound(m) || !isMatch(m))
                continue;
            int n = 0;
            for (int q = m - 1; q >= 0 && n < before; --q) {
                if (!inBound(q))
                    continue;
                ++n;
                if (want.at(q) == QLatin1Char('.'))
                    want[q] = QLatin1Char('c');
            }
            n = 0;
            for (int q = m + 1; q < kRows && n < after; ++q) {
                if (!inBound(q))
                    continue;
                ++n;
                if (want.at(q) == QLatin1Char('.'))
                    want[q] = QLatin1Char('c');
            }
            want[m] = QLatin1Char('M');
        }
        QCOMPARE(got, want);

        // (ii) The suffix invariant itself, restated over the finished output: for
        // every match, its whole leading window is present. That is what lets the
        // live path assume a new match needs nothing below what it has already
        // emitted — and therefore never has to insert into the middle of the list.
        for (int m = 0; m < kRows; ++m) {
            if (got.at(m) != QLatin1Char('M'))
                continue;
            int n = 0;
            for (int q = m - 1; q >= 0 && n < before; --q) {
                if (!inBound(q))
                    continue;
                ++n;
                QVERIFY2(emitted.contains(q),
                         qPrintable(QStringLiteral("row %1 missing from the leading window "
                                                   "of the match at %2 (trial %3, B=%4)")
                                        .arg(q).arg(m).arg(trial).arg(before)));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// (b) Document
// ---------------------------------------------------------------------------

void TestFilterContext::applyFiltersWithContext()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("ctx.log"));

    // 8 records; the two carrying "keep" are the only matches for the message filter.
    QByteArray whole;
    for (int i = 0; i < 8; ++i)
        whole += rec(i, "INFO ", (i == 2 || i == 6 ? "keep " : "plain ") + QByteArray::number(i));
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY2(openDoc(doc, path), qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.size(), 8);

    filterOnText(doc, "keep");

    doc.applyFilters();
    QCOMPARE(picture(doc, 8), QStringLiteral("..M...M."));
    QCOMPARE(doc.filtered().contextCount(), 0);

    doc.setContext(2, 1);
    doc.applyFilters();
    QCOMPARE(picture(doc, 8), QStringLiteral("ccMcccMc"));
    QCOMPARE(doc.filtered().recordCount(), 8);
    QCOMPARE(doc.filtered().contextCount(), 6);

    // The compact geometry covers the context rows too — this is the whole reason the
    // feature is cheap: they are just more ordinals in the same subset (invariant #6).
    QCOMPARE(doc.filtered().geometry().records.size(), 8);
    QCOMPARE(doc.filtered().sourceRow(0), 0);
    QCOMPARE(doc.filtered().sourceRow(7), 7);

    // Turning it back off returns exactly the unwidened subset.
    doc.setContext(0, 0);
    doc.applyFilters();
    QCOMPARE(picture(doc, 8), QStringLiteral("..M...M."));
    QCOMPARE(doc.filtered().contextCount(), 0);
}

void TestFilterContext::contextDoesNotActivateAnIdentityView()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("identity.log"));
    QVERIFY(writeWhole(path, rec(0, "INFO ", "a") + rec(1, "WARN ", "b")));

    Document doc;
    QVERIFY2(openDoc(doc, path), qPrintable(doc.lastError()));

    // Context with nothing filtered out is a no-op by construction: there is nothing
    // to be context TO. The FilteredIndex must stay on its allocation-free identity
    // path rather than materializing a compact copy of the whole file.
    doc.setContext(5, 5);
    doc.applyFilters();
    QVERIFY(!doc.filtered().active());
    QCOMPARE(doc.filtered().recordCount(), 2);
    QCOMPARE(doc.filtered().contextCount(), 0);
    QVERIFY(!doc.filtered().isContext(0));
}

void TestFilterContext::contextIsClampedAndPerFile()
{
    Document a;
    Document b;

    a.setContext(3, 4);
    QCOMPARE(a.contextBefore(), 3);
    QCOMPARE(a.contextAfter(), 4);
    // Per-FILE state (invariant #7): a second document is untouched.
    QCOMPARE(b.contextBefore(), 0);
    QCOMPARE(b.contextAfter(), 0);

    a.setContext(-1, Document::kMaxContext + 1000);
    QCOMPARE(a.contextBefore(), 0);
    QCOMPARE(a.contextAfter(), Document::kMaxContext);
}

void TestFilterContext::contextRespectsTheRunBound()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("ctxruns.log"));

    // Two runs of four records; the ERROR is the SECOND record of the second run, so
    // a -B 5 window would reach back across the boundary if the bound were ignored.
    QByteArray whole;
    int sec = 0;
    whole += rec(sec++, "INFO ", QByteArray(kMarker) + " #0");
    whole += rec(sec++, "INFO ", "a1");
    whole += rec(sec++, "INFO ", "a2");
    whole += rec(sec++, "INFO ", "a3");
    whole += rec(sec++, "INFO ", QByteArray(kMarker) + " #1");
    whole += rec(sec++, "ERROR", "boom");
    whole += rec(sec++, "INFO ", "b2");
    whole += rec(sec++, "INFO ", "b3");
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY2(openDoc(doc, path), qPrintable(doc.lastError()));
    doc.setRunStart(QString::fromLatin1(kMarker), /*regex=*/false, Qt::CaseInsensitive);
    QCOMPARE(doc.runs().size(), 2);
    QCOMPARE(doc.selectedRun(), 1);

    filterOnText(doc, "boom");
    doc.setContext(5, 5);
    doc.applyFilters();

    // Records 0-3 are in the other run and stay hidden however wide the window is.
    QCOMPARE(picture(doc, 8), QStringLiteral("....cMcc"));
}

void TestFilterContext::contextIsInertWithoutAMessageFilter()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("noctx.log"));

    QByteArray whole;
    const char *prios[] = {"INFO ", "INFO ", "WARN ", "INFO ", "DEBUG", "INFO ", "ERROR", "INFO "};
    for (int i = 0; i < 8; ++i)
        whole += rec(i, prios[i], "m" + QByteArray::number(i));
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY2(openDoc(doc, path), qPrintable(doc.lastError()));

    // Context is grep -B/-A over the MESSAGE search (SPEC.md §6). "Show me the two
    // records either side of every WARN" is not that question — the priority axis
    // selects a class of records, not an event to read around — so a metadata-only
    // filter is narrowed exactly as it was before the feature existed, however wide
    // the spinners are set. Nothing gates this: with the text axis off every record
    // the other axes admit is a match, and a match cannot also be context.
    doc.filters().priorityEnabled = true;
    doc.filters().minPriority = Priority::Warn;
    doc.setContext(3, 3);
    doc.applyFilters();

    QCOMPARE(picture(doc, 8), QStringLiteral("..M...M."));
    QCOMPARE(doc.filtered().contextCount(), 0);

    // Switching the message axis on is what wakes it up. Record 2 is now a NEIGHBOUR
    // rather than a match — it is the nearest record the priority floor still admits,
    // three INFOs and a DEBUG having been stepped over to reach it.
    filterOnText(doc, "m6");
    doc.applyFilters();
    QCOMPARE(picture(doc, 8), QStringLiteral("..c...M."));
}

void TestFilterContext::contextNeighboursPassTheOtherAxes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("stream.log"));

    // Alternating WARN and INFO. With the priority floor at WARN the stream being
    // searched is the even records, so "-B 2" from the match at 8 is records 6 and 4
    // — not 7 and 6, which is what counting ordinals would give.
    QByteArray whole;
    for (int i = 0; i < 10; ++i)
        whole += rec(i, (i % 2 == 0) ? "WARN " : "INFO ",
                     (i == 8 ? "keep " : "plain ") + QByteArray::number(i));
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY2(openDoc(doc, path), qPrintable(doc.lastError()));

    doc.filters().priorityEnabled = true;
    doc.filters().minPriority = Priority::Warn;
    filterOnText(doc, "keep");
    doc.setContext(2, 1);
    doc.applyFilters();

    QCOMPARE(picture(doc, 10), QStringLiteral("....c.c.M."));
    QCOMPARE(doc.filtered().contextCount(), 2);

    // Drop the floor and the INFOs rejoin the stream, so the same two-record window
    // now lands on the immediate neighbours instead.
    doc.filters().priorityEnabled = false;
    doc.applyFilters();
    QCOMPARE(picture(doc, 10), QStringLiteral("......ccMc"));
}

void TestFilterContext::theTwoHalvesOfTheViewPredicateComposeBack()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("compose.log"));

    QByteArray whole;
    for (int i = 0; i < 12; ++i)
        whole += rec(i, (i % 3 == 0) ? "ERROR" : "INFO ",
                     (i % 4 == 0 ? "keep " : "plain ") + QByteArray::number(i));
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY2(openDoc(doc, path), qPrintable(doc.lastError()));
    doc.setRunStart(QStringLiteral("00:00:04"), /*regex=*/false, Qt::CaseInsensitive);
    doc.filters().priorityEnabled = true;
    doc.filters().minPriority = Priority::Warn;
    filterOnText(doc, "keep");

    // The emitter's two callables are the view predicate cut a different way, so
    // their conjunction has to be the very same set — otherwise context at 0 would
    // quietly change what a filter shows.
    for (const Record &r : doc.index().records)
        QCOMPARE(doc.inContextStream(r) && doc.matchesTextAxis(r), doc.acceptsInView(r));
}

// ---------------------------------------------------------------------------
// (c) The live path
// ---------------------------------------------------------------------------

void TestFilterContext::liveConvergesWithAOneShotScan_data()
{
    QTest::addColumn<int>("before");
    QTest::addColumn<int>("after");

    QTest::newRow("no context") << 0 << 0;
    QTest::newRow("before only") << 3 << 0;
    QTest::newRow("after only") << 0 << 3;
    QTest::newRow("symmetric") << 2 << 2;
    QTest::newRow("wide") << 5 << 5;
}

void TestFilterContext::liveConvergesWithAOneShotScan()
{
    QFETCH(int, before);
    QFETCH(int, after);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("converge.log"));

    // The bytes, built up front, then dribbled in at RANDOM boundaries — several of
    // which land mid-record, so the trailing provisional record grows, re-splits and
    // (with the text filter below) flips its match status more than once. That is the
    // case the incremental emitter has to get right.
    QByteArray whole;
    for (int i = 0; i < 40; ++i) {
        whole += rec(i, (i % 7 == 3) ? "ERROR" : "INFO ",
                     (i % 5 == 0 ? "keep " : "plain ") + QByteArray::number(i));
        if (i % 6 == 4)
            whole += cont("  continued " + QByteArray::number(i));
    }

    QVERIFY(writeWhole(path, whole.left(20)));

    Document doc;
    QVERIFY2(openDoc(doc, path), qPrintable(doc.lastError()));
    filterOnText(doc, "keep");
    doc.setContext(before, after);
    doc.applyFilters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();

    QRandomGenerator rng(0xc0ffeeu);
    qint64 sent = 20;
    while (sent < whole.size()) {
        const qint64 n = qMin<qint64>(rng.bounded(3, 90), whole.size() - sent);
        QVERIFY(append(path, whole.mid(int(sent), int(n))));
        sent += n;
        live.checkNow();

        // Ascending and in range after EVERY tick, not just at the end.
        const FilteredIndex &f = doc.filtered();
        for (int row = 1; row < f.recordCount(); ++row)
            QVERIFY2(f.sourceRow(row) > f.sourceRow(row - 1), "visible rows must ascend");
    }

    // The reference: the same final bytes, scanned once.
    Document reference;
    QVERIFY2(openDoc(reference, path), qPrintable(reference.lastError()));
    filterOnText(reference, "keep");
    reference.setContext(before, after);
    reference.applyFilters();

    const int total = reference.index().records.size();
    QCOMPARE(doc.index().records.size(), total);
    QCOMPARE(picture(doc, total), picture(reference, total));
    QCOMPARE(doc.filtered().contextCount(), reference.filtered().contextCount());
    QCOMPARE(model.rowCount(), reference.filtered().recordCount());
}

void TestFilterContext::liveLeadingContextIsAlwaysATailAppend()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("tailappend.log"));

    QVERIFY(writeWhole(path, rec(0, "INFO ", "keep zero")));

    Document doc;
    QVERIFY2(openDoc(doc, path), qPrintable(doc.lastError()));
    filterOnText(doc, "keep");
    doc.setContext(3, 0);
    doc.applyFilters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(model.rowCount(), 1);

    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QVERIFY(inserted.isValid());

    // A run of non-matching records, then a match: the match must drag its three
    // predecessors in — and every one of them must arrive as an append at the END of
    // the model, never an insert in the middle (the suffix invariant, ContextEmitter.h).
    for (int i = 1; i <= 9; ++i) {
        const int expectedFirst = model.rowCount();
        QVERIFY(append(path, rec(i, "INFO ", (i == 5 || i == 9 ? "keep " : "plain ")
                                                 + QByteArray::number(i))));
        live.checkNow();
        if (inserted.isEmpty())
            continue;
        const QList<QVariant> args = inserted.takeLast();
        QCOMPARE(args.at(1).toInt(), expectedFirst); // first inserted row == old rowCount
    }

    // 0 M, 2c 3c 4c 5M (the -3 window), then 6c 7c 8c 9M.
    QCOMPARE(picture(doc, 10), QStringLiteral("M.cccMcccM"));
}

void TestFilterContext::liveAfterWindowFillsAcrossTicks()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("afterwindow.log"));

    QVERIFY(writeWhole(path, rec(0, "INFO ", "keep zero")));

    Document doc;
    QVERIFY2(openDoc(doc, path), qPrintable(doc.lastError()));
    filterOnText(doc, "keep");
    doc.setContext(0, 2);
    doc.applyFilters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(picture(doc, 1), QStringLiteral("M"));

    // The -A window is filled by records that do not exist yet: the match arrived on
    // an earlier tick, so "which match am I trailing?" has to be recovered from the
    // visible subset rather than remembered in a member somebody forgets to reset.
    for (int i = 1; i <= 4; ++i) {
        QVERIFY(append(path, rec(i, "INFO ", "plain " + QByteArray::number(i))));
        live.checkNow();
    }

    QCOMPARE(picture(doc, 5), QStringLiteral("Mcc.."));
}

void TestFilterContext::liveProvisionalStartsMatchingPullsItsContext()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("flipon.log"));

    QByteArray whole;
    whole += rec(0, "INFO ", "keep alpha");
    whole += rec(1, "INFO ", "plain one");
    whole += rec(2, "INFO ", "plain two");
    whole += rec(3, "INFO ", "plain three");
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY2(openDoc(doc, path), qPrintable(doc.lastError()));
    filterOnText(doc, "keep");
    doc.setContext(2, 0);
    doc.applyFilters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(picture(doc, 4), QStringLiteral("M..."));

    // The trailing record grows a continuation line that makes it match. It must now
    // appear WITH its two predecessors — appended below the existing row 0, never
    // spliced above it.
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QVERIFY(append(path, cont("    caused by: keep it")));
    live.checkNow();

    QCOMPARE(picture(doc, 4), QStringLiteral("MccM"));
    QCOMPARE(inserted.count(), 1);
    QCOMPARE(inserted.takeFirst().at(1).toInt(), 1); // appended at the tail
}

void TestFilterContext::liveProvisionalFlipDoesNotDropAnEarlierMatch()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("flipoff.log"));

    // A NEGATED text filter: a record matches while it does NOT contain "boom", so a
    // growing record can stop matching — the only direction that orphans context.
    QByteArray whole;
    whole += rec(0, "INFO ", "alpha");     // matches
    whole += rec(1, "INFO ", "boom one");  // does not
    whole += rec(2, "INFO ", "gamma");     // matches (provisional)
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY2(openDoc(doc, path), qPrintable(doc.lastError()));
    filterOnText(doc, "boom", /*negate=*/true);
    doc.setContext(2, 0);
    doc.applyFilters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(picture(doc, 3), QStringLiteral("McM"));

    // Record 2 grows a "boom" continuation and stops matching. Its context row (1)
    // must go with it — and record 0, which the widened pop also removes, MUST come
    // back. Restarting the re-scan at the provisional record instead of at the pop
    // point is what would lose it, permanently and silently.
    QVERIFY(append(path, cont("    boom happened here")));
    live.checkNow();

    QCOMPARE(picture(doc, 3), QStringLiteral("M.."));
    QCOMPARE(model.rowCount(), 1);
}

void TestFilterContext::liveNoContextPopsExactlyOneRow()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nochurn0.log"));

    QVERIFY(writeWhole(path, rec(0, "INFO ", "keep alpha") + rec(1, "INFO ", "keep beta")));

    Document doc;
    QVERIFY2(openDoc(doc, path), qPrintable(doc.lastError()));
    filterOnText(doc, "keep");
    doc.applyFilters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(model.rowCount(), 2);

    // With no context configured the live path must be byte-for-byte what it was
    // before the feature existed: the provisional record grows, its one view row is
    // removed and re-added, and nothing else moves.
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    QVERIFY(append(path, cont("    continued")));
    live.checkNow();

    QCOMPARE(removed.count(), 1);
    const QList<QVariant> args = removed.takeFirst();
    QCOMPARE(args.at(1).toInt(), 1); // first
    QCOMPARE(args.at(2).toInt(), 1); // last — exactly one row
    QCOMPARE(model.rowCount(), 2);
}

void TestFilterContext::liveNoFlipDoesNotChurnTheTail()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nochurn5.log"));

    QByteArray whole;
    for (int i = 0; i < 5; ++i)
        whole += rec(i, "INFO ", "plain " + QByteArray::number(i));
    whole += rec(5, "INFO ", "keep tail");
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY2(openDoc(doc, path), qPrintable(doc.lastError()));
    filterOnText(doc, "keep");
    doc.setContext(5, 0);
    doc.applyFilters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(picture(doc, 6), QStringLiteral("cccccM"));

    // The provisional record grows but stays a match. Popping its whole -B window
    // every tick would be correct and wasteful — six rows removed and re-inserted per
    // append, which a detached reader sees as the view jumping under them. Only a
    // genuine flip earns the wide pop.
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    QVERIFY(append(path, cont("    still keep")));
    live.checkNow();

    QCOMPARE(removed.count(), 1);
    const QList<QVariant> args = removed.takeFirst();
    QCOMPARE(args.at(1).toInt(), args.at(2).toInt()); // exactly one row
    QCOMPARE(picture(doc, 6), QStringLiteral("cccccM"));
}

void TestFilterContext::liveFlipReachesBackOverRejectedRecords()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("flipgap.log"));

    // A priority floor thins the stream context is measured over, so the trailing
    // record's -B window reaches back over records the floor rejects: with WARN set,
    // the two neighbours of record 6 are records 3 and 0, four and six ordinals away.
    QByteArray whole;
    whole += rec(0, "WARN ", "boom a");
    whole += rec(1, "INFO ", "i1");
    whole += rec(2, "INFO ", "i2");
    whole += rec(3, "WARN ", "boom b");
    whole += rec(4, "INFO ", "i4");
    whole += rec(5, "INFO ", "i5");
    whole += rec(6, "WARN ", "gamma");
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY2(openDoc(doc, path), qPrintable(doc.lastError()));
    doc.filters().priorityEnabled = true;
    doc.filters().minPriority = Priority::Warn;
    filterOnText(doc, "boom", /*negate=*/true);
    doc.setContext(2, 0);
    doc.applyFilters();
    QCOMPARE(picture(doc, 7), QStringLiteral("c..c..M"));

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();

    // The provisional record grows a "boom" and stops matching, orphaning both of
    // them. The pop point is the START OF ITS WINDOW, which contextWindowStart()
    // finds by counting in-stream records; `base - before` would put it at record 4,
    // pop only record 6, and leave two dimmed rows behind with nothing to be context
    // to — a view that says "here is the lead-up" to a match that is no longer there.
    QVERIFY(append(path, cont("    boom happened here")));
    live.checkNow();

    QCOMPARE(picture(doc, 7), QStringLiteral("......."));
    QCOMPARE(model.rowCount(), 0);
}

QTEST_GUILESS_MAIN(TestFilterContext)
#include "tst_filtercontext.moc"
