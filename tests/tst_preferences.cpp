#include <QtTest>

#include <QApplication>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>

#include "DefaultFormatStore.h"
#include "FormatCache.h"
#include "LogFormatDialog.h"
#include "PreferencesDialog.h"

using namespace loftail;

// The DEFAULT log format (SPEC.md §4 "Default log format", PLAN.md M18): what a file
// loftail has not seen before is tried with, so one house pattern is entered once rather
// than confirmed per file. Two halves are tested here — the store, which decides what a
// default IS, and the Preferences dialog, which is where one is entered. What the default
// then DOES on an open is tst_openflow's job, because only the real MainWindow can show
// that no dialog appeared.
class TestPreferences : public QObject
{
    Q_OBJECT

private:
    // A QSettings of its own per case: these write real keys, and the store is the
    // subject rather than the scaffolding.
    static QSettings *makeStore(QTemporaryDir &dir)
    {
        return new QSettings(dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);
    }

private slots:
    void unsetYieldsBuiltIn();
    void defaultRoundTrips();
    void anEmptyPatternIsAnAnswer();
    void defaultCarriesNoPerFileFields();
    void forgetAllClearsTheCache();
    void dialogSeedsAndReturns();
    void emptySampleIsHarmless();
    void logFormatDialogReportsUseAsDefault();
};

void TestPreferences::unsetYieldsBuiltIn()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    std::unique_ptr<QSettings> store(makeStore(dir));

    // Nothing saved: the built-in, never an empty pattern. An open always has something
    // to try, so load() has no "unset" answer to give.
    const FormatSettings s = DefaultFormatStore::load(*store);
    QCOMPARE(s, DefaultFormatStore::builtIn());
    QVERIFY(!s.pattern.isEmpty());
}

void TestPreferences::defaultRoundTrips()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    std::unique_ptr<QSettings> store(makeStore(dir));

    FormatSettings s;
    s.pattern = QStringLiteral("%D{%m/%d/%y %H:%M:%S} %-5p %c [%t] - %m%n");
    s.encoding = Encoding::Utf16LE;                  // not the default, so a lost write shows
    s.sourceZone.kind = ZoneChoice::Kind::FixedOffset;
    s.sourceZone.offsetSeconds = -5 * 3600;

    DefaultFormatStore::save(*store, s);

    const FormatSettings back = DefaultFormatStore::load(*store);
    QCOMPARE(back.pattern, s.pattern);
    QCOMPARE(back.encoding, Encoding::Utf16LE);
    QCOMPARE(back.sourceZone.kind, ZoneChoice::Kind::FixedOffset);
    QCOMPARE(back.sourceZone.offsetSeconds, -5 * 3600);
}

void TestPreferences::anEmptyPatternIsAnAnswer()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    std::unique_ptr<QSettings> store(makeStore(dir));

    // Clearing the field is how a user says "ask me about every log": an empty pattern
    // parses nothing, so every never-seen file reaches the dialog. Reading it back as
    // "nothing saved" would reinstate the built-in and make that setting unreachable —
    // which is why load() tests for PRESENCE rather than for emptiness.
    FormatSettings s;
    s.pattern.clear();
    DefaultFormatStore::save(*store, s);

    QVERIFY(DefaultFormatStore::load(*store).pattern.isEmpty());
}

void TestPreferences::defaultCarriesNoPerFileFields()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    std::unique_ptr<QSettings> store(makeStore(dir));

    // The default is the three things the Log Format dialog edits. The timestamp display
    // belongs to the timestamp column's header menu and the run-start axis to the Run
    // pane; both are statements about ONE log. Widening save() to the whole struct would
    // make every newly opened file inherit some other file's timestamp mode and run
    // splitting — invisible until someone wonders why a fresh log opened pre-split.
    FormatSettings s;
    s.pattern = QStringLiteral("%p %c - %m%n");
    s.timeDisplay = TimeDisplay::RunSeconds;
    s.runStartPattern = QStringLiteral("=== BOOT ===");
    s.runStartIsRegex = true;
    s.runStartCaseSensitive = true;

    DefaultFormatStore::save(*store, s);

    const FormatSettings back = DefaultFormatStore::load(*store);
    QCOMPARE(back.pattern, s.pattern);              // the format did survive
    QCOMPARE(back.timeDisplay, TimeDisplay::AsWritten);
    QVERIFY(back.runStartPattern.isEmpty());
    QVERIFY(!back.runStartIsRegex);
    QVERIFY(!back.runStartCaseSensitive);
}

void TestPreferences::forgetAllClearsTheCache()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    std::unique_ptr<QSettings> store(makeStore(dir));

    const QString a = dir.filePath(QStringLiteral("a.log"));
    const QString b = dir.filePath(QStringLiteral("b.log"));

    FormatSettings s;
    s.pattern = QStringLiteral("%p %c - %m%n");
    FormatCache::save(*store, a, s);
    FormatCache::save(*store, b, s);
    QVERIFY(FormatCache::load(*store, a).has_value());
    QVERIFY(FormatCache::load(*store, b).has_value());

    // The escape hatch behind Preferences ▸ Forget Remembered Formats: a per-file entry
    // outranks the default, so without this a changed default appears to do nothing for
    // every file already opened.
    FormatCache::forgetAll(*store);

    QVERIFY(!FormatCache::load(*store, a).has_value());
    QVERIFY(!FormatCache::load(*store, b).has_value());
    // And the store is still usable afterwards, rather than left in a state where the
    // next save writes into a half-removed array.
    FormatCache::save(*store, a, s);
    QVERIFY(FormatCache::load(*store, a).has_value());
}

void TestPreferences::dialogSeedsAndReturns()
{
    const QByteArray sample =
        "03/12/26 11:50:47 DEBUG Vms::App [] - log4cplus config:\n"
        "03/12/26 11:50:48 INFO  Vms::Http [7f2a] - listening on 8080\n";

    FormatSettings initial;
    initial.pattern = QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n");
    initial.encoding = Encoding::Utf8;

    // Constructed on the stack and never exec()'d: a modal dialog in a test is a nested
    // event loop looking for somewhere to deadlock. Children are reached by OBJECT NAME,
    // never by visible text (ARCHITECTURE.md §9.1).
    PreferencesDialog dlg(sample, initial);

    auto *pattern = dlg.findChild<QLineEdit *>(QStringLiteral("formatPatternEdit"));
    QVERIFY(pattern);
    QCOMPARE(pattern->text(), initial.pattern);

    auto *detect = dlg.findChild<QPushButton *>(QStringLiteral("formatDetectButton"));
    QVERIFY(detect);
    QVERIFY(detect->isEnabled()); // there is a sample to guess from

    QVERIFY(dlg.findChild<QPushButton *>(QStringLiteral("forgetFormatsButton")));
    QVERIFY(!dlg.formatCacheCleared()); // nothing destructive happens by merely opening it

    pattern->setText(QStringLiteral("%D{%m/%d/%y %H:%M:%S} %-5p %c [%t] - %m%n"));
    QCOMPARE(dlg.defaultFormat().pattern,
             QStringLiteral("%D{%m/%d/%y %H:%M:%S} %-5p %c [%t] - %m%n"));
    QCOMPARE(dlg.defaultFormat().encoding, Encoding::Utf8);
}

void TestPreferences::emptySampleIsHarmless()
{
    // Preferences is reachable with no log open — which is exactly when someone sets this
    // up — so the editor must cope with nothing to preview: Detect has nothing to guess
    // from and says so by being disabled, and typing a pattern still works.
    FormatSettings initial;
    initial.pattern = QStringLiteral("%p %c - %m%n");

    PreferencesDialog dlg(QByteArray(), initial);

    auto *detect = dlg.findChild<QPushButton *>(QStringLiteral("formatDetectButton"));
    QVERIFY(detect);
    QVERIFY(!detect->isEnabled());

    auto *pattern = dlg.findChild<QLineEdit *>(QStringLiteral("formatPatternEdit"));
    QVERIFY(pattern);
    pattern->setText(QStringLiteral("%d{%Y} %m%n"));
    QCOMPARE(dlg.defaultFormat().pattern, QStringLiteral("%d{%Y} %m%n"));
}

void TestPreferences::logFormatDialogReportsUseAsDefault()
{
    // The promotion path: a pattern is worth keeping once it has been checked against
    // real lines, which is what the per-file dialog was doing. The dialog only REPORTS
    // the tick — MainWindow does the saving — so this pins the wire between them, which
    // is otherwise the sort of thing that silently stops working (cf. the M14 note about
    // askPassword()'s `remember` flag being filled in and never read).
    FormatSettings initial;
    initial.pattern = QStringLiteral("%p %c - %m%n");

    LogFormatDialog dlg(QStringLiteral("some.log"), QByteArray(), initial);

    auto *check = dlg.findChild<QCheckBox *>(QStringLiteral("setAsDefaultCheck"));
    QVERIFY(check);
    QVERIFY(!check->isChecked()); // off by default: one file's format is not a habit
    QVERIFY(!dlg.useAsDefault());

    check->setChecked(true);
    QVERIFY(dlg.useAsDefault());

    // And the editor's value still comes back through the shell unchanged.
    QCOMPARE(dlg.settings().pattern, initial.pattern);
}

int main(int argc, char *argv[])
{
    // Isolate persistent state: the Preferences dialog's Forget button writes to the
    // real QSettings, and these must not touch the developer's own.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-preferences"));

    TestPreferences tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_preferences.moc"
