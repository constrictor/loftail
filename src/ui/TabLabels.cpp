#include "TabLabels.h"

#include "ArchiveLocation.h"
#include "RemoteLocation.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace loftail {
namespace {

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

QString elideMiddle(const QString &s, int max)
{
    if (s.size() <= max || max < 3)
        return s;
    const int keep = max - 1;
    const int head = (keep + 1) / 2;
    return s.left(head) + QChar(0x2026) + s.right(keep - head);
}

// --- The tab rule ----------------------------------------------------------

// What one address contributes to the decision, taken apart along the axes a label may
// be built from. Nothing here is group-relative; the path RUN is, and is computed per
// group in pathRuns() below.
struct Parts
{
    QString     bare;      // logSourceBareName() — the grouping key
    QString     device;    // the host, empty for a local log
    QString     container; // the archive container's own name, empty when there is none
    QStringList outer;     // dirs above the file, or above the container, ROOT FIRST
    QStringList inner;     // dirs above the member INSIDE the container, ROOT FIRST
};

// The directories above `path`, root first, with the leaf dropped. `path` here is always
// a plain path — a local one, or the decoded path half of a remote URL — never an
// address with a scheme on it.
QStringList parentSegments(const QString &path)
{
    QStringList segs = splitSegments(path);
    if (!segs.isEmpty())
        segs.removeLast(); // the leaf is the log's own name, already in the bare
    return segs;
}

// The directories above an address that may be remote, and the host if it is one.
void takeLocation(const QString &address, QStringList *segments, QString *device)
{
    if (const auto url = RemoteLocation::parse(address)) {
        *segments = parentSegments(url->path);
        *device = url->displayHost();
        return;
    }
    // A remote-shaped address that did NOT parse still has to give up its segments, and
    // the whole of what is left now reaches the label — where the old rule spent a
    // bounded number of parents and the userinfo happened to be the leaf it dropped.
    // `ssh://deploy:hunter2@web1` would arrive here with the password in a segment, so
    // it is taken out first and never by accident.
    *segments = parentSegments(RemoteLocation::isRemote(address)
                                   ? RemoteLocation::withoutPassword(address)
                                   : address);
    device->clear();
}

Parts partsOf(const QString &address)
{
    Parts p;
    p.bare = logSourceBareName(address);
    if (const auto loc = ArchiveLocation::split(address)) {
        takeLocation(loc->container, &p.outer, &p.device);
        // Only a NAMED member has directories of its own inside the container. Where
        // none is named the bare name IS the container's, so naming it again would read
        // "bundle.zip (bundle.zip)".
        if (!loc->member.isEmpty())
            p.inner = parentSegments(loc->member);
        const QString name = QFileInfo(loc->container).fileName();
        if (name != p.bare)
            p.container = name;
        return p;
    }
    takeLocation(address, &p.outer, &p.device);
    return p;
}

// How many leading entries every one of `lists` shares.
int commonPrefix(const QVector<const QStringList *> &lists)
{
    if (lists.isEmpty())
        return 0;
    int n = int(lists.first()->size());
    for (const QStringList *l : lists)
        n = qMin(n, int(l->size()));
    for (int i = 0; i < n; ++i) {
        const QString &v = lists.first()->at(i);
        for (const QStringList *l : lists) {
            if (l->at(i) != v)
                return i;
        }
    }
    return n;
}

// How many trailing entries every one of `lists` shares.
int commonSuffix(const QVector<const QStringList *> &lists)
{
    if (lists.isEmpty())
        return 0;
    int n = int(lists.first()->size());
    for (const QStringList *l : lists)
        n = qMin(n, int(l->size()));
    for (int i = 0; i < n; ++i) {
        const QString &v = lists.first()->at(lists.first()->size() - 1 - i);
        for (const QStringList *l : lists) {
            if (l->at(l->size() - 1 - i) != v)
                return i;
        }
    }
    return n;
}

// One list with what every member of the group shares stripped off BOTH ends. `head` is
// how many root-side entries they share, `tail` how many file-side ones.
QStringList residue(const QStringList &segs, int head, int tail)
{
    const int len = int(segs.size());
    // Clamped, and in this order: two members with identical paths make head and tail
    // both the whole length, and an unclamped subtraction gives a negative range.
    const int lo = qMin(head, len);
    const int hi = qMax(lo, len - tail);
    return segs.mid(lo, hi - lo);
}

// The path each member of a group shows: its directories with the ones EVERY member of
// the group carries stripped from both ends, root first.
//
// The two spaces are stripped separately and only then joined. Concatenating first would
// hide the boundary between "inside the archive" and "above it" from the stripper, and a
// run could then splice a directory outside a container onto one inside another.
QStringList pathRuns(const QVector<Parts> &parts, const QVector<int> &members)
{
    QVector<const QStringList *> outers;
    QVector<const QStringList *> inners;
    outers.reserve(members.size());
    inners.reserve(members.size());
    for (int i : members) {
        outers.append(&parts.at(i).outer);
        inners.append(&parts.at(i).inner);
    }
    const int outerHead = commonPrefix(outers);
    const int outerTail = commonSuffix(outers);
    const int innerHead = commonPrefix(inners);
    const int innerTail = commonSuffix(inners);

    QStringList runs;
    runs.reserve(members.size());
    for (int i : members) {
        QStringList segs = residue(parts.at(i).outer, outerHead, outerTail);
        segs += residue(parts.at(i).inner, innerHead, innerTail);
        runs.append(segs.join(u'/')); // address order: above the container, then inside
    }
    return runs;
}

// The axes a label may spend, in the order they are offered.
enum Axis { AxisDevice = 0, AxisContainer, AxisPath, AxisCount };

QString labelFor(const Parts &p, const QString &run, const bool (&use)[AxisCount],
                 int maxRunChars)
{
    QStringList components;
    if (use[AxisDevice] && !p.device.isEmpty())
        components.append(p.device);
    if (use[AxisContainer] && !p.container.isEmpty())
        components.append(p.container);
    if (use[AxisPath] && !run.isEmpty()) {
        // The RUN is elided and nothing else is: it is the one component with no bound,
        // and eliding the joined text instead eats the device's tail from the head side
        // — the device being the thing this rule offers first.
        components.append(elideMiddle(run, maxRunChars));
    }
    if (components.isEmpty())
        return p.bare; // a member with nothing to add wears its plain name, never "x ()"
    return QStringLiteral("%1 (%2)").arg(p.bare, components.join(QStringLiteral(", ")));
}

int distinctLabels(const QVector<Parts> &parts, const QVector<int> &members,
                   const QStringList &runs, const bool (&use)[AxisCount], int maxRunChars)
{
    QSet<QString> seen;
    for (int k = 0; k < members.size(); ++k)
        seen.insert(labelFor(parts.at(members.at(k)), runs.at(k), use, maxRunChars));
    return int(seen.size());
}

// --- The recent-files rule -------------------------------------------------

// What one address contributes to the OLDER rule, kept for the recent-files menu.
struct PrefixParts
{
    QString     base;    // the log's own name, host or container bracketed on
    QStringList parents; // its parent directories, NEAREST FIRST
};

QStringList prefixParentSegments(const QString &path)
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

PrefixParts prefixPartsOf(const QString &address)
{
    PrefixParts p;
    p.base = logSourceDisplayName(address);
    if (const auto loc = ArchiveLocation::split(address)) {
        // Inside the container first, then above it: `bundle.tar.gz` is bracketed onto
        // the name already, so what the container contributes is where IT sits — which
        // is what tells /srv/a/bundle.tar.gz from /srv/b/bundle.tar.gz apart.
        if (!loc->member.isEmpty())
            p.parents = prefixParentSegments(loc->member);
        p.parents += prefixParentSegments(loc->container);
        return p;
    }
    p.parents = prefixParentSegments(address);
    return p;
}

// The prefix `depth` parents make, outermost first and with a trailing separator.
QString prefixOf(const PrefixParts &p, int depth)
{
    QString out;
    for (int i = qMin(depth, int(p.parents.size())) - 1; i >= 0; --i)
        out += p.parents.at(i) + u'/';
    return out;
}

QString prefixLabelFor(const PrefixParts &p, int depth, int maxPrefixChars)
{
    if (depth <= 0)
        return p.base;
    return elideMiddle(prefixOf(p, depth), maxPrefixChars) + p.base;
}

// The indices of `addresses` grouped by `key`, in FIRST-APPEARANCE order so the answer
// does not depend on a hash's iteration order.
QVector<QVector<int>> groupsBy(const QStringList &keys)
{
    QHash<QString, int> where;
    QVector<QVector<int>> groups;
    for (int i = 0; i < keys.size(); ++i) {
        const auto it = where.constFind(keys.at(i));
        if (it == where.cend()) {
            where.insert(keys.at(i), int(groups.size()));
            groups.append(QVector<int>{i});
        } else {
            groups[it.value()].append(i);
        }
    }
    return groups;
}

} // namespace

QStringList tabLabelsFor(const QStringList &addresses, int maxQualifierChars)
{
    QVector<Parts> parts;
    parts.reserve(addresses.size());
    QStringList keys;
    keys.reserve(addresses.size());
    for (const QString &a : addresses) {
        parts.append(partsOf(a));
        keys.append(parts.last().bare);
    }

    QStringList labels;
    labels.resize(addresses.size());
    for (const QVector<int> &members : groupsBy(keys)) {
        // A log nothing else answers to keeps its plain name. This is the whole of "the
        // bracket appears only when there is something to be told from": a solitary
        // remote log reads `app.log`, not `app.log (web1)`.
        if (members.size() == 1) {
            labels[members.first()] = parts.at(members.first()).bare;
            continue;
        }

        const QStringList runs = pathRuns(parts, members);

        // One axis at a time, in priority order, and an axis is kept only when it BUYS
        // something. That is what leaves the host out of a group whose members all share
        // one — `app.log (svc-a)` rather than `app.log (host-a, svc-a)` — and what
        // settles a group no axis can separate: they keep the shared name, and the
        // tooltip, which is always the full address, does the telling.
        //
        // The comparison is on the label AS SHOWN, elision included, so a run that
        // elides down to one string cannot quietly hand two logs one label again.
        bool use[AxisCount] = {false, false, false};
        int best = 1; // what no axis achieves: one label for the whole group
        for (int axis = 0; axis < AxisCount && best < int(members.size()); ++axis) {
            use[axis] = true;
            const int distinct = distinctLabels(parts, members, runs, use, maxQualifierChars);
            if (distinct > best)
                best = distinct;
            else
                use[axis] = false;
        }

        for (int k = 0; k < members.size(); ++k) {
            labels[members.at(k)] =
                labelFor(parts.at(members.at(k)), runs.at(k), use, maxQualifierChars);
        }
    }
    return labels;
}

QStringList prefixedLabelsFor(const QStringList &addresses, int maxPrefixChars)
{
    QVector<PrefixParts> parts;
    parts.reserve(addresses.size());
    QStringList keys;
    keys.reserve(addresses.size());
    for (const QString &a : addresses) {
        parts.append(prefixPartsOf(a));
        keys.append(parts.last().base);
    }

    QStringList labels;
    labels.resize(addresses.size());
    for (const QVector<int> &members : groupsBy(keys)) {
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
            int bestSeparated = 1; // what depth 0 achieves: one label for the group
            for (int d = 1; d <= deepest; ++d) {
                QSet<QString> seen;
                for (int i : members)
                    seen.insert(prefixLabelFor(parts.at(i), d, maxPrefixChars));
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
            labels[i] = prefixLabelFor(parts.at(i), depth, maxPrefixChars);
    }
    return labels;
}

} // namespace loftail
