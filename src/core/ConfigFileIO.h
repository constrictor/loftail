#pragma once

#include <QByteArray>
#include <QString>

namespace loftail {

// Reading and writing a log's config file (SPEC.md §4).
//
// THE ONLY PLACE loftail WRITES A FILE THE USER NAMED. Everything else it writes is its
// own — the settings tree, the per-log pool, the session, a spool — under a directory it
// chose. That is why this is a seam of its own rather than a couple of QFile calls at
// the call site: a write to somebody's `log4cplus.properties` deserves one place where
// the rules about permissions, atomicity and refusing to create directories are stated
// and can be reviewed together.
struct ConfigReadResult
{
    bool       ok = false;
    // false with ok=true is the SUPPORTED "not there yet" case, not a failure: the editor
    // opens on an empty buffer and Save creates the file. Distinguishing it from a read
    // that failed is the whole reason this is not just a QByteArray.
    bool       existed = false;
    QByteArray bytes;
    QString    error;
};

struct ConfigWriteResult
{
    bool    ok = false;
    QString error;
};

// Read the whole file at `address`.
//
// A missing file is a SUCCESS with `existed` false. A file that is there and cannot be
// read is a failure with a reason — "not there" and "there and shut" are different
// sentences, the distinction logSourcePresence() already draws for logs.
ConfigReadResult readConfigFile(const QString &address);

// Write `bytes` to `address`, preserving what was there.
//
// Three rules, each of which is a way of not surprising somebody about their own file:
//
//   - THE DIRECTORY IS NEVER CREATED. A missing directory is refused, by name, so the
//     reader can see they mistyped a path rather than finding a config tree sprouted
//     somewhere unexpected. This is the one place the project's own AtomicJson rule is
//     deliberately inverted: that one mkpath()s, because it writes loftail's own tree.
//   - THE FILE'S PERMISSIONS SURVIVE. The write is a temp-file-and-rename, which creates
//     a NEW inode, so a config that was 0640 would come back 0644 unless the mode is read
//     first and restored after. A silent permission widening on a file that decides what
//     an application logs is the worst thing this function could do.
//   - THE WRITE IS ATOMIC where it can be. QSaveFile renames over the target, so a
//     crash or a full disk leaves the previous contents rather than half a file.
ConfigWriteResult writeConfigFile(const QString &address, const QByteArray &bytes);

// Whether `address` can be edited at all in this build and at this address.
//
// Remote config files are read and written over SSH, which is an optional dependency —
// so a build without it answers false with a reason, exactly as a remote LOG open does.
bool configAddressIsWritable(const QString &address, QString *reason);

} // namespace loftail
