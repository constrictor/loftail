#include "LogSettings.h"

#include "RemoteLocation.h"

#include <QRegularExpression>

namespace loftail {

QString wildcardToRegex(const QString &wildcard)
{
    QString out;
    out.reserve(wildcard.size() * 2);
    for (const QChar c : wildcard) {
        if (c == u'*')
            out += QLatin1String(".*");
        else if (c == u'?')
            out += u'.';
        else
            out += QRegularExpression::escape(QString(c));
    }
    return QRegularExpression::anchoredPattern(out);
}

bool LogPatternNode::matches(const QString &address) const
{
    if (match.isEmpty())
        return false;

    const QString target = logMatchTarget(address, matchFullPath);
    const auto options = caseSensitive ? QRegularExpression::NoPatternOption
                                       : QRegularExpression::CaseInsensitiveOption;

    // A wildcard is anchored (it describes the whole name) and a regular expression is
    // not (it is a search, so `app-\d+` finds itself anywhere in the name). Both are
    // choices a user can see the consequences of in the dialog's live match indicator.
    const QRegularExpression re(kind == Kind::Wildcard ? wildcardToRegex(match) : match,
                                options);
    // A pattern that does not compile claims nothing. The dialog shows the error; until
    // it is fixed, files fall through to whatever matches next.
    if (!re.isValid())
        return false;
    return re.match(target).hasMatch();
}

int LogSettingsTree::indexOfPatternId(const QString &id) const
{
    for (int i = 0; i < m_patterns.size(); ++i) {
        if (m_patterns.at(i).id == id)
            return i;
    }
    return -1;
}

int LogSettingsTree::indexOfFile(const QString &address) const
{
    const QString key = logSettingsKey(address);
    for (int i = 0; i < m_files.size(); ++i) {
        if (m_files.at(i).path == key)
            return i;
    }
    return -1;
}

LogSettingsTree::Resolution LogSettingsTree::resolve(const QString &address) const
{
    Resolution r;
    r.profile = m_defaults;

    // First match wins, so the order of the list is the order of precedence and the
    // user's reordering is the only tie-break there is.
    for (int i = 0; i < m_patterns.size(); ++i) {
        if (m_patterns.at(i).matches(address)) {
            r.patternIndex = i;
            r.profile = m_patterns.at(i).profile;
            break;
        }
    }

    if (const int f = indexOfFile(address); f >= 0) {
        r.fileIndex = f;
        r.profile = m_files.at(f).profile;
    }
    return r;
}

LogProfile LogSettingsTree::inherited(const QString &address) const
{
    for (const LogPatternNode &p : m_patterns) {
        if (p.matches(address))
            return p.profile;
    }
    return m_defaults;
}

bool LogSettingsTree::setFileProfile(const QString &address, const LogProfile &p)
{
    // Equal to what it already inherits: there is nothing for a node to say, so there
    // is no node. This is the rule that keeps the tree free of entries the user never
    // asked for — every open would otherwise leave one behind.
    if (p == inherited(address))
        return removeFile(address);

    if (const int i = indexOfFile(address); i >= 0 && m_files.at(i).profile == p)
        return false;

    insertFileProfile(address, p);
    return true;
}

void LogSettingsTree::insertFileProfile(const QString &address, const LogProfile &p)
{
    const QString key = logSettingsKey(address);
    if (const int i = indexOfFile(key); i >= 0) {
        m_files[i].profile = p;
        return;
    }
    m_files.push_back(LogFileNode{key, p});
}

bool LogSettingsTree::removeFile(const QString &address)
{
    const int i = indexOfFile(address);
    if (i < 0)
        return false;
    m_files.remove(i);
    return true;
}

bool LogSettingsTree::pruneRedundantFiles(const QString &except)
{
    const QString spared = except.isEmpty() ? QString() : logSettingsKey(except);
    bool removed = false;
    // Backwards, so removing one node does not step over the next.
    for (int i = m_files.size() - 1; i >= 0; --i) {
        if (!spared.isEmpty() && m_files.at(i).path == spared)
            continue;
        if (m_files.at(i).profile == inherited(m_files.at(i).path)) {
            m_files.remove(i);
            removed = true;
        }
    }
    return removed;
}

void LogSettingsTree::removePattern(int index)
{
    if (index < 0 || index >= m_patterns.size())
        return;
    // Its files are left alone on purpose. Nothing stores a parent link, so they simply
    // re-home under whichever pattern matches them next — or under none.
    m_patterns.remove(index);
}

void LogSettingsTree::movePattern(int index, int delta)
{
    const int to = index + delta;
    if (index < 0 || index >= m_patterns.size() || to < 0 || to >= m_patterns.size())
        return;
    m_patterns.move(index, to);
}

int LogSettingsTree::addPattern(LogPatternNode node)
{
    if (node.id.isEmpty() || indexOfPatternId(node.id) >= 0) {
        int n = m_patterns.size() + 1;
        // Ids are generated rather than taken from the match text: two patterns may
        // legitimately read the same while they are being edited.
        while (indexOfPatternId(QStringLiteral("p%1").arg(n)) >= 0)
            ++n;
        node.id = QStringLiteral("p%1").arg(n);
    }
    m_patterns.push_back(node);
    return m_patterns.size() - 1;
}

} // namespace loftail
