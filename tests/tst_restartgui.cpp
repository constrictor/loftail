#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>

#include "ConfigReset.h"
#include "LogFileStore.h"
#include "LogProfile.h"
#include "LogSettings.h"
#include "LogSettingsStore.h"
#include "MainWindow.h"
#include <QLabel>
#include "PreferencesDialog.h"
#include "RestartDialog.h"
#include "RestartTarget.h"

using namespace loftail;

// File ▸ Restart App as the user meets it (SPEC.md §4).
//
// Two halves. The DIALOG's rules — what closes itself, what stays up, what Abort does —
// are driven on a RestartDialog built on the stack, because a modal exec()'d from a test
// is a nested event loop looking for somewhere to deadlock. The MENU's rules go through a
// real MainWindow, because the claims are about gating and about which surface a refusal
// lands on, and neither is observable below that.
//
// Widgets are found by object name only, never by visible text.
class TestRestartGui : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void theMenuItemCarriesCtrlRAndNothingElseDoes();
    void theItemIsLiveOnALogAndDeadWithNoLogOpen();
    void withNoScriptTheBoxExplainsAndOffersPreferences();
    void aCleanRunClosesItselfAfterASecond();
    void outputOnStandardErrorKeepsTheDialogOpen();
    void aNonZeroExitKeepsTheDialogOpenAndNamesTheCode();
    void abortStopsAHangingScriptAndTheDialogSaysSo();
    void theConfiguredScriptIsWhatReachesTheDialog();

private:
    QString writeLog(const QString &name);
    static bool haveShell() { return QFileInfo::exists(QStringLiteral("/bin/sh")); }

    static RestartTarget local(const QString &script)
    {
        RestartTarget t;
        t.state = RestartTarget::State::Resolved;
        t.script = script;
        t.variables.append({QStringLiteral("LOGFILE"), QStringLiteral("/var/log/app.log")});
        return t;
    }

    // Spin the event loop until `done` or the budget runs out.
    template <class Predicate>
    static bool spinUntil(Predicate done, int budgetMs)
    {
        QElapsedTimer clock;
        clock.start();
        while (!done() && clock.elapsed() < budgetMs)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return done();
    }

    QTemporaryDir m_dir;
};

void TestRestartGui::initTestCase()
{
    QVERIFY(m_dir.isValid());
}

void TestRestartGui::init()
{
    QSettings settings;
    settings.remove(QStringLiteral("session"));
    settings.sync();
    // A log's settings outlive its tab, so a restart script left by an earlier case would
    // silently supply the very setting the next case is about.
    clearLogSettings();
}

QString TestRestartGui::writeLog(const QString &name)
{
    const QString path = m_dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    f.write("2026-01-01 00:00:00,000 [main] INFO  app - hello\n");
    return path;
}

void TestRestartGui::theMenuItemCarriesCtrlRAndNothingElseDoes()
{
    MainWindow w;
    auto *restart = w.findChild<QAction *>(QStringLiteral("restartAppAction"));
    QVERIFY(restart);
    QCOMPARE(restart->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_R));

    // "Ctrl+R is free" is a claim about the whole window and it decays: Reload took F5
    // deliberately so that this key stayed available, and nothing but a sweep keeps that
    // true the next time somebody reaches for an obvious accelerator.
    const QKeySequence ctrlR(Qt::CTRL | Qt::Key_R);
    int holders = 0;
    const auto actions = w.findChildren<QAction *>();
    for (const QAction *a : actions) {
        if (a->shortcuts().contains(ctrlR))
            ++holders;
    }
    QCOMPARE(holders, 1);
}

void TestRestartGui::theItemIsLiveOnALogAndDeadWithNoLogOpen()
{
    MainWindow w;
    w.show();
    auto *restart = w.findChild<QAction *>(QStringLiteral("restartAppAction"));
    QVERIFY(restart);
    QVERIFY2(!restart->isEnabled(), "live with no log open");

    const QString log = writeLog(QStringLiteral("app.log"));
    QVERIFY(!log.isEmpty());
    w.openFile(log);
    QTRY_VERIFY(restart->isEnabled());
}

void TestRestartGui::withNoScriptTheBoxExplainsAndOffersPreferences()
{
    const QString log = writeLog(QStringLiteral("noscript.log"));
    QVERIFY(!log.isEmpty());

    MainWindow w;
    w.show();
    w.openFile(log);
    QTest::qWait(200);

    // NOT a refusal and NOT a disabled item: a disabled QAction swallows Ctrl+R with no
    // feedback at all, and there is an answer to give.
    bool sawBox = false;
    bool sawPreferences = false;

    QTimer poll;
    poll.setInterval(20);
    QObject::connect(&poll, &QTimer::timeout, [&]() {
        QWidget *modal = QApplication::activeModalWidget();
        if (!modal)
            return;
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            if (box->objectName() != QLatin1String("restartNotConfiguredBox"))
                return;
            sawBox = true;
            auto *prefs =
                box->findChild<QPushButton *>(QStringLiteral("restartNotConfiguredPreferences"));
            QVERIFY(prefs);
            prefs->click();
            return;
        }
        if (qobject_cast<PreferencesDialog *>(modal)) {
            sawPreferences = true;
            // Escape, so the dialog leaves without writing anything.
            QTest::keyClick(modal, Qt::Key_Escape);
            poll.stop();
        }
    });
    poll.start();

    w.restartActiveApp();
    spinUntil([&]() { return sawPreferences; }, 4000);
    poll.stop();

    QVERIFY2(sawBox, "no explanation was offered for a log with no restart script");
    QVERIFY2(sawPreferences, "the box did not lead to Preferences");
}

void TestRestartGui::aCleanRunClosesItselfAfterASecond()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    RestartDialog dlg(QStringLiteral("app.log"), local(QStringLiteral("printf ok\n")));
    dlg.show();
    dlg.run();

    // Still up shortly after it finished: the dialog is meant to be SEEN to have worked,
    // not to flicker. Then gone, without anybody pressing anything.
    QVERIFY(spinUntil([&]() { return dlg.isFinished(); }, 5000));
    QVERIFY(dlg.succeeded());

    auto *output = dlg.findChild<QPlainTextEdit *>(QStringLiteral("restartOutput"));
    QVERIFY(output);
    QCOMPARE(output->toPlainText(), QStringLiteral("ok"));

    QVERIFY2(dlg.isVisible(), "closed before it could be read");
    QVERIFY(spinUntil([&]() { return !dlg.isVisible(); }, 4000));
}

void TestRestartGui::outputOnStandardErrorKeepsTheDialogOpen()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    // EXIT 0 AND A BYTE ON STDERR IS NOT A SUCCESS. A script that tidies up after a
    // failure still exits 0, and a warning nobody read is exactly what an auto-closing
    // dialog would hide.
    RestartDialog dlg(QStringLiteral("app.log"), local(QStringLiteral("printf boom >&2\n")));
    dlg.show();
    dlg.run();

    QVERIFY(spinUntil([&]() { return dlg.isFinished(); }, 5000));
    QVERIFY(!dlg.succeeded());

    auto *output = dlg.findChild<QPlainTextEdit *>(QStringLiteral("restartOutput"));
    QVERIFY(output);
    QVERIFY(output->toPlainText().contains(QStringLiteral("boom")));

    auto *status = dlg.findChild<QLabel *>(QStringLiteral("restartStatus"));
    QVERIFY(status);
    QVERIFY(!status->text().isEmpty());

    // The button swap is how the dialog says which state it is in, and it is asserted on
    // the two OBJECT NAMES rather than on any text: a swapping label would be unreadable
    // from outside.
    QVERIFY(!dlg.findChild<QPushButton *>(QStringLiteral("restartAbortButton"))->isVisible());
    QVERIFY(dlg.findChild<QPushButton *>(QStringLiteral("restartCloseButton"))->isVisible());

    // Well past the auto-close budget, and still up.
    QTest::qWait(1600);
    QVERIFY2(dlg.isVisible(), "a run that wrote to standard error closed itself");
}

void TestRestartGui::aNonZeroExitKeepsTheDialogOpenAndNamesTheCode()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    RestartDialog dlg(QStringLiteral("app.log"), local(QStringLiteral("exit 3\n")));
    dlg.show();
    dlg.run();

    QVERIFY(spinUntil([&]() { return dlg.isFinished(); }, 5000));
    QVERIFY(!dlg.succeeded());

    auto *status = dlg.findChild<QLabel *>(QStringLiteral("restartStatus"));
    QVERIFY(status);
    QVERIFY2(status->text().contains(QStringLiteral("3")), qPrintable(status->text()));

    QTest::qWait(1600);
    QVERIFY2(dlg.isVisible(), "a run that exited non-zero closed itself");
}

void TestRestartGui::abortStopsAHangingScriptAndTheDialogSaysSo()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    RestartDialog dlg(QStringLiteral("app.log"), local(QStringLiteral("sleep 60\n")));
    dlg.show();
    dlg.run();

    QTest::qWait(300);
    QVERIFY(!dlg.isFinished());

    auto *abort = dlg.findChild<QPushButton *>(QStringLiteral("restartAbortButton"));
    QVERIFY(abort);
    QVERIFY(abort->isVisible());
    abort->click();

    QVERIFY2(spinUntil([&]() { return dlg.isFinished(); }, 8000), "abort did not end the run");

    // It STAYS UP after an abort, and says so — an abort is a failure to restart, and
    // vanishing would leave nothing to say whether anything happened.
    QVERIFY(dlg.isVisible());
    QVERIFY(!dlg.succeeded());
    auto *status = dlg.findChild<QLabel *>(QStringLiteral("restartStatus"));
    QVERIFY(status);
    QVERIFY(!status->text().isEmpty());
}

void TestRestartGui::theConfiguredScriptIsWhatReachesTheDialog()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    // End to end through the settings tree: a script set at the DEFAULTS level reaches
    // the log that inherits it, with LOGFILE naming that log. The touched file is the
    // evidence, because it is the one thing only a real run can produce.
    const QString log = writeLog(QStringLiteral("configured.log"));
    QVERIFY(!log.isEmpty());
    const QString marker = m_dir.filePath(QStringLiteral("restart-ran"));
    QFile::remove(marker);

    LogSettingsStore store(LogSettingsStore::defaultDir());
    LogSettingsTree tree = store.load();
    LogProfile defaults = tree.defaults();
    defaults.restartScript =
        QStringLiteral("printf '%s' \"$LOGFILE\" > %1\n").arg(marker);
    tree.setDefaults(defaults);
    QVERIFY(store.save(tree));

    MainWindow w;
    w.show();
    w.openFile(log);
    QTest::qWait(200);

    // The dialog is modal and exec()s, so it is dismissed from a timer rather than waited
    // on — a modal exec()'d from a test is a nested event loop looking for a deadlock.
    QTimer poll;
    poll.setInterval(20);
    QObject::connect(&poll, &QTimer::timeout, [&]() {
        if (auto *dlg = qobject_cast<RestartDialog *>(QApplication::activeModalWidget())) {
            if (dlg->isFinished()) {
                dlg->accept();
                poll.stop();
            }
        }
    });
    poll.start();

    w.restartActiveApp();
    poll.stop();

    QVERIFY2(QFileInfo::exists(marker), "the configured script never ran");
    QFile f(marker);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(f.readAll()), log);
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
    QApplication::setApplicationName(QStringLiteral("loftail-test-restartgui"));

    TestRestartGui tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_restartgui.moc"
