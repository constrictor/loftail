// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QApplication>
#include <QFrame>
#include <QLineEdit>
#include <QPalette>
#include <QWidget>

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

    // The theme from the zebra report: a near-black table base. It is the palette the
    // old lighter(108) band did nothing on, and the pure-white Base above is the other
    // end of the same failure, so between them they bracket every real theme.
    static QPalette nearBlackDark()
    {
        QPalette p;
        p.setColor(QPalette::Window, QColor(0x1a, 0x1c, 0x1e));
        p.setColor(QPalette::WindowText, QColor(0xd0, 0xd3, 0xd6));
        p.setColor(QPalette::Base, QColor(0x14, 0x16, 0x18));
        p.setColor(QPalette::Text, QColor(0xd0, 0xd3, 0xd6));
        return p;
    }

private slots:
    void darkThemeIsRecognised();
    void anUnsetPlaceholderRoleIsRepaired();
    void aThemeThatSetsTheRoleIsLeftAlone();
    void chromeColoursCarryOnBothThemes();
    void theRemoteDialogsPlaceholdersAreReadable();
    void contextRowsRecedeWithoutBecomingUnreadable();
    void aSectionDividerIsVisibleWithoutBeingText();
    void theAlternatingRecordBandIsVisibleOnEveryTheme();
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

    // And each is legible against the surface it is drawn on. 4.5 is the WCAG bound for
    // BODY text, which is what these are: ordinary-sized sentences on a dialog, not
    // headings. The bound was 3.0 — the large-text allowance — and under it the light
    // warning amber sat at 3.2 and passed, while being the colour of the sentence that
    // says a password is about to be written to disk in the clear.
    for (const auto &entry : {std::pair{dark, QStringLiteral("dark")},
                              std::pair{light, QStringLiteral("light")}}) {
        const QPalette &p = entry.first;
        const QColor window = p.color(QPalette::Window);
        QVERIFY2(contrast(errorColor(p), window) >= 4.5, qPrintable(entry.second));
        QVERIFY2(contrast(warningColor(p), window) >= 4.5, qPrintable(entry.second));
        // Muted is deliberately quieter, but must not vanish.
        QVERIFY2(contrast(mutedColor(p), window) >= 2.0, qPrintable(entry.second));
    }

    // The exported helpers agree with the ones restated above. UiColors measures its own
    // colours now (it did not, which is how the amber went unnoticed), and the two
    // implementations existing side by side is only worth anything if they match.
    QCOMPARE(qRound(contrastRatio(warningColor(light), light.color(QPalette::Window)) * 100),
             qRound(contrast(warningColor(light), light.color(QPalette::Window)) * 100));
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
    // The four from the report: name, user, host, path.
    QCOMPARE(checked, 4);
}

void TestUiColors::contextRowsRecedeWithoutBecomingUnreadable()
{
    // A filter-context row (M15, SPEC.md §6) must read as recessed against a match
    // and still be legible: it is real log text the user asked to see, not a hint.
    // Both directions matter — on a dark theme "dimmer" moves the text DOWN in
    // luminance, on a light theme UP, and a fixed lightening factor would wash one of
    // them out. The functions take the row's own colours, so both fall out for free.
    for (const QPalette &palette : {plainLight(), brokenDark()}) {
        const QColor base = palette.color(QPalette::Base);
        const QColor text = palette.color(QPalette::Text);

        const QColor dim = contextTextColor(text, base);
        QVERIFY(dim.isValid());
        QVERIFY2(contrast(dim, base) < contrast(text, base),
                 "a context row must be visibly quieter than a match");
        QVERIFY2(contrast(dim, base) >= 3.0, "…but still comfortably readable");

        // A highlight rule's background is softened, not dropped: the rule still says
        // something about a context row, it just must not shout it as loudly as it
        // does on the match beside it.
        const QColor rule(0xc0, 0x39, 0x2b);
        const QColor soft = contextFillColor(rule, base);
        QVERIFY(soft.isValid());
        QCOMPARE_NE(soft, rule);
        QCOMPARE_NE(soft, base);
        QVERIFY2(contrast(soft, base) < contrast(rule, base),
                 "the softened fill must sit between the rule colour and the base");
    }
}

void TestUiColors::aSectionDividerIsVisibleWithoutBeingText()
{
    // The hairline the Highlighters pane's rule editor separates its sections with, in
    // place of a frame per section. It has to clear two bounds from opposite directions,
    // and the reason it is measured at all is that the idiomatic answer — a
    // QFrame::Sunken HLine, drawn by Fusion in Light/Mid — clears NEITHER on the stock
    // light palette: it is not visible against the pane it divides.
    for (const QPalette &palette : {plainLight(), brokenDark()}) {
        const QColor window = palette.color(QPalette::Window);
        const QColor text = palette.color(QPalette::WindowText);
        const QColor line = dividerColor(palette);

        QVERIFY(line.isValid());
        QVERIFY2(contrast(line, window) > 1.15, "a divider nobody can see divides nothing");
        QVERIFY2(contrast(line, window) < contrast(text, window),
                 "a divider heavier than the words beside it reads as a rule, not a seam");
    }

    // And it is a real difference from the roles it could have been read out of, on both
    // themes — the point of deriving it.
    QCOMPARE_NE(dividerColor(plainLight()), dividerColor(brokenDark()));

    // A SWITCHED-OFF section is drawn from QPalette::Disabled, and its divider has to dim
    // with it: a hairline at full strength over a body Qt has greyed is the one part of
    // the section still claiming to be in force. So the colour group is a parameter, and
    // dropping it — the signature this had first — is a silent regression on every
    // unticked axis.
    QPalette twoGroups = plainLight();
    twoGroups.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x9a, 0x9a, 0x9a));
    twoGroups.setColor(QPalette::Disabled, QPalette::Window, QColor(0xef, 0xef, 0xef));
    QCOMPARE_NE(dividerColor(twoGroups, QPalette::Disabled),
                dividerColor(twoGroups, QPalette::Active));
    QVERIFY2(contrast(dividerColor(twoGroups, QPalette::Disabled),
                      twoGroups.color(QPalette::Disabled, QPalette::Window))
                 < contrast(dividerColor(twoGroups, QPalette::Active),
                            twoGroups.color(QPalette::Active, QPalette::Window)),
             "a switched-off section's divider must be the quieter of the two");
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


void TestUiColors::theAlternatingRecordBandIsVisibleOnEveryTheme()
{
    // The log table shades every other RECORD (SPEC.md §5). The band was
    // base.lighter(108), which scales the HSV VALUE: on a near-black base that moves a
    // couple of levels and on a white one the value is already maxed and it moves
    // nothing at all — so the stripe was dead at both ends of the range and this test
    // measures both ends before it measures the fix.
    for (const QPalette &palette : {plainLight(), brokenDark(), nearBlackDark()}) {
        const QColor base = palette.color(QPalette::Base);
        const QColor text = palette.color(QPalette::Text);
        const QColor band = alternateRowColor(palette);

        QVERIFY2(contrast(base.lighter(108), base) < 1.05,
                 "the failing case is not actually invisible — the test proves nothing");

        QVERIFY(band.isValid());
        QCOMPARE_NE(band, base);
        // Visible: a band nobody can see separates nothing. Well under the WCAG text
        // bounds on purpose — this is a surface, not a mark, and it is read by noticing
        // where it changes rather than by looking at it.
        QVERIFY2(contrast(band, base) > 1.08, "the band is invisible against the base");
        // …and still a band rather than a second surface: a record is a paragraph three
        // or four lines tall in wrap-always-on, and a heavy stripe over that much area
        // competes with the highlight colours, which are the ones that mean something.
        QVERIFY2(contrast(band, base) < 1.35, "the band shouts louder than a highlight");
        // It moves TOWARD the text, so the direction is right on both themes with no
        // per-theme constant: darker than a white base, lighter than a near-black one.
        QVERIFY(relativeLuminance(band) > relativeLuminance(base)
                    ? relativeLuminance(text) > relativeLuminance(base)
                    : relativeLuminance(text) < relativeLuminance(base));
    }

    // Derived per theme, not a constant: the same call answers differently on a light
    // and a dark palette, which is the whole reason it is a function of one.
    QCOMPARE_NE(alternateRowColor(plainLight()), alternateRowColor(nearBlackDark()));
}

#include "tst_uicolors.moc"
