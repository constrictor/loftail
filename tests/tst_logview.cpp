#include <QtTest>

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHeaderView>
#include <QImage>
#include <QScrollBar>
#include <QHelpEvent>
#include <QItemSelectionModel>
#include <QPaintEngine>
#include <QProgressDialog>
#include <QProxyStyle>
#include <QSignalSpy>
#include <QStyle>
#include <QStyleOptionHeader>
#include <QTemporaryFile>
#include <QTextLayout>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>

#include "Document.h"
#include "Filter.h"
#include "Highlight.h"
#include "LiveController.h"
#include "LogFormat.h"
#include "LogModel.h"
#include "Fonts.h"
#include "LogView.h"
#include "Palette.h"
#include "Priority.h"
#include "Record.h"
#include "RecordIndex.h"
#include "UiColors.h"

#include <limits>
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
    void anAppendIntoAMeasuredBlockIsMeasuredAndNotReadPastTheEndOfIt();
    void aTrailingRecordThatGrowsInPlaceGrowsTheViewWithIt();
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
    void everyCaptionFitsTheRectTheStylePutsItIn();
    void aFittedColumnFitsItsCaptionUnderAWideHeaderMargin();
    void theTimeColumnOpensWideEnoughForItsOwnTimestampAtEveryZoom();
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

    // --- the line pitch is the one Qt lays out at (ARCHITECTURE.md §7.1.1) ------
    void everyWrappedLineOfARecordIsDrawnInsideTheRowItWasGiven();
    void aSelectedRecordIsGivenExactlyTheLinesItsWrappedTextTakes();

    // --- both wrapped-cell paths break lines the same way (bugs.md 4) -----------
    void aTabbedRecordBreaksWhereItAlwaysDidWhenFindIsArmed();
    void theSelectedRecordBreaksWhereItAlwaysDidWhenFindIsArmed();

    // --- marking a cell costs one redraw of it, not one per match (bugs.md 12) ---
    void aMarkedCellIsRedrawnOncePerCellAndNotOncePerMatch();
    void everyMatchOfAWrappedCellIsMarkedWhenTheyAreBatched();
    void anElidedMarkSitsOverTheGlyphsOfTheRunAndNotOverALogicalPrefix();

    // --- the wrap width is remeasured wherever the message origin moves (bugs.md 9) ---
    void aMovedColumnRemeasuresEveryRowUnderAlwaysOn();
    void aMovedColumnRemeasuresTheSelectedRecord();
    void aHorizontalScrollRemeasuresEveryRowUnderAlwaysOn();

    // --- the wrap width has a floor, in characters (bugs.md 11) ----------------
    void aMessageColumnAtTheViewportEdgeStillFitsSeveralRecordsOnAScreen();
    void aRecordWhoseColumnIsPastTheEdgeIsOneOfSeveralBandsOnScreen();
    void theSelectedRecordIsLaidOutInTheWidthItWasMeasuredIn();

    // --- Zoom (SPEC.md §5, ARCHITECTURE.md §7.1.5) ------------------------------
    void theLogTextSizeStopsAtBothBoundsAndComesBackOnReset();
    void aBiggerFontFitsFewerRecordsInTheSameViewport();
    void aZoomDropsTheWrappedHeightsMeasuredAtTheOldFont();
    void aZoomLeavesTheReaderOnTheRecordTheyWereReading();
    void aZoomThatShrinksTheLineSpaceDoesNotStartFollowingAgain();
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

// THE ONE THAT USED TO READ PAST THE MEASURED BLOCK (bugs.md 1). A block is measured
// as a whole and cached as a per-record vector of heights, but the index it was
// measured from is LIVE (invariant #5) and grows under it — and the estimator used to
// rebind only when the BLOCK COUNT moved, which is once every 4096 records. So the
// first 4095 appends into a partly-filled block left a measured block answering out of
// a vector one entry short per appended record: an abort in a debug build, and an
// unchecked read into allocation slack in a release one.
//
// Driven through LiveController::checkNow(), which is the real ingest step, with a
// paint either side of it because painting is what measures and what reads back.
void TestLogView::anAppendIntoAMeasuredBlockIsMeasuredAndNotReadPastTheEndOfIt()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 60), qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.size(), 60);

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400);
    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    LiveController live(&doc, &model);

    auto paint = [&view]() {
        QPixmap pm(view.viewport()->size());
        view.viewport()->render(&pm); // invokes LogView::paintEvent synchronously
    };

    const EstimatedGeometry &g = view.estimatedGeometry();
    paint();
    QVERIFY(g.isBlockMeasured(0));                 // one block, measured whole
    QCOMPARE(g.measuredRecordsInBlock(0), 60);
    QCOMPARE(g.blockCount(), 1);                   // 60 records is far short of kBlockSize

    // One ordinary record appended: the block count does not move, and the block the
    // estimator holds is now one record shorter than the block it describes.
    file.write(makeLog(1));
    file.flush();
    live.checkNow();
    QCOMPARE(doc.index().records.size(), 61);
    QCOMPARE(g.blockCount(), 1);

    // Before anything re-measures, the appended record is ESTIMATED rather than read
    // out of the short vector — this is the read that used to go off the end.
    const int newHeight = g.recordHeightLines(60);
    QVERIFY2(newHeight >= 1 && newHeight <= int(RecordIndex::kDisplayLineCap),
             qPrintable(QStringLiteral("record 60 answered %1 display lines").arg(newHeight)));
    // The measured prefix kept the 59 records nothing can have touched, so the next
    // paint decodes two records and not the whole block. Record 59 is given back with
    // the new one because one tick may both grow the trailing record and append after
    // it, and the estimator is told only what the index looks like now.
    QCOMPARE(g.measuredRecordsInBlock(0), 59);
    QVERIFY(!g.isBlockMeasured(0));

    // And after the paint the block is whole again and the mapping is exact end to end.
    paint();
    QVERIFY(g.isBlockMeasured(0));
    QCOMPARE(g.measuredRecordsInBlock(0), 61);
    qint64 acc = 0;
    for (int r = 0; r < 61; ++r) {
        QCOMPARE(g.firstLineOfRecord(r), acc);
        const int h = g.recordHeightLines(r);
        QVERIFY(h >= 1 && h <= int(RecordIndex::kDisplayLineCap));
        acc += h;
    }
    QCOMPARE(acc, g.totalLines());
    QCOMPARE(g.totalLines(), g.firstLineOfRecord(61));
}

// The same seam, the other trigger. Continuation lines appended to the record already
// at the end of the file grow it in place: no row is inserted, so the record COUNT does
// not move either, and tracking only the count would leave this one exactly as broken.
// The record went on being drawn at its stale height with the new lines clipped, and
// the scroll range never grew far enough to reach them.
void TestLogView::aTrailingRecordThatGrowsInPlaceGrowsTheViewWithIt()
{
    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 60), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    view.resize(700, 400);
    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    LiveController live(&doc, &model);

    auto paint = [&view]() {
        QPixmap pm(view.viewport()->size());
        view.viewport()->render(&pm);
    };

    const EstimatedGeometry &g = view.estimatedGeometry();
    paint();
    QVERIFY(g.isBlockMeasured(0));
    const int staleHeight = g.recordHeightLines(59);
    const int staleMax = view.verticalScrollBar()->maximum();
    QCOMPARE(doc.index().records.at(59).lineCount, quint16(1));

    // Twenty lines that do NOT match recordStartRe, so they attach to record 59
    // (invariant #2) rather than starting records of their own.
    QByteArray grown;
    for (int i = 0; i < 20; ++i)
        grown += "    at some.frame(Source.java:1)\n";
    file.write(grown);
    file.flush();
    live.checkNow();

    QCOMPARE(doc.index().records.size(), 60);                    // no new row
    QCOMPARE(doc.index().records.at(59).lineCount, quint16(21));  // twenty lines taller

    // The height the view answers with follows the record, and the scroll range grows
    // to hold it — both WITHOUT waiting for a paint, because handleTailChanged() is
    // where a live append lands and it is the scrollbar it has to fix.
    QVERIFY2(g.recordHeightLines(59) > staleHeight,
             "the trailing record kept the height it was measured at");
    QVERIFY2(view.verticalScrollBar()->maximum() > staleMax,
             "the scroll range never grew to reach the appended lines");

    // Re-measured, the last record is at least its twenty-one physical lines and the
    // mapping is exact again.
    paint();
    QVERIFY(g.isBlockMeasured(0));
    QVERIFY(g.recordHeightLines(59) >= 21);
    QCOMPARE(g.totalLines(), g.firstLineOfRecord(60));
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

    // The strip read below is the one past the right edge of the last column, and it
    // has to be MADE: a section is filled by its own text, and where no font resolves
    // (Windows offscreen ships none) every glyph is drawn as a box the full height of
    // the line, so the message column's text reaches the last pixel of its section
    // rather than stopping a few characters in. Narrowing every section leaves a strip
    // of nothing but row fill, which is what the band is a property of — so the whole
    // case below is asked in the same terms with a font and without one.
    view.header()->setMinimumSectionSize(10); // the style's floor is font-derived too
    for (int c = 0; c < view.header()->count(); ++c)
        view.header()->resizeSection(c, 40);
    QVERIFY(view.header()->length() < view.viewport()->width());

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

// The case the one above cannot see. A header section is not its label: the style
// spends PM_HeaderMargin either side before the caption starts, and a caption that fits
// the SECTION can still be the one painted "Priori…". Breeze — the style on the
// reference KDE desktop — spends 6 px a side against Fusion's 2, and this suite resolves
// Fusion, so the margin is forced here: without the proxy every assertion below passes
// against the unfixed code, which is exactly how the bug survived nine milestones.

namespace {

class WideHeaderMarginStyle : public QProxyStyle
{
public:
    int pixelMetric(PixelMetric metric, const QStyleOption *option,
                    const QWidget *widget) const override
    {
        if (metric == PM_HeaderMargin)
            return qMax(6, QProxyStyle::pixelMetric(metric, option, widget));
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
};

// Empty when the caption fits the rect the STYLE leaves for it, and otherwise why not —
// measured exactly as LogView::truncatedHeaderText() measures it, since that is the
// function that decides whether the header is showing a truncation.
QString captionOverflow(LogView &view, const LogModel &model, int column)
{
    QHeaderView *h = view.header();
    if (h->isSectionHidden(column))
        return {};
    const QString text = model.headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
    if (text.isEmpty())
        return {};
    QStyleOptionHeader opt;
    opt.initFrom(h);
    opt.orientation = Qt::Horizontal;
    opt.section = column;
    opt.text = text;
    opt.rect = QRect(h->sectionViewportPosition(column), 0, h->sectionSize(column),
                     qMax(1, h->height()));
    const int label = h->style()->subElementRect(QStyle::SE_HeaderLabel, &opt, h).width();
    const int caption = h->fontMetrics().horizontalAdvance(text);
    if (caption <= label)
        return {};
    return QStringLiteral("column %1 (\"%2\") is %3 px wide, which leaves the style %4 px "
                          "for a caption needing %5 — it renders as \"%6\"")
        .arg(column)
        .arg(text)
        .arg(h->sectionSize(column))
        .arg(label)
        .arg(caption)
        .arg(h->fontMetrics().elidedText(text, Qt::ElideRight, label));
}

} // namespace

void TestLogView::everyCaptionFitsTheRectTheStylePutsItIn()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts available to this platform plugin; nothing measures");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 3), qPrintable(doc.lastError()));

    LogModel model(&doc);
    WideHeaderMarginStyle style; // declared first, so it outlives the view using it
    LogView view(&doc, &model);
    view.header()->setStyle(&style);
    view.seedColumnWidths(); // the seed MainWindow runs when the scan finishes

    for (int c = 0; c < model.columnCount(); ++c)
        QVERIFY2(captionOverflow(view, model, c).isEmpty(),
                 qPrintable(captionOverflow(view, model, c)));

    // Priority is the one the per-role allowance never rescues — "Priority" is 8
    // characters against an allowance of 5 — so the caption term is the whole of its
    // width and it is where the missing inset showed.
    const int priority = columnOfRole(doc, FieldRole::Priority);
    QVERIFY(priority >= 0);
    QVERIFY(captionOverflow(view, model, priority).isEmpty());
}

// Fit to Contents asks the same question of contentWidthOf(), and a fix confined to the
// seed would leave it: any column whose values are all shorter than its caption — Thread
// over a log written by one thread, and an Elapsed or a Location over short values —
// opens truncated immediately after being asked to show everything.
void TestLogView::aFittedColumnFitsItsCaptionUnderAWideHeaderMargin()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts available to this platform plugin; nothing measures");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 3), qPrintable(doc.lastError()));

    LogModel model(&doc);
    WideHeaderMarginStyle style;
    LogView view(&doc, &model);
    view.resize(900, 400);
    view.header()->setStyle(&style);
    view.fitColumnsToContents();

    for (int c = 0; c < model.columnCount(); ++c)
        QVERIFY2(captionOverflow(view, model, c).isEmpty(),
                 qPrintable(captionOverflow(view, model, c)));

    // And the claim is not vacuous: the Thread column's every value in this log is
    // "main", four characters against a six-character caption, so its fitted width is
    // the caption's and nothing else.
    const int thread = columnOfRole(doc, FieldRole::Thread);
    QVERIFY(thread >= 0);
    QVERIFY(view.fontMetrics().horizontalAdvance(model.cellText(0, thread))
            < view.fontMetrics().horizontalAdvance(
                model.headerData(thread, Qt::Horizontal, Qt::DisplayRole).toString()));
}

namespace {
struct LogFontRestore
{
    ~LogFontRestore() { resetLogFontPointSize(); }
};
} // namespace

// The seed's whole premise is that the per-role allowance is "what a TYPICAL value of
// the field takes", so a column that elides the very value it was seeded for is the
// premise failing. It did, at four sizes: the allowance was one glyph's INTEGER advance
// multiplied out, and 23 characters of that rounding came to a whole character — at
// 29 pt the Time column opened one pixel narrower than its own timestamp. Style-
// independent, unlike the caption half above.
void TestLogView::theTimeColumnOpensWideEnoughForItsOwnTimestampAtEveryZoom()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; a point size buys no pixels here");
    LogFontRestore restore;

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 3), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    const int time = columnOfRole(doc, FieldRole::Date);
    QVERIFY(time >= 0);
    const QString stamp = model.cellText(0, time);
    QVERIFY(!stamp.isEmpty());

    for (int pt = kMinLogFontPointSize; pt <= kMaxLogFontPointSize; ++pt) {
        setLogFontPointSize(pt);
        view.setFont(logTextFont()); // the one channel LogView notices; it re-seeds
        const int section = view.header()->sectionSize(time);
        const int text = view.fontMetrics().horizontalAdvance(stamp);
        QVERIFY2(section >= text,
                 qPrintable(QStringLiteral("at %1 pt the Time column opened at %2 px for a "
                                           "timestamp %3 px wide, which draws as \"%4\"")
                                .arg(pt)
                                .arg(section)
                                .arg(text)
                                .arg(view.fontMetrics().elidedText(stamp, Qt::ElideRight,
                                                                   section))));
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

    // Rendered twice: a resize on an unshown widget reaches the viewport when it paints,
    // so the first image is the pre-layout 640 px one and the rightmost pixel of it can
    // land inside the message text rather than past it — which it does under a style
    // with a wide header margin, since that pushes the message column's origin right.
    renderViewport(view);
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
// not notice: it rebinds on the index's ADDRESS and folds in tail growth, and a font
// moves neither.
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
    // Pin the columns before the message, which claims them from the zoom's re-seed:
    // the message column then keeps the same PIXELS at both sizes, so the character
    // count moves for the one reason this case is about. Left to the seed, a 20 pt
    // re-seed pushes the message origin past the right edge and both counts come back
    // as the kMinWrapCols floor (bugs.md 11), which says nothing about either font.
    view.header()->setMinimumSectionSize(4);
    for (int c = 0; c < view.header()->count() - 1; ++c)
        view.header()->resizeSection(c, 30);
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

// The other half of the anchor, and the one that shows only when the line space actually
// SHRINKS. Under AlwaysOn a smaller font fits more characters across the message column,
// so the whole estimated line space contracts — and Qt clamps the old scroll value into
// the narrower range. That clamp lands at the bottom, which reads as the reader having
// scrolled there, so a view they had deliberately detached starts following the tail and
// the zoom throws them to the end of the log instead of leaving them where they were.
void TestLogView::aZoomThatShrinksTheLineSpaceDoesNotStartFollowingAgain()
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
    zoomTo(view, 20);
    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    view.measureBlockOfRecord(0);

    QScrollBar *sb = view.verticalScrollBar();
    sb->setValue(sb->maximum() / 2);
    QVERIFY(!view.following());
    const int parked = sb->value();

    zoomTo(view, 12);
    // The precondition is the whole point of the case, and it is a property of the font
    // rather than of the code: with an 8 px advance at every size (a platform whose font
    // database resolves nothing but is not empty) the column count does not move and
    // there is no clamp to misread.
    if (sb->maximum() >= parked)
        QSKIP("this font's zoom did not narrow the range past the reader; nothing to clamp");
    QVERIFY2(!view.following(),
             "a zoom that narrowed the scroll range re-attached follow on a detached view");
    QVERIFY2(sb->value() < sb->maximum(), "a zoom threw the reader to the end of the log");
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

// --- the view's line pitch is the number Qt lays text out at -----------------
//
// §7.1.1's whole height model is `n` display lines of lineHeight() pixels, and the
// painter is what places them: QPainter::drawText() steps a wrapped cell by
// QTextLine::height(), which is ceil(ascentF + descentF). QFontMetrics::height() rounds
// the two SEPARATELY and comes out a pixel short at over half the sizes this font is
// offered at — so a wrapped record is given fewer pixels than its own text needs and the
// bottom of it is clipped, silently: a wrapped message is deliberately not elided and
// offers no tooltip (SPEC.md §5), so there is nothing on screen to say text is missing.
//
// Neither symptom is observable except in pixels, so both cases render the viewport and
// read the ink back. Both need a resolved font — with an empty font database (Windows
// offscreen, which ships none) there is no ascent to round — and both need a point size
// at which the two metrics actually DISAGREE: the offscreen default is one of the sizes
// where they happen to agree, which is exactly why nothing caught this.

namespace {

// The height Qt gives one laid-out line of `f` — the number drawWrappedCell's unmarked
// drawText steps by, and the one its marked QTextLayout path positions on. Measured
// rather than derived, so the case states the rule instead of restating the code.
int laidOutLineHeight(const QFont &f)
{
    QTextLayout layout(QStringLiteral("Ag"), f);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (!line.isValid()) {
        layout.endLayout();
        return 0;
    }
    line.setLineWidth(10000);
    layout.endLayout();
    return int(line.height());
}

// A point size at which QFontMetrics::height() and the pitch Qt lays out at differ, or 0
// where they agree everywhere in range. At a size where they agree the defect cannot
// show, which is exactly why nothing caught it: the offscreen default is one of those.
// Readable sizes are tried first — the cases below tell the painted lines apart by the
// gap between them, and at 6 pt a glyph inks the whole pitch and leaves none.
int aPointSizeWhereTheMetricsDisagree()
{
    const auto disagrees = [](int pt) {
        QFont f = monospaceFont();
        f.setPointSize(pt);
        const int laid = laidOutLineHeight(f);
        return laid > 0 && laid != QFontMetrics(f).height();
    };
    for (int pt = 9; pt <= 16; ++pt)
        if (disagrees(pt))
            return pt;
    for (int pt = kMinLogFontPointSize; pt < 9; ++pt)
        if (disagrees(pt))
            return pt;
    return 0;
}

// The rows of `area` that carry ink, as {firstRow, rowCount} runs — one run per painted
// text line. Everything that is not the row's own fill counts, antialiased edges
// included.
QVector<std::pair<int, int>> inkRuns(const QImage &img, const QRect &area, const QColor &fill)
{
    QVector<std::pair<int, int>> runs;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        bool ink = false;
        for (int x = area.left(); x <= area.right() && !ink; ++x) {
            const QRgb px = img.pixel(x, y);
            ink = !(qAbs(qRed(px) - fill.red()) <= 1 && qAbs(qGreen(px) - fill.green()) <= 1
                    && qAbs(qBlue(px) - fill.blue()) <= 1);
        }
        if (!ink)
            continue;
        if (!runs.isEmpty() && runs.last().first + runs.last().second == y)
            ++runs.last().second;
        else
            runs.append({y, 1});
    }
    return runs;
}

// One record whose message is `chars` characters of ordinary words: long enough to wrap
// to many lines, and with nothing tall enough to close the gap between two of them.
QByteArray makeOneLongRecord(int chars)
{
    QByteArray msg;
    while (msg.size() < chars)
        msg += "connection reset by peer on socket ";
    msg.truncate(chars);
    return "2026-07-21 14:32:05,123 [main] INFO  net.socket - " + msg + "\n";
}

// Put the message column where a `chars`-character message wraps to about `targetLines`
// of them, whatever the font resolved to: the wrap width is what decides the count, and
// the count has to clear lineHeight() for the missing pixel to add up to a whole lost
// line while staying under the 100-line display cap. The message column is last, so
// everything before it shares what is left.
void aimWrapWidth(LogView &view, int chars, int targetLines)
{
    const int cols = qMax(8, chars / qMax(1, targetLines));
    const int vw = view.viewport()->width();
    const int avail = qBound(60, cols * qMax(1, view.fontMetrics().averageCharWidth()), vw - 16);
    const int lead = view.header()->count() - 1;
    view.header()->setMinimumSectionSize(4);
    for (int c = 0; c < lead; ++c)
        view.header()->resizeSection(c, qMax(4, (vw - avail) / qMax(1, lead)));
}

// Where the message column's text starts: it is the last column, and in AlwaysOn it is
// drawn from there to the right edge of the viewport whatever its section width.
QRect messageBand(LogView &view, int lines)
{
    const int x = view.header()->sectionViewportPosition(view.header()->count() - 1);
    return QRect(x, 0, view.viewport()->width() - x, lines * view.lineHeight());
}

} // namespace

// Line Wrap ▸ Always On. The record is given recordHeightLines() rows of lineHeight()
// pixels, and every one of those rows has to hold a drawn line. A pitch one pixel short
// per line clips the bottom of every wrapped record, and from lineHeight() lines up —
// about thirteen at the shipped size, an ordinary payload record — the last line is not
// drawn at all.
void TestLogView::everyWrappedLineOfARecordIsDrawnInsideTheRowItWasGiven()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; nothing has an ascent to round");
    const int pt = aPointSizeWhereTheMetricsDisagree();
    if (pt == 0)
        QSKIP("QFontMetrics::height() is the layout pitch at every size here");
    FontSizeGuard guard;

    constexpr int kChars = 1200;
    QTemporaryFile file;
    QVERIFY(writeLog(file, makeOneLongRecord(kChars)));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.size(), 1);

    LogModel model(&doc);
    LogView view(&doc, &model);
    // A near-black table, so what is fill and what is ink is not a matter of taste.
    QPalette pal = view.palette();
    pal.setColor(QPalette::Base, QColor(0x14, 0x16, 0x18));
    pal.setColor(QPalette::Text, QColor(0xd0, 0xd3, 0xd6));
    view.setPalette(pal);
    view.resize(700, 400);
    zoomTo(view, pt);

    // The claim, before anything is painted: the view measures in the unit the painter
    // draws in. Everything below is that same statement read off the pixels.
    const int lh = view.lineHeight();
    QCOMPARE(lh, laidOutLineHeight(view.font()));

    aimWrapWidth(view, kChars, lh + 3);
    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    view.measureBlockOfRecord(0);
    const int lines = view.estimatedGeometry().recordHeightLines(0);
    QVERIFY2(lines > lh, "the record has to be tall enough for the deficit to lose a line");
    QVERIFY(lines < RecordIndex::kDisplayLineCap);

    // Tall enough that the whole record is on screen: what is counted is the lines the
    // record was GIVEN, so the viewport must not be what cuts any of them off. Only the
    // height moves, so the wrap width — and therefore `lines` — does not.
    view.resize(view.width(), view.height() + (lines + 2) * lh);
    QCOMPARE(view.estimatedGeometry().recordHeightLines(0), lines);
    QVERIFY(view.viewport()->height() >= lines * lh);

    QImage img(view.viewport()->size(), QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    view.viewport()->render(&img, QPoint(), QRegion(), QWidget::DrawWindowBackground);

    const QRect band = messageBand(view, lines);
    const QVector<std::pair<int, int>> ink = inkRuns(img, band, pal.base().color());
    QVERIFY2(ink.size() > 1, "this font's glyphs leave no gap between painted lines");

    // The lines are spaced at the pitch the rows were measured in: a 16 px stride through
    // 15 px rows is the whole of the defect, and it is stated here in pixels.
    for (int i = 1; i < ink.size(); ++i)
        QCOMPARE(ink.at(i).first - ink.at(i - 1).first, lh);
    // Every drawn line has a row of its own. The count may be one under the model's
    // ceil(chars / cols) estimate — that estimate is what AlwaysOn is, §7.1.1 — but it
    // can never be over it, which is what "the text was given fewer pixels than it needs"
    // looks like from here.
    QVERIFY2(ink.size() <= lines, "the text takes more lines than the record was given");
    // ...and the last of them ends INSIDE the band rather than against its edge, which is
    // where clipping leaves it.
    QVERIFY2(ink.last().first + ink.last().second < band.bottom(), "the last line is cut off");
}

// Line Wrap ▸ Selected record only — the mirror symptom, and the reason fixing one end
// alone only moves the error. measureWrappedLines() divides a boundingRect height, which
// is in Qt's pitch, by the view's: while the two differ the answer is a line too many and
// the record wears a blank strip under its own text.
void TestLogView::aSelectedRecordIsGivenExactlyTheLinesItsWrappedTextTakes()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; nothing has an ascent to round");
    const int pt = aPointSizeWhereTheMetricsDisagree();
    if (pt == 0)
        QSKIP("QFontMetrics::height() is the layout pitch at every size here");
    FontSizeGuard guard;

    constexpr int kChars = 900;
    QTemporaryFile file;
    QVERIFY(writeLog(file, makeOneLongRecord(kChars)));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    QPalette pal = view.palette();
    pal.setColor(QPalette::Highlight, QColor(0x1e, 0x28, 0x3c));
    pal.setColor(QPalette::HighlightedText, QColor(0xe8, 0xea, 0xec));
    view.setPalette(pal);
    view.resize(700, 400);
    zoomTo(view, pt);

    const int lh = view.lineHeight();
    QCOMPARE(lh, laidOutLineHeight(view.font()));

    aimWrapWidth(view, kChars, 8);
    view.setWrapMode(LogView::WrapMode::SelectedRecordOnly);
    view.setCurrentRecord(0);

    // What measureWrappedLines() answered, which is what the record's rows are counted
    // in and what its band is filled to.
    const int claimed = view.selWrapLines();
    QVERIFY(claimed > 2);
    QVERIFY(claimed < RecordIndex::kDisplayLineCap);

    view.resize(view.width(), view.height() + (claimed + 2) * lh);
    QCOMPARE(view.selWrapLines(), claimed);
    QVERIFY(view.viewport()->height() >= claimed * lh);

    QImage img(view.viewport()->size(), QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    view.viewport()->render(&img, QPoint(), QRegion(), QWidget::DrawWindowBackground);

    // The record is selected, so its fill is the selection's rather than the band's.
    const QRect band = messageBand(view, claimed);
    const QVector<std::pair<int, int>> ink = inkRuns(img, band, pal.highlight().color());
    QVERIFY2(ink.size() > 1, "this font's glyphs leave no gap between painted lines");

    // Exactly as many painted lines as the record was given rows for: one more than the
    // text takes is a blank strip under it, one fewer is text cut off.
    QCOMPARE(ink.size(), claimed);
    for (int i = 1; i < ink.size(); ++i)
        QCOMPARE(ink.at(i).first - ink.at(i - 1).first, lh);
}

// --- the two wrapped-cell paths break lines identically ----------------------
//
// `drawWrappedCell()` lays a cell out with a QTextLayout whether or not there is
// anything to mark, and `measureWrappedLines()` counts through the same function. It did
// not always: the unmarked path was `QPainter::drawText()` with flags, and Qt maps NO
// flag pair onto `QTextOption::WrapAtWordBoundaryOrAnywhere` — `TextWordWrap |
// TextWrapAnywhere` behaves as plain `WrapAnywhere`. So the two disagreed, and the record
// the reader was in the middle of re-broke the moment a search armed. A tab made it worse
// and on the other mode: drawText gives U+0009 the width of one character, a QTextLayout
// advances it to the default 80 px tab stop, so a TAB-indented stack trace re-flowed
// under Always On too and the lines it gained past its allotted rows were clipped away.
//
// Neither is observable anywhere but in the pixels — every widget holds the same values
// marked or not — so both cases render the viewport twice and compare where the lines
// end. Both need a resolved font: with an empty font database (Windows offscreen, which
// ships none) nothing has an advance to wrap at.

namespace {

// Where each of `lines` line boxes down `band` stops inking, or -1 for a box with nothing
// drawn in it. This is where the line BREAKING is observable: a different break puts
// different characters on the line, so the line ends somewhere else. A mark cannot move
// it — the mark fills a run of glyphs that are already there, so it can only ink columns
// the text already reaches.
QVector<int> lineEnds(const QImage &img, const QRect &band, int lineHeight, int lines,
                      const QColor &fill)
{
    const auto isFill = [&](QRgb px) {
        return qAbs(qRed(px) - fill.red()) <= 1 && qAbs(qGreen(px) - fill.green()) <= 1
            && qAbs(qBlue(px) - fill.blue()) <= 1;
    };
    QVector<int> ends;
    for (int i = 0; i < lines; ++i) {
        int end = -1;
        const int y0 = band.top() + i * lineHeight;
        for (int y = y0; y < y0 + lineHeight && y <= band.bottom(); ++y) {
            for (int x = band.right(); x > end; --x) {
                if (!isFill(img.pixel(x, y))) {
                    end = x;
                    break;
                }
            }
        }
        ends.append(end);
    }
    return ends;
}

// A stack trace: one exception line and TAB-indented frames under it, which are
// continuations of the SAME record (invariant #2) and so are paragraphs of the one
// wrapped message cell. The frames are long enough to wrap and the last one is short, so
// the record's final row is the end of its text rather than the middle of it — which is
// what makes clipped text visible rather than merely different.
QByteArray makeTabbedStackTrace()
{
    QByteArray bytes = "2026-07-21 14:32:05,123 [main] ERROR net.socket - "
                       "java.lang.NullPointerException: the Handler could not dispatch\n";
    for (int i = 0; i < 5; ++i) {
        bytes += "\tat com.example.Handler.dispatch(Handler.java:";
        bytes += QByteArray::number(40 + i);
        bytes += ") ~[app.jar:1.0.0] while serving the request from 10.0.0.7\n";
    }
    bytes += "\tCaused by: read timed out\n";
    return bytes;
}

// The palette the pixel cases below read against: fixed rather than the desktop's, so the
// fill a line box is measured against is known.
void useKnownPalette(LogView &view)
{
    QPalette pal = view.palette();
    pal.setColor(QPalette::Base, QColor(0x18, 0x1a, 0x1c));
    pal.setColor(QPalette::Text, QColor(0xe0, 0xe2, 0xe4));
    pal.setColor(QPalette::Highlight, QColor(0x1e, 0x28, 0x3c));
    pal.setColor(QPalette::HighlightedText, QColor(0xe8, 0xea, 0xec));
    view.setPalette(pal);
}

} // namespace

// Line Wrap ▸ Always On over a record carrying tabs. Arming Find must not move a single
// break: the unmarked rendering is the one the ceil(chars / cols) height model was
// measured against, and a tab worth eleven columns re-flows the record and pushes its
// tail past the rows it was given, where the clip silently eats it.
void TestLogView::aTabbedRecordBreaksWhereItAlwaysDidWhenFindIsArmed()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; where a character landed is unanswerable");

    QTemporaryFile file;
    QVERIFY(writeLog(file, makeTabbedStackTrace()));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.size(), 1);

    LogModel model(&doc);
    LogView view(&doc, &model);
    useKnownPalette(view);
    view.resize(900, 900);
    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    // The viewport takes its geometry on the first paint, and the wrap width is measured
    // from it — so settle it before deciding where the message column goes, and again
    // after, or every width below is one the view never had.
    renderViewport(view);
    aimWrapWidth(view, 380, 8);
    renderViewport(view);
    QTest::qWait(250); // the debounced resize is what re-keys the estimator to this width

    const int lh = view.lineHeight();
    view.measureBlockOfRecord(0);
    const int lines = view.estimatedGeometry().recordHeightLines(0);
    QVERIFY2(lines > 6, "the record did not wrap; nothing here is being tested");
    QVERIFY(view.viewport()->height() >= lines * lh);

    const QRect band = messageBand(view, lines);
    const QColor fill = view.palette().base().color();

    const QImage plain = renderViewport(view);
    const QVector<int> before = lineEnds(plain, band, lh, lines, fill);
    // Every row the record was given holds a drawn line: with none of the tabs widened,
    // the text takes exactly the rows the height model counted.
    for (int i = 0; i < lines; ++i)
        QVERIFY2(before.at(i) >= 0, "a row of the unmarked record was left blank");
    QVERIFY2(before.last() < band.right() - 8, "the record's last row is not the end of it");

    TextMatcher matcher;
    matcher.set(QStringLiteral("Handler"), /*regex=*/false, Qt::CaseInsensitive);
    view.setFindMatcher(matcher);
    const QImage marked = renderViewport(view);
    const QVector<int> after = lineEnds(marked, band, lh, lines, fill);

    QCOMPARE(after, before);
    // And the record still ends where it ended: a break that moved would have pushed the
    // tail past the rows it was given, and the clip takes what does not fit.
    QVERIFY2(after.last() < band.right() - 8,
             "the record's last row is mid-text: arming Find dropped the end of it");
}

// Line Wrap ▸ Selected record only, which wraps at WORD boundaries — what the mode is for
// is reading one record in full. The break must not depend on whether a search is armed,
// and `measureWrappedLines()` must be counting the wrapping the paint performs, or the
// record is given the wrong number of rows in the first place.
void TestLogView::theSelectedRecordBreaksWhereItAlwaysDidWhenFindIsArmed()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; where a character landed is unanswerable");

    QTemporaryFile file;
    QVERIFY(writeLog(file,
                     "2026-07-21 14:32:05,123 [main] INFO  net.socket - Listening for "
                     "incoming connections on port 8780 from 10.0.0.7 and waiting for the "
                     "handler to accept them one at a time\n"
                     "2026-07-21 14:32:06,000 [main] INFO  net.socket - done\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.size(), 2);

    LogModel model(&doc);
    LogView view(&doc, &model);
    useKnownPalette(view);
    view.resize(900, 900);
    view.setWrapMode(LogView::WrapMode::SelectedRecordOnly);
    // The viewport takes its geometry on the first paint, and the wrap width is measured
    // from it — so settle it before deciding where the message column goes, and again
    // after, or every width below is one the view never had.
    renderViewport(view);
    aimWrapWidth(view, 150, 6);
    renderViewport(view);
    view.setCurrentRecord(0);

    const int lh = view.lineHeight();
    // What measureWrappedLines() answered — the rows the record is given and filled to.
    const int lines = view.selWrapLines();
    QVERIFY2(lines > 3, "the record did not wrap; nothing here is being tested");
    QVERIFY(view.viewport()->height() >= lines * lh);

    const QRect band = messageBand(view, lines);
    // The record is selected, so its fill is the selection's rather than the band's.
    const QColor fill = view.palette().highlight().color();

    const QImage plain = renderViewport(view);
    const QVector<int> before = lineEnds(plain, band, lh, lines, fill);
    for (int i = 0; i < lines; ++i)
        QVERIFY2(before.at(i) >= 0,
                 "the record was given a row its own text does not reach: the measurement "
                 "and the paint are wrapping differently");
    QVERIFY2(before.last() < band.right() - 8, "the record's last row is not the end of it");

    TextMatcher matcher;
    matcher.set(QStringLiteral("connections"), /*regex=*/false, Qt::CaseInsensitive);
    view.setFindMatcher(matcher);
    const QImage marked = renderViewport(view);
    const QVector<int> after = lineEnds(marked, band, lh, lines, fill);

    QCOMPARE(after, before);
    QVERIFY2(after.last() < band.right() - 8,
             "the record's last row is mid-text: arming Find dropped the end of it");
}

// --- marking a cell costs one redraw of it, not one per match (bugs.md 12) ---
//
// `paintMark()`'s contract is that the glyphs are redrawn by the SAME call that drew
// them, merely clipped (§7.1.4) — and that call draws the WHOLE cell. Issued once per
// matched run, a wrapped cell was therefore redrawn in full once per (display line x
// intersecting run) pair: both multipliers are capped rather than small, at 64 runs
// (`kMaxCellMarks`) over 100 display lines (`RecordIndex::kDisplayLineCap`), so a
// single-letter query took one repaint of one record from 2 ms to 32 ms. The cap on the
// number of runs is what makes the multiplier reachable, not what makes it cheap.
//
// The runs are collected into a QRegion and the redraw is issued ONCE through it, so the
// cost is O(runs) rectangles and O(1) redraws. That is not observable in any value the
// widget holds, and a wall clock on a shared runner is not a thing to assert on — so the
// draws themselves are counted, through a paint device that does nothing but tally what
// the painter asks of it.

namespace {

// A paint engine that records how many text runs and how many fills it was asked for and
// draws none of them. Everything QPainter can decompose into something else is a no-op,
// so nothing recurses back into the two counters.
class DrawCounter : public QPaintEngine
{
public:
    DrawCounter() : QPaintEngine(QPaintEngine::AllFeatures) {}

    bool begin(QPaintDevice *) override { return true; }
    bool end() override { return true; }
    void updateState(const QPaintEngineState &) override {}
    void drawPixmap(const QRectF &, const QPixmap &, const QRectF &) override {}
    void drawImage(const QRectF &, const QImage &, const QRectF &,
                   Qt::ImageConversionFlags) override {}
    void drawRects(const QRect *, int n) override { fills += n; }
    void drawRects(const QRectF *, int n) override { fills += n; }
    void drawLines(const QLine *, int) override {}
    void drawLines(const QLineF *, int) override {}
    void drawPolygon(const QPoint *, int, PolygonDrawMode) override {}
    void drawPolygon(const QPointF *, int, PolygonDrawMode) override {}
    void drawPath(const QPainterPath &) override {}
    void drawTextItem(const QPointF &, const QTextItem &) override { textRuns += 1; }
    Type type() const override { return QPaintEngine::User; }

    int textRuns = 0;
    int fills = 0;
};

class CountingDevice : public QPaintDevice
{
public:
    explicit CountingDevice(const QSize &size) : m_size(size) {}

    QPaintEngine *paintEngine() const override { return &m_engine; }

    int metric(PaintDeviceMetric m) const override
    {
        switch (m) {
        case PdmWidth: return m_size.width();
        case PdmHeight: return m_size.height();
        case PdmWidthMM: return m_size.width() * 254 / 960;
        case PdmHeightMM: return m_size.height() * 254 / 960;
        case PdmNumColors: return 1 << 24;
        case PdmDepth: return 32;
        case PdmDpiX:
        case PdmPhysicalDpiX: return 96;
        case PdmDpiY:
        case PdmPhysicalDpiY: return 96;
        case PdmDevicePixelRatio: return 1;
        case PdmDevicePixelRatioScaled: return int(devicePixelRatioFScale());
        }
        return 0;
    }

    mutable DrawCounter m_engine;

private:
    QSize m_size;
};

// One repaint of `view`'s viewport, counted rather than rendered.
struct DrawTally
{
    int textRuns = 0;
    int fills = 0;
};

DrawTally countOneRepaint(LogView &view)
{
    CountingDevice device(view.viewport()->size());
    view.viewport()->render(&device, QPoint(), QRegion(), QWidget::DrawWindowBackground);
    return DrawTally{ device.m_engine.textRuns, device.m_engine.fills };
}

// A view of one long record under Line Wrap ▸ Always On, wrapped to many display lines
// with a run of `matches` matching "socket" spread down them.
int prepareWrappedMarkView(LogView &view, int chars)
{
    useKnownPalette(view);
    view.resize(900, 900);
    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    renderViewport(view);
    aimWrapWidth(view, chars, 30);
    renderViewport(view);
    QTest::qWait(250); // the debounced resize re-keys the estimator to this width
    view.measureBlockOfRecord(0);
    return view.estimatedGeometry().recordHeightLines(0);
}

} // namespace

void TestLogView::aMarkedCellIsRedrawnOncePerCellAndNotOncePerMatch()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; nothing wraps and nothing is marked");

    constexpr int kChars = 3000;
    QTemporaryFile file;
    QVERIFY(writeLog(file, makeOneLongRecord(kChars)));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.size(), 1);

    LogModel model(&doc);
    LogView view(&doc, &model);
    const int lines = prepareWrappedMarkView(view, kChars);
    QVERIFY2(lines >= 10, "the record did not wrap far enough for the multiplier to show");

    const int plain = countOneRepaint(view).textRuns;
    QVERIFY2(plain > 0, "nothing was drawn at all; the counting device saw no text");

    // "socket" occurs about once per 35 characters of this message, so the 64-run cap is
    // reached and the runs are spread down most of the record's display lines.
    TextMatcher matcher;
    matcher.set(QStringLiteral("socket"), /*regex=*/false, Qt::CaseInsensitive);
    QVERIFY2(matcher.spans(model.cellText(0, view.header()->count() - 1), 64).size() >= 40,
             "the query did not match often enough for the multiplier to show");
    view.setFindMatcher(matcher);

    const DrawTally marked = countOneRepaint(view);
    QVERIFY2(marked.textRuns > plain, "arming Find drew nothing extra: nothing was marked");
    QVERIFY2(marked.fills > 0, "no run was filled: nothing was marked");

    // One redraw of the cell per paragraph, whatever the number of runs — so at most a
    // small multiple of the unmarked repaint. Redrawing per run costs the whole record
    // once per matched run, tens of times this.
    QVERIFY2(marked.textRuns <= 3 * plain,
             qPrintable(QStringLiteral("marking cost %1 text runs against %2 unmarked: the "
                                       "cell is being redrawn once per match")
                            .arg(marked.textRuns)
                            .arg(plain)));
}

// The other half of the same change: batching the runs into one region must not lose
// any of them. A region filled after the redraw would erase its own glyphs, and a region
// carrying only the last run would leave every earlier match unmarked — neither is
// visible in any value the widget holds, so this reads the marks off the pixels.
void TestLogView::everyMatchOfAWrappedCellIsMarkedWhenTheyAreBatched()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; where a character landed is unanswerable");

    constexpr int kChars = 3000;
    QTemporaryFile file;
    QVERIFY(writeLog(file, makeOneLongRecord(kChars)));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    const int lines = prepareWrappedMarkView(view, kChars);
    QVERIFY2(lines >= 10, "the record did not wrap far enough to hold marks on many lines");

    const int lh = view.lineHeight();
    const QRect band = messageBand(view, lines);
    QVERIFY(view.viewport()->height() >= lines * lh);
    const QColor text = view.palette().text().color();

    TextMatcher matcher;
    matcher.set(QStringLiteral("socket"), /*regex=*/false, Qt::CaseInsensitive);
    view.setFindMatcher(matcher);
    const QImage img = renderViewport(view);

    // The runs are capped at 64 and this message holds more matches than that, so the
    // marks stop partway down the record — but every line up to there carries one, and
    // that is what a lost patch takes away. Count the lines that show a mark: the run is
    // filled with the colour the text is drawn in, so a marked line inks whole columns
    // of it where an unmarked one inks only glyph strokes.
    int markedLines = 0;
    int lastMarked = -1;
    for (int i = 0; i < lines; ++i) {
        if (markedColumns(img, QRect(band.left(), i * lh, band.width(), lh), text) >= 4) {
            ++markedLines;
            lastMarked = i;
        }
    }
    QVERIFY2(markedLines >= 10,
             qPrintable(QStringLiteral("only %1 of %2 display lines carried a mark")
                            .arg(markedLines)
                            .arg(lines)));
    // Not merely the last one, and not merely the first: a region that kept one patch
    // would show exactly one line here.
    QVERIFY2(lastMarked > 0, "only the record's first display line was marked");
}

// A mark over an ELIDED cell is placed by a layout of the string as drawn, and the
// difference only shows where the shaped run is not where the logical prefix ends: a sum
// of per-character advances is the width of the first n characters, which is the run's
// visual left edge for Latin and is somewhere else entirely for Arabic, Hebrew or Indic
// text, where the bidi algorithm reorders the runs. It hits the message column in the
// default wrap-off mode as well as every metadata column in both modes.
void TestLogView::anElidedMarkSitsOverTheGlyphsOfTheRunAndNotOverALogicalPrefix()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; where a character landed is unanswerable");

    const QString message = QString::fromUtf8("log مرحبا بالعالم end");
    const QString query = QString::fromUtf8("مرحبا");
    const int at = message.indexOf(query);
    QVERIFY(at > 0);

    QTemporaryFile file;
    QVERIFY(writeLog(file, QByteArray("2026-07-21 14:32:05,123 [main] INFO  net.socket - ")
                               + message.toUtf8() + "\n"));

    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));
    QCOMPARE(doc.index().records.size(), 1);

    LogModel model(&doc);
    LogView view(&doc, &model);
    useKnownPalette(view);
    view.resize(1000, 200);
    view.setWrapMode(LogView::WrapMode::Off); // the elided path, and the shipped default
    renderViewport(view);

    // The message column last and wide enough that nothing is elided away: what is being
    // tested is where a run landed, not what the ellipsis took.
    const int msgCol = view.header()->count() - 1;
    aimWrapWidth(view, 400, 1);
    view.header()->resizeSection(msgCol, 600);
    renderViewport(view);

    // Where the run actually is, asked of a layout of the very string the view draws —
    // the oracle, and the thing the advance sum disagrees with.
    QTextLayout layout(message, view.font());
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    QVERIFY(line.isValid());
    line.setLineWidth(std::numeric_limits<int>::max());
    layout.endLayout();
    const int shaped0 = int(qMin(line.cursorToX(at), line.cursorToX(at + query.size())));
    const int shaped1 = int(qMax(line.cursorToX(at), line.cursorToX(at + query.size())));

    const QFontMetrics fm(view.font());
    const int logical1 = fm.horizontalAdvance(message, at + query.size());
    if (qAbs(logical1 - shaped1) <= 2)
        QSKIP("nothing here shapes Arabic; the reordered run cannot be told from the prefix");

    const int lh = view.lineHeight();
    const int left = view.header()->sectionViewportPosition(msgCol);
    const QColor text = view.palette().text().color();

    // A mark is told from a glyph by how much of the line box it inks, so a face that
    // draws every character as a filled box — Qt's last-resort engine, which is what an
    // empty font directory gets — leaves the two indistinguishable.
    if (markedColumns(renderViewport(view), QRect(left, 0, 600, lh), text) > 0)
        QSKIP("this face draws filled boxes; a mark cannot be told from a glyph");

    TextMatcher matcher;
    matcher.set(query, /*regex=*/false, Qt::CaseInsensitive);
    view.setFindMatcher(matcher);
    const QImage img = renderViewport(view);
    int first = -1;
    int last = -1;
    for (int x = left; x < left + 600 && x < img.width(); ++x) {
        if (markedColumns(img, QRect(x, 0, 1, lh), text) == 1) {
            if (first < 0)
                first = x;
            last = x;
        }
    }
    QVERIFY2(first >= 0, "the match was not marked at all");
    QVERIFY2(qAbs(first - (left + shaped0)) <= 2,
             qPrintable(QStringLiteral("the mark starts at %1, the run at %2")
                            .arg(first - left)
                            .arg(shaped0)));
    QVERIFY2(qAbs(last - (left + shaped1)) <= 2,
             qPrintable(QStringLiteral("the mark ends at %1, the run at %2 (the logical "
                                       "prefix ends at %3)")
                            .arg(last - left)
                            .arg(shaped1)
                            .arg(logical1)));
}

// --- the wrap width follows the message column's origin (bugs.md 9) ----------
//
// Under Line Wrap ▸ Always On a record is given ceil(chars / cols) rows, and `cols` is
// measured from the message column's ORIGIN to the right edge of the viewport (§7.1.1).
// Three gestures move that origin and only one of them used to say so. A column resize
// remeasured; a column MOVE across the message column did not, and neither did a
// horizontal scroll, which slides the origin left by exactly the value of the bar. Both
// left every row on screen measured against a width it no longer has — a blank band
// under every record where the wrap area grew, and the tail of every message clipped
// away where it shrank, since the paint clips the cell to the rows it was given.
//
// Only the pixels can tell. Every widget holds the right value throughout, the model is
// untouched, and each record is drawn from its own text either way: what is wrong is the
// number of rows it was drawn into. So all three cases render the viewport and read the
// ink back, and all three need a resolved font — with an empty font database (Windows
// offscreen, which ships none) there is no advance to wrap at.

namespace {

// The pixels between the last inked row of `band` and its bottom edge: the blank strip a
// record wears when it was measured against a narrower message column than it is drawn
// in. A whole line of it is a row the record did not need.
int blankTail(const QImage &img, const QRect &band, const QColor &fill)
{
    const QVector<std::pair<int, int>> ink = inkRuns(img, band, fill);
    if (ink.isEmpty())
        return band.height();
    return band.bottom() - (ink.last().first + ink.last().second - 1);
}

// Wait out the 120 ms remeasure debounce (§7.1.1) — the same delay a drag-resize pays,
// and the reason a corrected height lands just after the gesture rather than during it.
// On the column count rather than on the clock, so a loaded runner cannot decide the
// case; a view that never remeasures at all arrives at the assertions with its old
// count, which is exactly what they are about.
void settleWrapWidth(const EstimatedGeometry &g, int wasCols)
{
    QElapsedTimer waited;
    waited.start();
    while (g.columns() == wasCols && waited.elapsed() < 2000)
        QTest::qWait(20);
}

// Send a hidden view its pending resize: QWidget::resize() on something never shown
// leaves the event queued, and the viewport keeps the size it was born at until somebody
// renders it. Everything below measures a column width against the viewport, so it has
// to be the real one.
void settleLayout(LogView &view)
{
    (void)renderViewport(view);
}

// Let a remeasure that is already in flight land before the gesture under test. The
// debounce is 120 ms and everything in the setup goes through it — the wrap mode, each
// column resize, and a hidden view's own deferred layout — so a case that did not wait
// here could read the setup's remeasure as the gesture's answer.
void settleRemeasure()
{
    QTest::qWait(200);
}

// The band record 0 is drawn into: it sits at the top of the view, so its rows start at
// y 0, and in AlwaysOn its message is drawn from the column's origin to the right edge
// whatever the section's own width.
QRect topRecordBand(LogView &view, int messageColumn, int lines)
{
    const int x = qMax(0, view.header()->sectionViewportPosition(messageColumn));
    return QRect(x, 0, view.viewport()->width() - x, lines * view.lineHeight());
}

} // namespace

// Dragging a column past the message column, both ways. The rows have to be remeasured
// on the drop — one blank row per record where the message gained width, and a record
// cut off mid-text where it lost it.
void TestLogView::aMovedColumnRemeasuresEveryRowUnderAlwaysOn()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; there is no advance to wrap at");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 60), qPrintable(doc.lastError())); // ~200-char messages

    LogModel model(&doc);
    LogView view(&doc, &model);
    useKnownPalette(view);
    view.resize(880, 400);
    // The first render is what sends a hidden widget its pending resize, so the viewport
    // reaches its final width HERE. Measure a column against it before that and every
    // width below is computed against the default 640 the widget was born at.
    settleLayout(view);

    const int msg = view.header()->count() - 1; // the message is the last field
    aimWrapWidth(view, 200, 7);                 // and a narrow column, so it wraps deep
    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    settleRemeasure();
    view.measureBlockOfRecord(0);

    const EstimatedGeometry &g = view.estimatedGeometry();
    const int lh = view.lineHeight();
    const QColor fill = view.palette().base().color(); // record 0 is an even row
    const int narrowCols = g.columns();
    const int narrowLines = g.recordHeightLines(0);
    QVERIFY2(narrowLines >= 4, "the message must wrap to several rows for a gap to show");

    const QImage baseline = renderViewport(view);
    const QRect narrowBand = topRecordBand(view, msg, narrowLines);
    QVERIFY2(blankTail(baseline, narrowBand, fill) < lh,
             "the record is already drawn into more rows than its text fills");

    // Drag the message column to the front. Everything that was before it is now after
    // it, so it wraps within the whole viewport and needs a fraction of the rows. (What
    // it is drawn OVER while it sits there is `bugs.md` 18 and is a separate question;
    // what is asked here is only how many rows it was given.)
    view.header()->moveSection(view.header()->visualIndex(msg), 0);
    settleWrapWidth(g, narrowCols);
    view.measureBlockOfRecord(0);
    const int wideCols = g.columns();
    const int wideLines = g.recordHeightLines(0);

    // The symptom, in pixels: whatever rows the record was given, its text has to reach
    // the bottom of them.
    const QRect wideBand = topRecordBand(view, msg, wideLines);
    QVERIFY2(blankTail(renderViewport(view), wideBand, fill) < lh,
             "every record is drawn into rows measured against the old, narrower column");
    QVERIFY2(wideCols > narrowCols, "the move left the estimator on the old wrap width");
    QVERIFY(wideLines < narrowLines);

    // ...and back, which is the other symptom: rows measured at the wide origin are too
    // few for the narrow one, and what does not fit is clipped away in silence.
    view.header()->moveSection(0, view.header()->count() - 1);
    settleWrapWidth(g, wideCols);
    QCOMPARE(g.columns(), narrowCols);
    view.measureBlockOfRecord(0);
    QCOMPARE(g.recordHeightLines(0), narrowLines);

    const QImage back = renderViewport(view);
    const QVector<int> ends = lineEnds(back, narrowBand, lh, narrowLines, fill);
    for (int i = 0; i < narrowLines; ++i)
        QVERIFY2(ends.at(i) >= 0, "a row of the record was left blank or was never drawn");
    QVERIFY2(ends.last() < narrowBand.right() - 8,
             "the record's last row is mid-text: its tail was clipped away");
    // The move was a round trip, so the view is back where it started, pixel for pixel.
    QCOMPARE(back, baseline);
}

// The same drop under Line Wrap ▸ Selected record only, where there is no debounce and
// no estimator: what has to be remeasured is the one record's wrapped height, which is
// what its band is filled to. This is why the fix is not scoped to AlwaysOn.
void TestLogView::aMovedColumnRemeasuresTheSelectedRecord()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; there is no advance to wrap at");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 60), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    useKnownPalette(view);
    view.resize(880, 400);
    // The first render is what sends a hidden widget its pending resize, so the viewport
    // reaches its final width HERE. Measure a column against it before that and every
    // width below is computed against the default 640 the widget was born at.
    settleLayout(view);

    const int msg = view.header()->count() - 1;
    aimWrapWidth(view, 200, 7);
    view.setWrapMode(LogView::WrapMode::SelectedRecordOnly);
    view.setCurrentRecord(0);
    settleRemeasure();

    const int lh = view.lineHeight();
    const QColor fill = view.palette().highlight().color(); // the selected record's band
    const int narrow = view.selWrapLines();
    QVERIFY2(narrow >= 4, "the message must wrap to several rows for a gap to show");
    QVERIFY2(blankTail(renderViewport(view), topRecordBand(view, msg, narrow), fill) < lh,
             "the record is already drawn into more rows than its text fills");

    view.header()->moveSection(view.header()->visualIndex(msg), 0);
    const int wide = view.selWrapLines();
    QVERIFY2(blankTail(renderViewport(view), topRecordBand(view, msg, wide), fill) < lh,
             "the selected record is banded to a height its text no longer takes");
    QVERIFY2(wide < narrow, "the selected record kept the height it had at the old origin");
}

// Scrolling sideways, which the message column being seeded wider than the viewport makes
// the ordinary case. The bar's value is subtracted from every section's position, so the
// message column's origin slides left and the space it wraps within grows by exactly as
// much — with nothing to remeasure it, every record on screen keeps the height it had at
// the left edge and wears the difference as a blank band.
void TestLogView::aHorizontalScrollRemeasuresEveryRowUnderAlwaysOn()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; there is no advance to wrap at");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 60), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    useKnownPalette(view);
    view.resize(880, 400);
    // The first render is what sends a hidden widget its pending resize, so the viewport
    // reaches its final width HERE. Measure a column against it before that and every
    // width below is computed against the default 640 the widget was born at.
    settleLayout(view);

    const int msg = view.header()->count() - 1;
    aimWrapWidth(view, 200, 8);
    // Wide enough that the header runs past the right edge: without a horizontal
    // scrollbar there is nothing to drag.
    view.header()->resizeSection(msg, view.viewport()->width());
    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    QScrollBar *hb = view.horizontalScrollBar();
    QVERIFY2(hb->maximum() > 0, "the header has to be wider than the viewport");
    settleRemeasure();
    view.measureBlockOfRecord(0);

    const EstimatedGeometry &g = view.estimatedGeometry();
    const int lh = view.lineHeight();
    const QColor fill = view.palette().base().color();
    const int narrowCols = g.columns();
    const int narrowLines = g.recordHeightLines(0);
    QVERIFY2(narrowLines >= 4, "the message must wrap to several rows for a gap to show");

    const QImage baseline = renderViewport(view);
    QVERIFY2(blankTail(baseline, topRecordBand(view, msg, narrowLines), fill) < lh,
             "the record is already drawn into more rows than its text fills");

    // Right, to within 40 px of the message column's own origin.
    const int msgX = view.header()->sectionViewportPosition(msg);
    hb->setValue(qMin(hb->maximum(), msgX - 40));
    QVERIFY(view.header()->sectionViewportPosition(msg) < msgX);
    settleWrapWidth(g, narrowCols);
    view.measureBlockOfRecord(0);
    const int wideCols = g.columns();
    const int wideLines = g.recordHeightLines(0);
    QVERIFY2(blankTail(renderViewport(view), topRecordBand(view, msg, wideLines), fill) < lh,
             "every record keeps the height it was given at the left edge of the log");
    QVERIFY2(wideCols > narrowCols, "the scroll left the estimator on the old wrap width");
    QVERIFY(wideLines < narrowLines);

    // ...and scrolling back is a round trip, exactly as the resize it behaves like is.
    hb->setValue(0);
    settleWrapWidth(g, wideCols);
    QCOMPARE(g.columns(), narrowCols);
    view.measureBlockOfRecord(0);
    QCOMPARE(g.recordHeightLines(0), narrowLines);
    QCOMPARE(renderViewport(view), baseline);
}

// --- the wrap width has a floor, and it is in characters (bugs.md 11) --------
//
// A wrapped message is laid out from the message column's ORIGIN to the right edge of
// the viewport (§7.1.1), and the columns before it are seeded from the intern tables
// when the scan finishes — so on a narrow window their sum can reach or pass that edge.
// The width was floored at one pixel, so every record wrapped at one character per line,
// measured the 100-line display cap, and filled the whole viewport by itself: 1400 px of
// record against a 466 px viewport, one record per screen at both ends of the file, and a
// scrollbar whose whole travel was a hundredth of a record per notch.
//
// The floor is kMinWrapCols CHARACTERS because log text zooms (Fonts.h): a pixel constant
// would be 28 columns at 7 pt and 6 at 30 pt. Nothing else in this file goes below 60 px
// of available width, which is why the whole pathological range was unguarded.

namespace {

// Put the message column's origin `avail` pixels from the viewport's right edge by
// widening everything before it. `avail` at or below zero is the origin AT or PAST the
// edge, which is what a 34-character logger name and a 23-character thread name seed on
// a 640 px document area. Resizing a section claims it, so the scan-completion re-seed
// leaves these widths alone.
void putMessageOriginAt(LogView &view, int avail)
{
    QHeaderView *h = view.header();
    const int msg = h->count() - 1;
    const int want = qMax(0, view.viewport()->width() - avail);
    h->setMinimumSectionSize(4);
    const int each = qMax(4, want / qMax(1, msg));
    for (int c = 0; c < msg; ++c)
        h->resizeSection(c, c == 0 ? want - each * (msg - 1) : each);
}

// The pixel width kMinWrapCols characters come to at the size the view is drawn at.
int floorWidth(const LogView &view)
{
    return LogView::kMinWrapCols * qMax(1, view.fontMetrics().averageCharWidth());
}

} // namespace

// Line Wrap ▸ Always On with the message column's origin at 40 px from the right edge, on
// it, and 60 px past it. Whatever the columns before it do, a record of ordinary length
// keeps a height a screen can hold several of — and the scrollbar keeps a range measured
// in those heights rather than in hundreds of lines a record.
void TestLogView::aMessageColumnAtTheViewportEdgeStillFitsSeveralRecordsOnAScreen()
{
    constexpr int kRecords = 60;
    constexpr int kChars = 200; // makeLog()'s "payload " x 25

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, kRecords), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    useKnownPalette(view);
    view.resize(640, 500);
    settleLayout(view);
    view.setWrapMode(LogView::WrapMode::AlwaysOn);

    const EstimatedGeometry &g = view.estimatedGeometry();
    const int lh = view.lineHeight();
    const int vh = view.viewport()->height();
    // No record of this length may be taller than this at ANY origin: the floor is the
    // narrowest the text is ever laid out in, and one line of slack is the model's own
    // ceil(chars / cols).
    const int tallest = (kChars + LogView::kMinWrapCols - 1) / LogView::kMinWrapCols + 1;
    QVERIFY2(tallest * lh < vh, "the probe has to fit several records on a screen to begin with");

    for (int avail : {40, 0, -60}) {
        putMessageOriginAt(view, avail);
        settleRemeasure();
        view.measureBlockOfRecord(0);

        QVERIFY2(g.columns() >= LogView::kMinWrapCols,
                 "the wrap width was taken below its floor");
        const int lines = g.recordHeightLines(0);
        QVERIFY2(lines <= tallest,
                 qPrintable(QStringLiteral("record 0 is %1 lines at avail %2; nothing this "
                                           "long may exceed %3")
                                .arg(lines)
                                .arg(avail)
                                .arg(tallest)));
        QVERIFY2(lines < RecordIndex::kDisplayLineCap,
                 "the record measured the display cap, which is the collapse itself");
        QVERIFY2(lines * lh < vh / 2, "one record takes half the viewport or more");
        // Which is the whole of it from the reader's side: several records on the screen,
        // at the top of the log and at the end of it alike.
        QVERIFY2(g.recordAtLine(qint64(vh / lh) - 1) > 0, "the first screen holds one record");
        QVERIFY2(g.recordAtLine(g.totalLines() - 1) > g.recordAtLine(g.totalLines() - qint64(vh / lh)),
                 "the last screen holds one record");

        // And the scroll range is the sum of those heights rather than 100 lines apiece.
        // (The int overflow the cap used to reach at 21 million records is clamped rather
        // than cast, which no test can reach at this scale; what is asserted here is the
        // total it is computed from.)
        QVERIFY2(g.totalLines() <= qint64(kRecords) * tallest, "the total is the capped one");
        QVERIFY(view.verticalScrollBar()->maximum() > 0);
        QVERIFY(view.verticalScrollBar()->maximum() <= g.totalLines());
    }
}

// The same collapse read off the pixels, at the one origin where nothing else can see it:
// past the right edge the message is drawn wholly outside the viewport, so all that is on
// screen is the records' own alternating bands. One band is one record filling the screen.
void TestLogView::aRecordWhoseColumnIsPastTheEdgeIsOneOfSeveralBandsOnScreen()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; there is no advance to wrap at");

    QTemporaryFile file;
    Document doc;
    QVERIFY2(openLog(doc, file, 60), qPrintable(doc.lastError()));

    LogModel model(&doc);
    LogView view(&doc, &model);
    useKnownPalette(view);
    view.resize(640, 500);
    settleLayout(view);
    view.setWrapMode(LogView::WrapMode::AlwaysOn);
    putMessageOriginAt(view, -60);
    settleRemeasure();
    view.measureBlockOfRecord(0);

    // The rightmost pixel column: past every section and past the message's own origin,
    // so nothing is drawn there but the band each record is filled with.
    const QImage img = renderViewport(view);
    const int x = img.width() - 1;
    int bands = 0;
    for (int y = 0; y < img.height(); ++y)
        if (y == 0 || img.pixel(x, y) != img.pixel(x, y - 1))
            ++bands;
    QVERIFY2(bands >= 3,
             qPrintable(QStringLiteral("%1 record band(s) on a %2 px viewport: the record is "
                                       "taller than the screen")
                            .arg(bands)
                            .arg(img.height())));
}

// Line Wrap ▸ Selected record only. The rows the record is GIVEN and the width its text is
// laid out IN come from one expression, so below the floor the record is measured and
// drawn at the floor — not measured at one width and painted at another, which dropped
// the tail of the record with no ellipsis and no tooltip to say so.
//
// Two views over the same record: one wide enough that the floor's own width is entirely
// on screen, one narrow enough that the floor binds. Both must give the record the same
// rows, and the narrow one's visible strip must be pixel-identical to the same strip of
// the reference — which is the paint's half of the claim, since a cell laid out in 80 px
// rather than 140 breaks its lines in different places.
void TestLogView::theSelectedRecordIsLaidOutInTheWidthItWasMeasuredIn()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve on this platform; there is no advance to wrap at");

    QTemporaryFile file;
    QVERIFY(writeLog(file, makeOneLongRecord(400)
                         + "2026-07-21 14:32:06,000 [main] INFO  net.socket - done\n"));
    Document doc;
    QVERIFY2(doc.open(file.fileName(),
                      QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    const auto prepare = [&](LogView &view, int avail) {
        useKnownPalette(view);
        settleLayout(view);
        view.setWrapMode(LogView::WrapMode::SelectedRecordOnly);
        putMessageOriginAt(view, avail);
        view.setCurrentRecord(0);
        settleRemeasure();
    };

    LogModel wideModel(&doc);
    LogView wide(&doc, &wideModel);
    wide.resize(900, 800);
    const int floorPx = floorWidth(wide);
    prepare(wide, floorPx); // the floor's own width, every pixel of it on screen

    LogModel narrowModel(&doc);
    LogView narrow(&doc, &narrowModel);
    narrow.resize(640, 800);
    const int strip = floorPx - 60; // below the floor: the width is taken up to it
    QVERIFY(strip > 0);
    prepare(narrow, strip);

    const int lines = wide.selWrapLines();
    QVERIFY2(lines > 3, "the record did not wrap; nothing here is being tested");
    QVERIFY(lines < RecordIndex::kDisplayLineCap);
    QVERIFY(wide.viewport()->height() >= lines * wide.lineHeight());
    QCOMPARE(narrow.selWrapLines(), lines);

    const int msg = wide.header()->count() - 1;
    const QRect from(wide.header()->sectionViewportPosition(msg), 0, strip,
                     lines * wide.lineHeight());
    const QRect to(narrow.header()->sectionViewportPosition(msg), 0, strip,
                   lines * narrow.lineHeight());
    QVERIFY(to.right() < narrow.viewport()->width());
    QCOMPARE(renderViewport(narrow).copy(to), renderViewport(wide).copy(from));
}

#include "tst_logview.moc"
