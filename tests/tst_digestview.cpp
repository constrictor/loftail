#include <QtTest>

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QHeaderView>
#include <QScrollBar>
#include <QTemporaryDir>

#include "Document.h"
#include "DocumentContext.h"
#include "DocumentView.h"
#include "Highlight.h"
#include "LogModel.h"
#include "LogView.h"
#include "MatchCriteria.h"
#include "Palette.h"
#include "Priority.h"

using namespace loftail;

// M19 — the digest STRIP (SPEC.md §7, ARCHITECTURE.md §7.5.1/§7.5.2).
//
// The content is tst_digest's business; this is about the widget's promises, which are
// the ones a user actually sees: it is exactly as tall as its rows and is not scrolled,
// it is not there at all when there is nothing to show, it lines up with the table
// above it, and a row wears the colours of the rule that put it there whether or not
// that rule also colours the log.
//
// It drives a DocumentView over a hand-built DocumentContext rather than a MainWindow,
// which is the smallest thing that has both LogViews in the same layout.
class TestDigestView : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";

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

    static bool writeWhole(const QString &path, const QByteArray &bytes)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        f.write(bytes);
        f.close();
        return true;
    }

    static HighlightRule textRule(const char *needle, HighlightActions actions,
                                  int background = HighlightPalette::kDefault,
                                  int foreground = HighlightPalette::kDefault)
    {
        HighlightRule r;
        r.actions = actions;
        r.background = background;
        r.foreground = foreground;
        r.match.text.enabled = true;
        r.match.text.matcher.set(QString::fromLatin1(needle), /*regex=*/false,
                                 Qt::CaseInsensitive);
        return r;
    }

    // The smallest context a DocumentView will accept: a Document plus the two models.
    // No controllers — nothing here needs indexing to be asynchronous.
    static void buildContext(DocumentContext &ctx, const QString &path)
    {
        ctx.doc = std::make_unique<Document>();
        QVERIFY(ctx.doc->open(path, QString::fromLatin1(kPattern), Encoding::Utf8,
                              QTimeZone::utc()));
        ctx.model = new LogModel(ctx.doc.get());
        ctx.digestModel = new LogModel(ctx.doc.get());
        ctx.digestModel->setViewIndex(&ctx.doc->digest());
        ctx.digestModel->setHighlightAction(HighlightAction::Digest);
    }

    // Republish the digest exactly as MainWindow::rebuildDigestFor() does, so the
    // widget sees the same signal it does in the application.
    static void refresh(DocumentContext &ctx)
    {
        ctx.digestModel->beginFilterReset();
        ctx.doc->refreshHighlighting();
        ctx.digestModel->endFilterReset();
    }

private slots:
    void noDigestRuleMeansNoStrip();
    void theStripAppearsWithItsFirstMatch();
    void clearingTheRuleRemovesTheStrip();
    void theStripIsExactlyAsTallAsItsRowsAndDoesNotScroll();
    void aTallRecordDoesNotEatTheView();
    void theStripFollowsTheMainViewsColumns();
    void theStripFollowsTheMainViewsHorizontalOffset();
    void aDigestRowWearsItsOwnRulesColoursWithoutColouringTheLog();
    void theStripIsNotTheViewTheTestsCount();
};

void TestDigestView::noDigestRuleMeansNoStrip()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full")));

    DocumentContext ctx;
    buildContext(ctx, path);
    ctx.doc->highlighters().rules = {textRule("disk", HighlightAction::Color)};
    refresh(ctx);

    DocumentView view(&ctx);
    view.resize(800, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QVERIFY(view.digestView() != nullptr); // always constructed...
    QVERIFY(view.digestView()->isHidden()); // ...and not there when nothing asks for it
}

void TestDigestView::theStripAppearsWithItsFirstMatch()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full")));

    DocumentContext ctx;
    buildContext(ctx, path);
    DocumentView view(&ctx);
    view.resize(800, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QVERIFY(view.digestView()->isHidden());

    ctx.doc->highlighters().rules = {textRule("disk", HighlightAction::Digest)};
    refresh(ctx);

    QVERIFY(!view.digestView()->isHidden());
    QCOMPARE(view.digestView()->recordCount(), 1);
}

void TestDigestView::clearingTheRuleRemovesTheStrip()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full")));

    DocumentContext ctx;
    buildContext(ctx, path);
    ctx.doc->highlighters().rules = {textRule("disk", HighlightAction::Digest)};
    refresh(ctx);

    DocumentView view(&ctx);
    view.resize(800, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QVERIFY(!view.digestView()->isHidden());

    ctx.doc->highlighters().rules.clear();
    refresh(ctx);
    QVERIFY(view.digestView()->isHidden());
}

void TestDigestView::theStripIsExactlyAsTallAsItsRowsAndDoesNotScroll()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full")
                                 + rec(2, "WARN ", "retrying")
                                 + rec(3, "INFO ", "ok")));

    DocumentContext ctx;
    buildContext(ctx, path);
    ctx.doc->highlighters().rules = {textRule("disk", HighlightAction::Digest),
                                     textRule("retry", HighlightAction::Digest)};
    refresh(ctx);

    DocumentView view(&ctx);
    view.resize(800, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    LogView *strip = view.digestView();
    QCOMPARE(strip->recordCount(), 2);

    // "Sized to fit its rows" is the whole contract: two rules, two single-line
    // records, two lines — plus the frame, and NOT a header, which is hidden because
    // the strip borrows the table's columns rather than repeating its captions.
    const int lineHeight = strip->fontMetrics().height();
    const int expected = strip->frameWidth() * 2 + 2 * lineHeight;
    QCOMPARE(strip->sizeHint().height(), expected);
    QVERIFY(strip->header()->isHidden());

    // And not scrolled.
    QCOMPARE(strip->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    QCOMPARE(strip->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
}

void TestDigestView::aTallRecordDoesNotEatTheView()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));

    // One digest record with a great many continuation lines — an ordinary stack trace.
    QByteArray bytes = rec(1, "ERROR", "stack trace follows");
    for (int i = 0; i < 200; ++i)
        bytes += "    at Frame.number" + QByteArray::number(i) + "()\n";
    QVERIFY(writeWhole(path, bytes));

    DocumentContext ctx;
    buildContext(ctx, path);
    ctx.doc->highlighters().rules = {textRule("stack trace", HighlightAction::Digest)};
    refresh(ctx);

    DocumentView view(&ctx);
    view.resize(800, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    LogView *strip = view.digestView();
    const int lineHeight = strip->fontMetrics().height();
    const int capLines = LogView::kDigestMaxLines;

    // A strip that ate the log it sits under would be worse than no strip. The content
    // is capped and — only here — the vertical scrollbar comes back, so what is past
    // the cap stays reachable rather than truncated.
    QVERIFY(strip->sizeHint().height() <= strip->frameWidth() * 2 + capLines * lineHeight);
    QVERIFY(strip->sizeHint().height() < view.height() / 2);
    QCOMPARE(strip->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
}

void TestDigestView::theStripFollowsTheMainViewsColumns()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full")));

    DocumentContext ctx;
    buildContext(ctx, path);
    ctx.doc->highlighters().rules = {textRule("disk", HighlightAction::Digest)};
    refresh(ctx);

    DocumentView view(&ctx);
    view.resize(800, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QHeaderView *main = view.logView()->header();
    QHeaderView *strip = view.digestView()->header();
    QVERIFY(main->count() > 1);

    main->resizeSection(0, main->sectionSize(0) + 37);
    QCOMPARE(strip->sectionSize(0), main->sectionSize(0));

    main->moveSection(0, 1);
    QCOMPARE(strip->visualIndex(0), main->visualIndex(0));
}

void TestDigestView::theStripFollowsTheMainViewsHorizontalOffset()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full")));

    DocumentContext ctx;
    buildContext(ctx, path);
    ctx.doc->highlighters().rules = {textRule("disk", HighlightAction::Digest)};
    refresh(ctx);

    DocumentView view(&ctx);
    view.resize(300, 400); // narrow, so the table genuinely scrolls sideways
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QScrollBar *bar = view.logView()->horizontalScrollBar();
    if (bar->maximum() <= 0)
        QSKIP("the table does not overflow horizontally at this width");

    bar->setValue(bar->maximum());
    // Mirroring the column STATE alone lines the columns up only at offset zero, which
    // would make "rendered exactly as it is in the log" false the moment the user
    // scrolls right.
    QCOMPARE(view.digestView()->horizontalScrollBar()->value(),
             qMin(bar->value(), view.digestView()->horizontalScrollBar()->maximum()));
}

void TestDigestView::aDigestRowWearsItsOwnRulesColoursWithoutColouringTheLog()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full")));

    DocumentContext ctx;
    buildContext(ctx, path);
    // Digest ONLY — the rule does not colour. It still carries a background and a text
    // slot, and the strip is where they are spent: that is what tells the user which
    // rule a row is for, without a column the log format does not have.
    ctx.doc->highlighters().rules = {
        textRule("disk", HighlightAction::Digest, /*background=*/0, /*foreground=*/8)};
    refresh(ctx);

    DocumentView view(&ctx);
    view.resize(800, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QColor logBg, logFg, stripBg, stripFg;
    ctx.model->rowColors(0, logBg, logFg);
    ctx.digestModel->rowColors(0, stripBg, stripFg);

    QVERIFY2(!logBg.isValid(), "a digest-only rule must not colour the log");
    QVERIFY2(!logFg.isValid(), "a digest-only rule must not colour the log");
    QVERIFY2(stripBg.isValid(), "the digest row must wear its rule's background");
    QVERIFY2(stripFg.isValid(), "the digest row must wear its rule's text colour");
    QCOMPARE(stripBg, HighlightPalette::color(0, ctx.digestModel->darkTheme()));
}

void TestDigestView::theStripIsNotTheViewTheTestsCount()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a.log"));
    QVERIFY(writeWhole(path, rec(1, "ERROR", "disk full")));

    DocumentContext ctx;
    buildContext(ctx, path);
    DocumentView view(&ctx);

    // A DocumentView now holds TWO LogViews, so findChildren<LogView *>() stopped
    // counting views the day this shipped. Object names are the test contract precisely
    // because they are not the visible text, and every counting site across tests/ now
    // names the one it means.
    QCOMPARE(view.findChildren<LogView *>().size(), 2);
    QCOMPARE(view.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QCOMPARE(view.findChildren<LogView *>(QStringLiteral("digestView")).size(), 1);
    QCOMPARE(view.findChild<LogView *>(QStringLiteral("logView")), view.logView());
    QCOMPARE(view.findChild<LogView *>(QStringLiteral("digestView")), view.digestView());
}

QTEST_MAIN(TestDigestView)
#include "tst_digestview.moc"
