#include "ConfigFileIO.h"

#include "RemoteLocation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace loftail {

namespace {
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::ConfigFileIO)
};

// A config file is a config file, not a log: it is read whole, into an editor, by
// somebody about to change it. A cap keeps a mistyped path — a core dump, a database, a
// log — from being pulled into a QPlainTextEdit that would then try to lay it out.
constexpr qint64 kMaxConfigBytes = 16 * 1024 * 1024;
} // namespace

ConfigReadResult readConfigFile(const QString &address)
{
    ConfigReadResult out;

    if (RemoteLocation::isRemote(address)) {
        // Not yet: the remote half of this feature is its own piece of work, because
        // loftail has never written to a machine that is not the user's and the write
        // side of that is a security boundary. Said in words rather than silently
        // returning an empty buffer, which would look like an empty config file and
        // invite somebody to save over a real one.
        out.error = Tr::tr("Config files on another machine cannot be opened yet: %1")
                        .arg(RemoteLocation::withoutPassword(address));
        return out;
    }

    const QFileInfo info(address);
    if (!info.exists()) {
        // THE SUPPORTED CASE, not a failure. The editor opens empty and Save creates it.
        out.ok = true;
        out.existed = false;
        return out;
    }
    if (info.isDir()) {
        out.error = Tr::tr("%1 is a directory, not a config file.").arg(address);
        return out;
    }
    if (info.size() > kMaxConfigBytes) {
        out.error = Tr::tr("%1 is too large to edit as a config file (%2 MB).")
                        .arg(address)
                        .arg(info.size() / (1024 * 1024));
        return out;
    }

    QFile file(address);
    if (!file.open(QIODevice::ReadOnly)) {
        // "There and shut" is a different sentence from "not there", and this is where
        // they are told apart: a file whose mode is 000 must not be described as one
        // that has not appeared yet.
        out.error = Tr::tr("Cannot read %1: %2").arg(address, file.errorString());
        return out;
    }
    out.bytes = file.readAll();
    out.ok = true;
    out.existed = true;
    return out;
}

ConfigWriteResult writeConfigFile(const QString &address, const QByteArray &bytes)
{
    ConfigWriteResult out;

    if (RemoteLocation::isRemote(address)) {
        out.error = Tr::tr("Config files on another machine cannot be saved yet: %1")
                        .arg(RemoteLocation::withoutPassword(address));
        return out;
    }

    const QFileInfo info(address);
    const QDir dir = info.absoluteDir();
    if (!dir.exists()) {
        // REFUSED, BY NAME, and deliberately not created. The inverse of AtomicJson's
        // rule, and inverted on purpose: that one writes loftail's own tree, where
        // creating a missing directory is right. Here a missing directory almost always
        // means a mistyped path, and the useful answer is to say which one — not to
        // sprout a config tree somewhere the user did not ask for.
        out.error = Tr::tr("There is no directory %1, so %2 cannot be saved. "
                           "Create the directory first, or correct the path in "
                           "File ▸ Preferences.")
                        .arg(dir.absolutePath(), info.fileName());
        return out;
    }

    // Read the mode BEFORE the write, while the original inode is still there.
    const bool existed = info.exists();
    const QFile::Permissions before = existed ? QFile::permissions(address) : QFile::Permissions();

    QSaveFile file(address);
    if (!file.open(QIODevice::WriteOnly)) {
        out.error = file.errorString();
        return out;
    }
    if (file.write(bytes) != bytes.size()) {
        out.error = file.errorString();
        file.cancelWriting(); // the target keeps its previous contents
        return out;
    }
    if (!file.commit()) {
        out.error = file.errorString();
        return out;
    }

    // AFTER the rename, for the reason AtomicJson::writePrivate() records: commit()
    // replaces the file, so a mode set on the temporary would not survive it. The
    // difference is that this RESTORES what was there rather than imposing a mode of its
    // own — a config that was 0640 and group-readable must not come back 0644 because
    // loftail happened to save it.
    if (existed && before != QFile::Permissions() && QFile::permissions(address) != before) {
        if (!QFile::setPermissions(address, before)) {
            // Reported rather than swallowed: the file IS saved, so this is not a
            // failure of the write, but somebody whose config just became world-readable
            // needs to be told.
            out.ok = true;
            out.error = Tr::tr("%1 was saved, but its original permissions could not be "
                               "restored.")
                            .arg(info.fileName());
            return out;
        }
    }

    out.ok = true;
    return out;
}

bool configAddressIsWritable(const QString &address, QString *reason)
{
    if (RemoteLocation::isRemote(address)) {
        if (reason) {
            *reason = Tr::tr("This config file is on another machine. loftail can read "
                             "logs there, but cannot yet edit files there.");
        }
        return false;
    }
    return true;
}

} // namespace loftail
