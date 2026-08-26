#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QLineEdit>
#include <QScrollBar>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>

#include "ConfigReset.h"
#include "DensityScrollBar.h"
#include "Document.h"
#include "DocumentContext.h"
#include "DocumentView.h"
#include "FindBar.h"
#include "Highlight.h"
#include "LogModel.h"
#include "LogView.h"
#include "MainWindow.h"

using namespace loftail;

// The density marks inside the scrollbar (SPEC.md §5, ARCHITECTURE.md §7.1.7), driven
// through a REAL MainWindow like tst_multidoc and tst_find.
//
// Almost everything here has to be read off RENDERED PIXELS, because that is the only
// place any of it shows: the widget holds the right buckets whether or not they are
// drawn where the thumb would put them, and the whole claim of the feature is about
// where a mark sits.
class TestDensityBar : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    // One FATAL among `lines` INFO records at `fatalAt`, which the seeded rules
    // (HighlighterSet::defaults) colour and nothing else in the log matches.
    // Records below `tallBefore` get continuation lines every `tallEvery`, which makes
    // the file's LINE space and its RECORD space differ NON-UNIFORMLY. Uniformly would
    // not do: stretching every record equally leaves the two coordinates proportional,
    // so a bar placing marks by record index would land in the same place and the
    // placement case would assert nothing.
    static void writeLog(const QString &path, int lines, int fatalAt, int tallEvery = 0,
                         int tallBefore = 0)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        for (int i = 0; i < lines; ++i) {
            f.write(QStringLiteral("2026-08-25 10:00:%1,000 [main] %2 app.svc - line %3 needle%4\n")
                        .arg(i % 60, 2, 10, QLatin1Char('0'))
                        .arg(i == fatalAt ? QStringLiteral("FATAL") : QStringLiteral("INFO "))
                        .arg(i)
                        .arg(i % 100 == 3 ? QStringLiteral("!") : QString())
                        .toUtf8());
            if (tallEvery > 0 && i < tallBefore && i % tallEvery == 0)
                for (int k = 0; k < 16; ++k)
                    f.write("    at continuation line\n"); // no timestamp: same record
        }
        f.close();
    }

    static LogView *logView(const MainWindow &w)
    {
        return w.findChild<LogView *>(QStringLiteral("logView"));
    }

    static DensityScrollBar *bar(const MainWindow &w)
    {
        LogView *v = logView(w);
        return v ? v->densityBar() : nullptr;
    }

    static void openAndSettle(MainWindow &w, const QString &path)
    {
        w.openFile(path);
        QTRY_VERIFY(logView(w) != nullptr);
        QTRY_VERIFY(logView(w)->recordCount() > 0);
        // The scan is sliced over the next few frames by design; a test wants the
        // finished answer, so it asks for it directly.
        QTRY_VERIFY(bar(w) != nullptr);
        bar(w)->scanNowForTests();
        QCoreApplication::processEvents();
    }

    // The vertical run of pixels in the bar that are neither its base fill nor its
    // divider — i.e. the marks. Returns {-1, -1} when there are none.
    //
    // Rows under the THUMB are skipped: it is drawn translucent over the marks, so its
    // own pixels differ from the base too and would otherwise read as a mark the length
    // of a viewport. Every case that reads a band therefore parks the thumb away from
    // the mark it is asking about.
    static QPair<int, int> markBand(DensityScrollBar *s, int fromX, int toX)
    {
        const QImage img = s->grab().toImage();
        const QColor base = s->palette().base().color();
        const QRect thumb = s->thumbRect();
        int top = -1;
        int bottom = -1;
        for (int y = 0; y < img.height(); ++y) {
            if (y >= thumb.top() && y <= thumb.bottom())
                continue;
            bool marked = false;
            for (int x = fromX; x < qMin(toX, img.width()); ++x)
                if (QColor(img.pixel(x, y)) != base)
                    marked = true;
            if (!marked)
                continue;
            if (top < 0)
                top = y;
            bottom = y;
        }
        return {top, bottom};
    }

    // The rules lane occupies the left of the bar and the find lane the right; the
    // exact split is DensityScrollBar's business, so the bands are sampled well inside
    // each.
    static QPair<int, int> rulesBand(DensityScrollBar *s) { return markBand(s, 2, s->width() / 2); }
    static QPair<int, int> findBand(DensityScrollBar *s)
    {
        return markBand(s, s->width() - 3, s->width());
    }

    // Park the thumb at the top of the range, so a mark anywhere else in the log is not
    // hidden under it. Also detaches follow, which none of the band cases cares about.
    static void scrollToTop(MainWindow &w)
    {
        logView(w)->verticalScrollBar()->setValue(0);
        QCoreApplication::processEvents();
    }

private slots:
    void init()
    {
        clearLogSettings();
        // The marks' on/off is an APPLICATION preference in plain QSettings, not one of
        // the per-log stores — so a case that switches them off leaves them off for every
        // case after it, and this suite would pass or fail on the order QtTest ran them
        // in. The same trap ConfigReset.h exists for, one store over.
        QSettings().remove(QStringLiteral("densityStrip"));
    }

    // The marks are IN the scrollbar, so there is one control and not two: the view's
    // own verticalScrollBar() is the marked bar, and switching the marks off leaves an
    // ordinary — narrower — scrollbar behind.
    void theMarksLiveInTheScrollbarItselfAndGiveTheirWidthBackWhenSwitchedOff()
    {
        const QString path = m_dir.filePath(QStringLiteral("a.log"));
        writeLog(path, 400, 200);
        MainWindow w;
        w.resize(900, 600);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        openAndSettle(w, path);

        LogView *v = logView(w);
        QVERIFY(v->densityStripVisible());
        DensityScrollBar *s = bar(w);
        QVERIFY(s);
        // Not a widget BESIDE the scrollbar: the view has exactly one, and this is it.
        QCOMPARE(static_cast<QScrollBar *>(s), v->verticalScrollBar());
        QVERIFY(s->isVisible());
        const int withMarks = v->viewport()->width();
        const int markedWidth = s->width();

        QAction *toggle = w.findChild<QAction *>(QStringLiteral("densityStripAction"));
        QVERIFY(toggle);
        QVERIFY(toggle->isChecked());
        toggle->trigger();
        QVERIFY(!v->densityStripVisible());
        QVERIFY(v->densityBar() == nullptr); // replaced, so it stops scanning
        QVERIFY(v->verticalScrollBar() != nullptr);
        QCoreApplication::processEvents();
        // A plain bar is narrower, and every pixel it gives up goes to the text.
        const int plainWidth = v->verticalScrollBar()->width();
        QVERIFY2(plainWidth <= markedWidth, "a plain scrollbar is not wider than the marked one");
        QCOMPARE(v->viewport()->width(), withMarks + (markedWidth - plainWidth));
    }

    // The claim the whole design turns on: a mark sits where the SCROLLBAR would put
    // that record, in line units — not at its record index, which on a log carrying
    // multi-line records is somewhere else entirely.
    void aMarkSitsWhereTheScrollbarWouldPutItsRecord()
    {
        const QString path = m_dir.filePath(QStringLiteral("tall.log"));
        // The FIRST 300 records carry the continuation lines and the FATAL is three
        // quarters of the way through by record — so by line it is nine tenths of the
        // way down, and the two candidate positions are some ninety pixels apart.
        writeLog(path, 800, 600, 4, 300);
        MainWindow w;
        w.resize(900, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        openAndSettle(w, path);
        scrollToTop(w);

        LogView *v = logView(w);
        DensityScrollBar *s = bar(w);
        const auto band = rulesBand(s);
        QVERIFY2(band.first >= 0, "the seeded FATAL rule marked nothing at all");

        const int fatalRow = 600;
        const int h = s->height();
        // Both fractions are counted from the file that was WRITTEN, not asked of the
        // view: an expectation taken from scrollFractionOfRow() would move with the very
        // function under test and the case would pass however that function was spelled.
        // 800 records; the first 300 carry 16 continuation lines every 4th, so 75 x 16 =
        // 1200 extra lines, all of them ABOVE the FATAL. 2000 lines in all, 1800 of them
        // before it.
        QCOMPARE(v->recordCount(), 800);
        const int lineY = int(1800.0 / 2000.0 * h);
        const int rowY = int(qreal(fatalRow) / 800.0 * h);
        QVERIFY2(qAbs(lineY - rowY) > 20, "line space and record space did not diverge");
        // The band is a bucket wide plus the minimum mark height, so it is a range and
        // not a point; what matters is which of the two candidate positions it covers.
        QVERIFY(band.first <= lineY + 4 && band.second >= lineY - 4);
        QVERIFY(!(band.first <= rowY + 4 && band.second >= rowY - 4));
    }

    // What uniting the two controls BUYS, and the thing a separate strip could not do:
    // scroll until the marked record is the one at the top of the viewport, and the mark
    // is under the thumb. A strip beside the bar was out by the header's height plus
    // whatever the style spends on arrow buttons, and by the page step besides.
    void theMarkForARecordIsCoveredByTheThumbThatShowsIt()
    {
        const QString path = m_dir.filePath(QStringLiteral("thumb.log"));
        writeLog(path, 800, 600, 4, 300);
        MainWindow w;
        w.resize(900, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        openAndSettle(w, path);

        LogView *v = logView(w);
        DensityScrollBar *s = bar(w);
        // The band while the thumb is parked out of the way, so markBand can see it.
        scrollToTop(w);
        const auto band = rulesBand(s);
        QVERIFY2(band.first >= 0, "the seeded FATAL rule marked nothing at all");

        // Now put the FATAL record at the top of the viewport — the scroll position the
        // reader lands on by clicking that mark — and ask where the thumb went.
        const qreal f = v->scrollFractionOfRow(600);
        v->verticalScrollBar()->setValue(
            int(f * (v->verticalScrollBar()->maximum() + v->verticalScrollBar()->pageStep())));
        QCoreApplication::processEvents();
        const QRect thumb = s->thumbRect();
        QVERIFY2(band.first <= thumb.bottom() && band.second >= thumb.top(),
                 "the mark and the thumb showing that record are in different places");
        // And specifically at its TOP, not merely somewhere along a viewport-tall thumb.
        QVERIFY(qAbs(band.first - thumb.top()) <= 6);
    }

    // Find marks go in their own lane, and arming Find must not throw away the rule
    // lane — that is the whole reason there are two lanes over one bucket geometry.
    void findMarksItsOwnLaneAndLeavesTheRuleLaneAlone()
    {
        const QString path = m_dir.filePath(QStringLiteral("find.log"));
        writeLog(path, 400, 200);
        MainWindow w;
        w.resize(900, 600);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        openAndSettle(w, path);
        scrollToTop(w);

        DensityScrollBar *s = bar(w);
        QVERIFY(rulesBand(s).first >= 0);
        QCOMPARE(findBand(s).first, -1); // nothing is being searched for yet

        FindBar *fb = w.findChild<FindBar *>();
        QVERIFY(fb);
        QLineEdit *box = fb->findChild<QLineEdit *>();
        QVERIFY(box);
        fb->activate();
        QTest::keyClicks(box, QStringLiteral("needle!"));
        QTRY_VERIFY(!logView(w)->findMatcher().isEmpty());
        s->scanNowForTests();
        scrollToTop(w);

        QVERIFY2(findBand(s).first >= 0, "the armed query marked nothing");
        QVERIFY2(rulesBand(s).first >= 0, "arming Find threw the rule lane away");

        // And closing the bar takes the find marks with it, exactly as it takes the
        // in-table ones: the query is off the screen, so a mark claiming it is not.
        logView(w)->clearFindMatcher();
        s->scanNowForTests();
        QCOMPARE(findBand(s).first, -1);
        QVERIFY(rulesBand(s).first >= 0);
    }

    // A click is a scroll, and it is the USER's scroll — so it detaches follow exactly
    // as dragging the thumb does, rather than being fought by the next append. On a
    // minimap a click means "go there", never the page-step an ordinary scrollbar does.
    void clickingTheBarScrollsThereAndDetachesFollow()
    {
        const QString path = m_dir.filePath(QStringLiteral("click.log"));
        writeLog(path, 4000, 3000);
        MainWindow w;
        w.resize(900, 600);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        openAndSettle(w, path);

        LogView *v = logView(w);
        DensityScrollBar *s = bar(w);
        QVERIFY(v->following()); // every open follows the tail

        QTest::mouseClick(s, Qt::LeftButton, Qt::NoModifier, QPoint(s->width() / 2, 10));
        QVERIFY(!v->following());
        // Near the TOP of the range, because that is where the click landed — a page
        // step would have moved it by one screen and left it near the bottom.
        QVERIFY(v->verticalScrollBar()->value() < v->verticalScrollBar()->maximum() / 4);
    }

    // A background tab must not scan: ten open logs would otherwise be ten scans, and
    // nine of them for marks nobody can see.
    void aBackgroundTabDoesNotScan()
    {
        const QString first = m_dir.filePath(QStringLiteral("bg1.log"));
        const QString second = m_dir.filePath(QStringLiteral("bg2.log"));
        writeLog(first, 300, 100);
        writeLog(second, 300, 100);
        MainWindow w;
        w.resize(900, 600);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.openFile(first);
        w.openFile(second);
        QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);
        const auto views = w.findChildren<LogView *>(QStringLiteral("logView"));
        QTRY_VERIFY(views.at(0)->recordCount() > 0 && views.at(1)->recordCount() > 0);

        QTabWidget *tabs = w.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
        QVERIFY(tabs);
        LogView *background = views.at(tabs->currentIndex() == 0 ? 1 : 0);
        QVERIFY(background->densityBar());
        QVERIFY(!background->densityBar()->isVisible());
        // Let a good many slices' worth of timer fire. The visible tab's bar finishes;
        // the hidden one has not started, because its timer never runs.
        QTest::qWait(200);
        QCOMPARE(background->densityBar()->map().scanned(DensityMap::Lane::Rules), 0);
    }
};

int main(int argc, char *argv[])
{
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test-densitybar"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-densitybar"));

    TestDensityBar tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_densitybar.moc"
