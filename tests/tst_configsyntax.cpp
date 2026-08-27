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

#include <QPalette>

#include "ConfigSyntax.h"
#include "UiColors.h"

using namespace loftail;

// Which grammar a config file is coloured in (SPEC.md §4), and whether the colours it is
// drawn in can actually be read.
//
// The extension decides where it says anything; the contents decide only where it does
// not. That split is the whole rule, and it is what keeps a guessed answer from
// overriding a stated one — a `.json` holding a syntax error is a JSON file with a
// mistake in it, and colouring it as something else would hide the mistake rather than
// show it.
class TestConfigSyntax : public QObject
{
    Q_OBJECT

private slots:
    void theExtensionDecidesWhereItSaysAnything();
    void log4cplusOwnExtensionIsRecognised();
    void theContentsDecideOnlyWhereTheExtensionDoesNot();
    void jsonIsTriedBeforeTheBracketCue();
    void aCommentBlockIsSkippedToReachTheFirstRealLine();
    void nothingRecognisableStaysPlainText();
    void everySyntaxColourReadsOnBothThemes();

private:
    // The two themes, built the way tst_uicolors builds them: what separates them is
    // Base and Text, which is exactly what isDarkPalette() reads and what a syntax
    // colour has to hold its contrast against.
    static QPalette lightPalette()
    {
        QPalette p;
        p.setColor(QPalette::Base, QColor(0xff, 0xff, 0xff));
        p.setColor(QPalette::Text, QColor(0x1a, 0x1a, 0x1a));
        p.setColor(QPalette::Window, QColor(0xef, 0xef, 0xef));
        p.setColor(QPalette::WindowText, QColor(0x1a, 0x1a, 0x1a));
        return p;
    }
    static QPalette darkPalette()
    {
        QPalette p;
        p.setColor(QPalette::Base, QColor(0x1e, 0x1e, 0x1e));
        p.setColor(QPalette::Text, QColor(0xe8, 0xe8, 0xe8));
        p.setColor(QPalette::Window, QColor(0x2b, 0x2b, 0x2b));
        p.setColor(QPalette::WindowText, QColor(0xe8, 0xe8, 0xe8));
        return p;
    }
};

void TestConfigSyntax::theExtensionDecidesWhereItSaysAnything()
{
    QCOMPARE(syntaxForExtension(u"ini"), ConfigSyntax::Ini);
    QCOMPARE(syntaxForExtension(u"conf"), ConfigSyntax::Ini);
    QCOMPARE(syntaxForExtension(u"cfg"), ConfigSyntax::Ini);
    QCOMPARE(syntaxForExtension(u"json"), ConfigSyntax::Json);
    QCOMPARE(syntaxForExtension(u"xml"), ConfigSyntax::Xml);
    QCOMPARE(syntaxForExtension(u"xsd"), ConfigSyntax::Xml);

    // Case-insensitively, because an extension is not a promise about case.
    QCOMPARE(syntaxForExtension(u"JSON"), ConfigSyntax::Json);
    QCOMPARE(syntaxForExtension(u"Xml"), ConfigSyntax::Xml);

    // And nothing else claims a grammar.
    QCOMPARE(syntaxForExtension(u"log"), ConfigSyntax::PlainText);
    QCOMPARE(syntaxForExtension(u""), ConfigSyntax::PlainText);
}

void TestConfigSyntax::log4cplusOwnExtensionIsRecognised()
{
    // The case the whole feature is for: a log4cplus application's configuration file is
    // `log4cplus.properties`. Getting the other three INI spellings and missing this one
    // would leave the commonest config in the population uncoloured.
    QCOMPARE(syntaxForExtension(u"properties"), ConfigSyntax::Ini);
}

void TestConfigSyntax::theContentsDecideOnlyWhereTheExtensionDoesNot()
{
    QCOMPARE(sniffSyntax("<?xml version=\"1.0\"?>\n<log4j:configuration/>"),
             ConfigSyntax::Xml);
    QCOMPARE(sniffSyntax("<configuration>\n  <appender/>\n</configuration>"),
             ConfigSyntax::Xml);
    QCOMPARE(sniffSyntax("{\n  \"level\": \"DEBUG\"\n}"), ConfigSyntax::Json);
    QCOMPARE(sniffSyntax("log4cplus.rootLogger=DEBUG, STDOUT"), ConfigSyntax::Ini);
    QCOMPARE(sniffSyntax("[section]\nkey=value"), ConfigSyntax::Ini);

    // A known extension is NEVER second-guessed by the sniffer. The two functions are
    // separate for exactly this reason, and the caller's rule is to ask the extension
    // first — asserted here as the property that makes that rule safe.
    QCOMPARE(syntaxForExtension(u"json"), ConfigSyntax::Json);
    QCOMPARE(sniffSyntax("this is not json at all"), ConfigSyntax::PlainText);
}

void TestConfigSyntax::jsonIsTriedBeforeTheBracketCue()
{
    // THE ORDER IS THE RULE. `[` opens a JSON array and an INI section alike, so a
    // JSON document that happens to be an array must not be read as an INI file with a
    // very odd section header. Parsing as JSON first is what settles it, and this is the
    // case that fails if the two tests are ever swapped.
    QCOMPARE(sniffSyntax("[1, 2, 3]"), ConfigSyntax::Json);
    QCOMPARE(sniffSyntax("[\n  {\"name\": \"a\"}\n]"), ConfigSyntax::Json);

    // And the other side of it: a real INI section header is not JSON, so it falls
    // through to the bracket cue and is answered correctly.
    QCOMPARE(sniffSyntax("[log4cplus]\nrootLogger=DEBUG"), ConfigSyntax::Ini);
}

void TestConfigSyntax::aCommentBlockIsSkippedToReachTheFirstRealLine()
{
    // A config file routinely opens with a licence or an explanation. Deciding on the
    // literal first line would call every one of these plain text.
    QCOMPARE(sniffSyntax("# Copyright someone\n# Do not edit\n\nrootLogger=DEBUG"),
             ConfigSyntax::Ini);
    QCOMPARE(sniffSyntax("; classic INI comment\n[section]\nk=v"), ConfigSyntax::Ini);
    QCOMPARE(sniffSyntax("<!-- a licence -->\n<configuration/>"), ConfigSyntax::Xml);
}

void TestConfigSyntax::nothingRecognisableStaysPlainText()
{
    QCOMPARE(sniffSyntax(""), ConfigSyntax::PlainText);
    QCOMPARE(sniffSyntax("   \n\n  "), ConfigSyntax::PlainText);
    QCOMPARE(sniffSyntax("just some prose, nothing structured"), ConfigSyntax::PlainText);

    // Prose containing a separator is still prose: a key is a word, not a sentence.
    // Without that test, "Note: this file is generated" reads as an INI key.
    QCOMPARE(sniffSyntax("Note that this file is generated: do not edit"),
             ConfigSyntax::PlainText);
}

void TestConfigSyntax::everySyntaxColourReadsOnBothThemes()
{
    // The rule the whole colour table is bounded by, and the ONLY place it is
    // observable: a colour is either above 4.5:1 against the field it is drawn on or it
    // is not, and nothing in the rendered page says which. Measured in both themes
    // because a hue that clears the line on white routinely fails on near-black, which
    // is the failure the highlight palette's three tone bands exist to prevent.
    const QList<ConfigRole> roles = {
        ConfigRole::Comment, ConfigRole::Key,    ConfigRole::Value, ConfigRole::String,
        ConfigRole::Number,  ConfigRole::Tag,    ConfigRole::Attribute,
    };
    const QList<QPair<QString, QPalette>> themes = {
        {QStringLiteral("light"), lightPalette()},
        {QStringLiteral("dark"), darkPalette()},
    };

    for (const auto &theme : themes) {
        const QColor base = theme.second.color(QPalette::Base);
        for (ConfigRole role : roles) {
            const QColor c = syntaxColor(theme.second, role);
            const qreal ratio = contrastRatio(c, base);
            QVERIFY2(ratio >= 4.5,
                     qPrintable(QStringLiteral("%1 role %2: %3 on %4 is %5:1")
                                    .arg(theme.first)
                                    .arg(int(role))
                                    .arg(c.name(), base.name())
                                    .arg(ratio, 0, 'f', 2)));
        }
    }
}

QTEST_MAIN(TestConfigSyntax)
#include "tst_configsyntax.moc"
