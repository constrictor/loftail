#include "ConfigSyntax.h"

#include "UiColors.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPalette>
#include <QTextCharFormat>
#include <QTextDocument>

namespace loftail {

namespace {
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::ConfigSyntax)
};

// How many bytes of the file the sniffer looks at. Enough to get past a licence header
// in a comment block, small enough that the JSON parse below is cheap on a large file.
constexpr int kSniffBytes = 8192;
} // namespace

QString configSyntaxName(ConfigSyntax syntax)
{
    switch (syntax) {
    case ConfigSyntax::Ini:  return Tr::tr("INI / properties");
    case ConfigSyntax::Json: return Tr::tr("JSON");
    case ConfigSyntax::Xml:  return Tr::tr("XML");
    case ConfigSyntax::PlainText: break;
    }
    return Tr::tr("Plain text");
}

ConfigSyntax syntaxForExtension(QStringView suffix)
{
    const QString s = suffix.toString().toLower();
    // `.properties` first in spirit if not in code: it is what log4cplus's own
    // configuration file is called, and the reason this table has four INI spellings
    // rather than the one the feature was asked for.
    if (s == QLatin1String("ini") || s == QLatin1String("properties")
        || s == QLatin1String("conf") || s == QLatin1String("cfg")) {
        return ConfigSyntax::Ini;
    }
    if (s == QLatin1String("json"))
        return ConfigSyntax::Json;
    if (s == QLatin1String("xml") || s == QLatin1String("xsd"))
        return ConfigSyntax::Xml;
    return ConfigSyntax::PlainText;
}

ConfigSyntax sniffSyntax(QByteArrayView head)
{
    // Built from the pointer and a length rather than QByteArrayView::left(), which
    // arrived after the Qt 6.4 floor: it compiles on a newer dev machine and breaks the
    // reference toolchain, MSVC and the sanitizer job at once — the version gap
    // CLAUDE.md records, and only CI checks it.
    const QByteArray sample(head.constData(), qMin<qsizetype>(head.size(), kSniffBytes));
    if (sample.trimmed().isEmpty())
        return ConfigSyntax::PlainText;

    // 1. JSON first, because it is the only EXACT test here — the others are cues. A
    //    truncated sample will not parse, which is why this is tried on the head and
    //    then simply falls through rather than being trusted to refuse.
    QJsonParseError err{};
    QJsonDocument::fromJson(sample, &err);
    if (err.error == QJsonParseError::NoError)
        return ConfigSyntax::Json;

    // Walk to the first line that carries anything but a comment. An XML file may open
    // with a declaration, a licence comment and blank lines before its root element,
    // and an INI file routinely opens with a block of `#` comments.
    const QList<QByteArray> lines = sample.split('\n');
    for (const QByteArray &raw : lines) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith('#') || line.startsWith(';') || line.startsWith("//"))
            continue;
        if (line.startsWith("<!--"))
            continue;

        // 2. A tag. `<?xml` is decisive; a bare `<` is the ordinary opening of a
        //    document with no declaration.
        if (line.startsWith('<'))
            return ConfigSyntax::Xml;

        // 3. A section header, or a key with a separator. Checked AFTER the tag test
        //    for the reason the header gives: the cues overlap, and `[` opens a JSON
        //    array as readily as an INI section — which is why the JSON parse ran first
        //    and why reaching here means it was not JSON.
        if (line.startsWith('[') && line.endsWith(']'))
            return ConfigSyntax::Ini;
        const int eq = int(line.indexOf('='));
        const int colon = int(line.indexOf(':'));
        const int sep = (eq >= 0 && (colon < 0 || eq < colon)) ? eq : colon;
        if (sep > 0) {
            // A key is a word, not a sentence: something with spaces before the
            // separator is prose that happens to contain one.
            const QByteArray key = line.left(sep).trimmed();
            if (!key.isEmpty() && !key.contains(' '))
                return ConfigSyntax::Ini;
        }
        // A first real line that says none of the above decides the file: reading on
        // would let a stray `<` deep in a plain-text file rename the whole thing.
        break;
    }
    return ConfigSyntax::PlainText;
}

ConfigHighlighter::ConfigHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document)
{
    rebuildRules();
}

void ConfigHighlighter::setSyntax(ConfigSyntax syntax)
{
    if (m_syntax == syntax)
        return;
    m_syntax = syntax;
    rebuildRules();
    rehighlight();
}

void ConfigHighlighter::setPalette(const QPalette &palette)
{
    // A QSyntaxHighlighter owns no widget, so it cannot ask for the palette in force;
    // the page it belongs to hands it one here and again whenever the theme changes
    // under a running window.
    m_palette = palette;
    rehighlight();
}

void ConfigHighlighter::rebuildRules()
{
    m_rules.clear();
    m_multilineComments = false;

    const auto add = [this](const char *pattern, ConfigRole role, int capture = 0) {
        Rule r;
        r.re = QRegularExpression(QString::fromLatin1(pattern));
        r.role = role;
        r.capture = capture;
        m_rules.append(r);
    };

    switch (m_syntax) {
    case ConfigSyntax::Ini:
        // Order matters within a grammar too: later rules paint over earlier ones, so
        // the comment rule goes LAST and wins over anything that looked like a key.
        add(R"(^\s*\[([^\]]*)\])", ConfigRole::Tag, 1);          // [section]
        add(R"(^\s*([^=:#;\s][^=:]*?)\s*[=:])", ConfigRole::Key, 1);
        // ANCHORED AT THE START OF THE LINE, so the capture runs from the FIRST separator
        // to the end. Written as a bare `[=:]\s*(.*)$` it matches globally, so every
        // later colon re-starts a "value" — and a log4cplus conversion pattern is full of
        // them (`%H:%M:%S`), which left the tail of the commonest line in the file
        // uncoloured from the first timestamp specifier onward.
        add(R"(^[^=:#;\n]*[=:]\s*(.*)$)", ConfigRole::Value, 1);
        add(R"("[^"]*")", ConfigRole::String);
        add(R"(^\s*[#;].*$)", ConfigRole::Comment);
        break;
    case ConfigSyntax::Json:
        add(R"(-?\b\d+(\.\d+)?([eE][+-]?\d+)?\b)", ConfigRole::Number);
        add(R"(\btrue\b|\bfalse\b|\bnull\b)", ConfigRole::Number);
        add(R"("(?:[^"\\]|\\.)*")", ConfigRole::String);
        // A string immediately followed by a colon is a member NAME, not a value, and
        // painting it after the string rule above is what lets it win.
        add(R"("(?:[^"\\]|\\.)*"(?=\s*:))", ConfigRole::Key);
        break;
    case ConfigSyntax::Xml:
        add(R"(<[?/]?\s*([A-Za-z_][\w.:-]*))", ConfigRole::Tag, 1);
        add(R"(([A-Za-z_][\w.:-]*)\s*=)", ConfigRole::Attribute, 1);
        add(R"("(?:[^"\\]|\\.)*"|'[^']*')", ConfigRole::String);
        add(R"(&\w+;)", ConfigRole::Number);                     // entities
        m_multilineComments = true;
        m_commentStart = QRegularExpression(QStringLiteral("<!--"));
        m_commentEnd = QRegularExpression(QStringLiteral("-->"));
        break;
    case ConfigSyntax::PlainText:
        break;
    }
}

void ConfigHighlighter::highlightBlock(const QString &text)
{
    if (m_syntax == ConfigSyntax::PlainText)
        return;

    const auto formatFor = [this](ConfigRole role) {
        QTextCharFormat f;
        switch (role) {
        case ConfigRole::Key:
        case ConfigRole::Tag:
            // WEIGHT, not hue. A bold role needs no contrast argument at all — it is
            // the theme's own text colour — which is what keeps the set of colours
            // small enough to hold above 4.5:1 in two themes.
            f.setFontWeight(QFont::Bold);
            f.setForeground(syntaxColor(m_palette, role));
            break;
        case ConfigRole::Attribute:
            f.setFontItalic(true);
            f.setForeground(syntaxColor(m_palette, role));
            break;
        default:
            f.setForeground(syntaxColor(m_palette, role));
            break;
        }
        return f;
    };

    for (const Rule &rule : std::as_const(m_rules)) {
        auto it = rule.re.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const int start = int(m.capturedStart(rule.capture));
            const int length = int(m.capturedLength(rule.capture));
            if (start < 0 || length <= 0)
                continue;
            setFormat(start, length, formatFor(rule.role));
        }
    }

    if (!m_multilineComments)
        return;

    // The one construct that spans blocks. Everything above is decided within a line,
    // so this is the only place the block state carries anything.
    setCurrentBlockState(0);
    int start = 0;
    if (previousBlockState() != 1) {
        const auto m = m_commentStart.match(text);
        start = m.hasMatch() ? int(m.capturedStart()) : -1;
    }
    while (start >= 0) {
        const auto end = m_commentEnd.match(text, start);
        int length = 0;
        if (end.hasMatch()) {
            length = int(end.capturedEnd()) - start;
        } else {
            setCurrentBlockState(1);
            length = int(text.length()) - start;
        }
        setFormat(start, length, formatFor(ConfigRole::Comment));
        if (!end.hasMatch())
            break;
        const auto next = m_commentStart.match(text, start + length);
        start = next.hasMatch() ? int(next.capturedStart()) : -1;
    }
}

} // namespace loftail
