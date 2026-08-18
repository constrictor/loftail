#include "TabLabels.h"

#include "ArchiveLocation.h"
#include "RemoteLocation.h"

#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace loftail {
namespace {

// What one address contributes to the decision.
struct Parts
{
    QString     base;    // the log's own name, host or container bracketed on
    QStringList parents; // its parent directories, NEAREST FIRST
};

QStringList splitSegments(const QString &path)
{
    QString p = path;
#ifdef Q_OS_WIN
    // Native separators, and only here: a backslash is an ordinary character in a
    // POSIX file name, so folding it into a separator everywhere would split one
    // segment into two on the platform where it is not one.
    p.replace(u'\\', u'/');
#endif
    QStringList out = p.split(u'/', Qt::SkipEmptyParts);
    out.removeAll(QStringLiteral("."));
    return out;
}

// The directories above `path`, nearest first. A remote URL contributes the
// directories on its own host — never the host itself, which the log's name carries.
QStringList parentSegments(const QString &path)
{
    QString local = path;
    if (const auto url = RemoteLocation::parse(path))
        local = url->path;
    QStringList segs = splitSegments(local);
    if (!segs.isEmpty())
        segs.removeLast(); // the leaf is the log's own name, already in the base
    std::reverse(segs.begin(), segs.end());
    return segs;
}

Parts partsOf(const QString &address)
{
    Parts p;
    p.base = logSourceDisplayName(address);
    if (const auto loc = ArchiveLocation::split(address)) {
        // Inside the container first, then above it: `bundle.tar.gz` is bracketed onto
        // the name already, so what the container contributes is where IT sits — which
        // is what tells /srv/a/bundle.tar.gz from /srv/b/bundle.tar.gz apart.
        if (!loc->member.isEmpty())
            p.parents = parentSegments(loc->member);
        p.parents += parentSegments(loc->container);
        return p;
    }
    p.parents = parentSegments(address);
    return p;
}

// The prefix `depth` parents make, outermost first and with a trailing separator.
QString prefixOf(const Parts &p, int depth)
{
    QString out;
    for (int i = qMin(depth, int(p.parents.size())) - 1; i >= 0; --i)
        out += p.parents.at(i) + u'/';
    return out;
}

QString elideMiddle(const QString &s, int max)
{
    if (s.size() <= max || max < 3)
        return s;
    const int keep = max - 1;
    const int head = (keep + 1) / 2;
    return s.left(head) + QChar(0x2026) + s.right(keep - head);
}

QString labelFor(const Parts &p, int depth, int maxPrefixChars)
{
    if (depth <= 0)
        return p.base;
    return elideMiddle(prefixOf(p, depth), maxPrefixChars) + p.base;
}

} // namespace

QStringList tabLabelsFor(const QStringList &addresses, int maxPrefixChars)
{
    QVector<Parts> parts;
    parts.reserve(addresses.size());
    for (const QString &a : addresses)
        parts.append(partsOf(a));

    // Logs sharing a name are the only ones with anything to decide; everything else
    // keeps the plain name it has always worn. Grouping on the base is also what keeps
    // the answer stable — a prefix never turns one group's label into another's,
    // because a base carries no separator and so is always the tail of its own label.
    QHash<QString, QVector<int>> groups;
    for (int i = 0; i < parts.size(); ++i)
        groups[parts.at(i).base].append(i);

    QStringList labels;
    labels.resize(addresses.size());
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        const QVector<int> &members = it.value();
        int depth = 0;
        if (members.size() > 1) {
            int deepest = 0;
            for (int i : members)
                deepest = qMax(deepest, int(parts.at(i).parents.size()));
            // One segment at a time, and the same number for every log in the group: a
            // group whose members were cut at different depths reads as several
            // different rules rather than one. The test is on the label AS SHOWN,
            // elision included, so a cut prefix can never quietly hand two logs one
            // label again.
            //
            // A segment is kept only when it BUYS something, which is what settles the
            // case no depth can separate — two accounts on one host reading one path.
            // Growing those to their full depth would spend the whole address on a
            // distinction it never makes; the deepest useful depth for such a group is
            // 0, so both keep the plain shared name and the tooltip does the telling.
            int bestSeparated = 1; // what depth 0 achieves: one label for the group
            for (int d = 1; d <= deepest; ++d) {
                QSet<QString> seen;
                for (int i : members)
                    seen.insert(labelFor(parts.at(i), d, maxPrefixChars));
                const int distinct = int(seen.size());
                if (distinct > bestSeparated) {
                    bestSeparated = distinct;
                    depth = d; // stays 0 while no depth has bought anything
                }
                if (bestSeparated == int(members.size()))
                    break;
            }
        }
        for (int i : members)
            labels[i] = labelFor(parts.at(i), depth, maxPrefixChars);
    }
    return labels;
}

} // namespace loftail
