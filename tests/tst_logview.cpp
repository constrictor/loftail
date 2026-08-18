#include <QtTest>

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QHeaderView>
#include <QImage>
#include <QScrollBar>
#include <QHelpEvent>
#include <QItemSelectionModel>
#include <QProgressDialog>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>

#include "Document.h"
#include "Filter.h"
#include "Highlight.h"
#include "LogFormat.h"
#include "LogModel.h"
#include "Fonts.h"
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

    // How many columns of `band` are painted in `c` over at least `minShare` of its
    // height — how a filled MARK is told apart from the glyphs around it, which are
    // drawn in that same colour (see the find-mark cases at the bottom of this file).
    static int markedColumns(const QImage &img, const QRect &band, const QColor &c,
                             double minShare = 0.5);

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

    // The records of makeMixedLog() as they stand in the FILE — the oracle a raw copy
    // is judged against, since one record is one line here.
    static QStringList rawLines(int n)
    {
        return QString::fromUtf8(makeMixedLog(n)).split(QLatin1Char('\n'));
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
    void everyColumnStartsWideEnoughForItsOwnHeading();
    void aColumnSeedsFromTheWidestInternedName();
    void aWidthTheUserDraggedSurvivesEverySeed();
    void aRestoredColumnLayoutIsNeverReseeded();
    void fittingAColumnShowsItsWidestValueAndKeepsIt();
    void aFitIsBoundedHoweverWideTheValueIs();
    void doubleClickingASectionDividerFitsThatColumn();
    void draggingThePointerSelectsEveryRecordItPassesOver();
    void ctrlClickingTakesOneRecordInAndOutOfTheSelection();
    void aDragPastTheViewportEdgeScrollsAndGoesOnSelecting();
    void aModelResetEndsADragInFlight();
    void aRightPressLeavesAMultiRecordSelectionAlone();
    void aDoubleClickReportsTheRecordAndColumnUnderThePointer();
    void aDoubleClickBelowTheLastRecordReportsNothing();
    void aDoubleClickLeavesNoDragArmedBehindIt();
    void selectAllTakesEveryRecordAndLeavesTheReaderWhereTheyAre();
    void selectAllStopsAtWhatTheFilterLeftVisible();
    void whatFindMatchedIsMarkedInsideTheRecordsOnScreen();
    void aRuleColouredRecordShowsItsColourAndTheMarkTogether();
    void aMatchStraddlingAWrappedLineIsMarkedOnBothOfThem();

    // --- Zoom (SPEC.md §5, ARCHITECTURE.md §7.1.5) ------------------------------
    void theLogTextSizeStopsAtBothBoundsAndComesBackOnReset();
    void aBiggerFontFitsFewerRecordsInTheSameViewport();
    void aZoomDropsTheWrappedHeightsMeasuredAtTheOldFont();
    void aZoomLeavesTheReaderOnTheRecordTheyWereReading();
    void aZoomWidensTheSeededColumnsAndLeavesADraggedOneAlone();
    void ctrlWheelAsksForAZoomAndAPlainWheelStillScrolls();

    // --- Copying a selection is bounded (SPEC.md §5, ARCHITECTURE.md §7.1.6) ------
    void aSmallCopyYieldsTheSelectedRecordsAndNothingElse();
    void aCtrlClickedSelectionCopiesEveryRangeItNamesExactlyOnce();
    void aCopyBigEnoughToWaitForOffersToStopAndCopiesTheSameText();
    void cancellingACopyLeavesTheClipboardAsItWas();
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

// --- columns start at a width that fits (SPEC.md §5) -------------------------
//
// The widths are measured from the font, so every case here needs one resolved — with
// an empty font database (Windows offscreen, which ships none) every advance is zero
// and the whole question is meaningless.

namespace {

// A log whose subsystem name is far longer than any typical-value allowance, so the
// seed taken from the intern table is tellable apart from the seed taken without one.
QByteArray makeLongNameLog(const QByteArray &logger, int n, int messageChars = 20)
{
    QByteArray bytes;
    for (int i = 0; i < n; ++i) {
        bytes += "2026-07-21 14:32:0";
        bytes += QByteArray::number(i % 10);
        bytes += ",123 [main] INFO  " + logger + " - ";
        bytes += QByteArray("m").repeated(messageChars);
        bytes += "\n";
    }
    return bytes;
}

bool openBytes(Document &doc, QTemporaryFile &file, const QByteArray &bytes)
{
    if (!file.open())
        return false;
    file.write(bytes);
    file.flush();
    return doc.open(file.fileName(),
                    QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                    Encoding::Utf8, QTimeZone::utc());
}

int captionWidth(const LogView &view, const LogModel &model, int column)
{
    return view.fontMetrics().horizontalAdvance(
        model.headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
}

} // namespace

// The complaint that started this: Priority shipped at 60 px, which cannot fit the word
// "Priority" in any font. A column that cannot say what it is is worse than a wide one.
void TestLogView::everyColumnStartsWideEnoughForItsOwnHeading()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts available to this platform plugin; nothing measures");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 3), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);

    for (int c = 0; c < model.columnCount(); ++c) {
        const int caption = captionWidth(view, model, c);
        QVERIFY2(view.header()->sectionSize(c) >= caption,
                 qPrintable(QStringLiteral("column %1 (%2) starts at %3 px, narrower than "
                                           "its own heading at %4 px")
                                .arg(c)
                                .arg(model.headerData(c, Qt::Horizontal).toString())
                                .arg(view.header()->sectionSize(c))
                                .arg(caption)));
    }
}

// The second seed, the one that runs when the scan finishes: the intern table holds
// every subsystem name in the file exactly once (invariant #4), so the Subsystem column
// can be opened to the widest of them without touching a record.
void TestLogView::aColumnSeedsFromTheWidestInternedName()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts available to this platform plugin; nothing measures");

    const QByteArray longName = "com.example.deeply.nested.subsystem";

    QTemporaryFile shortFile;
    Document shortDoc;
    QVERIFY2(openBytes(shortDoc, shortFile, makeLongNameLog("db", 5)),
             qPrintable(shortDoc.lastError()));
    LogModel shortModel(&shortDoc);
    LogView shortView(&shortDoc, &shortModel);
    shortView.seedColumnWidths();

    QTemporaryFile longFile;
    Document longDoc;
    QVERIFY2(openBytes(longDoc, longFile, makeLongNameLog(longName, 5)),
             qPrintable(longDoc.lastError()));
    LogModel longModel(&longDoc);
    LogView longView(&longDoc, &longModel);
    longView.seedColumnWidths(); // what MainWindow::onIndexFinished calls

    const int logger = columnOfRole(longDoc, FieldRole::Logger);
    QVERIFY(logger >= 0);
    QVERIFY(longView.header()->sectionSize(logger)
            >= longView.fontMetrics().horizontalAdvance(QString::fromLatin1(longName)));
    // And it is the NAME doing it, not the format: the same column over a log whose
    // subsystem is "db" stays at its typical-value allowance.
    QVERIFY(shortView.header()->sectionSize(logger) < longView.header()->sectionSize(logger));
}

// The trap in the whole feature: a seed arrives after the user has already dragged a
// divider, and must leave it exactly where they put it. Reset Widths is the way back.
void TestLogView::aWidthTheUserDraggedSurvivesEverySeed()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openBytes(doc, file, makeLongNameLog("com.example.deeply.nested.subsystem", 5)),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    const int logger = columnOfRole(doc, FieldRole::Logger);
    QVERIFY(logger >= 0);
    const int seeded = view.header()->sectionSize(logger);

    view.header()->resizeSection(logger, 37); // the user drags it narrow
    view.seedColumnWidths();
    QCOMPARE(view.header()->sectionSize(logger), 37);
    view.seedColumnWidths(); // and a second scan does not get a second chance either
    QCOMPARE(view.header()->sectionSize(logger), 37);

    // Reset Widths forgets every user width, which is what makes the drag undoable —
    // and re-seeds, so the column comes back at least as wide as it opened.
    view.resetColumnWidths();
    QVERIFY(view.header()->sectionSize(logger) >= seeded);
}

// Session restore runs BEFORE indexing starts (MainWindow::restoreSession), so the seed
// that fires at the end of the scan is the one thing that could quietly widen a column
// the user had narrowed in the last session.
void TestLogView::aRestoredColumnLayoutIsNeverReseeded()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openBytes(doc, file, makeLongNameLog("com.example.deeply.nested.subsystem", 5)),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    const int logger = columnOfRole(doc, FieldRole::Logger);
    QVERIFY(logger >= 0);

    LogView saved(&doc, &model);
    saved.header()->resizeSection(logger, 45);
    const QByteArray state = saved.saveColumnState();

    LogView restored(&doc, &model);
    QVERIFY(restored.restoreColumnState(state));
    QCOMPARE(restored.header()->sectionSize(logger), 45);
    restored.seedColumnWidths();
    QCOMPARE(restored.header()->sectionSize(logger), 45);
}

// "Fit to Contents" is the other question: not what a typical value takes but what the
// widest one does. Over the metadata columns that answer comes from the intern table.
void TestLogView::fittingAColumnShowsItsWidestValueAndKeepsIt()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts available to this platform plugin; nothing measures");

    const QByteArray longName = "com.example.a.name.longer.than.any.seed.allowance.at.all";

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openBytes(doc, file, makeLongNameLog(longName, 5)), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(800, 400);
    const int logger = columnOfRole(doc, FieldRole::Logger);
    QVERIFY(logger >= 0);

    const int full = view.fontMetrics().horizontalAdvance(QString::fromLatin1(longName));
    QVERIFY(view.header()->sectionSize(logger) < full); // the seed clamps; a fit does not
    view.fitColumnsToContents();
    QVERIFY(view.header()->sectionSize(logger) >= full);

    // A fit is the user's choice as much as a drag is, so the next seed leaves it alone.
    const int fitted = view.header()->sectionSize(logger);
    view.seedColumnWidths();
    QCOMPARE(view.header()->sectionSize(logger), fitted);

    // And the message column fits its message rather than the 200-character allowance
    // it opened with — fitting is allowed to make a column NARROWER.
    const int message = columnOfRole(doc, FieldRole::Message);
    QVERIFY(message >= 0);
    QVERIFY(view.header()->sectionSize(message)
            >= view.fontMetrics().horizontalAdvance(model.cellText(0, message)));
    QVERIFY(view.header()->sectionSize(message) < 400);
}

// A fit measures real values, and a log may hold one that is 20,000 characters wide. The
// resulting column is bounded, or "fit to contents" hands the user a header they cannot
// scroll to the end of.
void TestLogView::aFitIsBoundedHoweverWideTheValueIs()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openBytes(doc, file, makeLongNameLog("db", 3, /*messageChars=*/20000)),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(800, 400);
    const int message = columnOfRole(doc, FieldRole::Message);
    QVERIFY(message >= 0);

    view.fitColumnToContents(message);
    QVERIFY2(view.header()->sectionSize(message) <= 2400,
             qPrintable(QStringLiteral("a fit produced %1 px")
                            .arg(view.header()->sectionSize(message))));
}

// Double-clicking a divider fits the column to its left. QHeaderView emits the signal
// and does nothing with it on its own, so before this it was a gesture with no effect.
void TestLogView::doubleClickingASectionDividerFitsThatColumn()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts available to this platform plugin; nothing measures");

    const QByteArray longName = "com.example.a.name.longer.than.any.seed.allowance.at.all";

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openBytes(doc, file, makeLongNameLog(longName, 5)), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(900, 400);
    view.show();

    const int logger = columnOfRole(doc, FieldRole::Logger);
    QVERIFY(logger >= 0);
    QHeaderView *header = view.header();
    const int before = header->sectionSize(logger);

    // The right-hand grip of the Subsystem section, which is the handle that resizes it.
    const int x = header->sectionViewportPosition(logger) + header->sectionSize(logger) - 1;
    QTest::mouseDClick(header->viewport(), Qt::LeftButton, Qt::KeyboardModifiers(),
                       QPoint(x, qMax(2, header->viewport()->height() / 2)));

    QVERIFY2(header->sectionSize(logger) > before, "the divider double-click did nothing");
    QVERIFY(header->sectionSize(logger)
            >= view.fontMetrics().horizontalAdvance(QString::fromLatin1(longName)));
}

// --- building a selection with the pointer (SPEC.md §5) -----------------------
//
// Multi-record selection has always existed and both copy commands act on it; until now
// the only way to build one was a click and a Shift+click. These drive real mouse events
// at the viewport rather than the handlers, because the whole of what is being added is
// the gesture — every one of them passes against handlers called directly.
//
// All of them run in WrapMode::Off over makeMixedLog's single-line records, so a view row
// is one line tall and a viewport y is arithmetic rather than a second geometry.

namespace {

// Which view rows are selected, ascending. The selection MODEL, not the copy commands:
// what a Ctrl+click has to be judged on is the set, and the copy path's own range walk
// falls back to the focused record when the set is empty.
QVector<int> selectedRows(const LogView &view)
{
    QVector<int> rows;
    for (const QModelIndex &i : view.selectionModel()->selectedRows(0))
        rows.push_back(i.row());
    std::sort(rows.begin(), rows.end());
    return rows;
}

// A move with the left button still down. QTest::mouseMove tracks the buttons a
// QTest::mousePress left down, so it is the right tool DURING a drag — but once the
// release has cleared them it moves the cursor and sends no event at all, which is
// exactly the case "a move after the release must not extend" needs to observe.
void dragMoveTo(QWidget *target, const QPoint &pos)
{
    QMouseEvent move(QEvent::MouseMove, pos, target->mapToGlobal(pos),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &move);
}

} // namespace

void TestLogView::draggingThePointerSelectsEveryRecordItPassesOver()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 60), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    const int lh = view.fontMetrics().height();
    QVERIFY(view.viewport()->height() > lh * 8); // room for every row this drag covers
    const auto rowAt = [&](int row) {
        return QPoint(50, (row - view.verticalScrollBar()->value()) * lh + lh / 2);
    };

    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), rowAt(1));
    QCOMPARE(selectedRows(view), QVector<int>({1}));

    QTest::mouseMove(view.viewport(), rowAt(4));
    QCOMPARE(selectedRows(view), QVector<int>({1, 2, 3, 4}));
    QCOMPARE(view.currentRecord(), 4);

    // Back toward the anchor SHRINKS the range: a drag names its two ends, not every
    // record the pointer has ever been over.
    QTest::mouseMove(view.viewport(), rowAt(2));
    QCOMPARE(selectedRows(view), QVector<int>({1, 2}));

    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), rowAt(2));
    // The release ends the drag, so a move that arrives afterwards is not one.
    dragMoveTo(view.viewport(), rowAt(6));
    QCOMPARE(selectedRows(view), QVector<int>({1, 2}));
}

void TestLogView::ctrlClickingTakesOneRecordInAndOutOfTheSelection()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 60), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    const int lh = view.fontMetrics().height();
    QVERIFY(view.viewport()->height() > lh * 8);
    const auto rowAt = [&](int row) {
        return QPoint(50, (row - view.verticalScrollBar()->value()) * lh + lh / 2);
    };

    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), rowAt(1));
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::ShiftModifier, rowAt(3));
    QCOMPARE(selectedRows(view), QVector<int>({1, 2, 3}));

    // A record nowhere near the range joins it, and the range survives.
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::ControlModifier, rowAt(6));
    QCOMPARE(selectedRows(view), QVector<int>({1, 2, 3, 6}));
    QCOMPARE(view.currentRecord(), 6);

    // And the same gesture takes one back out, leaving the rest where it was.
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::ControlModifier, rowAt(2));
    QCOMPARE(selectedRows(view), QVector<int>({1, 3, 6}));

    // The anchor followed the pointer, so a Shift+click extends from the record last
    // Ctrl+clicked rather than from wherever the previous range began.
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::ShiftModifier, rowAt(4));
    QCOMPARE(selectedRows(view), QVector<int>({2, 3, 4}));

    // Shift outranks Ctrl: both held is still an extend.
    QTest::mouseClick(view.viewport(), Qt::LeftButton,
                      Qt::ShiftModifier | Qt::ControlModifier, rowAt(6));
    QCOMPARE(selectedRows(view), QVector<int>({2, 3, 4, 5, 6}));
}

void TestLogView::aDragPastTheViewportEdgeScrollsAndGoesOnSelecting()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 400), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 300);
    view.show(); // a hide has to have something to undo, for the second half

    QScrollBar *sb = view.verticalScrollBar();
    sb->setValue(0);
    const int lh = view.fontMetrics().height();
    const int onScreen = view.viewport()->height() / lh;
    QVERIFY(onScreen > 4);
    const QPoint belowTheEdge(50, view.viewport()->height() + 4 * lh);

    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(),
                      QPoint(50, lh / 2));
    QTest::mouseMove(view.viewport(), belowTheEdge);
    QTest::qWait(300); // several autoscroll ticks

    QVERIFY2(sb->value() > 0, "a drag held past the bottom edge must scroll");
    const QVector<int> rows = selectedRows(view);
    QVERIFY2(rows.size() > onScreen, "the selection must have grown past one screenful");
    QCOMPARE(rows.first(), 0);                      // still anchored where the press was
    QCOMPARE(rows.last(), rows.size() - 1);         // and contiguous throughout
    QCOMPARE(view.currentRecord(), rows.last());

    // The release stops it: a timer left running would go on scrolling a view nobody is
    // dragging any more.
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(),
                        belowTheEdge);
    const int stopped = sb->value();
    QTest::qWait(200);
    QCOMPARE(sb->value(), stopped);

    // So does a hide — a view whose tab is switched away mid-drag never sees a release.
    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(),
                      QPoint(50, lh / 2));
    QTest::mouseMove(view.viewport(), belowTheEdge);
    QTest::qWait(150);
    QVERIFY(sb->value() > 0);
    view.hide();
    const int hidden = sb->value();
    QTest::qWait(200);
    QCOMPARE(sb->value(), hidden);
}

void TestLogView::aModelResetEndsADragInFlight()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 60), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    const int lh = view.fontMetrics().height();
    const auto rowAt = [&](int row) { return QPoint(50, row * lh + lh / 2); };

    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), rowAt(1));
    QTest::mouseMove(view.viewport(), rowAt(3));
    QCOMPARE(selectedRows(view), QVector<int>({1, 2, 3}));

    // A rotation rescan: the record space is replaced, so the anchor the drag extends
    // from is gone with it.
    model.beginFilterReset();
    model.endFilterReset();
    QCOMPARE(view.currentRecord(), -1);
    QVERIFY(selectedRows(view).isEmpty());

    dragMoveTo(view.viewport(), rowAt(6));
    QVERIFY2(selectedRows(view).isEmpty(), "a drag must not survive the reset that dropped its anchor");
    QCOMPARE(view.currentRecord(), -1);
}

// A right press must leave the selection alone, because the context menu that follows it
// decides for itself: it moves the selection onto the record under the cursor ONLY when
// that record is not already in it, which is what makes "these five records" survive
// long enough to be acted on. The press used to collapse it first, which nothing could
// observe while a multi-record selection took a Shift+click to build.
void TestLogView::aRightPressLeavesAMultiRecordSelectionAlone()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 60), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    const int lh = view.fontMetrics().height();
    const auto rowAt = [&](int row) { return QPoint(50, row * lh + lh / 2); };

    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), rowAt(1));
    QTest::mouseMove(view.viewport(), rowAt(3));
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), rowAt(3));
    QCOMPARE(selectedRows(view), QVector<int>({1, 2, 3}));

    QSignalSpy spy(&view, &LogView::recordMenuRequested);
    QTest::mousePress(view.viewport(), Qt::RightButton, Qt::KeyboardModifiers(), rowAt(2));
    QContextMenuEvent menu(QContextMenuEvent::Mouse, rowAt(2),
                           view.viewport()->mapToGlobal(rowAt(2)));
    QApplication::sendEvent(view.viewport(), &menu);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toInt(), 2);
    QCOMPARE(selectedRows(view), QVector<int>({1, 2, 3}));

    // Outside it, the menu still moves the selection onto what was clicked.
    QTest::mousePress(view.viewport(), Qt::RightButton, Qt::KeyboardModifiers(), rowAt(8));
    QContextMenuEvent elsewhere(QContextMenuEvent::Mouse, rowAt(8),
                                view.viewport()->mapToGlobal(rowAt(8)));
    QApplication::sendEvent(view.viewport(), &elsewhere);
    QCOMPARE(selectedRows(view), QVector<int>({8}));
}

// --- the double-click gesture (SPEC.md §5) -------------------------------------------
//
// The view decides nothing about what a double-click MEANS — that is the window's, and
// tst_recordmenu drives it end to end. What is the view's is the report: which record,
// which column, and the two cases where there is nothing to report.

void TestLogView::aDoubleClickReportsTheRecordAndColumnUnderThePointer()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 60), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(900, 400);

    const int logger = columnOfRole(doc, FieldRole::Logger);
    QVERIFY(logger >= 0);
    QHeaderView *header = view.header();
    const int x = header->sectionViewportPosition(logger) + header->sectionSize(logger) / 2;
    const int lh = view.fontMetrics().height();
    const QPoint pos(x, 3 * lh + lh / 2);

    QSignalSpy spy(&view, &LogView::recordDoubleClicked);
    QTest::mouseDClick(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), pos);
    QCOMPARE(spy.count(), 1);
    QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toInt(), 3);
    QCOMPARE(args.at(1).toInt(), logger);
    // The record double-clicked is the one focused afterwards, whatever the press that
    // began the pair did with the selection.
    QCOMPARE(view.currentRecord(), 3);

    // A modifier already means something on the press that begins the pair, so a
    // double-click carrying one is not this gesture.
    QTest::mouseDClick(view.viewport(), Qt::LeftButton, Qt::ControlModifier, pos);
    QTest::mouseDClick(view.viewport(), Qt::LeftButton, Qt::ShiftModifier, pos);
    QCOMPARE(spy.count(), 0);

    // Nor is a right-button one, which is the context menu's gesture.
    QTest::mouseDClick(view.viewport(), Qt::RightButton, Qt::KeyboardModifiers(), pos);
    QCOMPARE(spy.count(), 0);
}

void TestLogView::aDoubleClickBelowTheLastRecordReportsNothing()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 3), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(900, 400);

    const int lh = view.fontMetrics().height();
    QVERIFY(view.viewport()->height() > 10 * lh); // there really is empty space below

    QSignalSpy spy(&view, &LogView::recordDoubleClicked);
    QTest::mouseDClick(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(),
                       QPoint(50, view.viewport()->height() - lh / 2));
    QCOMPARE(spy.count(), 0);
}

// A real double-click is a press, a release, and then this event — so the drag the press
// armed is over by the time it arrives. It has to stay over: the gesture applies a
// filter, which replaces the record space, and a drag still live across that would
// extend from an anchor naming a record that has moved.
void TestLogView::aDoubleClickLeavesNoDragArmedBehindIt()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 60), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(900, 400);

    const int lh = view.fontMetrics().height();
    const auto rowAt = [&](int row) { return QPoint(50, row * lh + lh / 2); };

    // The real sequence: the press and release of the first click, then the second
    // press arriving as a double-click.
    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), rowAt(2));
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), rowAt(2));
    QTest::mouseDClick(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), rowAt(2));
    QCOMPARE(selectedRows(view), QVector<int>({2}));

    // A move with the button still down (which is where QTest::mouseDClick leaves it)
    // must not extend anything.
    dragMoveTo(view.viewport(), rowAt(7));
    QCOMPARE(selectedRows(view), QVector<int>({2}));
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), rowAt(2));
}

// --- Select All (SPEC.md §5) --------------------------------------------------------
//
// Two claims, and the second is the one that makes the command worth having: "all" is
// what is IN VIEW (invariant #6), so it narrows with the filters instead of reaching
// past them into the file. The first is that it costs one selection range whatever the
// count, and that it does not move the reader.
void TestLogView::selectAllTakesEveryRecordAndLeavesTheReaderWhereTheyAre()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 60), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    // Somewhere in the middle, with a focused record, so "did not move" can fail.
    view.setCurrentRecord(45);
    const int scrollBefore = view.verticalScrollBar()->value();
    QVERIFY(scrollBefore > 0);

    // Through the key, since Ctrl+A is the whole of how anyone will reach it.
    QTest::keyClick(&view, Qt::Key_A, Qt::ControlModifier);

    const QVector<int> rows = selectedRows(view);
    QCOMPARE(rows.size(), 60);
    QCOMPARE(rows.first(), 0);
    QCOMPARE(rows.last(), 59);
    // ONE range covering the lot — a four-million-record log must not build four
    // million anything here.
    QCOMPARE(view.selectionModel()->selection().size(), 1);
    // Selecting everything says nothing about where the reader wants to be, so neither
    // the scroll position nor the focused record moves.
    QCOMPARE(view.verticalScrollBar()->value(), scrollBefore);
    QCOMPARE(view.currentRecord(), 45);
}

void TestLogView::selectAllStopsAtWhatTheFilterLeftVisible()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 60), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    // Priority >= WARN keeps every 4th record: 15 of the 60.
    refilter(view, model, doc, everyFourth());
    QCOMPARE(view.recordCount(), 15);

    view.selectAllRecords();
    const QVector<int> rows = selectedRows(view);
    QCOMPARE(rows.size(), 15);
    QCOMPARE(rows.last(), 14);
    // View rows, and every one of them is a record the filter kept — which is what the
    // copy commands then act on.
    for (int r : rows)
        QCOMPARE(doc.filtered().sourceRow(r) % 4, 0);

    // A filter that matches nothing leaves nothing to select, and the command is inert
    // rather than wrong.
    refilter(view, model, doc, messageContaining(QStringLiteral("no record says this")));
    QCOMPARE(view.recordCount(), 0);
    view.selectAllRecords();
    QVERIFY(selectedRows(view).isEmpty());

    // Widening it again puts the whole file back within reach.
    refilter(view, model, doc, FilterSet());
    view.selectAllRecords();
    QCOMPARE(selectedRows(view).size(), 60);
}

// --- the matched substring is marked (SPEC.md §5, ARCHITECTURE.md §7.1.4) -----
//
// Find selects a RECORD; on a long message that still leaves the reader hunting for the
// words that matched. The mark is the matched run painted in the record's own two
// colours SWAPPED, so it is read off the rendered viewport and nowhere else — every
// widget involved holds the same value marked or not, and only the pixels differ.
//
// Every case here needs a resolved font: the whole question is where a character landed,
// and with no font at all (Windows offscreen, which ships none) nothing measures.

namespace {

// Render a view's viewport, the way the banding case does.
QImage renderViewport(LogView &view)
{
    QImage img(view.viewport()->size(), QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    view.viewport()->render(&img, QPoint(), QRegion(), QWidget::DrawWindowBackground);
    return img;
}

} // namespace

// How many columns of `band` are painted in `c` over at least `minShare` of the band's
// height. A mark FILLS its run with the colour the text is otherwise drawn IN, so what
// tells the two apart is coverage down the line box rather than the colour itself: a
// glyph inks part of the height, a mark inks all of it but for the glyphs it inverts.
int TestLogView::markedColumns(const QImage &img, const QRect &band, const QColor &c,
                               double minShare)
{
    int columns = 0;
    const int height = band.height();
    if (height <= 0)
        return 0;
    for (int x = qMax(0, band.left()); x <= band.right() && x < img.width(); ++x) {
        int inked = 0;
        for (int y = qMax(0, band.top()); y <= band.bottom() && y < img.height(); ++y)
            if (isColour(img.pixel(x, y), c))
                ++inked;
        if (double(inked) >= minShare * height)
            ++columns;
    }
    return columns;
}

void TestLogView::whatFindMatchedIsMarkedInsideTheRecordsOnScreen()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; where a character landed is unanswerable");

    QTemporaryFile file;
    QVERIFY(writeLog(file,
                     "2026-07-21 10:00:00,000 [main] INFO  net.socket - opened the socket\n"
                     "2026-07-21 10:00:01,000 [main] INFO  net.socket - found a needle here\n"
                     "2026-07-21 10:00:02,000 [main] INFO  net.socket - closed the socket\n"
                     "2026-07-21 10:00:03,000 [main] INFO  net.socket - another needle now\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.size(), 4);

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(900, 400);

    const QColor text = view.palette().text().color();
    const int lh = view.fontMetrics().height();
    const int cw = qMax(1, view.fontMetrics().averageCharWidth());
    const int msgCol = columnOfRole(doc, FieldRole::Message);
    QVERIFY(msgCol >= 0);
    const int msgX = view.header()->sectionViewportPosition(msgCol);
    // One physical line per record and wrap off, so view row r owns exactly one line box.
    auto rowBand = [&](int r) {
        return QRect(msgX, r * lh, view.viewport()->width() - msgX - 1, lh);
    };

    // Nothing is being searched for, so nothing is marked: every column of every row is
    // glyphs on the row's fill, and no column is inked top to bottom.
    QImage img = renderViewport(view);

    TextMatcher matcher;
    matcher.set(QStringLiteral("needle"), /*regex=*/false, Qt::CaseInsensitive);
    view.setFindMatcher(matcher);

    img = renderViewport(view);
    // The two records carrying the word wear a filled run; the two that do not are
    // untouched — the mark is per match, not per row.
    QVERIFY2(markedColumns(img, rowBand(1), text) >= 2 * cw, "the match was not marked");
    QVERIFY2(markedColumns(img, rowBand(3), text) >= 2 * cw,
             "only the first matching record was marked");
    QVERIFY2(markedColumns(img, rowBand(0), text) < cw, "a record with no match was marked");
    QVERIFY2(markedColumns(img, rowBand(2), text) < cw, "a record with no match was marked");

    // A scroll repaints, and the marks are re-derived from the query rather than stored,
    // so they come back on whatever is now on screen.
    view.verticalScrollBar()->setValue(2);
    img = renderViewport(view);
    QVERIFY2(markedColumns(img, rowBand(1), text) >= 2 * cw,
             "the mark did not survive a scroll"); // record 3, now the second row shown
    QVERIFY(markedColumns(img, rowBand(0), text) < cw);
    view.verticalScrollBar()->setValue(0);

    // A query nobody typed marks nothing, and the previous one leaves nothing behind.
    view.clearFindMatcher();
    img = renderViewport(view);
    for (int r = 0; r < 4; ++r)
        QVERIFY2(markedColumns(img, rowBand(r), text) < cw, "a cleared query left its marks");

    // The case option is the SEARCH's, read off the one matcher: a case-sensitive
    // query for the capitalised form marks nothing here.
    matcher.set(QStringLiteral("NEEDLE"), /*regex=*/false, Qt::CaseSensitive);
    view.setFindMatcher(matcher);
    img = renderViewport(view);
    for (int r = 0; r < 4; ++r)
        QVERIFY2(markedColumns(img, rowBand(r), text) < cw, "the case option was not the search's");

    // And the regex option likewise — the same query, matched the other way.
    matcher.set(QStringLiteral("n[e]+dle"), /*regex=*/true, Qt::CaseInsensitive);
    view.setFindMatcher(matcher);
    img = renderViewport(view);
    QVERIFY(markedColumns(img, rowBand(1), text) >= 2 * cw);

    // The record that WRAPS under SelectedRecordOnly is drawn by the other half of the
    // paint path, and it marks too — in the selection's own pair swapped, since those
    // are the two colours that record is wearing.
    view.setWrapMode(LogView::WrapMode::SelectedRecordOnly);
    view.setCurrentRecord(1);
    img = renderViewport(view);
    const QColor selected = view.palette().highlightedText().color();
    QVERIFY2(markedColumns(img, rowBand(1), selected) >= 2 * cw,
             "the selected, wrapping record did not show the mark");
}

void TestLogView::aRuleColouredRecordShowsItsColourAndTheMarkTogether()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; where a character landed is unanswerable");

    QTemporaryFile file;
    QVERIFY(writeLog(file,
                     "2026-07-21 10:00:00,000 [main] INFO  net.socket - quiet line\n"
                     "2026-07-21 10:00:01,000 [main] INFO  net.socket - special needle line\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(900, 300);

    HighlightRule rule;
    rule.match.text.enabled = true;
    rule.match.text.matcher.set(QStringLiteral("special"), /*regex=*/false, Qt::CaseInsensitive);
    rule.background = 0;
    rule.foreground = HighlightPalette::readableTextSlot(0);
    doc.highlighters().rules.append(rule);
    doc.refreshHighlighting();

    const QColor ruleBg = HighlightPalette::color(rule.background, model.darkTheme());
    const QColor ruleFg = HighlightPalette::color(rule.foreground, model.darkTheme());
    QVERIFY(ruleBg.isValid() && ruleFg.isValid());

    TextMatcher matcher;
    matcher.set(QStringLiteral("needle"), /*regex=*/false, Qt::CaseInsensitive);
    view.setFindMatcher(matcher);

    const QImage img = renderViewport(view);
    const int lh = view.fontMetrics().height();
    const int cw = qMax(1, view.fontMetrics().averageCharWidth());
    const int msgCol = columnOfRole(doc, FieldRole::Message);
    const QRect band(view.header()->sectionViewportPosition(msgCol), lh,
                     view.viewport()->width() - view.header()->sectionViewportPosition(msgCol) - 1,
                     lh);

    // The rule still colours the whole record — the rightmost column is past every
    // field's text, so what is there is the row fill and nothing else.
    QVERIFY2(isColour(img.pixel(img.width() - 1, lh + lh / 2), ruleBg),
             "the rule's colour was lost under the mark");
    // And the mark inside it is the rule's own PAIR swapped, so it is as readable as the
    // rule made the record and can never be a colour of its own choosing.
    QVERIFY2(markedColumns(img, band, ruleFg) >= 2 * cw,
             "a rule-coloured record did not show the mark");
}

void TestLogView::aMatchStraddlingAWrappedLineIsMarkedOnBothOfThem()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; where a character landed is unanswerable");

    // Two passes, because the crafted message has to be positioned against a column
    // layout that only exists once a view does: the first view is opened only to be
    // asked where the message column starts and how wide a character is.
    QTemporaryFile probeFile;
    QVERIFY(writeLog(probeFile,
                     "2026-07-21 10:00:00,000 [main] INFO  net.socket - filler\n"));
    Document probeDoc;
    const QString pattern =
        QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n");
    QVERIFY(probeDoc.open(probeFile.fileName(), pattern, Encoding::Utf8, QTimeZone::utc()));
    LogModel probeModel(&probeDoc);
    LogView probe(&probeDoc, &probeModel);
    probe.resize(900, 400);
    renderViewport(probe); // a resize on an unshown widget reaches the viewport when it paints
    const int msgCol = columnOfRole(probeDoc, FieldRole::Message);
    QVERIFY(msgCol >= 0);
    const int msgX = probe.header()->sectionViewportPosition(msgCol);
    // The advance of the character the filler is made of, not averageCharWidth: the
    // question here is where the PAINTED line breaks, and that is decided by the
    // advance of the glyphs actually on it.
    const int cw = qMax(1, probe.fontMetrics().horizontalAdvance(QStringLiteral("x")));
    const int cols = qMax(1, (probe.viewport()->width() - msgX) / cw);
    QVERIFY(cols > 30);

    // One record whose message carries the token exactly once, straddling the wrap: it
    // starts seven characters before the break and runs fifteen, so a break anywhere
    // within seven characters of where the model puts it still falls inside the token.
    const QByteArray token = "ZEBRACROSSINGXX"; // 15 characters, once in the file
    QByteArray message = QByteArray("x").repeated(cols - 7) + token
                       + QByteArray("x").repeated(30);
    QTemporaryFile file;
    QVERIFY(writeLog(file, "2026-07-21 10:00:00,000 [main] INFO  net.socket - " + message + "\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(), pattern, Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.size(), 1);

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(900, 400);
    renderViewport(view); // as for the probe: the resize reaches the viewport when it paints
    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    view.measureBlockOfRecord(0);
    // The same column layout and the same width the token was positioned against.
    QCOMPARE(view.header()->sectionViewportPosition(msgCol), msgX);
    QCOMPARE(view.viewport()->width(), probe.viewport()->width());

    TextMatcher matcher;
    matcher.set(QString::fromLatin1(token), /*regex=*/false, Qt::CaseSensitive);
    view.setFindMatcher(matcher);

    const QImage img = renderViewport(view);
    const QColor text = view.palette().text().color();
    const int lh = view.fontMetrics().height();
    const QRect area(msgX, 0, view.viewport()->width() - msgX - 1, lh);

    // The one occurrence is marked on TWO display lines: the end of the first and the
    // start of the second. A mark drawn from a single rectangle would show on one.
    int linesMarked = 0;
    for (int line = 0; line * lh + lh <= view.viewport()->height(); ++line) {
        if (markedColumns(img, area.translated(0, line * lh), text) >= cw)
            ++linesMarked;
    }
    QCOMPARE(linesMarked, 2);
}

// --- Zoom (SPEC.md §5, ARCHITECTURE.md §7.1.5) -------------------------------
//
// The log text size is ONE application-wide point size (Fonts.h), pushed into every
// view as an ordinary QWidget font. What these pin is the half that is the view's: that
// everything derived from the line height and the character advance is invalidated when
// the font moves, and that the reader is left where they were reading.
//
// Each case puts the size back, because it is a process-wide setting and the cases below
// it in this binary measure fonts.

namespace {
// Set the log text size and push it into `view`, exactly as MainWindow does — via the
// widget's font, which is the only channel LogView notices (QEvent::FontChange).
void zoomTo(LogView &view, int points)
{
    setLogFontPointSize(points);
    view.setFont(logTextFont());
}

struct FontSizeGuard
{
    ~FontSizeGuard() { resetLogFontPointSize(); }
};
} // namespace

void TestLogView::theLogTextSizeStopsAtBothBoundsAndComesBackOnReset()
{
    FontSizeGuard guard;

    const int base = defaultLogFontPointSize();
    QVERIFY(base >= kMinLogFontPointSize && base <= kMaxLogFontPointSize);

    // Asking past either end lands ON it rather than being refused, so a held key walks
    // to the bound and stops there...
    QVERIFY(setLogFontPointSize(1000));
    QCOMPARE(logFontPointSize(), kMaxLogFontPointSize);
    // ...and the next press is a no-op, which is what tells the window not to re-font
    // every view and not to rewrite the setting.
    QVERIFY(!setLogFontPointSize(kMaxLogFontPointSize + 5));

    QVERIFY(setLogFontPointSize(-20));
    QCOMPARE(logFontPointSize(), kMinLogFontPointSize);
    QVERIFY(!setLogFontPointSize(0));

    // Reset is back to the platform's own size, in the platform's own unit: the font
    // is monospaceFont() itself again, not that size rounded to a point.
    QVERIFY(resetLogFontPointSize());
    QCOMPARE(logFontPointSize(), base);
    QCOMPARE(logTextFont(), monospaceFont());
    QVERIFY(!resetLogFontPointSize()); // already there
}

// The line height is what a zoom is FOR, and the view's page step is that height read
// back through the geometry: a viewport of a fixed pixel height holds fewer display
// lines in a bigger font. Nothing else in the exact path moves — a record's line COUNT
// is a property of the text, not of the font — which is why this is the assertion.
void TestLogView::aBiggerFontFitsFewerRecordsInTheSameViewport()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; a point size buys no pixels here");
    FontSizeGuard guard;

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 200), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400);

    zoomTo(view, 9);
    renderViewport(view); // a resize on an unshown widget reaches the viewport when it paints
    const int viewportHeight = view.viewport()->height();
    const int smallPage = view.verticalScrollBar()->pageStep();
    QVERIFY(smallPage > 0);

    zoomTo(view, 20);
    renderViewport(view);
    const int bigPage = view.verticalScrollBar()->pageStep();
    QVERIFY2(bigPage < smallPage,
             "a bigger font drew the same number of lines in the same viewport");

    // The header band grows with the font too — it renders in the same one — so the
    // viewport itself is a little shorter at 20 pt, which only sharpens the claim.
    QVERIFY(view.viewport()->height() <= viewportHeight);

    // And back down again, to exactly what it was: every quantity here is recomputed
    // from the font rather than adjusted, the header band included.
    zoomTo(view, 9);
    renderViewport(view);
    QCOMPARE(view.viewport()->height(), viewportHeight);
    QCOMPARE(view.verticalScrollBar()->pageStep(), smallPage);
}

// THE ONE THAT CATCHES A STALE ESTIMATOR. Under AlwaysOn every measured height is keyed
// by the column count, which is the message column's width divided by the character
// advance — so a font change invalidates every one of them. ensureEstimatorBound() will
// not notice: it rebinds on the index's ADDRESS and its block count, and a font moves
// neither.
void TestLogView::aZoomDropsTheWrappedHeightsMeasuredAtTheOldFont()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; every point size has the same advance");
    FontSizeGuard guard;

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 40), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400);
    zoomTo(view, 9);
    view.setWrapMode(LogView::WrapMode::AlwaysOn);

    const EstimatedGeometry &g = view.estimatedGeometry();
    view.measureBlockOfRecord(0);
    QVERIFY(g.isBlockMeasured(0));
    const int smallCols = g.columns();
    QVERIFY(smallCols > 0);

    zoomTo(view, 20);
    // Wider characters, so fewer of them across the same column...
    QVERIFY2(g.columns() < smallCols, "the estimator kept the old character count");
    // ...and every height measured at the old count is gone rather than being reported
    // as fact at a width where it is wrong.
    QVERIFY2(!g.isBlockMeasured(0), "the estimator kept heights measured at the old font");

    // Measuring again at the new size gives taller records: the same characters over
    // fewer columns is more visual lines.
    const qint64 wideTotal = g.totalLines();
    view.measureBlockOfRecord(0);
    QVERIFY(g.isBlockMeasured(0));
    QVERIFY(g.totalLines() >= wideTotal);
}

// A zoom is a change of size, not of place. In the exact path a record's first line does
// not move at all, so the scroll position is simply kept; under AlwaysOn the whole line
// space is re-scaled, and what is kept is the RECORD at the top of the viewport — the
// same anchor the debounced resize uses, and deliberately not the filter bracket's
// source ordinals, which exist because a refilter replaces the record space.
void TestLogView::aZoomLeavesTheReaderOnTheRecordTheyWereReading()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; no size change reaches the geometry");
    FontSizeGuard guard;

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 200), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400);
    zoomTo(view, 9);

    // Detached from the tail and parked in the middle of the file: following the tail
    // outranks the anchor, so a view still following would legitimately move.
    QScrollBar *sb = view.verticalScrollBar();
    sb->setValue(sb->maximum() / 2);
    QVERIFY(!view.following());
    const int exactLine = sb->value();

    zoomTo(view, 20);
    QCOMPARE(sb->value(), exactLine); // exact path: the line space did not move
    QVERIFY(!view.following());       // and the re-anchor is not read as the reader scrolling

    // AlwaysOn: the line space DOES move, so the assertion is about the record.
    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    view.measureBlockOfRecord(0);
    const EstimatedGeometry &g = view.estimatedGeometry();
    sb->setValue(sb->maximum() / 2);
    const int topRecord = g.recordAtLine(sb->value());
    QVERIFY(topRecord > 0);

    zoomTo(view, 12);
    QCOMPARE(g.recordAtLine(sb->value()), topRecord);
    QVERIFY(!view.following());
}

// A zoom re-seeds the columns nobody has spoken for — they hold text, and the text just
// changed size — and leaves alone any width the user set, which is a statement about
// their layout and not about the font (SPEC.md §5).
void TestLogView::aZoomWidensTheSeededColumnsAndLeavesADraggedOneAlone()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; every seed lands on the same floor");
    FontSizeGuard guard;

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 20), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(900, 400);
    zoomTo(view, 9);

    const int seeded = columnOfRole(doc, FieldRole::Priority);
    const int dragged = columnOfRole(doc, FieldRole::Logger);
    QVERIFY(seeded >= 0 && dragged >= 0);

    const int seededWas = view.header()->sectionSize(seeded);
    view.header()->resizeSection(dragged, 313); // the user, dragging a divider
    QCOMPARE(view.header()->sectionSize(dragged), 313);

    zoomTo(view, 20);
    QVERIFY2(view.header()->sectionSize(seeded) > seededWas,
             "a seeded column did not grow with the font");
    QCOMPARE(view.header()->sectionSize(dragged), 313);
}

// Ctrl+wheel is REPORTED, never acted on: the size is one application-wide setting, so
// a view that re-fonted itself would leave every other open view behind. A plain wheel
// is untouched and still scrolls.
void TestLogView::ctrlWheelAsksForAZoomAndAPlainWheelStillScrolls()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 200), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400);
    renderViewport(view);

    QSignalSpy zoom(&view, &LogView::zoomStepRequested);
    const QPointF pos(100, 100);
    const auto wheel = [&](Qt::KeyboardModifiers mods, int delta) {
        QWheelEvent e(pos, view.viewport()->mapToGlobal(pos.toPoint()), QPoint(),
                      QPoint(0, delta), Qt::NoButton, mods, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view.viewport(), &e);
    };

    // One mouse notch up is one step up. The font itself does not move — that is the
    // window's answer to this signal, not the view's.
    const QFont before = view.font();
    wheel(Qt::ControlModifier, 120);
    QCOMPARE(zoom.size(), 1);
    QCOMPARE(zoom.takeFirst().at(0).toInt(), 1);
    QCOMPARE(view.font(), before);

    // A trackpad's stream of small deltas ADDS UP to the same gesture rather than being
    // rounded away: four quarter-notches are one step, and the fifth is not a second one.
    for (int i = 0; i < 4; ++i)
        wheel(Qt::ControlModifier, 30);
    QCOMPARE(zoom.size(), 1);
    QCOMPARE(zoom.takeFirst().at(0).toInt(), 1);

    // And with no Ctrl the wheel is the scroll it always was.
    QScrollBar *sb = view.verticalScrollBar();
    sb->setValue(200);
    wheel(Qt::NoModifier, -120);
    QCOMPARE(zoom.size(), 0);
    QVERIFY2(sb->value() != 200, "a plain wheel stopped scrolling the log");
}


// --- copying a selection is bounded (SPEC.md §5, ARCHITECTURE.md §7.1.6) ------------
//
// The copy path is the one place that spends the whole selection's text at once, and
// Select All puts it one keystroke away on a file of any size. What is asserted here is
// that the bound changed nothing about what an ordinary copy produces — the range walk
// answers what selectedRows() answered, including for a Ctrl+clicked selection of
// several ranges — and that above the threshold the copy says so, can be stopped, and
// leaves the clipboard alone when it is.

namespace {

// The offscreen platform serves an in-process clipboard, which is what these read back.
// A platform whose clipboard does not round-trip has nothing to say about this path.
bool clipboardRoundTrips()
{
    QClipboard *cb = QApplication::clipboard();
    cb->setText(QStringLiteral("loftail-clipboard-probe"));
    return cb->text() == QStringLiteral("loftail-clipboard-probe");
}

// Copy-as-columns exactly as it was built BEFORE the bound: a grid of flattened cells
// through the two public builders. The oracle for "the output did not move".
QString expectedColumns(const LogView &view, const LogModel &model, const QVector<int> &rows)
{
    QVector<QVector<QString>> grid;
    for (int r : rows) {
        QVector<QString> cells;
        for (int vi = 0; vi < model.columnCount(); ++vi) {
            const int logical = view.header()->logicalIndex(vi);
            if (logical < 0 || view.header()->isSectionHidden(logical))
                continue;
            cells << LogView::flattenCell(model.cellText(r, logical));
        }
        grid << cells;
    }
    return LogView::columnsToTsv(grid);
}

QString joinedRawLines(const QStringList &lines, const QVector<int> &rows)
{
    QStringList wanted;
    for (int r : rows)
        wanted << lines.at(r);
    return wanted.join(QLatin1Char('\n'));
}

} // namespace

void TestLogView::aSmallCopyYieldsTheSelectedRecordsAndNothingElse()
{
    if (!clipboardRoundTrips())
        QSKIP("this platform's clipboard does not round-trip in process");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 10), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    // Well under the threshold, so this is the plain synchronous loop with no dialog
    // and no event processing at all — the case every other test in the tree relies on.
    QVERIFY(view.copyProgressThreshold() > 10);

    view.setCurrentRecord(2);
    view.setCurrentRecord(4, true);
    QCOMPARE(selectedRows(view), QVector<int>({2, 3, 4}));

    view.copySelectionRaw();
    QCOMPARE(QApplication::clipboard()->text(),
             joinedRawLines(rawLines(10), {2, 3, 4}));
    QVERIFY(view.findChild<QProgressDialog *>(QStringLiteral("copyProgress")) == nullptr);

    view.copySelectionAsColumns();
    QCOMPARE(QApplication::clipboard()->text(), expectedColumns(view, model, {2, 3, 4}));

    // Nothing selected copies the FOCUSED record, which is the fallback the per-row
    // form carried and the range walk had to keep.
    view.selectionModel()->clearSelection();
    view.copySelectionRaw();
    QCOMPARE(QApplication::clipboard()->text(), rawLines(10).at(4));
}

void TestLogView::aCtrlClickedSelectionCopiesEveryRangeItNamesExactlyOnce()
{
    if (!clipboardRoundTrips())
        QSKIP("this platform's clipboard does not round-trip in process");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 12), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    const int lh = view.fontMetrics().height();
    QVERIFY(view.viewport()->height() > lh * 9);
    const auto rowAt = [&](int row) {
        return QPoint(50, (row - view.verticalScrollBar()->value()) * lh + lh / 2);
    };

    // Three ranges, two of which touch: 1-2 built by a Shift+click, 3 added on its own
    // so it abuts them, and 7 far away. A record named by two ranges must still be
    // copied once, which answering in rows gave for free and merging is what keeps.
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), rowAt(1));
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::ShiftModifier, rowAt(2));
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::ControlModifier, rowAt(3));
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::ControlModifier, rowAt(7));
    const QVector<int> rows = selectedRows(view);
    QCOMPARE(rows, QVector<int>({1, 2, 3, 7}));

    // The selection MODEL is the oracle: what the copy walks must be what it reports.
    view.copySelectionRaw();
    QCOMPARE(QApplication::clipboard()->text(), joinedRawLines(rawLines(12), rows));

    view.copySelectionAsColumns();
    QCOMPARE(QApplication::clipboard()->text(), expectedColumns(view, model, rows));

    // And a record taken back out is not copied.
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::ControlModifier, rowAt(2));
    QCOMPARE(selectedRows(view), QVector<int>({1, 3, 7}));
    view.copySelectionRaw();
    QCOMPARE(QApplication::clipboard()->text(), joinedRawLines(rawLines(12), {1, 3, 7}));
}

void TestLogView::aCopyBigEnoughToWaitForOffersToStopAndCopiesTheSameText()
{
    if (!clipboardRoundTrips())
        QSKIP("this platform's clipboard does not round-trip in process");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 10), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);

    view.selectAllRecords();
    view.copySelectionRaw();
    const QString unbounded = QApplication::clipboard()->text();
    QCOMPARE(unbounded, joinedRawLines(rawLines(10), {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));

    // The threshold is a record COUNT, so it can be driven at ten rather than at the
    // four million it ships for. Above it the copy announces itself and can be stopped.
    view.setCopyProgressThreshold(2);
    QApplication::clipboard()->setText(QStringLiteral("untouched"));

    bool sawDialog = false;
    bool cancelWasOffered = false;
    QTimer::singleShot(0, &view, [&]() {
        if (QProgressDialog *dlg =
                view.findChild<QProgressDialog *>(QStringLiteral("copyProgress"))) {
            sawDialog = true;
            cancelWasOffered = !dlg->wasCanceled();
        }
    });
    view.copySelectionRaw();

    QVERIFY2(sawDialog, "a copy above the threshold showed no progress dialog");
    QVERIFY(cancelWasOffered);
    // Same text, whichever side of the threshold it was copied on.
    QCOMPARE(QApplication::clipboard()->text(), unbounded);
    // And the dialog is gone with the copy, not left parented on the view.
    QVERIFY(view.findChild<QProgressDialog *>(QStringLiteral("copyProgress")) == nullptr);
}

void TestLogView::cancellingACopyLeavesTheClipboardAsItWas()
{
    if (!clipboardRoundTrips())
        QSKIP("this platform's clipboard does not round-trip in process");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openMixedLog(doc, file, 10), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.setWrapMode(LogView::WrapMode::Off);
    view.resize(700, 400);
    view.setCopyProgressThreshold(0); // every copy takes the bounded path

    view.selectAllRecords();

    // Pressing Cancel is what this posts, one event pass into the copy. The clipboard
    // is written once at the end, so a cancelled copy leaves it whole rather than half
    // filled — including a cancel that lands after the last record was read.
    const QString before = QStringLiteral("what was on the clipboard before");
    for (int i = 0; i < 2; ++i) {
        QApplication::clipboard()->setText(before);
        QTimer::singleShot(0, &view, [&]() {
            if (QProgressDialog *dlg =
                    view.findChild<QProgressDialog *>(QStringLiteral("copyProgress")))
                dlg->cancel();
        });
        if (i == 0)
            view.copySelectionRaw();
        else
            view.copySelectionAsColumns();
        QCOMPARE(QApplication::clipboard()->text(), before);
    }
}

#include "tst_logview.moc"
