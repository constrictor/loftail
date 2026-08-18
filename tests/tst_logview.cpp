#include <QtTest>

#include <QApplication>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QHeaderView>
#include <QImage>
#include <QScrollBar>
#include <QHelpEvent>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QToolTip>

#include "Document.h"
#include "Filter.h"
#include "Highlight.h"
#include "LogFormat.h"
#include "LogModel.h"
#include "LogView.h"
#include "Palette.h"
#include "Priority.h"
#include "Record.h"
#include "RecordIndex.h"
#include "UiColors.h"

#include <utility>

using namespace loftail;

// LogView coverage that does not need a shown window: the pure line<->record
// geometry mapping (exact mode, wrap off and selected-record-only), the
// copy-as-columns text builders, and the QHeaderView column hide/reorder state
// round-trip that backs "column layout is remembered" (SPEC.md §5). Runs under an
// offscreen QApplication (forced in main) so QWidget construction is legal.
class TestLogView : public QObject
{
    Q_OBJECT

private:
    // A synthetic index spanning several blocks with known line counts.
    static RecordIndex makeIndex(int n)
    {
        RecordIndex idx;
        idx.records.reserve(n);
        for (int i = 0; i < n; ++i) {
            Record r{};
            r.lineCount = quint16((i % 4) + 1); // 1..4 physical lines, under the cap
            idx.records.append(r);
        }
        idx.rebuildBlockSums();
        return idx;
    }

    static bool writeLog(QTemporaryFile &file, const QByteArray &bytes)
    {
        if (!file.open())
            return false;
        file.write(bytes);
        file.flush();
        return true;
    }

    // A log with `n` records whose messages are long enough to wrap in AlwaysOn.
    static QByteArray makeLog(int n)
    {
        QByteArray bytes;
        for (int i = 0; i < n; ++i) {
            bytes += "2026-07-21 14:32:05,123 [main] INFO  net.socket - ";
            // A ~200-char message so it wraps to several visual lines at any
            // realistic viewport width, driving the expansion factor above 1.
            bytes += QByteArray("payload ").repeated(25);
            bytes += "\n";
        }
        return bytes;
    }

    // Six records of alternating HEIGHT: the even ones one physical line, the odd ones
    // three (a message plus two continuation lines, which do not match recordStartRe —
    // invariant #2). Records 2 and 3 carry a word a highlight rule can key on, and they
    // are deliberately one even and one odd so a rule spans a band boundary.
    static QByteArray makeTallShortLog()
    {
        QByteArray bytes;
        for (int i = 0; i < 6; ++i) {
            bytes += "2026-07-21 10:00:0";
            bytes += QByteArray::number(i);
            bytes += ",000 [main] INFO  net.socket - record ";
            bytes += QByteArray::number(i);
            if (i == 2 || i == 3)
                bytes += " special";
            bytes += "\n";
            if (i % 2)
                bytes += "    continued\n    continued\n";
        }
        return bytes;
    }

    // Vertical runs of identical pixels down one column of a rendered viewport: the
    // painted band boundaries, read back without knowing the font's line height.
    static QVector<std::pair<QRgb, int>> columnRuns(const QImage &img, int x)
    {
        QVector<std::pair<QRgb, int>> runs;
        for (int y = 0; y < img.height(); ++y) {
            const QRgb px = img.pixel(x, y);
            if (!runs.isEmpty() && runs.last().first == px)
                ++runs.last().second;
            else
                runs.append({px, 1});
        }
        return runs;
    }

    static bool isColour(QRgb px, const QColor &c)
    {
        return qAbs(qRed(px) - c.red()) <= 1 && qAbs(qGreen(px) - c.green()) <= 1
            && qAbs(qBlue(px) - c.blue()) <= 1;
    }

    // A log for the filter-anchor cases: `n` single-line records, every 4th of them
    // ERROR and the rest INFO, each numbered so one record can be named exactly by a
    // text filter and each message long enough to wrap under SelectedRecordOnly.
    // ONE physical line per record, so in WrapMode::Off a scroll line index IS a view
    // row and every expectation below is arithmetic rather than a second geometry
    // implementation.
    static QByteArray makeMixedLog(int n)
    {
        QByteArray bytes;
        for (int i = 0; i < n; ++i) {
            bytes += "2026-07-21 14:32:05,123 [main] ";
            bytes += (i % 4 == 0) ? "ERROR " : "INFO  ";
            bytes += "net.socket - record ";
            bytes += QByteArray::number(i).rightJustified(4, '0');
            bytes += " ";
            bytes += QByteArray("payload ").repeated(25); // ~200 chars: wraps
            bytes += "\n";
        }
        return bytes;
    }

    static bool openMixedLog(Document &doc, QTemporaryFile &file, int n)
    {
        if (!writeLog(file, makeMixedLog(n)))
            return false;
        return doc.open(file.fileName(),
                        QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                        Encoding::Utf8, QTimeZone::utc());
    }

    // The five calls MainWindow::applyFiltersFor() makes, in its order.
    static void refilter(LogView &view, LogModel &model, Document &doc, const FilterSet &fs)
    {
        doc.filters() = fs;
        view.beginFilterUpdate();
        model.beginFilterReset();
        doc.applyFilters();
        model.endFilterReset();
        view.endFilterUpdate();
    }

    // Priority >= WARN, which over makeMixedLog keeps exactly every 4th record.
    static FilterSet everyFourth()
    {
        FilterSet fs;
        fs.priorityEnabled = true;
        fs.minPriority = Priority::Warn;
        return fs;
    }

    static FilterSet messageContaining(const QString &needle)
    {
        FilterSet fs;
        fs.text.enabled = true;
        fs.text.matcher.set(needle, false, Qt::CaseInsensitive);
        return fs;
    }

    // The source ordinal of the record the view is scrolled to the top of. Single-line
    // records under WrapMode::Off, so the scroll value is the top view row.
    static int topSourceRow(const LogView &view, const Document &doc)
    {
        return doc.filtered().sourceRow(view.verticalScrollBar()->value());
    }

    static int currentSourceRow(const LogView &view, const Document &doc)
    {
        return view.currentRecord() < 0 ? -1 : doc.filtered().sourceRow(view.currentRecord());
    }

    static bool openLog(Document &doc, QTemporaryFile &file, int n)
    {
        if (!writeLog(file, makeLog(n)))
            return false;
        return doc.open(file.fileName(),
                        QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                        Encoding::Utf8, QTimeZone::utc());
    }

private slots:
    void wrapOffMappingMatchesBase();
    void selectedRecordWrapPatchesGeometry();
    void flattenAndTsvBuilders();
    void columnStateRoundTrips();
    void everyColumnRendersFixedPitch();
    void alwaysOnMeasuresAndRefines();
    void switchingToExactKeepsEstimationCache();
    void alwaysOnUnreachableFromExactPath();
    void alwaysOnPaintRefinesWhileScrolling();
    void filterRestrictsVisibleSetAndGeometry();
    void followDetachesAndReattaches();
    void rightClickReportsTheRecordUnderIt();
    void aFilterKeepsTheSelectedRecordAtTheSameOffset();
    void aFilterThatHidesTheSelectionKeepsThePlaceInTheFile();
    void aHiddenSelectionComesBackWhenTheFilterIsWidened();
    void clickingAnotherRecordForgetsAHiddenSelection();
    void aRescanForgetsAHiddenSelection();
    void aFilterNeverChangesTheFollowState();
    void aFilterWithNothingSelectedStillKeepsThePlace();
    void anEmptyResultLandsAtZeroWithNothingSelected();
    void clearingAFilterPutsTheViewBackWhereItWas();
    void theWrappedSelectionIsFoldedInBeforeTheViewIsRepositioned();
    void theAlternatingBandChangesAtRecordsAndNotAtLines();
    void aValueTooWideForItsColumnNamesItselfInFullOnHover();
    void aValueThatFitsItsColumnOffersNoTooltip();
    void aWrappedMessageOffersNoTooltipAndAnUnwrappedOneDoes();
    void aHeaderCaptionTooNarrowToReadNamesItselfOnHover();
};

// --- what does not fit says so (SPEC.md §5) ----------------------------------
//
// The columns elide and hovering a cut-short value shows it in full; a value that fits
// says nothing, which is what makes the tooltip mean "there is more here". These drive
// the real QEvent::ToolTip rather than a seam of their own — the decision is the view's
// answer to that event and to nothing else.
//
// Every case needs a resolved font: the whole question is whether text fits a width, and
// with no font at all (Windows offscreen, which ships none) nothing measures.

namespace {

// Hover a viewport point and report the tooltip it produced, or a null string.
//
// QToolTip hides on a timer rather than at once, so the previous case's tip has to be
// waited out before this one is asked — otherwise every answer is the last one's and a
// "nothing is offered here" assertion can never fail. Showing, by contrast, is
// immediate, so what is on screen straight after the event is this hover's answer.
QString tooltipAt(QWidget *target, const QPoint &pos)
{
    QToolTip::hideText();
    QElapsedTimer waited;
    waited.start();
    while (QToolTip::isVisible() && waited.elapsed() < 2000)
        QTest::qWait(20);

    QHelpEvent event(QEvent::ToolTip, pos, target->mapToGlobal(pos));
    QApplication::sendEvent(target, &event);
    return QToolTip::isVisible() ? QToolTip::text() : QString();
}

// Which column carries a field, since the format decides the order.
int columnOfRole(const Document &doc, FieldRole role)
{
    const QVector<Field> &fields = doc.format().fields;
    for (int c = 0; c < fields.size(); ++c)
        if (fields.at(c).role == role)
            return c;
    return -1;
}

} // namespace

void TestLogView::wrapOffMappingMatchesBase()
{
    const RecordIndex idx = makeIndex(RecordIndex::kBlockSize * 2 + 9);
    const int n = idx.records.size();

    // Wrap off => selRecord == -1: the view mapping is exactly the base mapping.
    QCOMPARE(LogView::totalScrollLines(idx, -1, 0), idx.totalLines());

    qint64 acc = 0;
    for (int r = 0; r < n; ++r) {
        QCOMPARE(LogView::scrollLineOfRecord(idx, -1, 0, r), acc);
        QCOMPARE(LogView::recordAtScrollLine(idx, -1, 0, acc), r);
        const qint64 mid = acc + RecordIndex::displayLines(idx.records.at(r)) - 1;
        QCOMPARE(LogView::recordAtScrollLine(idx, -1, 0, mid), r);
        acc += RecordIndex::displayLines(idx.records.at(r));
    }
    QCOMPARE(acc, idx.totalLines());
}

void TestLogView::selectedRecordWrapPatchesGeometry()
{
    RecordIndex idx = makeIndex(50);
    const int sel = 20;
    const int base = RecordIndex::displayLines(idx.records.at(sel)); // its unwrapped height
    const int extra = 6;
    const int selWrap = base + extra; // the wrapped selected record is taller

    const qint64 selStart = idx.firstLineOfRecord(sel);

    // Total grows by exactly the extra lines.
    QCOMPARE(LogView::totalScrollLines(idx, sel, selWrap), idx.totalLines() + extra);

    // Records before the selection are unshifted; records after shift by `extra`.
    QCOMPARE(LogView::scrollLineOfRecord(idx, sel, selWrap, sel), selStart);
    QCOMPARE(LogView::scrollLineOfRecord(idx, sel, selWrap, sel - 1),
             idx.firstLineOfRecord(sel - 1));
    QCOMPARE(LogView::scrollLineOfRecord(idx, sel, selWrap, sel + 1),
             idx.firstLineOfRecord(sel + 1) + extra);

    // Every line inside the wrapped selected record maps back to it.
    QCOMPARE(LogView::recordAtScrollLine(idx, sel, selWrap, selStart), sel);
    QCOMPARE(LogView::recordAtScrollLine(idx, sel, selWrap, selStart + selWrap - 1), sel);
    // The first line past it is the next record.
    QCOMPARE(LogView::recordAtScrollLine(idx, sel, selWrap, selStart + selWrap), sel + 1);

    // Round-trip for a record after the selection.
    const qint64 after = LogView::scrollLineOfRecord(idx, sel, selWrap, 40);
    QCOMPARE(LogView::recordAtScrollLine(idx, sel, selWrap, after), 40);
}

void TestLogView::flattenAndTsvBuilders()
{
    // Flattening keeps TSV structure intact against embedded tabs/newlines.
    QCOMPARE(LogView::flattenCell(QStringLiteral("a\tb\nc\rd")), QStringLiteral("a b c d"));

    QVector<QVector<QString>> rows = {
        {QStringLiteral("2026"), QStringLiteral("INFO"), QStringLiteral("hello")},
        {QStringLiteral("2027"), QStringLiteral("WARN"), QStringLiteral("world")},
    };
    QCOMPARE(LogView::columnsToTsv(rows),
             QStringLiteral("2026\tINFO\thello\n2027\tWARN\tworld"));
    QCOMPARE(LogView::columnsToTsv({}), QString());
}

void TestLogView::columnStateRoundTrips()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 14:32:05,123 [main] INFO  net.socket - a\n"
        "2026-07-21 14:32:06,000 [worker] WARN  db.pool - b\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    QCOMPARE(model.columnCount(), 5);

    LogView v1(&doc, &model);
    // Hide the Priority column (logical 2) and move Message (logical 4) to the front.
    v1.header()->setSectionHidden(2, true);
    v1.header()->moveSection(v1.header()->visualIndex(4), 0);
    const QByteArray state = v1.saveColumnState();

    // A fresh view restores the same hidden flags and visual order.
    LogView v2(&doc, &model);
    QVERIFY(!v2.header()->isSectionHidden(2)); // default: visible before restore
    QVERIFY(v2.restoreColumnState(state));
    QVERIFY(v2.header()->isSectionHidden(2));
    QCOMPARE(v2.header()->logicalIndex(0), 4); // Message moved to the front
    QCOMPARE(v2.header()->visualIndex(4), 0);
}

// Every column renders in a fixed-pitch font, header included. This is load-bearing
// for the estimated-geometry path, which models a wrapped record's height as
// ceil(chars / cols) instead of shaping text (ARCHITECTURE.md §7.1.1), and it is
// what makes columns line up vertically.
void TestLogView::everyColumnRendersFixedPitch()
{
    // Nothing can resolve as fixed-pitch where nothing resolves at all. The
    // offscreen plugin on Windows uses Qt's own font database, which looks in
    // $QTDIR/lib/fonts -- and Qt no longer ships fonts, so the family list comes
    // back EMPTY and every font is the same non-font. (Offscreen on Linux has
    // fontconfig, so the assertions below do run there, and on any real desktop.)
    // Skip rather than assert: this says nothing about monospaceFont(), and
    // weakening the assertions would drop a real SPEC.md §5 requirement.
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts available to this platform plugin; cannot test font resolution");

    QTemporaryFile file;
    QVERIFY(writeLog(file, "2026-07-21 14:32:05,123 [main] INFO  net.socket - a\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);

    // QFontInfo reports the font actually resolved, not the one requested, so this
    // fails if the family falls back to a proportional face.
    QVERIFY(QFontInfo(view.font()).fixedPitch());
    // Cells are painted on the viewport and headings by the header view; both must
    // inherit the same font rather than the proportional application default.
    QVERIFY(QFontInfo(view.viewport()->font()).fixedPitch());
    QVERIFY(QFontInfo(view.header()->font()).fixedPitch());
    QCOMPARE(view.header()->font().family(), view.font().family());

    // A fixed-pitch font is only useful to the geometry model if the advance really
    // is uniform: narrow and wide characters must measure the same.
    const QFontMetrics fm(view.font());
    QCOMPARE(fm.horizontalAdvance(QStringLiteral("iiiiiiiiii")),
             fm.horizontalAdvance(QStringLiteral("WWWWWWWWWW")));
}

// AlwaysOn engages the estimated geometry: measuring the visible block records
// their exact wrapped heights and refines the total toward truth (§7.1.1).
void TestLogView::alwaysOnMeasuresAndRefines()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 40), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400); // a finite viewport so wrapping has a width to work with

    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    const EstimatedGeometry &g = view.estimatedGeometry();
    QVERIFY(g.isBound());
    QVERIFY(g.blockCount() >= 1);
    QVERIFY(!g.isBlockMeasured(0)); // nothing measured until painted/forced

    const qint64 beforeTotal = g.totalLines();

    // Force the block containing record 0 to be measured (what painting does
    // lazily) and confirm the wrapped total exceeds the unwrapped physical total.
    view.measureBlockOfRecord(0);
    QVERIFY(g.isBlockMeasured(0));
    QVERIFY(g.expansionFactor() > 1.0);
    QVERIFY(g.totalLines() > beforeTotal);
    QCOMPARE(g.totalLines(), g.firstLineOfRecord(doc.index().records.size()));
}

// Switching to an exact wrap mode must NOT touch the estimation cache: the exact
// path never reads it, and a round-trip back to AlwaysOn should not re-measure.
void TestLogView::switchingToExactKeepsEstimationCache()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 30), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400);

    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    view.measureBlockOfRecord(0);
    const EstimatedGeometry &g = view.estimatedGeometry();
    QVERIFY(g.isBlockMeasured(0));
    const int cols = g.columns();
    const int measured = g.measuredBlockCount();
    const qint64 total = g.totalLines();

    // Switch to each exact mode; the cache is left exactly as it was.
    view.setWrapMode(LogView::WrapMode::Off);
    QVERIFY(g.isBlockMeasured(0));
    QCOMPARE(g.measuredBlockCount(), measured);
    QCOMPARE(g.columns(), cols);
    QCOMPARE(g.totalLines(), total);

    view.setWrapMode(LogView::WrapMode::SelectedRecordOnly);
    view.setCurrentRecord(5); // exercises the exact selected-record wrap path
    QVERIFY(g.isBlockMeasured(0));
    QCOMPARE(g.measuredBlockCount(), measured);
    QCOMPARE(g.columns(), cols);

    // Returning to AlwaysOn at the same width keeps the measurements (no reset).
    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    QVERIFY(g.isBlockMeasured(0));
    QCOMPARE(g.measuredBlockCount(), measured);
}

// In the exact modes the estimator is never bound at all — proof the estimation
// machinery is unreachable from the exact path (invariant #6).
void TestLogView::alwaysOnUnreachableFromExactPath()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 12), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400);

    // Default mode is Off; geometry queries and navigation exercise the exact
    // path, which must never construct/bind the estimator.
    QVERIFY(!view.estimatedGeometry().isBound());
    view.setWrapMode(LogView::WrapMode::SelectedRecordOnly);
    view.setCurrentRecord(3);
    QVERIFY(!view.estimatedGeometry().isBound());
    // measureBlockOfRecord is a no-op outside AlwaysOn.
    view.measureBlockOfRecord(0);
    QVERIFY(!view.estimatedGeometry().isBound());
}

// The real paint path (not the forced measure hook) drives measurement lazily:
// rendering the viewport measures the visible block, and scrolling into a second
// block measures it too, refining the total further. Uses a >1-block synthetic
// log and QWidget::render to trigger paintEvent synchronously, headless.
void TestLogView::alwaysOnPaintRefinesWhileScrolling()
{
    QTemporaryFile file;
    Document doc;
    // 5000 records spans two index blocks (kBlockSize == 4096).
    QVERIFY2(openLog(doc, file, 5000), qPrintable(doc.lastError()));
    QVERIFY(doc.index().records.size() > RecordIndex::kBlockSize);

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400);
    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    const EstimatedGeometry &g = view.estimatedGeometry();
    QVERIFY(g.blockCount() >= 2);

    auto paint = [&view]() {
        QPixmap pm(view.viewport()->size());
        view.viewport()->render(&pm); // invokes LogView::paintEvent synchronously
    };

    // First paint at the top measures block 0 and refines the total upward.
    const qint64 estTotal = g.totalLines(); // pre-measurement estimate
    paint();
    QVERIFY(g.isBlockMeasured(0));
    QCOMPARE(g.measuredBlockCount(), 1);
    QVERIFY(g.totalLines() > estTotal);

    // Scroll into the second block and paint: it gets measured too.
    view.setCurrentRecord(4500); // scrolls the target into view
    paint();
    QVERIFY(g.isBlockMeasured(1));
    QVERIFY(g.measuredBlockCount() >= 2);
    // Fully measured blocks make their portion of the mapping exact.
    QCOMPARE(g.totalLines(), g.firstLineOfRecord(doc.index().records.size()));
}

// A filter narrows the visible set: the model presents only visible rows and the
// view's line-unit geometry runs over the FILTERED subset (M4, invariant #6). The
// model reset that wraps the recompute is what the UI drives on a filter change.
void TestLogView::filterRestrictsVisibleSetAndGeometry()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file,
        "2026-07-21 10:00:00,000 [main] INFO  net.socket - a\n"   // 0 hidden
        "2026-07-21 10:00:01,000 [main] WARN  net.socket - b\n"   // 1 shown
        "2026-07-21 10:00:02,000 [main] ERROR net.socket - c\n"   // 2 shown
        "2026-07-21 10:00:03,000 [main] DEBUG net.socket - d\n"   // 3 hidden
        "2026-07-21 10:00:04,000 [main] FATAL net.socket - e\n")); // 4 shown

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.size(), 5);

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400);
    QCOMPARE(model.rowCount(), 5); // unfiltered: identity

    // Apply priority >= WARN via the model-reset path the UI uses.
    doc.filters().priorityEnabled = true;
    doc.filters().minPriority = Priority::Warn;
    model.beginFilterReset();
    doc.applyFilters();
    model.endFilterReset();

    // The model now presents only the 3 visible rows, in order.
    QCOMPARE(model.rowCount(), 3);
    const int prioCol = 2; // Time, Thread, Priority, Subsystem, Message
    QCOMPARE(model.cellText(0, prioCol), QStringLiteral("WARN"));
    QCOMPARE(model.cellText(1, prioCol), QStringLiteral("ERROR"));
    QCOMPARE(model.cellText(2, prioCol), QStringLiteral("FATAL"));

    // The view's geometry runs over the FILTERED index: total lines == the visible
    // rows' display lines, and the last scroll line resolves to the last view row.
    const RecordIndex &geo = doc.filtered().geometry();
    QCOMPARE(geo.records.size(), 3);
    QCOMPARE(LogView::totalScrollLines(geo, -1, 0), geo.totalLines());
    QCOMPARE(LogView::recordAtScrollLine(geo, -1, 0, 0), 0);
    QCOMPARE(LogView::recordAtScrollLine(geo, -1, 0, geo.totalLines() - 1), 2);

    // Navigation stays inside the filtered set.
    view.setCurrentRecord(99);
    QCOMPARE(view.currentRecord(), 2); // clamped to the last visible row

    // Clearing the filter restores the identity view.
    doc.filters() = FilterSet{};
    model.beginFilterReset();
    doc.applyFilters();
    model.endFilterReset();
    QCOMPARE(model.rowCount(), 5);
}

// Follow mode (SPEC.md §3, M6): every open follows the tail; scrolling away detaches;
// scrolling back to the bottom OR the return-to-bottom control re-attaches.
void TestLogView::followDetachesAndReattaches()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 300), qPrintable(doc.lastError())); // enough to scroll

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(400, 120); // a small viewport so a vertical scroll range exists

    QSignalSpy spy(&view, &LogView::followingChanged);

    // Normalize to "following at the bottom" (as an open does).
    view.followTail();
    QVERIFY(view.following());
    QScrollBar *sb = view.verticalScrollBar();
    QVERIFY(sb->maximum() > 0);
    QCOMPARE(sb->value(), sb->maximum());

    // Scrolling up detaches follow.
    sb->setValue(sb->maximum() / 2);
    QVERIFY(!view.following());

    // Scrolling back to the bottom re-attaches.
    sb->setValue(sb->maximum());
    QVERIFY(view.following());

    // Detach again, then re-attach via the return-to-bottom control.
    sb->setValue(0);
    QVERIFY(!view.following());
    view.followTail();
    QVERIFY(view.following());
    QCOMPARE(sb->value(), sb->maximum());

    // The state actually toggled (not stuck): several transitions were signalled.
    QVERIFY(spy.count() >= 3);
}

// The record context menu's near end (SPEC.md §5). The view's whole part in it is to
// answer WHERE the click landed — the window builds the menu — and the answer has to
// survive the coordinate hop that made this worth a test: a context menu event is
// delivered to the VIEWPORT and forwarded to the scroll area, so its position is in
// viewport coordinates, the same ones the hit test and the header expect. Reading it
// as widget coordinates would shift every row by the header's height.
void TestLogView::rightClickReportsTheRecordUnderIt()
{
    QTemporaryFile file;
    Document doc;
    // Few enough records that the viewport has empty space below them, which the
    // second half of this case needs.
    QVERIFY2(openLog(doc, file, 5), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(600, 300);

    QSignalSpy spy(&view, &LogView::recordMenuRequested);

    // Two lines into the viewport is the second record — every record here is one
    // line tall with wrap off.
    const int lineHeight = view.fontMetrics().height();
    const QPoint at(50, lineHeight * 2 + lineHeight / 2);
    QContextMenuEvent event(QContextMenuEvent::Mouse, at, view.viewport()->mapToGlobal(at));
    QApplication::sendEvent(view.viewport(), &event);

    QCOMPARE(spy.count(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toInt(), 2);
    // Right-clicking a record the selection did not cover moves the selection to it,
    // so the menu's copy items act on what is under the cursor.
    QCOMPARE(view.currentRecord(), 2);

    // A click in the empty space BELOW the last record reports nothing rather than
    // the nearest row: there is no record under the cursor to build a menu from.
    QContextMenuEvent below(QContextMenuEvent::Mouse, QPoint(50, 295),
                            view.viewport()->mapToGlobal(QPoint(50, 295)));
    QApplication::sendEvent(view.viewport(), &below);
    QCOMPARE(spy.count(), 0);
}

// --- the filter anchor (SPEC.md §6) ------------------------------------------
//
// Changing a filter remaps every view row, so the scroll value — a line index in the
// view's own line space — and the selected VIEW row both stop meaning what they meant.
// beginFilterUpdate()/endFilterUpdate() carry them across in SOURCE-record terms. The
// cases below run in WrapMode::Off over single-line records, where a scroll line IS a
// view row, so each expectation is arithmetic and not a second copy of the geometry.

void TestLogView::aFilterKeepsTheSelectedRecordAtTheSameOffset()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 400), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    QScrollBar *sb = view.verticalScrollBar();
    sb->setValue(100);
    view.setCurrentRecord(104);      // inside the viewport, and a survivor (104 % 4 == 0)
    QCOMPARE(sb->value(), 100);      // already visible: selecting it did not scroll
    const int offsetBefore = 104 - sb->value();

    refilter(view, model, doc, everyFourth());

    QCOMPARE(currentSourceRow(view, doc), 104);            // same record, new row
    QCOMPARE(view.currentRecord(), 26);                    // 104/4
    QCOMPARE(view.currentRecord() - sb->value(), offsetBefore); // same place in the window
}

void TestLogView::aFilterThatHidesTheSelectionKeepsThePlaceInTheFile()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 400), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    QScrollBar *sb = view.verticalScrollBar();
    sb->setValue(100);
    view.setCurrentRecord(101); // 101 % 4 != 0: the filter below will hide it

    refilter(view, model, doc, everyFourth());

    // Nothing is selected — the selection is never moved to a neighbour...
    QCOMPARE(view.currentRecord(), -1);
    // ...but the viewport still shows the same part of the file: record 100 survives,
    // so it is what the view is scrolled to.
    QCOMPARE(topSourceRow(view, doc), 100);
}

void TestLogView::aHiddenSelectionComesBackWhenTheFilterIsWidened()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 400), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    view.verticalScrollBar()->setValue(100);
    view.setCurrentRecord(101);

    refilter(view, model, doc, everyFourth());
    QCOMPARE(view.currentRecord(), -1); // hidden

    // Widening brings it back: the ordinal was kept while it was out of view.
    refilter(view, model, doc, FilterSet{});
    QCOMPARE(currentSourceRow(view, doc), 101);
}

void TestLogView::clickingAnotherRecordForgetsAHiddenSelection()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 400), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    view.verticalScrollBar()->setValue(100);
    view.setCurrentRecord(101);
    refilter(view, model, doc, everyFourth());
    QCOMPARE(view.currentRecord(), -1);

    // The user picks a different record while the old one is hidden. Widening must
    // honour that choice rather than re-selecting what they moved away from.
    view.setCurrentRecord(30); // view row 30 == source 120
    QCOMPARE(currentSourceRow(view, doc), 120);
    refilter(view, model, doc, FilterSet{});
    QCOMPARE(currentSourceRow(view, doc), 120);
}

void TestLogView::aRescanForgetsAHiddenSelection()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 400), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    view.verticalScrollBar()->setValue(100);
    view.setCurrentRecord(101);
    refilter(view, model, doc, everyFourth());
    QCOMPARE(view.currentRecord(), -1);

    // A model reset that is NOT a filter re-apply — a rotation rescan is the real one —
    // replaces the record space, where the remembered ordinal means something else.
    model.beginFilterReset();
    model.endFilterReset();

    refilter(view, model, doc, FilterSet{});
    QCOMPARE(view.currentRecord(), -1);
}

void TestLogView::aFilterNeverChangesTheFollowState()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 400), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(400, 300);

    QScrollBar *sb = view.verticalScrollBar();

    // Following: still following, still pinned to the newest record.
    view.followTail();
    QVERIFY(view.following());
    refilter(view, model, doc, everyFourth());
    QVERIFY(view.following());
    QCOMPARE(sb->value(), sb->maximum());

    // Detached: stays detached even when the new subset is SHORTER THAN A PAGE, so the
    // clamp inside endResetModel() lands the old value at the bottom. That clamp used to
    // arrive as an ordinary scroll and silently re-attach follow.
    refilter(view, model, doc, FilterSet{});
    sb->setValue(100);
    QVERIFY(!view.following());

    QSignalSpy spy(&view, &LogView::followingChanged);
    refilter(view, model, doc, messageContaining(QStringLiteral("record 0007")));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(sb->maximum(), 0); // one row, so the bottom IS the top
    QVERIFY(!view.following());
    QCOMPARE(spy.count(), 0);   // and the state never even wobbled
}

void TestLogView::aFilterWithNothingSelectedStillKeepsThePlace()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 400), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    view.verticalScrollBar()->setValue(101); // top is 101, which the filter hides
    QCOMPARE(view.currentRecord(), -1);

    refilter(view, model, doc, everyFourth());

    QCOMPARE(view.currentRecord(), -1);
    // The nearest survivor AT OR AFTER the old top comes to the top: reading direction,
    // so what has already been read scrolls off rather than back into view.
    QCOMPARE(topSourceRow(view, doc), 104);
}

void TestLogView::anEmptyResultLandsAtZeroWithNothingSelected()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 400), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    view.verticalScrollBar()->setValue(100);
    view.setCurrentRecord(104);

    refilter(view, model, doc, messageContaining(QStringLiteral("no such record")));

    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(view.currentRecord(), -1);
    QCOMPARE(view.verticalScrollBar()->value(), 0);

    // And the selection was only hidden, not forgotten: it comes back with the records.
    refilter(view, model, doc, FilterSet{});
    QCOMPARE(currentSourceRow(view, doc), 104);
}

void TestLogView::clearingAFilterPutsTheViewBackWhereItWas()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 400), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    QScrollBar *sb = view.verticalScrollBar();
    sb->setValue(100);
    view.setCurrentRecord(104);

    refilter(view, model, doc, everyFourth());
    refilter(view, model, doc, FilterSet{});

    QCOMPARE(currentSourceRow(view, doc), 104);
    QCOMPARE(sb->value(), 100); // exactly where it started
}

void TestLogView::theWrappedSelectionIsFoldedInBeforeTheViewIsRepositioned()
{
    // The selection is part of the line space in SelectedRecordOnly, so the target
    // scroll line has to be computed AFTER the selection is restored — otherwise a
    // record BELOW the selection is placed by a mapping that does not know the
    // selected record grew, and the view lands short by its extra lines.
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve under this QPA plugin; nothing wraps to measure");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 400), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::SelectedRecordOnly);
    view.resize(320, 400); // narrow, so the ~200-char message wraps hard

    QScrollBar *sb = view.verticalScrollBar();
    // A survivor selected near the TOP of the file, then scrolled away from: the top
    // anchor drives the scroll, and it sits below the selection.
    view.setCurrentRecord(8);
    sb->setValue(200);

    // Where the view actually is, in RECORD terms. Read back through the public statics
    // rather than assumed: in this mode the scroll line of a record below the selection
    // already carries the selection's extra wrapped lines, which is the whole point.
    int topBefore = 0;
    qint64 topOffset = 0;
    {
        const RecordIndex &before = doc.filtered().geometry();
        const qint64 extraBefore = qint64(sb->maximum()) + sb->pageStep() - before.totalLines();
        if (extraBefore <= 0)
            QSKIP("the selected record did not wrap in this environment");
        const int selWrapBefore = int(extraBefore) + 1; // one display line unwrapped
        topBefore = LogView::recordAtScrollLine(before, 8, selWrapBefore, sb->value());
        topOffset = sb->value()
                  - LogView::scrollLineOfRecord(before, 8, selWrapBefore, topBefore);
    }

    refilter(view, model, doc, everyFourth());

    QCOMPARE(currentSourceRow(view, doc), 8);
    const int selRow = view.currentRecord();
    const int topRow = doc.filtered().viewRowAtOrAfter(topBefore);
    QVERIFY(topRow < model.rowCount());
    QVERIFY(topRow > selRow); // the anchor is BELOW the selection: the case that bites

    const RecordIndex &geo = doc.filtered().geometry();
    const qint64 extra = qint64(sb->maximum()) + sb->pageStep() - geo.totalLines();
    if (extra <= 0)
        QSKIP("the selected record did not wrap in this environment");

    // `+ extra` is the assertion: the anchor row is placed by a mapping that already
    // knows the restored selection wraps. Computing the target before restoring the
    // selection lands the view `extra` lines short.
    qint64 expected = geo.firstLineOfRecord(topRow) + extra;
    if (doc.filtered().sourceRow(topRow) == topBefore)
        expected += topOffset;
    QCOMPARE(qint64(sb->value()), expected);
}

// The Logger column carries "net.socket" in every record of this log. Squeezed to a
// width no font can fit it in, the cell is painted with an ellipsis and the tooltip
// hands back the whole name — the value is not otherwise recoverable without resizing
// the column, which is exactly the complaint.
void TestLogView::aValueTooWideForItsColumnNamesItselfInFullOnHover()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts available to this platform plugin; nothing measures");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 5), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(600, 300);

    const int logger = columnOfRole(doc, FieldRole::Logger);
    QVERIFY(logger >= 0);
    view.header()->resizeSection(logger, 20); // narrower than any rendering of the name

    const QPoint at(view.header()->sectionViewportPosition(logger) + 2,
                    view.fontMetrics().height() / 2); // the first record
    QCOMPARE(tooltipAt(view.viewport(), at), model.cellText(0, logger));
    QCOMPARE(QToolTip::text(), QStringLiteral("net.socket"));
    QToolTip::hideText();
}

// The other half of the rule, and the half that is easy to lose: a tooltip repeating a
// value that is already fully on screen tells the reader nothing and trains them to
// ignore the ones that do.
void TestLogView::aValueThatFitsItsColumnOffersNoTooltip()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts available to this platform plugin; nothing measures");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 5), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(600, 300);

    const int logger = columnOfRole(doc, FieldRole::Logger);
    QVERIFY(logger >= 0);
    view.header()->resizeSection(logger, 400); // room to spare

    const QPoint at(view.header()->sectionViewportPosition(logger) + 2,
                    view.fontMetrics().height() / 2);
    QVERIFY(tooltipAt(view.viewport(), at).isNull());

    // And nothing at all below the last record: five records occupy five lines, so the
    // bottom of a 300 px viewport is empty space that is not a cell.
    QVERIFY(tooltipAt(view.viewport(), QPoint(at.x(), 295)).isNull());
    QToolTip::hideText();
}

// The message column is the one that interacts with wrapping, so it answers by the same
// rule rather than by a rule of its own: with wrap off it is clipped exactly like the
// others and elides and tooltips like them, and with wrapping on every character is
// already on screen and there is nothing a tooltip could add.
void TestLogView::aWrappedMessageOffersNoTooltipAndAnUnwrappedOneDoes()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts available to this platform plugin; nothing measures");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 5), qPrintable(doc.lastError())); // ~200-char messages

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(600, 300);

    const int message = columnOfRole(doc, FieldRole::Message);
    QVERIFY(message >= 0);
    view.header()->resizeSection(message, 120);

    const QPoint at(view.header()->sectionViewportPosition(message) + 2,
                    view.fontMetrics().height() / 2);
    QCOMPARE(tooltipAt(view.viewport(), at), model.cellText(0, message));

    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    QVERIFY(tooltipAt(view.viewport(), at).isNull());
    QToolTip::hideText();
}

// The first complaint was the HEADER, which read "Priorit" with no way to tell that from
// a field genuinely called that. It elides through the header's own elide mode and names
// itself on hover, measured against the rect the style puts the label in rather than the
// raw section — the margin either side is what turns a name that fits into one that does
// not.
void TestLogView::aHeaderCaptionTooNarrowToReadNamesItselfOnHover()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts available to this platform plugin; nothing measures");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 5), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(600, 300);

    QCOMPARE(view.header()->textElideMode(), Qt::ElideRight);

    const int logger = columnOfRole(doc, FieldRole::Logger);
    QVERIFY(logger >= 0);
    const QString caption = model.headerData(logger, Qt::Horizontal, Qt::DisplayRole).toString();
    QVERIFY(!caption.isEmpty());

    view.header()->resizeSection(logger, 20);
    const QPoint at(view.header()->sectionViewportPosition(logger) + 2,
                    view.header()->height() / 2);
    QCOMPARE(tooltipAt(view.header()->viewport(), at), caption);

    view.header()->resizeSection(logger, 400);
    QVERIFY(tooltipAt(view.header()->viewport(), at).isNull());
    QToolTip::hideText();
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestLogView tc;
    return QTest::qExec(&tc, argc, argv);
}


// The log table shades every other RECORD (SPEC.md §5), and the unit of the band is
// the record rather than the physical line (invariant #2) — which is the whole point
// of it in wrap-always-on, where nothing else says where one record ends. Rendered and
// read back off the pixels, because the band's unit and its visibility are both things
// only the painted result can be asked about. Children are deliberately NOT drawn: the
// follow-tail overlay sits in the bottom-right corner of this very column.
void TestLogView::theAlternatingBandChangesAtRecordsAndNotAtLines()
{
    QTemporaryFile file;
    QVERIFY(writeLog(file, makeTallShortLog()));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.size(), 6);
    QCOMPARE(int(doc.index().records.at(0).lineCount), 1);
    QCOMPARE(int(doc.index().records.at(1).lineCount), 3);

    LogModel model(&doc);
    LogView view(&doc, &model);
    // The theme the band was reported invisible on: a near-black table.
    QPalette pal = view.palette();
    pal.setColor(QPalette::Base, QColor(0x14, 0x16, 0x18));
    pal.setColor(QPalette::Text, QColor(0xd0, 0xd3, 0xd6));
    view.setPalette(pal);
    view.resize(700, 400);

    const QColor base = view.palette().base().color();
    const QColor band = alternateRowColor(view.palette());

    auto render = [&view] {
        QImage img(view.viewport()->size(), QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        view.viewport()->render(&img, QPoint(), QRegion(), QWidget::DrawWindowBackground);
        return img;
    };

    QImage img = render();
    // The rightmost column of the viewport: past every field's text, so what is there
    // is the row fill and nothing else.
    QVector<std::pair<QRgb, int>> runs = columnRuns(img, img.width() - 1);
    QVERIFY2(runs.size() >= 4, "nothing was painted");

    const int lh = runs.at(0).second; // record 0 is one line tall, so this IS the pitch
    QVERIFY(lh > 0);
    QVERIFY2(isColour(runs.at(0).first, base), "the first record is not on the base fill");
    QVERIFY2(isColour(runs.at(1).first, band), "the second record is not banded");
    QVERIFY2(!isColour(runs.at(1).first, base), "the band is not distinguishable from the base");
    QVERIFY2(isColour(runs.at(2).first, base), "the band did not alternate back");
    QVERIFY2(isColour(runs.at(3).first, band), "the band did not alternate again");
    // The three-line records are one band each, not three: per-line shading would make
    // every run lh tall.
    QCOMPARE(runs.at(1).second, 3 * lh);
    QCOMPARE(runs.at(2).second, lh);
    QCOMPARE(runs.at(3).second, 3 * lh);

    // A record a highlight rule coloured keeps that colour (SPEC.md §7): the band is
    // what a record wears when nothing else has claimed it, so records 2 and 3 — one
    // even, one odd — paint as ONE unbroken block of the rule's colour rather than as
    // two shades of it.
    HighlightRule rule;
    rule.match.text.enabled = true;
    rule.match.text.matcher.set(QStringLiteral("special"), /*regex=*/false,
                                Qt::CaseInsensitive);
    rule.background = 0;
    doc.highlighters().rules.append(rule);
    doc.refreshHighlighting();

    const QColor ruleBg = HighlightPalette::color(0, model.darkTheme());
    QVERIFY(ruleBg.isValid());

    img = render();
    runs = columnRuns(img, img.width() - 1);
    QVERIFY(runs.size() >= 4);
    QVERIFY2(isColour(runs.at(0).first, base), "an unmatched record lost its fill");
    QVERIFY2(isColour(runs.at(1).first, band), "an unmatched record lost its band");
    QVERIFY2(isColour(runs.at(2).first, ruleBg), "a matched record is not wearing its rule");
    QCOMPARE(runs.at(2).second, 4 * lh); // one block over both matched records
}

#include "tst_logview.moc"
