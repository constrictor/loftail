#include <QtTest>

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "Document.h"
#include "FilteredIndex.h"
#include "Highlight.h"
#include "LiveController.h"
#include "LogModel.h"
#include "LogSource.h"
#include "Palette.h"
#include "Priority.h"
#include "RecordIndex.h"

#include <cstring>

using namespace loftail;

// M6 — the tail harness (PLAN.md "Done when"). Drives LiveController against a real
// temp file the test grows/truncates/rotates, calling checkNow() SYNCHRONOUSLY so
// the ingest step is deterministic (no reliance on the watcher or wall-clock poll —
// the watcher/timer never fire because the test never spins the event loop). It
// asserts that:
//   (a) incremental append — including a multi-line record split across two writes
//       so the trailing record is provisional and later grows — converges to exactly
//       the same index as scanning the whole file at once;
//   (b) a truncation is caught and the file cleanly rescanned;
//   (c) a rotation (rename + new file at the path) re-indexes the new file;
//   (d) a REWRITE IN PLACE is caught too — at the same length and at a greater one,
//       neither of which moves the inode or drops the size below what was indexed, so
//       both were read as appends until the content check landed (HeadWitness.h) — and
//       an ordinary append across the witness boundary still is not one;
//   (e) appended records pass through an active filter and highlight rule unchanged;
//   (f) each of (b)-(d) reports the right ReloadCause, which is the notice's whole
//       content (SPEC.md §3). Every case asserts on BOTH signals, and that the nullary
//       `rescanned` counters are untouched M6 code is the point: `reloaded` was added
//       BESIDE that signal, not instead of it, so a change that broke the old lambdas
//       would mean the seam had stopped being inert. (d) is where the classification is
//       actually decided — a rewrite in place sets wasTruncated() while the file grows,
//       so reading the cause off that flag would announce a truncation about a log that
//       got longer.
//
// Linux only for the mmap + rename identity behavior; Windows file-sharing/rotation
// semantics differ and must be exercised on Windows separately (not done here).
class TestTail : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";

    // A single complete record line (record-start line + trailing newline).
    static QByteArray rec(int sec, const char *thread, const char *prio, const char *logger,
                          const QByteArray &msg)
    {
        QByteArray out = "2026-07-21 00:00:";
        out += QByteArray::number(sec).rightJustified(2, '0');
        out += ",000 [";
        out += thread;
        out += "] ";
        out += prio;
        out += "  ";
        out += logger;
        out += " - ";
        out += msg;
        out += '\n';
        return out;
    }

    // A continuation line: does NOT match recordStartRe, so it attaches to the
    // preceding record (invariant #2 multi-line record).
    static QByteArray cont(const QByteArray &text) { return text + "\n"; }

    static bool writeWhole(const QString &path, const QByteArray &bytes)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        f.write(bytes);
        f.flush();
        f.close();
        return true;
    }

    static bool append(const QString &path, const QByteArray &bytes)
    {
        QFile f(path);
        if (!f.open(QIODevice::Append))
            return false;
        f.write(bytes);
        f.flush();
        f.close();
        return true;
    }

    // Compare two record vectors byte-for-byte plus their interned name tables — the
    // convergence assertion: the incrementally-built index must be identical to a
    // one-shot scan of the same final bytes (ids match because both scan the same
    // bytes in the same forward order).
    static bool sameIndex(const RecordIndex &a, const RecordIndex &b, QString *why)
    {
        if (a.records.size() != b.records.size()) {
            if (why)
                *why = QStringLiteral("record count %1 vs %2")
                           .arg(a.records.size()).arg(b.records.size());
            return false;
        }
        for (int i = 0; i < a.records.size(); ++i) {
            if (std::memcmp(&a.records.at(i), &b.records.at(i), sizeof(Record)) != 0) {
                if (why)
                    *why = QStringLiteral("record %1 differs (off %2/%3 len %4/%5 lines %6/%7)")
                               .arg(i)
                               .arg(a.records.at(i).offset).arg(b.records.at(i).offset)
                               .arg(a.records.at(i).length).arg(b.records.at(i).length)
                               .arg(a.records.at(i).lineCount).arg(b.records.at(i).lineCount);
                return false;
            }
        }
        if (a.loggers.names() != b.loggers.names()) {
            if (why)
                *why = QStringLiteral("logger tables differ");
            return false;
        }
        if (a.threads.names() != b.threads.names()) {
            if (why)
                *why = QStringLiteral("thread tables differ");
            return false;
        }
        return true;
    }

private slots:
    void appendConvergesWithSplitMultiline();
    void appendUnterminatedStartLineFlips();
    void truncateTriggersRescan();
    void rotateTriggersReindex();
    void overwriteInPlaceTriggersRescan();
    void sameSizeOverwriteTriggersRescan();
    void aPlainAppendNeverRescans();
    void filteredAndHighlightedAppend();
};

void TestTail::appendConvergesWithSplitMultiline()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("live.log"));

    // Initial content: two complete records.
    QByteArray whole;
    whole += rec(1, "t0", "INFO ", "logger.a", "first");
    whole += rec(2, "t1", "WARN ", "logger.b", "second");
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY2(doc.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start(); // seeds the size baseline; the watcher never fires (no event loop)

    QCOMPARE(model.rowCount(), 2);

    // Chunk 1: a complete record, then a multi-line record left OPEN (its start line
    // + one continuation, no following record yet) — the provisional trailing record.
    QByteArray chunk1;
    chunk1 += rec(3, "t0", "ERROR", "logger.a", "third");
    QByteArray recD = rec(4, "t2", "INFO ", "logger.d", "line0"); // record D start line
    chunk1 += recD;
    chunk1 += cont("    continuation one");
    whole += chunk1;
    QVERIFY(append(path, chunk1));
    live.checkNow();

    // D is present as an open 2-line record: 2 initial + "third" + D = 4 records.
    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(doc.index().records.size(), 4);
    QCOMPARE(doc.index().records.at(3).lineCount, quint16(2));

    // Chunk 2: ANOTHER continuation for D (the provisional record grows), then a new
    // complete record. Exercises the trailing-record-continuation case (invariant #2).
    QByteArray chunk2;
    chunk2 += cont("    continuation two");
    chunk2 += rec(5, "t1", "INFO ", "logger.e", "fifth");
    whole += chunk2;
    QVERIFY(append(path, chunk2));
    live.checkNow();

    QCOMPARE(model.rowCount(), 5);
    QCOMPARE(doc.index().records.size(), 5);
    QCOMPARE(doc.index().records.at(3).lineCount, quint16(3)); // D grew to 3 lines

    // Convergence: the incrementally-built index must equal a one-shot scan.
    Document reference;
    QVERIFY(reference.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    QString why;
    QVERIFY2(sameIndex(doc.index(), reference.index(), &why), qPrintable(why));

    // Prefix sums stayed consistent (extended in place, not rebuilt wrong).
    QCOMPARE(doc.index().totalLines(), reference.index().totalLines());
}

void TestTail::appendUnterminatedStartLineFlips()
{
    // A record-start line split mid-write. Until the delimiter " - " arrives the
    // partial line does NOT match recordStartRe, so — per invariant #2 — it attaches
    // as a CONTINUATION of the preceding record (the provisional trailing record
    // grows). When the delimiter completes the line it matches, and the provisional
    // record must RE-SPLIT in place: the continuation becomes its own parsed record.
    // This exercises the trailing-record-shrinks-and-splits path, the mirror of the
    // grows path — both driven by re-reading only the last confirmed record.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("flip.log"));

    QByteArray whole = rec(1, "t0", "INFO ", "logger.a", "first");
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY(doc.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(doc.index().records.at(0).lineCount, quint16(1));

    // Append a partial start line WITHOUT a newline (delimiter " - " absent yet): it
    // is absorbed as a continuation line of record 0, which stays a single row.
    const QByteArray partial = "2026-07-21 00:00:02,000 [t3] ERROR logger.z";
    whole += partial;
    QVERIFY(append(path, partial));
    live.checkNow();
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(doc.index().records.at(0).lineCount, quint16(2)); // partial attached

    // Complete the line: it now matches the pattern, so the provisional record
    // re-splits into the original INFO plus a new parsed ERROR record.
    const QByteArray rest = " - completed\n";
    whole += rest;
    QVERIFY(append(path, rest));
    live.checkNow();
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(doc.index().records.at(0).lineCount, quint16(1)); // split back out
    QCOMPARE(doc.index().records.at(1).priorityEnum(), Priority::Error);

    Document reference;
    QVERIFY(reference.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    QString why;
    QVERIFY2(sameIndex(doc.index(), reference.index(), &why), qPrintable(why));
}

void TestTail::truncateTriggersRescan()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("trunc.log"));

    QByteArray whole;
    for (int i = 0; i < 6; ++i)
        whole += rec(i, "t0", "INFO ", "logger.a", QByteArray("m") + QByteArray::number(i));
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY(doc.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(model.rowCount(), 6);

    int rescans = 0;
    connect(&live, &LiveController::rescanned, &live, [&] { ++rescans; });
    QVector<ReloadCause> causes;
    connect(&live, &LiveController::reloaded, &live, [&](ReloadCause c) { causes.append(c); });

    // The writer truncates and rewrites two fresh records (copytruncate-style).
    QByteArray fresh;
    fresh += rec(0, "t9", "FATAL", "logger.new", "after truncate a");
    fresh += rec(1, "t9", "FATAL", "logger.new", "after truncate b");
    QVERIFY(writeWhole(path, fresh)); // shrinks the file
    live.checkNow();

    QCOMPARE(rescans, 1);
    // The one honest truncation: this same file got shorter. It is also the ONLY shape
    // that reports itself this way — see the two rewrite cases below.
    QCOMPARE(causes.size(), 1);
    QVERIFY(causes.value(0) == ReloadCause::Truncated);
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(doc.index().records.size(), 2);
    QCOMPARE(doc.index().records.at(0).priorityEnum(), Priority::Fatal);

    Document reference;
    QVERIFY(reference.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    QString why;
    QVERIFY2(sameIndex(doc.index(), reference.index(), &why), qPrintable(why));
}

void TestTail::rotateTriggersReindex()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("rotate.log"));

    QByteArray whole;
    for (int i = 0; i < 4; ++i)
        whole += rec(i, "t0", "INFO ", "logger.a", QByteArray("orig") + QByteArray::number(i));
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY(doc.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(model.rowCount(), 4);

    int rescans = 0;
    connect(&live, &LiveController::rescanned, &live, [&] { ++rescans; });
    QVector<ReloadCause> causes;
    connect(&live, &LiveController::reloaded, &live, [&](ReloadCause c) { causes.append(c); });

    // Rotation: move the current file aside and create a NEW file (new inode) at the
    // path with fresh content — logrotate's default create mode.
    QVERIFY(QFile::rename(path, path + QStringLiteral(".1")));
    QByteArray fresh;
    fresh += rec(0, "t5", "WARN ", "logger.rot", "rotated one");
    fresh += rec(1, "t5", "WARN ", "logger.rot", "rotated two");
    fresh += rec(2, "t5", "WARN ", "logger.rot", "rotated three");
    QVERIFY(writeWhole(path, fresh));
    live.checkNow();

    QCOMPARE(rescans, 1);
    // A different inode is at the path. Note the new file is SHORTER than the one it
    // replaced, which is the ordinary case for a rotation and the reason `replaced` is
    // tested before the shrink: the other order calls almost every rotation a truncation.
    QCOMPARE(causes.size(), 1);
    QVERIFY(causes.value(0) == ReloadCause::Replaced);
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(doc.index().records.size(), 3);

    Document reference;
    QVERIFY(reference.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    QString why;
    QVERIFY2(sameIndex(doc.index(), reference.index(), &why), qPrintable(why));
}

// A rewrite in place that GROWS past the old length. The inode does not move, so
// wasReplaced() is false; the size never dips below what was indexed, so the shrink
// check is false too. Before HeadWitness the tick read it as an append and resumed
// indexing from the old tail offset: the three pre-rewrite records stayed on screen for
// good and the new bytes were parsed from the middle of a record.
void TestTail::overwriteInPlaceTriggersRescan()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("overwrite.log"));

    QByteArray whole;
    for (int i = 0; i < 3; ++i)
        whole += rec(i, "t0", "INFO ", "logger.old", QByteArray("old") + QByteArray::number(i));
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY(doc.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(model.rowCount(), 3);

    int rescans = 0;
    connect(&live, &LiveController::rescanned, &live, [&] { ++rescans; });
    QVector<ReloadCause> causes;
    connect(&live, &LiveController::reloaded, &live, [&](ReloadCause c) { causes.append(c); });

    QByteArray fresh;
    for (int i = 0; i < 6; ++i)
        fresh += rec(i, "t9", "WARN ", "logger.new",
                     QByteArray("rewritten in place ") + QByteArray::number(i));
    QVERIFY(fresh.size() > whole.size()); // the size check alone cannot see this
    QVERIFY(writeWhole(path, fresh));
    live.checkNow();

    QCOMPARE(rescans, 1);
    // Replaced, NOT Truncated, and this is the case that decides it: the file GREW.
    // wasTruncated() is true here — the HeadWitness latches it — so classifying on that
    // flag would announce "was truncated" about a log that got longer. SPEC.md §3 is
    // what settles the wording: rewriting a log in place counts as replacing it.
    QCOMPARE(causes.size(), 1);
    QVERIFY(causes.value(0) == ReloadCause::Replaced);
    QCOMPARE(model.rowCount(), 6);
    QCOMPARE(doc.index().records.at(0).priorityEnum(), Priority::Warn);
    QVERIFY(!doc.index().loggers.names().contains(QStringLiteral("logger.old")));

    Document reference;
    QVERIFY(reference.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    QString why;
    QVERIFY2(sameIndex(doc.index(), reference.index(), &why), qPrintable(why));
}

// The same rewrite at EXACTLY the same length — nothing about the file's metadata moves
// at all, so without the content check the tick sees no growth, no shrink and no new
// inode, and simply does nothing. The view then shows the pre-rewrite log for the rest
// of the session with no way for the user to tell.
void TestTail::sameSizeOverwriteTriggersRescan()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("samesize.log"));

    QByteArray whole;
    for (int i = 0; i < 3; ++i)
        whole += rec(i, "t0", "INFO ", "logger.aaa", "identical length body");
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY(doc.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(model.rowCount(), 3);

    int rescans = 0;
    connect(&live, &LiveController::rescanned, &live, [&] { ++rescans; });
    QVector<ReloadCause> causes;
    connect(&live, &LiveController::reloaded, &live, [&](ReloadCause c) { causes.append(c); });

    QByteArray fresh;
    for (int i = 0; i < 3; ++i)
        fresh += rec(i, "t0", "INFO ", "logger.bbb", "identical length body");
    QCOMPARE(fresh.size(), whole.size());
    QVERIFY(writeWhole(path, fresh));
    live.checkNow();

    QCOMPARE(rescans, 1);
    // The same rewrite at exactly the old length: nothing in the metadata moved at all,
    // so this too is a replacement rather than a truncation.
    QCOMPARE(causes.size(), 1);
    QVERIFY(causes.value(0) == ReloadCause::Replaced);
    QCOMPARE(model.rowCount(), 3);
    QVERIFY(doc.index().loggers.names().contains(QStringLiteral("logger.bbb")));
    QVERIFY(!doc.index().loggers.names().contains(QStringLiteral("logger.aaa")));
}

// The other half of the same contract, and the one a content check is at risk of
// breaking: an ordinary append must go on being an append. Including across the witness
// boundary — the log starts shorter than HeadWitness::kBytes and grows well past it, so
// the witness is extended by the very ticks that must not rescan.
void TestTail::aPlainAppendNeverRescans()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("append.log"));

    // Deliberately a few hundred bytes: short enough that the witness taken at open does
    // not yet cover its full kBytes.
    QByteArray whole;
    for (int i = 0; i < 3; ++i)
        whole += rec(i, "t0", "INFO ", "logger.a", QByteArray("start") + QByteArray::number(i));
    QVERIFY(whole.size() < 1024);
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY(doc.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();

    int rescans = 0;
    connect(&live, &LiveController::rescanned, &live, [&] { ++rescans; });
    QVector<ReloadCause> causes;
    connect(&live, &LiveController::reloaded, &live, [&](ReloadCause c) { causes.append(c); });

    for (int i = 3; i < 40; ++i) {
        QVERIFY(append(path, rec(i % 60, "t0", "INFO ", "logger.a",
                                 QByteArray("grow") + QByteArray::number(i))));
        live.checkNow();
    }

    QCOMPARE(rescans, 0);
    // And nothing is announced either. An append is what a log DOES; a reader told about
    // one would be told about every log they ever open, twice a second.
    QVERIFY(causes.isEmpty());
    QCOMPARE(model.rowCount(), 40);
    QVERIFY(QFileInfo(path).size() > 1024); // the witness was extended past its cap

    Document reference;
    QVERIFY(reference.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    QString why;
    QVERIFY2(sameIndex(doc.index(), reference.index(), &why), qPrintable(why));
}

void TestTail::filteredAndHighlightedAppend()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("filtered.log"));

    // Initial: one INFO (excluded by a WARN+ filter) and one ERROR (included). Both
    // from logger.quiet; logger.hot first appears only in the appended chunk, so the
    // rule is inert until LiveController re-resolves it against the grown intern table.
    QByteArray whole;
    whole += rec(1, "t0", "INFO ", "logger.quiet", "info one");
    whole += rec(2, "t0", "ERROR", "logger.quiet", "error one");
    QVERIFY(writeWhole(path, whole));

    Document doc;
    QVERIFY(doc.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));

    // Filter: minimum priority WARN — hides INFO, keeps WARN/ERROR/FATAL.
    doc.filters().priorityEnabled = true;
    doc.filters().minPriority = Priority::Warn;
    doc.applyFilters();

    // Highlight rule: color records from logger.hot.
    HighlightRule rule;
    rule.match.loggerEnabled = true;
    rule.match.loggerNames = QStringList{QStringLiteral("logger.hot")};
    rule.background = 3;
    doc.highlighters().rules.append(rule);
    doc.resolveHighlighters();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();

    QVERIFY(doc.filtered().active());
    QCOMPARE(model.rowCount(), 1); // only the initial ERROR is visible

    // Append: one INFO (excluded), one WARN from logger.hot (included + highlighted),
    // one ERROR from logger.quiet (included, not highlighted).
    QByteArray chunk;
    chunk += rec(3, "t1", "INFO ", "logger.quiet", "info two");
    chunk += rec(4, "t1", "WARN ", "logger.hot", "warn hot");
    chunk += rec(5, "t1", "ERROR", "logger.quiet", "error two");
    whole += chunk;
    QVERIFY(append(path, chunk));
    live.checkNow();

    // Two of the three appended records pass the filter → 1 + 2 = 3 visible.
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(doc.filtered().recordCount(), 3);
    QCOMPARE(doc.index().records.size(), 5); // all appended into the source index

    // The visible set maps to the right source records (the two excluded INFO stay
    // hidden): source rows 1 (error one), 3 (warn hot), 4 (error two).
    QCOMPARE(doc.filtered().sourceRow(0), 1);
    QCOMPARE(doc.filtered().sourceRow(1), 3);
    QCOMPARE(doc.filtered().sourceRow(2), 4);

    // Highlighting still resolves for an APPENDED record: the WARN from logger.hot
    // (visible row 1) gets the rule's background; the ERROR from logger.quiet does not.
    const QColor hot = model.highlightColor(1, /*background=*/true);
    QVERIFY(hot.isValid());
    QCOMPARE(hot, HighlightPalette::color(3, /*dark=*/false));
    QVERIFY(!model.highlightColor(2, /*background=*/true).isValid());

    // Visible row 0 is the initial logger.quiet ERROR — not highlighted.
    QVERIFY(!model.highlightColor(0, /*background=*/true).isValid());

    // Convergence of the SOURCE index against a one-shot scan (filter is a view).
    Document reference;
    QVERIFY(reference.open(path, QString::fromLatin1(kPattern), Encoding::Utf8, QTimeZone::utc()));
    QString why;
    QVERIFY2(sameIndex(doc.index(), reference.index(), &why), qPrintable(why));
}

QTEST_GUILESS_MAIN(TestTail)
#include "tst_tail.moc"
