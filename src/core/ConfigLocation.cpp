#include "ConfigLocation.h"

#include "ArchiveLocation.h"
#include "RemoteLocation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLatin1String>
#include <QStringList>

#include <optional>

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr(), and every `reason` below is read by a person — it reaches the editor's
// notice strip and the refusal strip. See RemoteLocation.cpp for the same shim.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::ConfigLocation)
};

// Where a log physically IS: a filesystem, and a path on it.
//
// `remote` empty means the local machine. `path` is a path on whichever that is, so the
// two travel together — placing a config path means keeping the filesystem and replacing
// the path, which is exactly what keeps "the config is on the same device as the log"
// true without any caller having to restate it.
struct Anchor
{
    std::optional<RemoteLocation> remote; // nullopt == local
    QString path;
};

// The directory part of a POSIX-style path, WITHOUT going through QFileInfo.
//
// QFileInfo is native-path-aware, so on Windows it grows a drive letter and backslashes
// onto somebody else's /var/log — which is the whole content of a remote path. This is
// deliberately dumb string work for that reason, and it is used for the local branch too
// so that the two cannot come to disagree about what "the directory it is in" means.
QString posixDirOf(const QString &path)
{
    const int slash = path.lastIndexOf(u'/');
    if (slash < 0)
        return QString();
    if (slash == 0)
        return QStringLiteral("/"); // "/app.log" -> "/", not ""
    return path.left(slash);
}

// `configured` placed in `dir`, cleaned.
//
// QDir::cleanPath and NEVER canonicalFilePath(): a config file that does not exist yet is
// a SUPPORTED case (the editor opens empty and Save creates it), and canonicalFilePath()
// answers "" for a path that is not there — the trap logSettingsKey() already records.
// cleanPath is also pure string work, which keeps the remote branch honest.
QString placedIn(const QString &dir, const QString &configured)
{
    if (QDir::isAbsolutePath(configured))
        return QDir::cleanPath(configured);
    if (dir.isEmpty())
        return QDir::cleanPath(configured);
    return QDir::cleanPath(dir + u'/' + configured);
}

// The log's filesystem and its own path on it, or nullopt when the address is one we
// cannot place anything against.
//
// The archive branch reduces ONCE MORE rather than returning the container directly,
// because a container may itself be remote (`ssh://h/srv/b.tar.gz/app.log`): the anchor
// then has to be the host plus the container's path, not the container string.
std::optional<Anchor> anchorOf(const QString &logAddress)
{
    if (logAddress.isEmpty())
        return std::nullopt;

    // Peel containers until the address names something that sits on a filesystem
    // rather than inside a file. `ssh://h/srv/b.tar.gz/app.log` peels once, to
    // `ssh://h:22/srv/b.tar.gz`, whose directory is then the base.
    //
    // split() is reused verbatim rather than the rule being re-derived, which is what
    // keeps a real directory named `bundle.zip` working (its own rule 0).
    //
    // TERMINATION IS ON INEQUALITY AND NOTHING ELSE. split() applied to a BARE container
    // answers with that same container and an empty member — a `.zip` with nothing
    // picked, a `.gz` whose member is implied — so peeling unconditionally spins on an
    // unchanged string and overflows the stack. It must NOT also test that the container
    // got shorter: for a remote address split() returns the container in NORMAL FORM,
    // with the port spelled out, so `ssh://host/srv/b.zip/m` peels to the strictly
    // LONGER `ssh://host:22/srv/b.zip`, and a length test would refuse the one peel that
    // address needs. The loop bound is a backstop against a cycle no known input
    // produces, not the argument for why this ends.
    QString address = logAddress;
    for (int peel = 0; peel < 8; ++peel) {
        const auto archive = ArchiveLocation::split(address);
        if (!archive || archive->container == address)
            break;
        address = archive->container;
    }

    if (RemoteLocation::isRemote(address)) {
        const auto loc = RemoteLocation::parse(address);
        if (!loc || !loc->isValid())
            return std::nullopt; // refused by the caller, which words it
        Anchor a;
        a.remote = loc;
        a.path = loc->path;
        return a;
    }

    // The peeled address, NOT the one we were handed: for an archived log this is the
    // container, and anchoring on the original would put the config inside the archive —
    // the exact thing the ruling forbids.
    Anchor a;
    a.path = address;
    return a;
}

// Put `path` back on the anchor's filesystem as an address.
QString addressOn(const Anchor &anchor, const QString &path)
{
    if (!anchor.remote)
        return path;
    // Copied from the anchor so user and port ride along, then emitted through
    // toString(), which is the normal form and never writes a password. Building the
    // string by hand is how a credential ends up in a new address.
    RemoteLocation loc = *anchor.remote;
    loc.path = path;
    return loc.toString();
}
} // namespace

ConfigAddress resolveConfigAddress(const QString &logAddress, const QString &configuredPath)
{
    ConfigAddress out;

    // Nothing configured. Not an error — the caller opens a file dialog.
    if (configuredPath.trimmed().isEmpty())
        return out;

    // A config path is a PATH, never a URL. The transport is derived from the log's, so
    // a second URL here would name a second host — a second credential prompt, and at
    // the pattern level one host named for every log the pattern matches.
    if (RemoteLocation::isRemote(configuredPath)) {
        out.state = ConfigAddress::State::Refused;
        out.reason = Tr::tr("A config file lives on the same machine as its log, so its "
                            "path may not be a remote address: %1")
                         .arg(RemoteLocation::withoutPassword(configuredPath));
        return out;
    }

    const auto anchor = anchorOf(logAddress);
    if (!anchor) {
        out.state = ConfigAddress::State::Refused;
        // withoutPassword() is the ONE filter for an address that is about to be shown,
        // and it matters most here: this branch is reached precisely when parse() FAILED,
        // which is the case that never went through parse()'s own password-dropping.
        out.reason = logAddress.isEmpty()
            ? Tr::tr("There is no log to resolve a config file against.")
            : Tr::tr("Not a valid log address: %1")
                  .arg(RemoteLocation::withoutPassword(logAddress));
        return out;
    }

    const QString base = posixDirOf(anchor->path);
    const QString resolved = placedIn(base, configuredPath);

    // Landing inside a container. The ruling puts a config BESIDE the archive, and a
    // member cannot be written without rebuilding the container anyway — so this is
    // refused in words rather than opened read-only, which would be a second kind of
    // editor tab to explain. Asked of split() so the answer cannot drift from the one
    // the rest of the application gives.
    //
    // Run for a REMOTE path too, not only a local one. split()'s rule 0 (never split a
    // path that exists as a local file) simply does not fire for a remote path, which
    // leaves rules 1 and 2 — pure string work on the extension — and those are exactly
    // the ones that matter here. `ssh://h/srv/bundle.zip/x.properties` is as much inside
    // a container as its local twin, and skipping the check for remote would open an
    // editor onto a member the save path cannot write.
    if (const auto inside = ArchiveLocation::split(resolved)) {
        out.state = ConfigAddress::State::Refused;
        out.reason = Tr::tr("A config file cannot be read from inside an archive: "
                            "%1 is in %2.")
                         .arg(resolved, inside->container);
        return out;
    }

    out.state = ConfigAddress::State::Resolved;
    out.address = addressOn(*anchor, resolved);
    out.baseDir = addressOn(*anchor, base);
    return out;
}

} // namespace loftail
