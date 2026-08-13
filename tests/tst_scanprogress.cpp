#include <QtTest>

#include <QApplication>
#include <QFile>
#include <QHeaderView>
#include <QProgressBar>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWidget>

#include "LogModel.h"
#include "LogSettingsStore.h"
#include "LogView.h"
#include "MainWindow.h"

using namespace loftail;

// The scan indicator and its stop button (SPEC.md §3, "Scanning can be cancelled").
// Cancelling used to be a File-menu entry, which was the only thing in the menu bar
// enabled for a fraction of a second at a time; it is now a button beside the progress
// bar, and the two are shown and hidden as one. What this pins is the WIRING — that the
// pair appears exactly while a scan is running, and that pressing the button reaches
// IndexController::cancel(). That the cancel leaves a consistent partial index is
// tst_indexcontroller's, and is not repeated here.
class TestScanProgress : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString       m_small; // two records: the scan is over before it is worth watching
    QString       m_large; // several Indexer chunks, so a cancel has somewhere to land

    // Roughly 100 bytes a record, so kRecords records span several of the indexer's
    // 4 MB chunks — the cancel is requested on the first progress report, which is one
    // chunk in, and the assertion is that the rest never got scanned.
    static constexpr int kRecords = 300000;

    static bool write(const QString &path, const QByteArray &bytes)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        return f.write(bytes) == bytes.size();
    }

    static QWidget *progressBox(MainWindow &w)
    {
        return w.statusBar()->findChild<QWidget *>(QStringLiteral("scanProgress"));
    }
    static QToolButton *stopButton(MainWindow &w)
    {
        return w.statusBar()->findChild<QToolButton *>(QStringLiteral("cancelIndexButton"));
    }
    static LogModel *modelOf(LogView *view)
    {
        return view ? qobject_cast<LogModel *>(view->header()->model()) : nullptr;
    }

private slots:
    void initTestCase();
    void init();
    void theStopButtonSitsInTheStatusBarAndIsHiddenWithNoFile();
    void theScanIndicatorGoesWhenTheScanDoes();
    void pressingStopEndsTheScanShort();
};

void TestScanProgress::init()
{
    // A restored session would open a tab of its own and start a second scan.
    QSettings settings;
    settings.remove(QStringLiteral("session"));
    settings.sync();
    QFile::remove(LogSettingsStore(LogSettingsStore::defaultDir()).filePath());
}

void TestScanProgress::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_small = m_dir.filePath(QStringLiteral("small.log"));
    m_large = m_dir.filePath(QStringLiteral("large.log"));

    QVERIFY(write(m_small,
        "2026-07-21 10:00:00,000 [main] INFO  net.io - starting\n"
        "2026-07-21 10:00:01,000 [work] ERROR db.pool - boom\n"));

    QByteArray big;
    big.reserve(kRecords * 110);
    for (int i = 0; i < kRecords; ++i) {
        big += QStringLiteral("2026-07-21 10:00:00,%1 [work] INFO  net.io - record %2 "
                              "with enough text to make it a realistic width\n")
                   .arg(i % 1000, 3, 10, QLatin1Char('0'))
                   .arg(i)
                   .toUtf8();
    }
    QVERIFY(write(m_large, big));
}

void TestScanProgress::theStopButtonSitsInTheStatusBarAndIsHiddenWithNoFile()
{
    MainWindow w;
    w.show();

    QWidget *box = progressBox(w);
    QVERIFY2(box, "the scan indicator is not in the status bar");
    QVERIFY(stopButton(w));
    QVERIFY(box->findChild<QProgressBar *>(QStringLiteral("scanProgressBar")));

    // Nothing is being scanned, so there is nothing to stop and nothing to report.
    QVERIFY(!box->isVisible());
    w.close();
}

void TestScanProgress::theScanIndicatorGoesWhenTheScanDoes()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();

    w.openFile(m_small);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);

    // Both halves are the same widget's children, so the button cannot outlive the bar
    // and offer to stop a scan that has finished.
    QWidget *box = progressBox(w);
    QVERIFY(box);
    QTRY_VERIFY(!box->isVisible());
    QVERIFY(!stopButton(w)->isVisible());
    w.close();
}

void TestScanProgress::pressingStopEndsTheScanShort()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();

    w.openFile(m_large);
    QWidget *box = progressBox(w);
    QVERIFY(box);

    // The indicator appears on the first progress report, which is one 4 MB chunk into
    // a file of several. Press stop in that same pass of the event loop: spinning it
    // again here would be racing the scan we are about to cut short.
    QTRY_VERIFY_WITH_TIMEOUT(box->isVisible(), 30000);
    QVERIFY(stopButton(w)->isVisible());
    QTest::mouseClick(stopButton(w), Qt::LeftButton);

    QTRY_VERIFY_WITH_TIMEOUT(!box->isVisible(), 30000);

    // The tab is still there and holds what was scanned before the stop — which is
    // some of the file and not all of it.
    const QList<LogView *> views = w.findChildren<LogView *>(QStringLiteral("logView"));
    QCOMPARE(views.size(), 1);
    LogModel *model = modelOf(views.at(0));
    QVERIFY(model);
    QVERIFY(model->rowCount() > 0);
    QVERIFY2(model->rowCount() < kRecords, "the whole file was scanned: stop did nothing");
    w.close();
}

int main(int argc, char *argv[])
{
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-scanprogress"));

    TestScanProgress tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_scanprogress.moc"
