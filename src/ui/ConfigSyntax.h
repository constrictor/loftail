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

#pragma once

#include <QByteArrayView>
#include <QRegularExpression>
#include <QString>
#include <QSyntaxHighlighter>
#include <QVector>

#include <QPalette>

QT_BEGIN_NAMESPACE
class QTextDocument;
QT_END_NAMESPACE

namespace loftail {

// Which grammar a config file is coloured in (SPEC.md §4). The three that ship are the
// ones a log4cplus application is actually configured with: a `.properties`/`.ini`
// key=value file, JSON, and XML.
enum class ConfigSyntax {
    PlainText,
    Ini,
    Json,
    Xml,
};

// The syntax's name as SHOWN, so the reader can see which one was picked and change it.
QString configSyntaxName(ConfigSyntax syntax);

// From the file's extension. `suffix` is the extension alone ("properties"), never a
// path: taking a suffix off an address is the caller's job, and it has to be done with
// logSourceBareName() rather than QFileInfo, which is wrong for a remote address.
//
// `.properties` matters most of the four INI spellings — it is what log4cplus's own
// configuration file is called.
ConfigSyntax syntaxForExtension(QStringView suffix);

// From the file's first bytes, for a file whose extension says nothing.
//
// A FALLBACK ONLY. A known extension is never second-guessed: a `.json` holding
// something unparseable is a JSON file with a mistake in it, and colouring it as
// something else would hide the mistake.
//
// THE ORDER IS THE RULE, because the cues overlap: `[` opens a JSON array AND an INI
// section, and an XML document can begin with whitespace and a comment. So: parse as
// JSON first (the only test that is exact), then look for a tag, then for a section
// header or a key=value line, then give up.
ConfigSyntax sniffSyntax(QByteArrayView head);

// The colours a grammar is drawn in, resolved against a palette.
//
// Every one of these must clear 4.5:1 against QPalette::Base in BOTH themes, which is
// what tst_uicolors::everySyntaxColourReadsOnBothThemes measures. That is why the set is
// deliberately small: three coloured roles and two that spend WEIGHT instead of colour.
// Every additional coloured role is another pair to keep above the line in two themes,
// and the log table's own palette work is the record of how easily that is lost.
enum class ConfigRole : quint8 {
    Comment,
    Key,      // an INI key, a JSON member name
    Value,    // an unquoted INI value
    String,   // a quoted string, an XML attribute value
    Number,
    Tag,      // an XML element name
    Attribute,
};

// One highlighter, a rule table per grammar — not three subclasses. The grammars differ
// only in their patterns and which role each maps to, so three classes would triplicate
// the block loop AND the colour resolution, which is the half with the contrast rule
// under it. A fourth grammar is then a table, not a class.
class ConfigHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    explicit ConfigHighlighter(QTextDocument *document);

    void setSyntax(ConfigSyntax syntax);
    ConfigSyntax syntax() const { return m_syntax; }

    // Re-resolve every colour against `palette` and redraw. Called when the theme
    // changes under a running window — the page notices the change itself, so it does
    // not matter who made it (the rule LogView follows for a font change).
    void setPalette(const QPalette &palette);

protected:
    void highlightBlock(const QString &text) override;

private:
    struct Rule
    {
        QRegularExpression re;
        ConfigRole role = ConfigRole::Value;
        int capture = 0; // which group to paint; 0 is the whole match
    };

    void rebuildRules();

    ConfigSyntax   m_syntax = ConfigSyntax::PlainText;
    // A highlighter owns no widget, so the palette is handed to it rather than asked
    // for. Defaults to the application's, which is right until a page says otherwise.
    QPalette       m_palette;
    QVector<Rule>  m_rules;
    // XML comments are the one construct that spans blocks, so the block state carries
    // whether we are inside one. Everything else is decided within a line.
    QRegularExpression m_commentStart;
    QRegularExpression m_commentEnd;
    bool               m_multilineComments = false;
};

} // namespace loftail
