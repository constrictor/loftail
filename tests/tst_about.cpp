#include <QtTest>

#include <QAction>
#include <QApplication>

#include "MainWindow.h"
#include "Version.h"

using namespace loftail;

// Help ▸ About (SPEC.md §1 "Which build this is"): the only place a RUNNING loftail
// says which build it is. `--version` answers the same question, but an installed
// package launched from a desktop menu has no command line to ask it on — which was
// the whole gap.
//
// The text is tested rather than the dialog: aboutText() is public precisely so that
// this needs no modal window (the same split buildRecordMenu() has). What the dialog
// adds beyond it — plain-text format, selectable text — is Qt's behaviour, not
// loftail's.
class TestAbout : public QObject
{
    Q_OBJECT

private slots:
    void theMenuOffersAbout();
    void theTextNamesTheReleaseAndTheBuild();
};

void TestAbout::theMenuOffersAbout()
{
    MainWindow w;
    auto *about = w.findChild<QAction *>(QStringLiteral("aboutAction"));
    QVERIFY(about);
    // Always available. A window that has opened nothing — because the log is on a
    // host that is not answering — is exactly when the build id is asked for.
    QVERIFY(about->isEnabled());
    // AboutRole is what puts it in the application menu on macOS instead of leaving a
    // Help menu that duplicates the platform's own.
    QCOMPARE(about->menuRole(), QAction::AboutRole);
}

// Both configurations at once, as tst_scaffold does for the version string itself:
// which one this binary is depends on how it was configured, and neither is the odd
// case — a local build carries no build id and a CI build does.
void TestAbout::theTextNamesTheReleaseAndTheBuild()
{
    const QString text = MainWindow::aboutText();

    QVERIFY(text.contains(QStringLiteral("loftail")));
    QVERIFY(text.contains(applicationVersion()));

    const QString build = applicationBuildId();
    if (build.isEmpty()) {
        // Says it is a local build rather than showing an empty field, which would
        // read as "loftail does not know" instead of "there is no CI run behind this".
        QVERIFY(!text.contains(QStringLiteral("Build: \n")));
        QVERIFY(!text.endsWith(QStringLiteral("Build: ")));
    } else {
        // Verbatim, so what is pasted into a bug report is what the workflow stamped
        // and what release.yml checks the promoted artifacts against.
        QVERIFY(text.contains(build));
    }
}

QTEST_MAIN(TestAbout)
#include "tst_about.moc"
