#include <QtTest>

#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QLineEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include "LogFormatDialog.h"
#include "LogView.h"
#include "MainWindow.h"

using namespace loftail;

// The open flow around the Log Format dialog (SPEC.md §4). The dialog appears only
// when loftail is asked to open a never-seen file the fallback pattern cannot parse,
// and dismissing it (Esc) CANCELS THE OPEN: no file is opened, and whatever was on
// screen stays. Accepting it opens the file with the pattern entered. Drives the real
// MainWindow under the offscreen platform, dismissing the modal dialog from a timer.
class TestOpenFlow : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString       m_good;   // parses with the app's default pattern — never prompts
    QString       m_weird;  // a real-world log4cplus layout the default cannot parse

    static bool write(const QString &path, const QByteArray &bytes)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(bytes);
        return true;
    }

    // Dismiss the modal dialog once it appears, by the given key. Returns a flag the
    // caller reads afterwards to assert the dialog really was shown — an assertion
    // that would otherwise pass vacuously if the prompt silently stopped appearing.
    struct Dismisser
    {
        bool seen = false;
        QTimer timer;
    };
    static void dismissWhenShown(Dismisser &d, Qt::Key key)
    {
        d.timer.setInterval(10);
        QObject::connect(&d.timer, &QTimer::timeout, [&d, key]() {
            auto *dlg = qobject_cast<LogFormatDialog *>(QApplication::activeModalWidget());
            if (!dlg)
                return;
            d.seen = true;
            d.timer.stop();
            QTest::keyClick(dlg, key);
        });
        d.timer.start();
    }

private slots:
    void initTestCase();
    // Each case closes its window, which saves a session that the NEXT case's window
    // would restore — and an open now ADDS a tab rather than replacing one, so a
    // leaked session would show up as an extra view. Start each case clean.
    void init();
    // First: it needs a MainWindow with nothing open, which only holds before any
    // case below closes a window and saves a session pointing at its file.
    void escapeWithNothingOpenLeavesEmptyView();
    void escapeCancelsOpenAndKeepsCurrentFile();
    void acceptedPatternOpensTheFile();
};

void TestOpenFlow::init()
{
    QSettings settings;
    settings.remove(QStringLiteral("session"));
    settings.sync();
}

void TestOpenFlow::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_good = m_dir.filePath(QStringLiteral("good.log"));
    m_weird = m_dir.filePath(QStringLiteral("weird.log"));

    QVERIFY(write(m_good,
        "2026-07-21 10:00:00,000 [main] INFO  net.io - starting\n"
        "2026-07-21 10:00:01,000 [work] ERROR db.pool - boom\n"));

    // %D{%m/%d/%y %H:%M:%S} %-5p %c [%t] - %m%n, with a multi-line first record.
    QVERIFY(write(m_weird,
        "03/12/26 11:50:47 DEBUG Vms::App [] - log4cplus config:\n"
        "log4cplus.threadPoolSize=1\n"
        "03/12/26 11:50:48 INFO  Vms::Http [7f2a] - listening on 8080\n"));
}

void TestOpenFlow::escapeWithNothingOpenLeavesEmptyView()
{
    MainWindow w;
    w.show();
    QCOMPARE(w.findChildren<LogView *>().size(), 0);

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape);
    w.openFile(m_weird);
    QVERIFY2(d.seen, "the format dialog was never shown");

    // Cancelled with nothing to fall back to: no view at all, rather than a table
    // of unparsed plain text.
    QTest::qWait(100);
    QCOMPARE(w.findChildren<LogView *>().size(), 0);
    w.close();
}

void TestOpenFlow::escapeCancelsOpenAndKeepsCurrentFile()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();

    w.openFile(m_good); // parses with the default pattern: no prompt
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 1);
    QTest::qWait(200); // let indexing finish
    LogView *before = w.findChild<LogView *>();
    QCOMPARE(w.windowTitle(), QStringLiteral("loftail — good.log"));

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape);
    w.openFile(m_weird); // the default cannot parse it: prompts, and we press Esc
    QVERIFY2(d.seen, "the format dialog was never shown");

    // The cancelled open changed nothing: same file, same view, still usable.
    QCOMPARE(w.windowTitle(), QStringLiteral("loftail — good.log"));
    // A cancelled open must create NOTHING: with several files openable, "the view is
    // unchanged" also has to mean "no second tab appeared".
    QCOMPARE(w.findChildren<LogView *>().size(), 1);
    QCOMPARE(w.findChild<LogView *>(), before);
    w.close();
}

void TestOpenFlow::acceptedPatternOpensTheFile()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();

    // Type the layout that does parse this file, then accept.
    Dismisser d;
    d.timer.setInterval(10);
    connect(&d.timer, &QTimer::timeout, [&d, &w]() {
        auto *dlg = qobject_cast<LogFormatDialog *>(QApplication::activeModalWidget());
        if (!dlg)
            return;
        d.seen = true;
        d.timer.stop();
        auto *edit = dlg->findChild<QLineEdit *>();
        QVERIFY(edit);
        edit->setText(QStringLiteral("%D{%m/%d/%y %H:%M:%S} %-5p %c [%t] - %m%n"));
        dlg->accept();
    });
    d.timer.start();

    w.openFile(m_weird);
    QVERIFY2(d.seen, "the format dialog was never shown");
    QTRY_COMPARE(w.findChildren<LogView *>().size(), 1);
    QTest::qWait(200);
    QCOMPARE(w.windowTitle(), QStringLiteral("loftail — weird.log"));
    w.close();
}

int main(int argc, char *argv[])
{
    // Isolate persistent state: the per-file format cache must start empty, or a
    // remembered format would suppress the very prompt under test.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-openflow"));

    TestOpenFlow tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_openflow.moc"
