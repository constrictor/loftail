#include <QtTest>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>

#include "CollapsibleSection.h"
#include "FakeSecretStore.h"
#include "HostBookmarkStore.h"
#include "OpenRemoteDialog.h"

using namespace loftail;

// The Open Remote dialog as a piece of user interface, rather than as a route into
// MainWindow — which is what tst_remoteopen covers, and which is why the store-facing
// half of the dialog (Save replacing by name, paths surviving or not surviving a change
// of machine) is still asserted there and not repeated here.
//
// What is here is everything the dialog says and refuses on its own: which controls are
// live in which state, what the sign-in section discloses about where a password would
// go, and that the remembered-path list is visible and prunable rather than an invisible
// side effect of pressing Save.
//
// No network, no keychain, no QApplication settings: a QTemporaryDir for the store and
// tests/FakeSecretStore.h for the backend, so every one of these is ungated.
class TestRemoteDialog : public QObject
{
    Q_OBJECT

private:
    static HostBookmark host(const QString &name, const QString &h, const QStringList &paths,
                             HostBookmark::Auth auth = HostBookmark::Auth::Agent)
    {
        HostBookmark b;
        b.label = name;
        b.user = QStringLiteral("deploy");
        b.host = h;
        b.paths = paths;
        b.auth = auth;
        return b;
    }

    static QLineEdit *field(const OpenRemoteDialog &d, const char *name)
    {
        return d.findChild<QLineEdit *>(QString::fromLatin1(name));
    }

private slots:
    void openIsRefusedUntilTheAddressIsComplete();
    void saveNeedsAHostAndRemoveNeedsASelection();
    void saveSaysUpdateWhenItWouldReplace();
    void theRememberedPathsAreListedAndPrunable();
    void theConsentNoteIsPresentInBothSignInModes();
    void theEmptyListExplainsItself();
    void advancedIsFoldedUnlessTheHostChangedSomethingInIt();
};

// The button used to be permanently enabled, and accept() returned silently when the
// location was incomplete — pressing Open on a fresh dialog looked exactly like a hang.
void TestRemoteDialog::openIsRefusedUntilTheAddressIsComplete()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());
    OpenRemoteDialog dialog(&store);

    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(buttons);
    QAbstractButton *open = buttons->button(QDialogButtonBox::Open);
    QVERIFY(open);

    QVERIFY2(!open->isEnabled(), "a fresh dialog has neither host nor path");

    field(dialog, "remoteHostField")->setText(QStringLiteral("web1"));
    QVERIFY2(!open->isEnabled(), "a host with no path is not a log");

    field(dialog, "remotePathField")->setText(QStringLiteral("/var/log/app.log"));
    QVERIFY(open->isEnabled());

    // Whitespace is not an address: currentFields() trims, so the button must too.
    field(dialog, "remoteHostField")->setText(QStringLiteral("   "));
    QVERIFY(!open->isEnabled());
}

void TestRemoteDialog::saveNeedsAHostAndRemoveNeedsASelection()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());
    store.save(host(QStringLiteral("prod"), QStringLiteral("web1"),
                    {QStringLiteral("/var/log/a.log")}));

    OpenRemoteDialog dialog(&store);
    auto *save = dialog.findChild<QPushButton *>(QStringLiteral("remoteSaveButton"));
    auto *remove = dialog.findChild<QPushButton *>(QStringLiteral("remoteRemoveButton"));
    auto *list = dialog.findChild<QListWidget *>(QStringLiteral("remoteBookmarkList"));
    QVERIFY(save && remove && list);

    QVERIFY2(!save->isEnabled(), "nothing to save with no host");
    QVERIFY2(!remove->isEnabled(), "nothing selected to remove");

    field(dialog, "remoteHostField")->setText(QStringLiteral("web9"));
    QVERIFY2(save->isEnabled(), "a host alone is a saveable bookmark — the path is optional");

    list->setCurrentRow(0);
    QVERIFY(remove->isEnabled());
}

void TestRemoteDialog::saveSaysUpdateWhenItWouldReplace()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());
    store.save(host(QStringLiteral("prod"), QStringLiteral("web1"),
                    {QStringLiteral("/var/log/a.log")}));

    OpenRemoteDialog dialog(&store);
    auto *save = dialog.findChild<QPushButton *>(QStringLiteral("remoteSaveButton"));
    QVERIFY(save);

    field(dialog, "remoteHostField")->setText(QStringLiteral("web9"));
    QVERIFY2(!save->text().contains(QStringLiteral("Update")),
             "a name nobody is using is a new entry");

    // The name is the identity, and it is compared trimmed and case-insensitively —
    // so the button has to agree with HostBookmarkStore about what collides.
    field(dialog, "remoteNameField")->setText(QStringLiteral(" PROD "));
    QVERIFY2(save->text().contains(QStringLiteral("Update")),
             "saving under an existing name replaces it, and should say so first");

    field(dialog, "remoteNameField")->setText(QStringLiteral("staging"));
    QVERIFY(!save->text().contains(QStringLiteral("Update")));
}

// Saving appends to a host's remembered paths. Until the combo box the list was
// write-only: nothing showed what had accumulated and nothing could drop an entry.
void TestRemoteDialog::theRememberedPathsAreListedAndPrunable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());
    store.save(host(QStringLiteral("prod"), QStringLiteral("web1"),
                    {QStringLiteral("/var/log/a.log"), QStringLiteral("/var/log/b.log"),
                     QStringLiteral("/var/log/c.log")}));

    OpenRemoteDialog dialog(&store);
    auto *list = dialog.findChild<QListWidget *>(QStringLiteral("remoteBookmarkList"));
    auto *paths = dialog.findChild<QComboBox *>(QStringLiteral("remotePathCombo"));
    QVERIFY(list && paths);

    list->setCurrentRow(0);
    QCOMPARE(paths->count(), 3);
    QCOMPARE(paths->currentText(), QStringLiteral("/var/log/a.log"));

    // Dropping one and saving must not have it come back from the stored entry — the
    // form's list is what the user can see, so it is the one that counts.
    paths->removeItem(paths->findText(QStringLiteral("/var/log/b.log")));
    paths->setEditText(QStringLiteral("/var/log/a.log"));
    dialog.findChild<QPushButton *>(QStringLiteral("remoteSaveButton"))->click();

    const QStringList saved = store.all().at(0).paths;
    QCOMPARE(saved.size(), 2);
    QVERIFY(!saved.contains(QStringLiteral("/var/log/b.log")));

    // Point the form at another machine and the list goes with the old one: those paths
    // describe logs on a host this form no longer names.
    field(dialog, "remoteHostField")->setText(QStringLiteral("web2"));
    QCOMPARE(paths->count(), 0);
    QCOMPARE(paths->currentText(), QStringLiteral("/var/log/a.log")); // what was typed stays
}

// The row that says where a remembered password goes. It used to be hidden entirely for
// agent auth, which resized the dialog by some eighty pixels whenever the sign-in method
// changed; it is now always present and merely says something different.
void TestRemoteDialog::theConsentNoteIsPresentInBothSignInModes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());

    {
        FakeSecretStore secrets;
        secrets.setAvailable(true);
        secrets.setBackendName(QStringLiteral("KWallet"));
        InstalledSecretStore installed(&secrets);

        OpenRemoteDialog dialog(&store);
        auto *note = dialog.findChild<QLabel *>(QStringLiteral("remoteConsentNote"));
        auto *auth = dialog.findChild<QComboBox *>(QStringLiteral("remoteAuthCombo"));
        auto *remember = dialog.findChild<QCheckBox *>(QStringLiteral("remoteRememberPassword"));
        QVERIFY(note && auth && remember);

        // Agent auth: present, saying nothing is kept, and carrying no warning glyph.
        QVERIFY(note->isVisibleTo(&dialog));
        QVERIFY(!note->text().contains(QString::fromUtf8("⚠")));
        QVERIFY(!remember->isEnabled());

        auth->setCurrentIndex(1); // Password
        QVERIFY(remember->isEnabled());
        QVERIFY2(note->text().contains(QStringLiteral("KWallet")),
                 "the destination is named before the box can be ticked");
        QVERIFY(!note->text().contains(QString::fromUtf8("⚠")));
    }

    // No keychain: the same row, the warning wording, and the file named in full.
    FakeSecretStore secrets;
    secrets.setAvailable(false);
    secrets.setBackendName(QString());
    InstalledSecretStore installed(&secrets);

    OpenRemoteDialog dialog(&store);
    auto *note = dialog.findChild<QLabel *>(QStringLiteral("remoteConsentNote"));
    dialog.findChild<QComboBox *>(QStringLiteral("remoteAuthCombo"))->setCurrentIndex(1);
    QVERIFY(note->text().contains(QString::fromUtf8("⚠")));
    QVERIFY(note->text().contains(QStringLiteral("plain text")));
    QVERIFY2(note->text().contains(store.filePath().toHtmlEscaped()),
             "a warning that does not name the file is not much of a warning");
}

void TestRemoteDialog::theEmptyListExplainsItself()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());

    {
        OpenRemoteDialog dialog(&store);
        auto *hint = dialog.findChild<QLabel *>(QStringLiteral("remoteBookmarkListEmptyHint"));
        QVERIFY(hint);
        QVERIFY2(!hint->isHidden(), "an empty list is the biggest thing here on a first run");
        QVERIFY(!hint->text().isEmpty());
    }

    store.save(host(QStringLiteral("prod"), QStringLiteral("web1"), {}));
    OpenRemoteDialog dialog(&store);
    QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("remoteBookmarkListEmptyHint"))->isHidden());
}

// Advanced holds the two settings with a good default. It opens by itself only for a
// host that has moved one of them off it — otherwise the fold would hide the fact.
void TestRemoteDialog::advancedIsFoldedUnlessTheHostChangedSomethingInIt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());
    store.save(host(QStringLiteral("plain"), QStringLiteral("web1"),
                    {QStringLiteral("/var/log/a.log")}));

    HostBookmark tuned = host(QStringLiteral("tuned"), QStringLiteral("web2"),
                              {QStringLiteral("/var/log/b.log")});
    tuned.tailStartBytes = qint64(32) * 1024 * 1024;
    store.save(tuned);

    OpenRemoteDialog dialog(&store);
    auto *advanced = dialog.findChild<CollapsibleSection *>(
        QStringLiteral("remoteAdvancedSection"));
    auto *list = dialog.findChild<QListWidget *>(QStringLiteral("remoteBookmarkList"));
    QVERIFY(advanced && list);

    QVERIFY2(!advanced->isExpanded(), "folded by default");

    list->setCurrentRow(0); // plain
    QVERIFY(!advanced->isExpanded());

    list->setCurrentRow(1); // tuned: fetches only the tail
    QVERIFY2(advanced->isExpanded(), "a setting moved off its default must not be hidden");
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestRemoteDialog test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_remotedialog.moc"
