#include "ConfigLocation.h"

#include "ArchiveLocation.h"
#include "LogAnchor.h"
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

// The directory part of a POSIX-style path, WITHOUT going through QFileInfo.
//
// QFileInfo is native-path-aware, so on Windows it grows a drive letter and backslashes
// onto somebody else's /var/log — which is the whole content of a remote path. This is
// deliberately dumb string work for that reason, and it is used for the local branch too
// so that the two cannot come to disagree about what "the directory it is in" means.
QString posixDirOf(const QString &path)
{
    const int slash = int(path.lastIndexOf(u'/'));
    if (slash < 0)
        return {};
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

// Put `path` back on the anchor's filesystem as an address.
QString addressOn(const LogAnchor &anchor, const QString &path)
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

    const auto anchor = logAnchorOf(logAddress);
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
