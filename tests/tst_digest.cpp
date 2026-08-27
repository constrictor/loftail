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

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>

#include "Document.h"
#include "FilteredIndex.h"
#include "Highlight.h"
#include "LiveController.h"
#include "LogModel.h"
#include "MatchCriteria.h"
#include "Priority.h"
#include "RecordIndex.h"

using namespace loftail;

// M19 — the highlight digest (SPEC.md §7, ARCHITECTURE.md §7.5.2): for every enabled
// rule carrying HighlightAction::Digest, the LAST record that rule matched.
//
// Core-only and GUILESS, the same shape as tst_filtercontext: a temp file plus
// LiveController::checkNow(), no event loop and no QApplication. The digest is a
// per-FILE derivation — it comes from per-file rules over the per-file index inside the
// per-file run bound — so all of it is reachable without a widget, which is exactly why
// the FilteredIndex lives on the Document rather than on the view that draws it.
//
// The live cases are the ones worth having. A one-shot rebuild is a backward scan and
// hard to get wrong; the incremental update has to agree with it while the trailing
// record is still being re-read, which is where the analogous M15 work actually went.
class TestDigest : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
    static constexpr auto kMarker = "RUN START";

    static QByteArray rec(int sec, const char *prio, const QByteArray &msg,
                          const char *logger = "svc")
    {
        QByteArray out = "2026-07-21 00:00:";
        out += QByteArray::number(sec).rightJustified(2, '0');
        out += ",000 [t1] ";
        out += prio;
        out += "  ";
        out += logger;
        out += " - ";
        out += msg;
        out += '\n';
        return out;
    }

    // Does not match recordStartRe, so it attaches to the preceding record
    // (invariant #2) and makes that record's TEXT grow — the only way a record already
    // in the index can change what a rule thinks of it.
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

    // A rule matching message text, carrying whichever actions are asked for.
    static HighlightRule textRule(const char *needle,
                                  HighlightActions actions = HighlightAction::Digest)
    {
        HighlightRule r;
        r.actions = actions;
        r.match.text.enabled = true;
        r.match.text.matcher.set(QString::fromLatin1(needle), /*regex=*/false,
                                 Qt::CaseInsensitive);
        return r;
    }

    static HighlightRule priorityRule(Priority min,
                                      HighlightActions actions = HighlightAction::Digest)
    {
        HighlightRule r;
        r.actions = actions;
        r.match.priorityEnabled = true;
        r.match.minPriority = min;
        return r;
    }

    // The digest as a list of SOURCE ordinals, which is what it fundamentally is.
    //
    // The active() guard is not defensive. An INACTIVE FilteredIndex is the identity
    // view over the whole source index — recordCount() returns every record and
    // sourceRow() is the identity — so "empty digest" is !active(), never
    // recordCount() == 0. Reading it the other way makes an empty strip look like a
    // strip listing the entire file, which is how the first draft of this file passed
    // two cases it should have failed.
    static QVector<int> ordinals(const Document &doc)
    {
        QVector<int> out;
        const FilteredIndex &d = doc.digest();
        if (!d.active())
            return out;
        for (int row = 0; row < d.recordCount(); ++row)
            out.append(d.sourceRow(row));
        return out;
    }

private slots:
    void lastMatchPerRuleOnly();
    void twoRulesSharingARecordYieldOneRow();
    void publishedOrderOwesNothingToRuleOrder();
    void rowsAreInTimestampOrderNotFileOrder();
    void aRecordWithNoTimestampKeepsItsSlot();
    void aRuleWithNoMatchHasNoRow();
    void noRuleOptsInMeansAnEmptyDigest();
    void aColourOnlyRuleIsNotInTheDigest();
    void aDisabledRuleIsNotInTheDigest();
    void digestIgnoresTheFilters();
    void digestNeverLeavesTheSelectedRun();
    void theLookbackFenceStopsTheScan();

    // The live path.
    void appendPromotesANewLastMatch();
    void appendThatMatchesNothingLeavesTheDigestUntouched();
    void aProvisionalThatGrewRefreshesItsDigestRow();
    void aProvisionalThatStoppedMatchingRefindsThePreviousMatch();
    void turningTheLastDigestRuleOffEmptiesTheStrip();
    void aColourOnlySetUpNeverEntersTheLivePath();
};

void TestDigest::lastMatchPerRuleOnly()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full")
                                 + rec(2, "INFO ", "ok")
                                 + rec(3, "ERROR", "disk full again")
                                 + rec(4, "WARN ", "retrying")));
    Document doc;
    QVERIFY(openDoc(doc, path));

    doc.highlighters().rules = {textRule("disk"), textRule("retry")};
    doc.refreshHighlighting();

    // One row per rule, each the NEWEST record that rule matched — not the first, and
    // not every match.
    QCOMPARE(ordinals(doc), QVector<int>({2, 3}));
}

void TestDigest::twoRulesSharingARecordYieldOneRow()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "INFO ", "starting")
                                 + rec(2, "ERROR", "disk full")));
    Document doc;
    QVERIFY(openDoc(doc, path));

    doc.highlighters().rules = {textRule("disk"), priorityRule(Priority::Error)};
    doc.refreshHighlighting();

    // Both rules' newest match is record 1; the digest is a set of ordinals, so it is
    // one row and not two identical ones.
    QCOMPARE(ordinals(doc), QVector<int>({1}));
}

void TestDigest::publishedOrderOwesNothingToRuleOrder()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "INFO ", "alpha")
                                 + rec(2, "INFO ", "beta")
                                 + rec(3, "INFO ", "gamma")));
    Document doc;
    QVERIFY(openDoc(doc, path));

    // Rule order puts the newest match first; the digest is published in the order it
    // is to be READ, which owes nothing to the order the rules were written in. Here
    // the file's timestamps ascend with its records, so reading order is record order —
    // the two come apart in rowsAreInTimestampOrderNotFileOrder() below.
    doc.highlighters().rules = {textRule("gamma"), textRule("beta"), textRule("alpha")};
    doc.refreshHighlighting();

    QCOMPARE(ordinals(doc), QVector<int>({0, 1, 2}));
}

// The digest is a handful of rows from different points in the file read as "what
// happened, and when" — the one place in loftail where records that are nowhere near
// each other are stacked. log4cplus appends in the order threads reach the appender,
// not in the order they stamped their records, so file order is very nearly timestamp
// order and, on a busy log, not quite.
void TestDigest::rowsAreInTimestampOrderNotFileOrder()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    // Written 1, 5, 3 — the third record was stamped before the second reached the file.
    QVERIFY(writeWhole(path, rec(1, "INFO ", "alpha")
                                 + rec(5, "INFO ", "beta")
                                 + rec(3, "INFO ", "gamma")));
    Document doc;
    QVERIFY(openDoc(doc, path));

    doc.highlighters().rules = {textRule("alpha"), textRule("beta"), textRule("gamma")};
    doc.refreshHighlighting();

    // Ordinal 2 (00:00:03) before ordinal 1 (00:00:05): the published list is NOT
    // ascending, which nothing below it requires — FilteredIndex copies the records in
    // the order given, and the digest never uses the incremental append path.
    QCOMPARE(ordinals(doc), QVector<int>({0, 2, 1}));

    const FilteredIndex &d = doc.digest();
    QVector<qint64> stamps;
    for (int row = 0; row < d.recordCount(); ++row)
        stamps.append(doc.index().records.at(d.sourceRow(row)).timestamp);
    QVERIFY(std::is_sorted(stamps.begin(), stamps.end()));
}

// A record with no timestamp cannot be placed in time, so it keeps the slot it has and
// the timestamped rows are ordered around it. Sorting it by Record::kNoTimestamp would
// pin it above everything (that value is qint64's minimum), and handing the comparator
// an "unorderable, false either way" case would not be a strict weak ordering at all —
// which is undefined behaviour in std::stable_sort rather than merely an odd order.
void TestDigest::aRecordWithNoTimestampKeepsItsSlot()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    // A leading line that does not match recordStartRe is its own record with no
    // timestamp (SPEC.md §4 — plain text stays visible), and the two after it are out
    // of timestamp order as above.
    QVERIFY(writeWhole(path, QByteArray("plain preamble, no timestamp here\n")
                                 + rec(5, "INFO ", "beta")
                                 + rec(3, "INFO ", "gamma")));
    Document doc;
    QVERIFY(openDoc(doc, path));
    QCOMPARE(doc.index().records.at(0).timestamp, Record::kNoTimestamp);

    doc.highlighters().rules = {textRule("preamble"), textRule("beta"), textRule("gamma")};
    doc.refreshHighlighting();

    // Slot 0 is still the unplaceable record; slots 1 and 2 hold the other two in
    // timestamp order.
    QCOMPARE(ordinals(doc), QVector<int>({0, 2, 1}));
}

void TestDigest::aRuleWithNoMatchHasNoRow()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "INFO ", "alpha")));
    Document doc;
    QVERIFY(openDoc(doc, path));

    doc.highlighters().rules = {textRule("alpha"), textRule("never-appears")};
    doc.refreshHighlighting();

    // No placeholder row: a rule that has not matched yet says nothing rather than
    // occupying a line to say so.
    QCOMPARE(ordinals(doc), QVector<int>({0}));
}

void TestDigest::noRuleOptsInMeansAnEmptyDigest()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full")));
    Document doc;
    QVERIFY(openDoc(doc, path));

    doc.highlighters().rules = {textRule("disk", HighlightAction::Color)};
    doc.refreshHighlighting();

    // ACTIVE with no rows, never inactive. An inactive FilteredIndex is the IDENTITY
    // view, so recordCount() would report the whole file — and the strip's visibility
    // is driven straight off its model's rowCount(), so an inactive empty digest would
    // put the entire log in the strip under the log.
    QVERIFY(doc.digest().active());
    QCOMPARE(doc.digest().recordCount(), 0);
    QVERIFY(ordinals(doc).isEmpty());
}

void TestDigest::aColourOnlyRuleIsNotInTheDigest()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "INFO ", "alpha") + rec(2, "INFO ", "beta")));
    Document doc;
    QVERIFY(openDoc(doc, path));

    doc.highlighters().rules = {textRule("beta", HighlightAction::Color),
                                textRule("alpha", HighlightAction::Digest)};
    doc.refreshHighlighting();

    // Only the digest rule contributes, and the colour rule does not shadow it despite
    // sitting above it in the list — first-match-wins is per action.
    QCOMPARE(ordinals(doc), QVector<int>({0}));
}

void TestDigest::aDisabledRuleIsNotInTheDigest()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "INFO ", "alpha") + rec(2, "INFO ", "beta")));
    Document doc;
    QVERIFY(openDoc(doc, path));

    HighlightRule off = textRule("beta");
    off.enabled = false;
    doc.highlighters().rules = {textRule("alpha"), off};
    doc.refreshHighlighting();

    QCOMPARE(ordinals(doc), QVector<int>({0}));
}

void TestDigest::digestIgnoresTheFilters()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full") + rec(2, "INFO ", "ok")));
    Document doc;
    QVERIFY(openDoc(doc, path));

    doc.highlighters().rules = {textRule("disk")};
    doc.refreshHighlighting();
    QCOMPARE(ordinals(doc), QVector<int>({0}));

    // Filter the main view to something the digest rule does not match. The strip must
    // keep answering "the newest of each thing I care about" ABOUT THE FILE — that is
    // its whole premise, and coupling it to the filters would empty it exactly when the
    // premise applies.
    doc.filters().text.enabled = true;
    doc.filters().text.matcher.set(QStringLiteral("ok"), /*regex=*/false,
                                   Qt::CaseInsensitive);
    doc.applyFilters();
    doc.rebuildDigest();

    QCOMPARE(doc.filtered().recordCount(), 1); // the main view narrowed
    QCOMPARE(ordinals(doc), QVector<int>({0})); // the digest did not
}

void TestDigest::digestNeverLeavesTheSelectedRun()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full")       // run 1
                                 + rec(2, "INFO ", kMarker)     // run 2 starts
                                 + rec(3, "INFO ", "ok")));
    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.setRunStart(QString::fromLatin1(kMarker), /*regex=*/false, Qt::CaseSensitive);
    doc.detectRuns();
    doc.selectLastRun();

    doc.highlighters().rules = {textRule("disk")};
    doc.refreshHighlighting();

    // The rule's only match is in the PREVIOUS run. Presenting it as "the newest" would
    // be the same error §7.2.1 already ruled out for filter context: the newest of a
    // run is a claim about that run.
    QVERIFY(ordinals(doc).isEmpty());

    doc.selectRun(-1); // all runs
    doc.rebuildDigest();
    QCOMPARE(ordinals(doc), QVector<int>({0}));
}

void TestDigest::theLookbackFenceStopsTheScan()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));

    // One match, then more than the fence of records that do not match it. The scan is
    // a hard cap rather than a disclosure because HighlighterPane::commit() fires on
    // every keystroke in the message field: unbounded, typing a nine-character regex
    // into a digest rule on a large log would be nine full-file decode scans.
    QByteArray bytes = rec(1, "ERROR", "needle");
    bytes.reserve(Document::kDigestLookback * 48);
    for (int i = 0; i < Document::kDigestLookback + 10; ++i)
        bytes += rec(2, "INFO ", "hay");
    QVERIFY(writeWhole(path, bytes));

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.highlighters().rules = {textRule("needle")};

    QElapsedTimer t;
    t.start();
    doc.refreshHighlighting();
    const qint64 ms = t.elapsed();

    // Out of reach: the strip answers "what is the newest of each thing I care about",
    // and a match a hundred thousand records behind the tail is not answering it.
    QVERIFY(ordinals(doc).isEmpty());
    // And bounded independently of file size. Generous, since this runs under ASan too.
    QVERIFY2(ms < 10000, qPrintable(QStringLiteral("fenced scan took %1 ms").arg(ms)));
}

// --- the live path ----------------------------------------------------------

void TestDigest::appendPromotesANewLastMatch()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full") + rec(2, "INFO ", "ok")));

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.highlighters().rules = {textRule("disk")};
    doc.refreshHighlighting();
    QCOMPARE(ordinals(doc), QVector<int>({0}));

    LogModel model(&doc);
    LogModel digestModel(&doc);
    digestModel.setViewIndex(&doc.digest());
    LiveController live(&doc, &model);
    live.setDigestModel(&digestModel);
    live.start();

    QVERIFY(append(path, rec(3, "ERROR", "disk full once more")));
    live.checkNow();

    // The entry moved forward rather than a second row appearing: one row per rule.
    QCOMPARE(ordinals(doc), QVector<int>({2}));
    QCOMPARE(digestModel.rowCount(), 1);
}

void TestDigest::appendThatMatchesNothingLeavesTheDigestUntouched()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full")));

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.highlighters().rules = {textRule("disk")};
    doc.refreshHighlighting();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();

    QVERIFY(append(path, rec(2, "INFO ", "nothing to see")));
    live.checkNow();

    // A quiet tick must not churn the strip: every reset jolts a reader's eye for
    // nothing, which is why the update reports whether anything actually changed.
    QCOMPARE(ordinals(doc), QVector<int>({0}));
}

void TestDigest::aProvisionalThatGrewRefreshesItsDigestRow()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "stack trace follows")));

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.highlighters().rules = {textRule("stack trace")};
    doc.refreshHighlighting();
    QCOMPARE(ordinals(doc), QVector<int>({0}));
    QCOMPARE(doc.digest().geometry().records.at(0).lineCount, 1);

    LogModel model(&doc);
    LogModel digestModel(&doc);
    digestModel.setViewIndex(&doc.digest());
    LiveController live(&doc, &model);
    live.setDigestModel(&digestModel);
    live.start();

    // The trailing record grows continuation lines — the ordinary live case. The
    // ordinal list does not move, but FilteredIndex holds a 32-byte COPY of the Record,
    // so without republishing it the strip renders this row at its old height for the
    // rest of the session.
    QVERIFY(append(path, cont("    at Frame.one()") + cont("    at Frame.two()")));
    live.checkNow();

    QCOMPARE(ordinals(doc), QVector<int>({0}));
    QCOMPARE(doc.index().records.at(0).lineCount, 3);
    QCOMPARE(doc.digest().geometry().records.at(0).lineCount, 3);
}

void TestDigest::aProvisionalThatStoppedMatchingRefindsThePreviousMatch()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    // Two records the rule matches; the second is provisional and will grow text that
    // makes a NEGATED rule stop accepting it.
    QVERIFY(writeWhole(path, rec(1, "INFO ", "alpha") + rec(2, "INFO ", "beta")));

    Document doc;
    QVERIFY(openDoc(doc, path));
    HighlightRule r = textRule("noise");
    r.match.text.negate = true; // matches every record whose text lacks "noise"
    doc.highlighters().rules = {r};
    doc.refreshHighlighting();
    QCOMPARE(ordinals(doc), QVector<int>({1}));

    LogModel model(&doc);
    LogModel digestModel(&doc);
    digestModel.setViewIndex(&doc.digest());
    LiveController live(&doc, &model);
    live.setDigestModel(&digestModel);
    live.start();

    QVERIFY(append(path, cont("noise noise noise")));
    live.checkNow();

    // Record 1 now contains "noise" and no longer matches, so the rule's newest match
    // falls BACK to record 0 rather than being left pointing at a record that no longer
    // qualifies, or dropped entirely.
    QCOMPARE(ordinals(doc), QVector<int>({0}));
}

void TestDigest::turningTheLastDigestRuleOffEmptiesTheStrip()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full")));

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.highlighters().rules = {textRule("disk")};
    doc.refreshHighlighting();
    QVERIFY(doc.digest().active());

    // Colour only, from here on. The strip goes with the rule rather than freezing on
    // its last content — and goes to an ACTIVE empty subset, not an inactive one, which
    // would be the identity view and would put the whole log in the strip.
    doc.highlighters().rules[0].actions = HighlightAction::Color;
    doc.refreshHighlighting();
    QVERIFY(doc.digest().active());
    QCOMPARE(doc.digest().recordCount(), 0);
    QVERIFY(ordinals(doc).isEmpty());
}

void TestDigest::aColourOnlySetUpNeverEntersTheLivePath()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full")));

    Document doc;
    QVERIFY(openDoc(doc, path));
    doc.highlighters().rules = {textRule("disk", HighlightAction::Color)};
    doc.refreshHighlighting();

    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();

    QVERIFY(append(path, rec(2, "ERROR", "disk full again")));
    live.checkNow();

    // The gate: with no rule carrying Digest, Tab or Notify the live pass returns after
    // one walk of the rule list, having touched no record and decoded nothing. What is
    // observable from here is that nothing was produced.
    QCOMPARE(doc.digest().recordCount(), 0);
    const LiveController::BatchAlerts &alerts = live.lastBatchAlerts();
    QCOMPARE(alerts.tabMatches, 0);
    QCOMPARE(alerts.notifyMatches, 0);
}

QTEST_APPLESS_MAIN(TestDigest)
#include "tst_digest.moc"
