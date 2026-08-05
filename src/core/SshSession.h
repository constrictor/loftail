#pragma once

#include "RemoteLocation.h"

#include <QString>

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
        qint64 mtime = 0;
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

    // Connect, verify the host key, and authenticate. Blocking, bounded by
    // `timeoutMs`. `prompter` may be null, in which case anything needing a person
    // fails rather than waits. Returns false and fills `error` — never with anything
    // derived from a credential.
    bool connectTo(const RemoteLocation &location, SshPrompter *prompter, int timeoutMs,
                   QString *error, Failure *failure = nullptr);

    void close();
    bool isConnected() const;

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
    bool fstatTracksHandle() const;

    // Read up to `length` bytes at `offset` of the open file. Returns the number of
    // bytes read, 0 at EOF, or -1 on error (with `error` filled). Forward-only in
    // practice (invariant #9); the seek exists to resume after a reconnect.
    qint64 readAt(qint64 offset, char *buffer, qint64 length, QString *error);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace loftail
