#include <QtTest>

#include <QApplication>
#include <QLineEdit>
#include <QPalette>

#include "HostBookmarkStore.h"
#include "OpenRemoteDialog.h"
#include "UiColors.h"

#include <cmath>

using namespace loftail;

// Chrome colours on a dark theme (SPEC.md §8, ARCHITECTURE.md §8).
//
// The bug this was written for: QPalette::PlaceholderText arrived in Qt 5.12, and a
// platform theme that does not fill it in leaves the role at Qt's built-in BLACK at
// 50% alpha — regardless of how dark the field behind it is. Every placeholder in the
// application then renders black on a dark field: present, occupying space, unreadable.
// It is not hypothetical; it is what a real dark desktop showed.
//
// The same class of defect covered the hardcoded #c0392b / #b9770e / #b04a00 / "gray"
// that had been chosen against a light theme.
class TestUiColors : public QObject
{
    Q_OBJECT

private:
    // WCAG relative luminance and contrast ratio — the same definition UiColors uses,
    // restated here so the test measures the property rather than the implementation.
    static qreal luminance(const QColor &c)
    {
        const auto ch = [](qreal v) {
            return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * ch(c.redF()) + 0.7152 * ch(c.greenF()) + 0.0722 * ch(c.blueF());
    }
    static qreal contrast(const QColor &a, const QColor &b)
    {
        const qreal la = luminance(a);
        const qreal lb = luminance(b);
        return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
    }

    // A dark theme that NEVER SETS PlaceholderText — the failing case exactly.
    static QPalette brokenDark()
    {
        QPalette p;
        p.setColor(QPalette::Window, QColor(0x2b, 0x2f, 0x36));
        p.setColor(QPalette::WindowText, QColor(0xdd, 0xe1, 0xe6));
        p.setColor(QPalette::Base, QColor(0x23, 0x26, 0x2b));
        p.setColor(QPalette::Text, QColor(0xdd, 0xe1, 0xe6));
        p.setColor(QPalette::Button, QColor(0x3a, 0x3f, 0x48));
        p.setColor(QPalette::ButtonText, QColor(0xdd, 0xe1, 0xe6));
        // Qt's own default for the role it was never told about.
        p.setColor(QPalette::PlaceholderText, QColor(0, 0, 0, 128));
        return p;
    }

    static QPalette plainLight()
    {
        QPalette p;
        p.setColor(QPalette::Window, QColor(0xef, 0xef, 0xef));
        p.setColor(QPalette::WindowText, Qt::black);
        p.setColor(QPalette::Base, Qt::white);
        p.setColor(QPalette::Text, Qt::black);
        p.setColor(QPalette::PlaceholderText, QColor(0, 0, 0, 128));
        return p;
    }

private slots:
    void darkThemeIsRecognised();
    void anUnsetPlaceholderRoleIsRepaired();
    void aThemeThatSetsTheRoleIsLeftAlone();
    void chromeColoursCarryOnBothThemes();
    void theRemoteDialogsPlaceholdersAreReadable();
};

void TestUiColors::darkThemeIsRecognised()
{
    QVERIFY(isDarkPalette(brokenDark()));
    QVERIFY(!isDarkPalette(plainLight()));
}

void TestUiColors::anUnsetPlaceholderRoleIsRepaired()
{
    QLineEdit edit;
    edit.setPalette(brokenDark());
    edit.setPlaceholderText(QStringLiteral("ssh://user@host:22/var/log/app.log"));

    const QColor base = edit.palette().color(QPalette::Active, QPalette::Base);
    const QColor before = edit.palette().color(QPalette::Active, QPalette::PlaceholderText);
    // Black at 50% over a near-black field: what the photograph showed, and about as
    // close to invisible as text gets.
    QVERIFY2(contrast(QColor(0x11, 0x13, 0x15), base) < 1.5,
             "the failing case is not actually unreadable — the test proves nothing");

    ensureReadablePlaceholder(&edit);

    const QColor after = edit.palette().color(QPalette::Active, QPalette::PlaceholderText);
    QVERIFY(after != before);
    QVERIFY2(contrast(after, base) >= 2.0, "the repaired placeholder is still unreadable");
    // A hint, not content: it must stay dimmer than the field's own text.
    QVERIFY(contrast(after, base)
            < contrast(edit.palette().color(QPalette::Active, QPalette::Text), base));
    // Inactive too — a field in an unfocused window still shows its placeholder.
    QCOMPARE(edit.palette().color(QPalette::Inactive, QPalette::PlaceholderText), after);
}

void TestUiColors::aThemeThatSetsTheRoleIsLeftAlone()
{
    // A theme that did its job is not second-guessed: the repair is conditional on the
    // measured contrast, so a deliberate choice survives untouched.
    QPalette good = brokenDark();
    const QColor chosen(0x9a, 0xa2, 0xac);
    good.setColor(QPalette::Active, QPalette::PlaceholderText, chosen);
    good.setColor(QPalette::Inactive, QPalette::PlaceholderText, chosen);

    QLineEdit edit;
    edit.setPalette(good);
    ensureReadablePlaceholder(&edit);
    QCOMPARE(edit.palette().color(QPalette::Active, QPalette::PlaceholderText), chosen);
}

void TestUiColors::chromeColoursCarryOnBothThemes()
{
    const QPalette dark = brokenDark();
    const QPalette light = plainLight();

    // Each hue differs by theme — the whole reason these are not constants any more.
    QVERIFY(errorColor(dark) != errorColor(light));
    QVERIFY(warningColor(dark) != warningColor(light));

    // And each is legible against the surface it is drawn on. 3.0 is the WCAG bound
    // for large text; these are short, emphatic strings on a dialog.
    for (const auto &entry : {std::pair{dark, QStringLiteral("dark")},
                              std::pair{light, QStringLiteral("light")}}) {
        const QPalette &p = entry.first;
        const QColor window = p.color(QPalette::Window);
        QVERIFY2(contrast(errorColor(p), window) >= 3.0, qPrintable(entry.second));
        QVERIFY2(contrast(warningColor(p), window) >= 3.0, qPrintable(entry.second));
        // Muted is deliberately quieter, but must not vanish.
        QVERIFY2(contrast(mutedColor(p), window) >= 2.0, qPrintable(entry.second));
    }
}

void TestUiColors::theRemoteDialogsPlaceholdersAreReadable()
{
    // End to end, through the real dialog from the report: every field carrying a
    // placeholder must be readable under a theme that never set the role.
    QApplication::setPalette(brokenDark());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());
    OpenRemoteDialog dialog(&store);

    int checked = 0;
    for (QLineEdit *edit : dialog.findChildren<QLineEdit *>()) {
        if (edit->placeholderText().isEmpty())
            continue;
        const QColor base = edit->palette().color(QPalette::Active, QPalette::Base);
        const QColor hint = edit->palette().color(QPalette::Active, QPalette::PlaceholderText);
        QVERIFY2(contrast(hint, base) >= 2.0,
                 qPrintable(QStringLiteral("unreadable placeholder: %1")
                                .arg(edit->placeholderText())));
        ++checked;
    }
    // The four from the report: address, name, user, path.
    QCOMPARE(checked, 4);
}

int main(int argc, char *argv[])
{
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test-uicolors"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-uicolors"));

    TestUiColors tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_uicolors.moc"
