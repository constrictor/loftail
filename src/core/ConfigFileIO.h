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

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

namespace loftail {

class SshPrompter;

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

// Whether `bytes` read back as `text` — the guard every config save runs before a byte
// leaves the process, with `name` naming the file in the sentence it gives back.
//
// The write path replays a file's own encoding, byte-order mark and line endings rather
// than re-deciding them, which is only safe while the two directions of Decoder agree
// about every character. Once they did not: decode() dropped a leading U+FEFF that
// encode() had written, so a config opened, not edited and saved came back one character
// shorter, silently, on the file that decides what an application logs — and the remote
// write is in place, so there is no previous inode to fall back to (bugs.md 43).
//
// So the bytes are read back through the very detection the editor's own read runs, and
// a save that would not survive its own round trip is REFUSED rather than published. It
// is a guard and not a fix: the asymmetry that prompted it is fixed in Decoder, and this
// is here for the next one. It costs one decode of one config file per save.
bool configBytesReadBackAs(const QString &name, const QByteArray &bytes, const QString &text,
                           QString *reason);

// Whether `address` can be edited at all in this build and at this address.
//
// Remote config files are read and written over SSH, which is an optional dependency —
// so a build without it answers false with a reason, exactly as a remote LOG open does.
bool configAddressIsWritable(const QString &address, QString *reason);

// Whether reaching `address` means a network round trip, and therefore a worker.
bool configAddressIsRemote(const QString &address);

// Stop every transfer still running and wait, up to `budgetMs`, for their threads to end.
//
// Called by the window on its way out. A transfer is abandoned rather than joined while
// the process is alive — see ConfigTransfer — but at shutdown that is not enough: Qt's
// own globals go with the application object, and a worker still inside QTcpSocket then
// writes through a pointer that has just become null. Bounded, because a quit that can
// hang is worse than a budget that can be exceeded.
void drainConfigTransfers(int budgetMs = 3000);

// One read or one write of a config file on ANOTHER MACHINE, off the calling thread.
//
// A local config is a QFile call and is answered where it is asked. A remote one is a
// connect — up to a twenty-second timeout, possibly a host-key question and a password —
// and running that where the window lives is exactly what M17 took out of the log path
// (ARCHITECTURE.md §6.3.3). So this does the work on a thread of its own and delivers
// the answer back through a queued signal.
//
// LIFETIME, which is the whole difficulty, LIVES IN SshWorkerPool.h. A transfer must not
// be JOINED on the way out: closing a tab on a host that is not answering would then wait
// out the connect, and a worker that can be blocked asking the application thread for a
// password makes that wait a deadlock — the reason ~SourceSpool retires its fetcher
// instead of joining it. The thread, the shared state block, the relay it owns and the
// bounded shutdown drain are all shared with the restart runner (M23), because they are
// the same promise made twice and each rule in them was learned from a crash.
class ConfigTransfer : public QObject
{
    Q_OBJECT

public:
    // `parent` owns it: destroying the parent abandons the transfer.
    explicit ConfigTransfer(QObject *parent = nullptr);
    ~ConfigTransfer() override;

    // NO PROMPTER ARGUMENT. The question a connect may ask has to be answered on the
    // application thread, so it goes through a PromptRelay — and that relay must outlive
    // the WORKER, not the caller. Passing one in is how this crashed: a relay owned by
    // the window was destroyed while a detached thread was still inside connectTo(),
    // and the next virtual call on it aborted the process with "pure virtual method
    // called". The transfer owns its own now, in the block the worker keeps a reference
    // to, so it cannot go early. PromptRelay holds no prompter of its own — it resolves
    // sshPrompter() inside each marshalled call — so one per transfer costs nothing.
    void startRead(const QString &address);
    void startWrite(const QString &address, const QByteArray &bytes);

    // What the worker and the owner share: the abandon flag and the session to abort.
    // Public because the free function that runs the connect needs it, and a friend
    // declaration for one helper in one .cpp buys nothing over saying so.
    struct Shared;

signals:
    // Emitted on the application thread, exactly once, or never if the transfer was
    // abandoned first.
    void readFinished(ConfigReadResult result);
    void writeFinished(ConfigWriteResult result);

private:
    std::shared_ptr<Shared> m_shared;
};

} // namespace loftail
