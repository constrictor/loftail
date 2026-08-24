#include "ArchiveLocation.h"

#include "RemoteLocation.h"

#include <QDir>
#include <QFileInfo>
#include <QLatin1String>

namespace loftail {

namespace {

// A container may hold several members and so needs one named. Checked BEFORE the
// single-stream table below, which is the whole reason `.tar.gz` is a tar rather than
// a gz: the longest match has to win, and these are the longer suffixes.
constexpr const char *kContainerSuffixes[] = {
    ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.z",
    ".tgz", ".tbz", ".tbz2", ".txz", ".tzst", ".taz",
    ".tar", ".zip", ".7z",
};

// A bare compressed stream: exactly one member, never named.
constexpr const char *kSingleStreamSuffixes[] = {
    ".gz", ".bz2", ".xz", ".zst", ".lzma", ".z",
};

// The last path component of a plain path or a URL. Both tables are about a file
// NAME, so every classifier reduces to this first and callers may pass either.
QStringView lastComponent(const QString &path)
{
    const auto cut = path.lastIndexOf(u'/');
    return cut < 0 ? QStringView(path) : QStringView(path).mid(cut + 1);
}

bool endsWithAny(QStringView name, const char *const *suffixes, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const auto suffix = QLatin1String(suffixes[i]);
        if (name.size() > suffix.size() && name.endsWith(suffix, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

// Where the container ends inside `path`: the offset of the separator that follows the
// first component carrying an archive extension of EITHER kind, or path.size() when it
// is the last one. -1 when no component is an archive.
//
// Both tables cut, not just the multi-member one, so that the collapse rule below is
// total: a typed `/logs/app.log.gz/app.log` reduces to `/logs/app.log.gz` rather than
// becoming a second spelling of one log. Which table matched decides what the address
// MEANS — isSingleStream() — but not where it splits.
qsizetype archiveCut(const QString &path)
{
    qsizetype start = 0;
    while (start <= path.size()) {
        qsizetype end = path.indexOf(u'/', start);
        if (end < 0)
            end = path.size();
        const QStringView comp = QStringView(path).mid(start, end - start);
        if (endsWithAny(comp, kContainerSuffixes, std::size(kContainerSuffixes))
            || endsWithAny(comp, kSingleStreamSuffixes, std::size(kSingleStreamSuffixes))) {
            return end;
        }
        if (end == path.size())
            return -1;
        start = end + 1;
    }
    return -1;
}

} // namespace

bool ArchiveLocation::isSingleStreamName(const QString &name)
{
    const QStringView comp = lastComponent(name);
    if (endsWithAny(comp, kContainerSuffixes, std::size(kContainerSuffixes)))
        return false; // .tar.gz is a tar, not a gz
    return endsWithAny(comp, kSingleStreamSuffixes, std::size(kSingleStreamSuffixes));
}

bool ArchiveLocation::isContainerName(const QString &name)
{
    return endsWithAny(lastComponent(name), kContainerSuffixes, std::size(kContainerSuffixes));
}

bool ArchiveLocation::isArchivePath(const QString &path)
{
    return split(path).has_value();
}

std::optional<ArchiveLocation> ArchiveLocation::split(const QString &path)
{
    if (path.isEmpty())
        return std::nullopt;

    const bool remote = RemoteLocation::isRemote(path);

    // Rule 0. A local path that already names a regular file is never split — which is
    // what keeps a real directory called `bundle.zip` working: the file inside it wins
    // over the reading where the directory is an archive. Cannot apply remotely, where
    // the answer would cost a round trip on a path that is normalized constantly.
    if (!remote) {
        const QString native = QDir::fromNativeSeparators(path);
        if (QFileInfo(native).isFile()) {
            ArchiveLocation loc;
            if (isContainerName(native) || isSingleStreamName(native)) {
                loc.container = native;
                return loc;
            }
            return std::nullopt;
        }
    }

    // Rules 1 and 2, over whichever path string the address actually contains. Both
    // rules are one cut: which table matched decides the meaning, not the boundary.
    if (remote) {
        const auto url = RemoteLocation::parse(path);
        if (!url)
            return std::nullopt;
        const qsizetype cut = archiveCut(url->path);
        if (cut < 0)
            return std::nullopt;
        RemoteLocation containerUrl = *url;
        containerUrl.path = url->path.left(cut);
        ArchiveLocation loc;
        loc.container = containerUrl.toString();
        loc.member = url->path.mid(cut + 1);
        return loc;
    }

    const QString native = QDir::fromNativeSeparators(path);
    const qsizetype cut = archiveCut(native);
    if (cut < 0)
        return std::nullopt;
    ArchiveLocation loc;
    loc.container = native.left(cut);
    loc.member = native.mid(cut + 1);
    return loc;
}

QString ArchiveLocation::normalize(const QString &path)
{
    if (const auto loc = split(path))
        return loc->toString();
    return path;
}

QString ArchiveLocation::toString() const
{
    if (container.isEmpty())
        return {};

    QString base = RemoteLocation::isRemote(container)
        ? RemoteLocation::normalize(container)
        : QFileInfo(QDir::fromNativeSeparators(container)).absoluteFilePath();

    // The collapse rule: a bare compressed stream keeps its plain path and never grows
    // a member, so `/logs/app.log.gz` has exactly one spelling.
    if (isSingleStream() || member.isEmpty())
        return base;
    return base + u'/' + member;
}

QString ArchiveLocation::displayMember() const
{
    if (!isSingleStream())
        return QFileInfo(member).fileName();

    // One unnamed member: it is the container's own name with the compression suffix
    // taken off, which is what the writer called the log before it was compressed.
    QString name = QFileInfo(lastComponent(container).toString()).fileName();
    for (const char *suffix : kSingleStreamSuffixes) {
        const auto s = QLatin1String(suffix);
        if (name.size() > s.size() && name.endsWith(s, Qt::CaseInsensitive))
            return name.left(name.size() - s.size());
    }
    return name;
}

} // namespace loftail
