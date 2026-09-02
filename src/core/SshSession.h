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

#include "RemoteLocation.h"
#include "SshExecCommands.h" // SizeSource, kUnknownMtime — no libssh2, always compiled

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>

namespace loftail {

class SshPrompter;


// One SSH connection with one remote file open on it (ARCHITECTURE.md §6.3).
//
// THREADING: a LIBSSH2_SESSION is not thread-safe, and this class does nothing to
// make it so. The rule is that exactly one thread touches an instance at a time.
// In practice a session is CONNECTED on the thread that opened the document and then
// handed to that fetcher's own thread for the tail loop — a handoff, never sharing.
//
// libssh2 is confined to the .cpp: this header names no libssh2 type, so the rest of
// the application (and SourceSpool, which builds one of these) needs no include path
// for it, and the no-SSH build is a matter of not compiling one file.
class SshSession
{
public:
    SshSession();
    ~SshSession();

    SshSession(const SshSession &) = delete;
    SshSession &operator=(const SshSession &) = delete;

    struct Attrs
    {
        bool   valid = false;
        qint64 size = 0;
        // kUnknownMtime when this server has no way to report one — an exec session
        // measuring with `ls` or `wc` (§6.3.1). READ IT BY NAME: -1 compares as less
        // than every real mtime, so a caller that only ever asks "did it advance?" would
        // conclude "no" forever and stop detecting rotation without ever saying so.
        qint64 mtime = kUnknownMtime;
    };

    // Why a connect or an open failed, in the only terms the caller cares about: is it
    // worth trying again on its own (M13, §6.5)?
    //
    // The split is not about severity, it is about whether ANYTHING WILL CHANGE without
    // somebody doing something. A host that is down comes back by itself; a host key
    // that has changed will still have changed in five minutes' time, and retrying it
    // would be hammering a host loftail has just refused to talk to.
    enum class Failure {
        None,
        // Retryable, unattended: the host was unreachable, or it answered and the path
        // is not there. Both resolve themselves — a machine reboots, a log gets written.
        Unreachable,
        NoSuchFile,
        // Retryable only after a PERSON acts: a password is needed and there was nobody
        // to ask, or the host is not in known_hosts. An unattended retry gets the same
        // answer forever, so the caller must surface it and wait to be asked again.
        NeedsPerson,
        // Not retryable: a changed host key, credentials rejected, the user cancelled,
        // or the server offers no method that could work.
        Refused,
    };

    // What the caller means to DO with the connection, settled at connectTo() (§6.3).
    //
    // The default is what every reader wants and what `SshFetcher` gets by saying nothing.
    // The other value exists to buy back a wait, and the size of that wait is the whole
    // reason there is a parameter here at all — see connectTo() below.
    enum class Need {
        // Read something: the log this session was connected for, or a config file at an
        // arbitrary path. The connect settles a transport — SFTP where the server offers
        // it, the exec fallback where it does not (§6.3.1) — so that openFile(),
        // statPath(), statHandle(), readAt(), readFileAt() and writeFileAt() all have
        // something to dispatch on.
        LogTransport,
        // Run a command, and nothing else. runScript() is the ONLY operation such a
        // session supports; every one of the six above refuses it by name.
        ExecOnly,
    };

    // Whether this connection asks the far end to deflate what it sends.
    //
    // ONE FLAG AND NO LEVEL, because libssh2 offers no level: it hardcodes
    // Z_DEFAULT_COMPRESSION, so "zlib" is the whole of what can be requested.
    enum class Compression {
        None,
        // libssh2_session_flag(LIBSSH2_FLAG_COMPRESS), which puts `zlib` and
        // `zlib@openssh.com` in front of `none` in the client's KEX list. `none` stays
        // last, so a server built or configured without compression settles on it by
        // ordinary negotiation rather than failing (§6.3).
        Zlib,
    };

    // Who is connecting, for the one rule that differs between them.
    enum class Purpose {
        // Reading a log: the whole file once and then every line of it for as long as
        // the tab is open. The only connect worth compressing.
        Fetch,
        // A config read, a config write, a restart script. Kilobytes, on somebody else's
        // CPU (SshWorkerPool.inl, §6.8, §6.9).
        Errand,
    };

    // What a connect made for `purpose` asks for, given what the user ticked for the host.
    //
    // THE ONE PLACE THE "A FETCH MAY COMPRESS, AN ERRAND MAY NOT" RULE LIVES, so that it
    // is a fact about this function rather than about which call site remembered to leave
    // an argument out. Every errand connect passes `userAsked = true` deliberately: it
    // states that an errand does not compress even for a host whose fetches do, instead of
    // depending on the option happening to be off — and it makes the rule falsifiable in a
    // build with no libssh2, which is every build CI runs (tst_sshoptions).
    //
    // constexpr and defined here, not in SshSession.cpp, for exactly that reason: the .cpp
    // is the one file gated on LOFTAIL_HAVE_SSH.
    static constexpr Compression compressionFor(Purpose purpose, bool userAsked)
    {
        return purpose == Purpose::Fetch && userAsked ? Compression::Zlib : Compression::None;
    }

    // Connect, verify the host key, and authenticate. Blocking, bounded by
    // `timeoutMs`. `prompter` may be null, in which case anything needing a person
    // fails rather than waits. Returns false and fills `error` — never with anything
    // derived from a credential.
    //
    // `compression` IS AN ARGUMENT HERE RATHER THAN A SETTER, and the reason is an
    // ordering trap: `libssh2_session_flag(LIBSSH2_FLAG_COMPRESS)` has to be set between
    // `libssh2_session_init()` and `libssh2_session_handshake()`, both of which are inside
    // this function, because compression is settled by the key exchange and nothing after
    // it can move. A `setCompression()` called after connectTo() would therefore be a
    // silent no-op — the box ticked, the option stored, the session uncompressed, and
    // nothing anywhere to say so. Settled at the connect, exactly as `need` is.
    //
    // `need` DECIDES WHETHER SFTP IS ASKED FOR AT ALL, and the twenty seconds that saves
    // is the point of it. A `LogTransport` connect ends in `libssh2_sftp_init()`, and on
    // a server that ACCEPTS the subsystem channel with no `sftp-server` behind it — the
    // stripped-down embedded image the exec fallback exists for (§6.3.1) — libssh2 waits
    // for a version packet that is never coming and gives up with a plain TIMEOUT, at
    // `timeoutMs`, which is 20 s for every attended gesture (`kSshWorkerConnectTimeoutMs`).
    // Only then does the shell probe rescue it. A caller that was only ever going to open
    // an exec channel paid that whole wait for a subsystem it did not want; on a server
    // that DOES answer SFTP it paid a needless channel open and version exchange instead.
    // `Need::ExecOnly` skips the init and the probe together and returns as soon as the
    // login is in — which is what File ▸ Restart App does, and why it now starts running
    // the script at once instead of after twenty seconds of silence (§6.9).
    //
    // AN ExecOnly SESSION HAS NO SFTP HANDLE AND NO SETTLED SIZE RUNG; DO NOT REACH FOR
    // ONE. It never asked the server which transport it offers, so there is nothing to
    // dispatch a read on and no probe result to read a size ladder out of. Handing one to
    // openFile()/statPath()/statHandle()/readAt()/readFileAt()/writeFileAt() is a
    // programming error, and each of those refuses it in words rather than answering: the
    // natural failures are all SILENT — readAt() would return 0, which is exactly what end
    // of file looks like, and readFileAt()/writeFileAt() would quietly take the *shell*
    // path on a server whose SFTP is perfectly good.
    bool connectTo(const RemoteLocation &location, SshPrompter *prompter, int timeoutMs,
                   QString *error, Failure *failure = nullptr,
                   Need need = Need::LogTransport,
                   Compression compression = Compression::None);

    // What the key exchange actually settled on for compression, in each direction, in
    // the server's own spelling — `zlib@openssh.com`, `zlib`, or `none`. Empty before a
    // connect, and latched at the handshake so it survives close().
    //
    // WORTH HAVING BECAUSE THE FEATURE IS OTHERWISE UNANSWERABLE FROM OUTSIDE. "I ticked
    // the box and it is not faster" has three explanations that look identical — the
    // server runs `Compression no`, this libssh2 was built without zlib (in which case it
    // offers no compression methods at all and the negotiation is clean but silent), or
    // the box did not reach the connect — and only what came back off the wire tells them
    // apart. connectTo() writes it to the diagnostic log once per connect for that reason;
    // this accessor is what a test can assert on.
    struct CompressionMethods
    {
        QString inbound;  // server → client, LIBSSH2_METHOD_COMP_SC — the one that matters
        QString outbound; // client → server, LIBSSH2_METHOD_COMP_CS
    };
    CompressionMethods negotiatedCompression() const;

    // Consulted repeatedly while connecting; returning true abandons the attempt with
    // Failure::Unreachable. Set before connectTo() by an owner that may be asked to stop
    // — which is every fetcher — and called from the connecting thread, so it must not
    // touch anything that thread does not already own.
    //
    // This is what keeps `timeoutMs` from being the price of closing a tab. Without it a
    // connect to a host that is not answering runs its full twenty seconds no matter who
    // has lost interest, and the registry's shutdown drain hits its cap every time.
    void setAbandonCheck(std::function<bool()> check);

    // Break the connection from ANOTHER THREAD so that a blocking libssh2 call returns
    // now instead of waiting out its timeout. The owning thread then fails, reports and
    // tears down exactly as it would for a dropped link — nothing here frees anything.
    //
    // The one call on this class that is safe to make while another thread is inside it,
    // and it is deliberately the smallest possible such call: it shuts the socket down
    // and touches nothing else. setAbandonCheck() covers the waits loftail controls;
    // this covers the ones inside libssh2, which are the rest of them.
    void abort();

    void close();
    bool isConnected() const;

    // How this session reads the remote file.
    enum class Mode {
        Sftp, // the normal one: a real handle, real fstat, random access
        Exec, // the fallback: `stat` and `tail` over a plain exec channel (§6.3.1)
    };

    // The mode's name for the diagnostic log, NOT for the user — untranslated, for the
    // reason fetchStateName() and sizeSourceName() are (DiagnosticLog.h).
    static const char *modeName(Mode mode)
    {
        return mode == Mode::Exec ? "exec" : "sftp";
    }

    // Which of the two connectTo() settled on. Sftp unless the server refused it, in
    // which case the fallback was tried before giving up. Worth surfacing rather than
    // hiding: the exec transport spends a process per read on the far end and detects
    // rotation more weakly, so a user seeing odd behaviour deserves to know which one
    // they are on.
    //
    // Exec for a `Need::ExecOnly` session too, which is NOT the same statement: there the
    // server was never asked, and the answer means "this session talks over exec channels"
    // rather than "this server refused SFTP". It is deliberately not a third enumerator —
    // every `mode == Exec` branch inside this class routes away from the null `d->sftp`,
    // which is exactly where an ExecOnly session must not go, so the binary spelling is
    // what makes it safe by construction. Nothing outside reads mode() except the
    // fetcher's diagnostic line, and a fetcher never holds an ExecOnly session.
    Mode mode() const;

    // How an exec session measures the file, settled at openFile() by probing this
    // server rather than by assuming (§6.3.1). SizeSource::None in Mode::Sftp, where the
    // question does not arise, and before the first successful open.
    //
    // Worth surfacing for the same reason mode() is, and one more: the `Wc` rung reads
    // the whole file to answer, so the fetcher slows its poll down when it is in use.
    SizeSource sizeSource() const;

    // Open the remote file named by the location this session connected for. Keeping
    // the handle open is what makes rotation detectable: fstat() follows the file the
    // handle refers to, while stat() re-resolves the name (see fstatTracksHandle()).
    bool openFile(QString *error, Failure *failure = nullptr);
    void closeFile();
    bool hasFile() const;

    // Attributes of the NAME (re-resolved every call).
    Attrs statPath() const;
    // Attributes of the OPEN HANDLE.
    Attrs statHandle() const;

    // Whether this server's FSTAT actually follows the handle rather than
    // re-resolving the path. Probed once at connect: OpenSSH's sftp-server does, and
    // that is what stands in for a POSIX inode when detecting a rotation. A server
    // that does not forces the weaker size/head-compare fallback in SshFetcher.
    //
    // Always false in Mode::Exec — there is no handle to stat, only a path re-resolved
    // per command — so that fallback is what an exec session always uses. Which of the
    // two forms of it applies depends on whether Attrs::mtime is known: an mtime that
    // advanced without the size growing, or, with no mtime at all, a size that has
    // stalled, checked on a timer rather than on every poll.
    bool fstatTracksHandle() const;

    // --- Whole-file operations at an ARBITRARY path (SPEC.md §4) --------------
    //
    // Everything above is about THE file this session was connected for — one open
    // handle, read forward, watched for rotation. A log's config file is a different
    // path on the same machine, read once and written once, so it gets its own pair
    // rather than a mode on `openFile()`: nothing here touches `location`, the open
    // handle, or the size ladder, and a session that is tailing a log is unaffected by
    // one of these running on it.

    // Read the whole of `path`. A file that is NOT THERE is a success with `existed`
    // false, not a failure — the editor opens empty on it and saving creates it, and
    // an empty file that does exist has to be tellable from it.
    bool readFileAt(const QString &path, QByteArray *out, bool *existed, QString *error);

    // Replace the contents of `path`, creating it if it is not there.
    //
    // IN PLACE WHEN IT EXISTS, and that is a deliberate choice against atomicity. A
    // temp-and-rename would leave a file owned by whoever loftail connected as, with a
    // fresh mode — so a config that was `0640 root:adm` would come back `0644 you:you`,
    // silently, on the file that decides what an application logs. Truncating the
    // existing inode keeps owner, group and mode; what it costs is that a write dying
    // halfway leaves a short file, which SPEC.md §4 states rather than leaving to be
    // discovered, and which the size check afterwards is there to catch.
    bool writeFileAt(const QString &path, const QByteArray &bytes, QString *error);

    // --- Running the user's restart script (SPEC.md §4) -----------------------

    // Run `command` on a plain exec channel, streaming stdout and stderr SEPARATELY to
    // `onChunk` as they arrive, and answering the command's exit status.
    //
    // WORKS IN EITHER MODE. An exec channel needs only the session, not SFTP, so a server
    // that does do SFTP still runs this the same way — the exec fallback and this are
    // different uses of the same facility, not the same code path.
    //
    // STDERR IS KEPT, which is the one thing this does not share with the transport's own
    // runCommand(): that one drains stderr and DISCARDS it, deliberately, because a
    // server's warning is not the size it was asked for. Here it is half the answer — a
    // byte on stderr is one of the two ways a restart is reported as having gone wrong.
    //
    // BLOCKS FOR AS LONG AS THE SCRIPT RUNS, with the session timeout suspended for the
    // duration and restored afterwards: left at the connect budget, any restart taking
    // longer than that would be reported as a dropped link. Interrupting it is abort()'s
    // job, from another thread — the same mechanism a config transfer relies on.
    //
    // The exit status is only available once the channel closes, so a script that leaves
    // a child holding stdout open never returns one. That is stated in SPEC.md §4 and is
    // what the dialog's Abort button is for.
    bool runScript(const QString &command,
                   const std::function<void(const QByteArray &bytes, bool isStdErr)> &onChunk,
                   int *exitCode, QString *error);

    // Read up to `length` bytes at `offset` of the open file. Returns the number of
    // bytes read, 0 at EOF, or -1 on error (with `error` filled). Forward-only in
    // practice (invariant #9); the seek exists to resume after a reconnect.
    qint64 readAt(qint64 offset, char *buffer, qint64 length, QString *error);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace loftail
