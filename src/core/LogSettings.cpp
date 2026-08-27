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

LogProfile LogSettingsTree::inherited(const QString &address) const
{
    // First match wins, so the order of the list is the order of precedence and the
    // user's reordering is the only tie-break there is.
    for (const LogPatternNode &p : m_patterns) {
        if (p.matches(address))
            return p.profile;
    }
    return m_defaults;
}

int LogSettingsTree::matchingPattern(const QString &address) const
{
    for (int i = 0; i < m_patterns.size(); ++i) {
        if (m_patterns.at(i).matches(address))
            return i;
    }
    return -1;
}

bool LogSettingsTree::operator==(const LogSettingsTree &o) const
{
    if (m_defaults != o.m_defaults || m_patterns.size() != o.m_patterns.size())
        return false;
    for (int i = 0; i < m_patterns.size(); ++i) {
        const LogPatternNode &a = m_patterns.at(i);
        const LogPatternNode &b = o.m_patterns.at(i);
        // The id is deliberately NOT compared: it is an identity for the dialog's own
        // reselection and says nothing about what any log gets, so a tree that only
        // renumbered would spend a sweep of the whole pool for no change in any answer.
        // POSITION is compared, by walking in order, because first-match-wins makes the
        // order part of the answer.
        if (a.kind != b.kind || a.match != b.match || a.caseSensitive != b.caseSensitive
            || a.matchFullPath != b.matchFullPath || a.profile != b.profile)
            return false;
    }
    return true;
}

void LogSettingsTree::removePattern(int index)
{
    if (index < 0 || index >= m_patterns.size())
        return;
    // The logs under it are left alone on purpose. Nothing stores a parent link, so they
    // simply re-home under whichever pattern matches them next — or under none. What that
    // costs is the pool sweep (LogFileStore::pruneAgainst): a record that agreed with this
    // pattern now agrees with a different one, or with the defaults, and nothing writes it.
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
        int n = int(m_patterns.size()) + 1;
        // Ids are generated rather than taken from the match text: two patterns may
        // legitimately read the same while they are being edited.
        while (indexOfPatternId(QStringLiteral("p%1").arg(n)) >= 0)
            ++n;
        node.id = QStringLiteral("p%1").arg(n);
    }
    m_patterns.push_back(node);
    return int(m_patterns.size()) - 1;
}

} // namespace loftail
