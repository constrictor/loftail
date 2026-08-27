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

#include "ConfigFileIO.h"

#include "PromptRelay.h"
#include "RemoteLocation.h"
#include "SshPrompter.h"
#include "SshWorkerPool.h"

#if defined(LOFTAIL_HAVE_SSH)
#include "SshSession.h"
#endif

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>

namespace loftail {

namespace {
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::ConfigFileIO)
};

// A config file is a config file, not a log: it is read whole, into an editor, by
// somebody about to change it. A cap keeps a mistyped path — a core dump, a database, a
// log — from being pulled into a QPlainTextEdit that would then try to lay it out.
constexpr qint64 kMaxConfigBytes = 16LL * 1024 * 1024;

} // namespace

ConfigReadResult readConfigFile(const QString &address)
{
    ConfigReadResult out;

    if (RemoteLocation::isRemote(address)) {
        // Reached only by a caller that did not check configAddressIsRemote() first: a
        // remote read is a connect, and a connect does not belong on the thread that
        // asked for it. ConfigTransfer is the way in.
        out.error = Tr::tr("%1 is on another machine and must be read over SSH.")
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
                        .arg(info.size() / (1024LL * 1024));
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
        out.error = Tr::tr("%1 is on another machine and must be written over SSH.")
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

bool configAddressIsRemote(const QString &address)
{
    return RemoteLocation::isRemote(address);
}

bool configAddressIsWritable(const QString &address, QString *reason)
{
#if !defined(LOFTAIL_HAVE_SSH)
    if (RemoteLocation::isRemote(address)) {
        if (reason) {
            // The same sentence a remote LOG open gives in this configuration, and for
            // the same reason: what is missing is a dependency, not a feature.
            *reason = Tr::tr("This copy of loftail was built without SSH support, so a "
                             "config file on another machine cannot be opened.");
        }
        return false;
    }
#else
    Q_UNUSED(address);
    Q_UNUSED(reason);
#endif
    return true;
}

// --- ConfigTransfer ---------------------------------------------------------

// The shared block is SshWorkerPool's, not one of this file's own: a config transfer and
// a restart run make exactly the same lifetime promises, and two copies of the abandon
// rule would be two places for it to rot. See SshWorkerPool.h, where every rule in it is
// written down beside the crash that taught it.
struct ConfigTransfer::Shared : SshWorkerShared
{
};

ConfigTransfer::ConfigTransfer(QObject *parent)
    : QObject(parent), m_shared(std::make_shared<Shared>())
{
}

ConfigTransfer::~ConfigTransfer()
{
    // Abandon, never join — the rule and its reasoning are in SshWorkerShared::abandon().
    m_shared->abandon();
}

void drainConfigTransfers(int budgetMs)
{
    // Kept as a name of its own so MainWindow's one call site still reads as what it is,
    // and because the drain now covers the RESTART runner too — the pool is shared, so a
    // caller draining "config transfers" is draining both, which is what shutdown needs.
    drainSshWorkers(budgetMs);
}

void ConfigTransfer::startRead(const QString &address)
{
#if !defined(LOFTAIL_HAVE_SSH)
    ConfigReadResult out;
    configAddressIsWritable(address, &out.error);
    QTimer::singleShot(0, this, [this, out]() { emit readFinished(out); });
#else
    auto shared = m_shared;
    QPointer<ConfigTransfer> self(this);
    startSshWorker([address, shared, self]() {
        ConfigReadResult out;
        const QString error = withSshSession(
            address, &shared->relay, shared, [&out, &address](SshSession &session, const QString &path) {
                QString why;
                if (!session.readFileAt(path, &out.bytes, &out.existed, &why))
                    return why;
                if (out.bytes.size() > kMaxConfigBytes) {
                    out.bytes.clear();
                    return QCoreApplication::translate(
                               "loftail::ConfigFileIO",
                               "%1 is too large to edit as a config file.")
                        .arg(RemoteLocation::withoutPassword(address));
                }
                out.ok = true;
                return QString();
            });
        if (!error.isEmpty()) {
            out.ok = false;
            out.error = error;
        }
        if (shared->abandoned || !QCoreApplication::instance())
            return;
        // Back on the application thread. The QPointer is only ever DEREFERENCED there,
        // which is what makes carrying it across legal: if the owner went in the
        // meantime, this simply does nothing.
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, out]() {
                if (self)
                    emit self->readFinished(out);
            },
            Qt::QueuedConnection);
    });
#endif
}

void ConfigTransfer::startWrite(const QString &address, const QByteArray &bytes)
{
#if !defined(LOFTAIL_HAVE_SSH)
    Q_UNUSED(bytes);
    ConfigWriteResult out;
    configAddressIsWritable(address, &out.error);
    QTimer::singleShot(0, this, [this, out]() { emit writeFinished(out); });
#else
    auto shared = m_shared;
    QPointer<ConfigTransfer> self(this);
    startSshWorker([address, bytes, shared, self]() {
        ConfigWriteResult out;
        const QString error =
            withSshSession(address, &shared->relay, shared,
                        [&out, &bytes](SshSession &session, const QString &path) {
                            QString why;
                            if (!session.writeFileAt(path, bytes, &why))
                                return why;
                            out.ok = true;
                            return QString();
                        });
        if (!error.isEmpty()) {
            out.ok = false;
            out.error = error;
        }
        if (shared->abandoned || !QCoreApplication::instance())
            return;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, out]() {
                if (self)
                    emit self->writeFinished(out);
            },
            Qt::QueuedConnection);
    });
#endif
}

} // namespace loftail
