#include "SshSession.h"

#include "DiagnosticLog.h"

#include "ExecSizeProbe.h"
#include "SshPrompter.h"
#include "SecretStore.h"
#include "SocketDetach.h"
#include "SshExecCommands.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QThread>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <cstring>
#include <mutex>

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr() — and these strings are user-facing all the same: they travel up to
// the status bar through Document::lastError() and LiveController::sourceStatusChanged.
// Q_DECLARE_TR_FUNCTIONS is what lets lupdate file them under a name that means
// something rather than under the file they happen to sit in.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::SshSession)
};
} // namespace


namespace {

// libssh2 wants global init exactly once per process.
void ensureLibraryInit()
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (libssh2_init(0) == 0)
            qAddPostRoutine([] { libssh2_exit(); });
    });
}

QString knownHostsPath()
{
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (home.isEmpty())
        return {};
    return home + QStringLiteral("/.ssh/known_hosts");
}

// The OpenSSH spelling of a fingerprint: "SHA256:" + unpadded base64. Shown to the
// user so it can be compared against what `ssh-keygen -lf` prints on the server.
QString sha256Fingerprint(LIBSSH2_SESSION *session)
{
    const char *hash = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!hash)
        return {};
    const QByteArray raw(hash, 32);
    QByteArray b64 = raw.toBase64();
    while (b64.endsWith('='))
        b64.chop(1);
    return QStringLiteral("SHA256:") + QString::fromLatin1(b64);
}

QString keyTypeName(int type)
{
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:     return QStringLiteral("ssh-rsa");
    case LIBSSH2_HOSTKEY_TYPE_DSS:     return QStringLiteral("ssh-dss");
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return QStringLiteral("ecdsa-sha2-nistp256");
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return QStringLiteral("ecdsa-sha2-nistp384");
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return QStringLiteral("ecdsa-sha2-nistp521");
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return QStringLiteral("ssh-ed25519");
#endif
    default: return QStringLiteral("unknown");
    }
}

// The known-host typemask for a raw key obtained from a plain (unhashed) host name.
int knownHostTypeMask(int keyType)
{
    int mask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    switch (keyType) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: mask |= LIBSSH2_KNOWNHOST_KEY_SSHRSA; break;
    case LIBSSH2_HOSTKEY_TYPE_DSS: mask |= LIBSSH2_KNOWNHOST_KEY_SSHDSS; break;
#ifdef LIBSSH2_KNOWNHOST_KEY_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: mask |= LIBSSH2_KNOWNHOST_KEY_ECDSA_256; break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: mask |= LIBSSH2_KNOWNHOST_KEY_ECDSA_384; break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: mask |= LIBSSH2_KNOWNHOST_KEY_ECDSA_521; break;
#endif
#ifdef LIBSSH2_KNOWNHOST_KEY_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519: mask |= LIBSSH2_KNOWNHOST_KEY_ED25519; break;
#endif
    default: break;
    }
    return mask;
}

QString sessionError(LIBSSH2_SESSION *session, const QString &context)
{
    if (!session)
        return context;
    char *message = nullptr;
    int length = 0;
    libssh2_session_last_error(session, &message, &length, 0);
    if (!message || length <= 0)
        return context;
    return QStringLiteral("%1: %2").arg(context, QString::fromUtf8(message, length));
}

// Answers a keyboard-interactive challenge from the password we already hold. libssh2
// hands the callback no context but the session abstract, so the password is parked
// there for the duration of the call and cleared immediately afterwards.
void kbdIntCallback(const char *, int, const char *, int, int numPrompts,
                    const LIBSSH2_USERAUTH_KBDINT_PROMPT *,
                    LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void **abstract)
{
    const auto *password = abstract ? static_cast<QByteArray *>(*abstract) : nullptr;
    for (int i = 0; i < numPrompts; ++i) {
        // Only the first prompt gets the password; anything further (a second factor)
        // is answered empty, which fails cleanly rather than resending the secret.
        if (i == 0 && password) {
            responses[i].text = strdup(password->constData());
            responses[i].length = static_cast<unsigned int>(password->size());
        } else {
            responses[i].text = nullptr;
            responses[i].length = 0;
        }
    }
}

} // namespace

namespace {
// How long connectTo() waits for the TCP connection before checking whether anyone
// still wants it. Short enough that closing a tab on a host that is down feels
// instant; long enough that a healthy connect never sees more than one iteration.
constexpr int kConnectSliceMs = 250;
} // namespace

struct SshSession::Impl
{
    QTcpSocket        socket;   // resolves, connects, times out — then hands the fd over

    // OURS, not Qt's (SocketDetach.h). GUARDED, alone among these members, because
    // abort() reaches it from another thread while this one is inside libssh2 — and the
    // race that guard prevents is not a crash but something worse: teardown() closing
    // the descriptor between abort() reading it and shutting it down would aim a
    // shutdown at whatever the operating system had since handed that number to.
    std::mutex        fdMutex;
    qintptr           fd = -1;

    // Consulted while connecting so that a long connect can be given up on. Set before
    // connectTo() and read only by the connecting thread.
    std::function<bool()> abandonCheck;

    LIBSSH2_SESSION  *session = nullptr;
    LIBSSH2_SFTP     *sftp = nullptr;
    LIBSSH2_SFTP_HANDLE *file = nullptr;
    RemoteLocation    location;
    bool              fstatTracks = false;
    SshSession::Mode  mode = SshSession::Mode::Sftp;
    bool              execFileOpen = false; // Mode::Exec has no handle, only this flag
    ExecTools         execTools;            // what the connect-time probe found
    SizeSource        execSize = SizeSource::None; // settled at openFile(), per open

    ~Impl() { teardown(); }

    // The ladder, wired to this session. Built fresh per use — it holds no state worth
    // keeping and two std::functions cost nothing beside a round trip.
    //
    // The read seam binds execRead() and NOT SshSession::readAt(), which gates on
    // execFileOpen. Settling happens BEFORE that flag is set, so going through the
    // public method would make every validation read return zero bytes, reject every
    // rung, and disable the exec fallback entirely — silently, and only on the servers
    // it exists for.
    ExecSizeProbe sizeProbe()
    {
        return {
            location.path, execTools,
            [this](const QString &command, QByteArray *out) {
                int exitCode = 0;
                return runCommand(command, out, &exitCode);
            },
            [this](qint64 offset, qint64 length) {
                QByteArray buffer;
                buffer.resize(int(length));
                return execRead(offset, buffer.data(), length, nullptr);
            }};
    }

    // Run `command` on the server and collect its stdout. Blocking, bounded by the
    // session timeout like everything else here. Returns false when the channel could
    // not be opened or the command could not be started; a command that RAN and failed
    // returns true with whatever it printed, because "stat said nothing" and "stat
    // could not be launched" want different handling upstream.
    bool runCommand(const QString &command, QByteArray *stdOut, int *exitCode) const;

    // The same, with BYTES ON STDIN — which is the whole of the exec transport's write
    // path: `cat > 'path'` takes what it is given and puts it in the file. Separate
    // from runCommand() rather than an optional argument, because the write half has to
    // handle short writes and send EOF, and mixing that into the read path would put
    // channel-write bookkeeping on every `stat`.
    bool runCommandWithInput(const QString &command, const QByteArray &stdIn, int *exitCode,
                             QString *error) const;

    // Run `command` and stream BOTH streams back as they arrive, keeping them apart.
    //
    // Its own function rather than a flag on runCommand(), because it differs from that
    // one in three ways at once and each of them would be wrong there: stderr is KEPT
    // (there it is noise; here it is half the answer), the session runs NON-BLOCKING for
    // the duration (so that a script writing only to stderr is neither invisible nor able
    // to wedge the channel by filling its window), and the session timeout is SUSPENDED
    // (a restart that takes a minute is not a dropped link).
    bool runScriptStreaming(const QString &command,
                            const std::function<void(const QByteArray &, bool)> &onChunk,
                            int *exitCode, QString *error) const;

    void teardown()
    {
        if (file) {
            libssh2_sftp_close(file);
            file = nullptr;
        }
        if (sftp) {
            libssh2_sftp_shutdown(sftp);
            sftp = nullptr;
        }
        if (session) {
            libssh2_session_disconnect(session, "loftail closing");
            libssh2_session_free(session);
            session = nullptr;
        }
        // The descriptor goes LAST: libssh2_session_disconnect above writes a farewell
        // packet, and it needs a socket to write it to.
        {
            std::scoped_lock lock(fdMutex);
            closeDetachedSocket(fd);
            fd = -1;
        }
        if (socket.state() != QAbstractSocket::UnconnectedState)
            socket.disconnectFromHost();
    }

    // Host-key verification, before a single credential is sent.
    bool verifyHostKey(SshPrompter *prompter, QString *error, SshSession::Failure *failure);
    // Read `length` bytes at `offset` by running a command. Mode::Exec's readAt().
    qint64 execRead(qint64 offset, char *buffer, qint64 length, QString *error);
    bool authenticate(SshPrompter *prompter, QString *error, SshSession::Failure *failure);
    bool tryAgent() const;
    bool tryDefaultKeys() const;
    bool tryPassword(const QByteArray &password) const;
};

bool SshSession::Impl::runCommand(const QString &command, QByteArray *stdOut, int *exitCode) const
{
    if (!session)
        return false;

    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(session);
    if (!channel)
        return false;

    const QByteArray line = command.toUtf8();
    if (libssh2_channel_exec(channel, line.constData()) != 0) {
        libssh2_channel_free(channel);
        return false;
    }

    // Drain stdout to EOF. stderr is drained too and DISCARDED: a server that prints a
    // warning there must not wedge the channel by filling its window, but its complaint
    // is not the answer we asked for — a command that failed is recognised by what it
    // did not print on stdout, and by its exit status.
    QByteArray out;
    char buffer[8192];
    forever {
        const ssize_t n = libssh2_channel_read(channel, buffer, sizeof(buffer));
        if (n > 0) {
            out.append(buffer, int(n));
            continue;
        }
        ssize_t err = 0;
        do {
            err = libssh2_channel_read_stderr(channel, buffer, sizeof(buffer));
        } while (err > 0);
        if (n == 0 && libssh2_channel_eof(channel))
            break;
        if (n < 0)
            break; // timeout or transport error; whatever arrived is what we have
    }

    libssh2_channel_close(channel);
    libssh2_channel_wait_closed(channel);
    if (exitCode)
        *exitCode = libssh2_channel_get_exit_status(channel);
    libssh2_channel_free(channel);

    if (stdOut)
        *stdOut = out;
    return true;
}

namespace {
// How long the script loop sleeps when neither stream has anything yet.
//
// This is a POLL, which the rest of this file deliberately is not — the price of running
// non-blocking so that stdout and stderr can be watched at once. 50 ms is imperceptible
// beside a service restart and costs nothing next to the process being started on the
// far end.
constexpr int kScriptPollMs = 50;
} // namespace

bool SshSession::Impl::runScriptStreaming(
    const QString &command, const std::function<void(const QByteArray &, bool)> &onChunk,
    int *exitCode, QString *error) const
{
    const auto fail = [error](const QString &text) {
        if (error)
            *error = text;
        return false;
    };

    if (!session)
        return fail(Tr::tr("Not connected."));

    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(session);
    if (!channel)
        return fail(sessionError(session, Tr::tr("Opening a command channel failed")));

    const QByteArray line = command.toUtf8();
    if (libssh2_channel_exec(channel, line.constData()) != 0) {
        const QString why = sessionError(session, Tr::tr("Starting the command failed"));
        libssh2_channel_free(channel);
        return fail(why);
    }

    // SUSPEND THE TIMEOUT, and remember what it was. connectTo() leaves it at the connect
    // budget, so without this every restart script that outlives twenty seconds — which
    // is most of the interesting ones — would be reported as a dropped link rather than
    // as a script still running. Restored before the close below, which DOES want a bound.
    const long savedTimeout = libssh2_session_get_timeout(session);
    libssh2_session_set_timeout(session, 0);
    libssh2_session_set_blocking(session, 0);

    char buffer[8192];
    QString transportError;
    forever {
        bool moved = false;
        bool wouldBlock = false;
        bool broken = false;

        const ssize_t out = libssh2_channel_read(channel, buffer, sizeof(buffer));
        if (out > 0) {
            if (onChunk)
                onChunk(QByteArray(buffer, int(out)), false);
            moved = true;
        } else if (out == LIBSSH2_ERROR_EAGAIN) {
            wouldBlock = true;
        } else if (out < 0) {
            broken = true;
        }

        // BOTH STREAMS EVERY PASS, never stderr-only-when-stdout-is-quiet. A restart
        // script that says nothing on stdout and complains on stderr is the ordinary
        // failure, and reading stderr only after stdout has ended would hold every byte
        // of it back until the script finished — and, once its window filled, would stop
        // the far end writing at all.
        const ssize_t err = libssh2_channel_read_stderr(channel, buffer, sizeof(buffer));
        if (err > 0) {
            if (onChunk)
                onChunk(QByteArray(buffer, int(err)), true);
            moved = true;
        } else if (err == LIBSSH2_ERROR_EAGAIN) {
            wouldBlock = true;
        } else if (err < 0) {
            broken = true;
        }

        if (broken) {
            // The ordinary way out of an ABORTED run: abort() shuts the descriptor, so
            // the next read fails. Whatever arrived before that is what the reader keeps.
            transportError = sessionError(session, Tr::tr("The command was interrupted"));
            break;
        }
        if (moved)
            continue;
        if (!wouldBlock)
            break; // both streams at EOF: the command has finished writing
        QThread::msleep(kScriptPollMs);
    }

    // Back to blocking, and BOUNDED again, for the orderly close. The exit status is only
    // published once the channel is closed, and a close with no timeout on a socket that
    // abort() has just shut down is exactly the hang this whole layer exists to avoid.
    libssh2_session_set_timeout(session, savedTimeout > 0 ? savedTimeout : 20000);
    libssh2_session_set_blocking(session, 1);
    libssh2_channel_close(channel);
    libssh2_channel_wait_closed(channel);
    if (exitCode)
        *exitCode = libssh2_channel_get_exit_status(channel);
    libssh2_channel_free(channel);

    if (!transportError.isEmpty())
        return fail(transportError);
    return true;
}

bool SshSession::Impl::runCommandWithInput(const QString &command, const QByteArray &stdIn,
                                           int *exitCode, QString *error) const
{
    const auto fail = [error](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
    if (!session)
        return fail(Tr::tr("Not connected."));

    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(session);
    if (!channel)
        return fail(Tr::tr("The server would not open a channel to write with."));

    const QByteArray line = command.toUtf8();
    if (libssh2_channel_exec(channel, line.constData()) != 0) {
        libssh2_channel_free(channel);
        return fail(Tr::tr("The server would not run the command to write the file."));
    }

    // SHORT WRITES ARE THE NORMAL CASE, not an error: libssh2 writes as much as the
    // channel's window allows and returns how much that was. Treating the first return
    // as the whole thing silently truncates every file bigger than one window.
    qint64 written = 0;
    bool ok = true;
    while (written < stdIn.size()) {
        const ssize_t n = libssh2_channel_write(channel, stdIn.constData() + written,
                                                size_t(stdIn.size() - written));
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0) {
            // No progress and no error: the peer is gone or the session has timed out.
            // Spinning here would hang the worker for ever.
            ok = false;
            break;
        }
        written += qint64(n);
    }

    // EOF FIRST, then close. `cat` only finishes when its stdin ends, so a channel
    // closed without an EOF leaves the far end waiting and the exit status meaningless.
    libssh2_channel_send_eof(channel);
    libssh2_channel_wait_eof(channel);

    // Drain whatever it said so the window cannot fill and wedge the close.
    char sink[1024];
    while (libssh2_channel_read(channel, sink, sizeof(sink)) > 0) { }
    while (libssh2_channel_read_stderr(channel, sink, sizeof(sink)) > 0) { }

    libssh2_channel_close(channel);
    libssh2_channel_wait_closed(channel);
    const int code = libssh2_channel_get_exit_status(channel);
    libssh2_channel_free(channel);

    if (exitCode)
        *exitCode = code;
    if (!ok)
        return fail(Tr::tr("The connection dropped while writing the file."));
    return true;
}

qint64 SshSession::Impl::execRead(qint64 offset, char *buffer, qint64 length, QString *error)
{
    QByteArray out;
    int exitCode = 0;
    if (!runCommand(readCommand(location.path, offset, length), &out, &exitCode)) {
        if (error) {
            *error = Tr::tr("Reading %1 from %2 failed: the server would not run a "
                                    "command.")
                         .arg(location.path, location.host);
        }
        return -1;
    }
    // A short read is not an error: it is EOF, which is the ordinary answer while
    // tailing a log that has not grown since the size was taken.
    const qint64 got = qMin<qint64>(out.size(), length);
    if (got > 0)
        std::memcpy(buffer, out.constData(), size_t(got));
    return got;
}

bool SshSession::Impl::verifyHostKey(SshPrompter *prompter, QString *error,
                                    SshSession::Failure *failure)
{
    // Default for every exit below that does not say otherwise: a host-key problem is
    // a refusal, and refusals are not retried.
    *failure = SshSession::Failure::Refused;
    size_t keyLength = 0;
    int keyType = 0;
    const char *key = libssh2_session_hostkey(session, &keyLength, &keyType);
    if (!key) {
        *error = Tr::tr("%1 offered no host key.").arg(location.host);
        return false;
    }

    LIBSSH2_KNOWNHOSTS *hosts = libssh2_knownhost_init(session);
    if (!hosts) {
        *error = Tr::tr("Cannot read known hosts.");
        return false;
    }
    const QString knownHosts = knownHostsPath();
    if (!knownHosts.isEmpty()) {
        libssh2_knownhost_readfile(hosts, QFile::encodeName(knownHosts).constData(),
                                   LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    }

    const QByteArray host = location.host.toUtf8();
    struct libssh2_knownhost *known = nullptr;
    const int check = libssh2_knownhost_checkp(hosts, host.constData(), location.port,
                                               key, keyLength,
                                               knownHostTypeMask(keyType), &known);

    if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) {
        libssh2_knownhost_free(hosts);
        diagLog("ssh", QStringLiteral("host key %1: matches known_hosts (%2)")
                           .arg(location.target(), keyTypeName(keyType)));
        return true;
    }

    SshPrompter::HostKeyInfo info;
    info.host = location.host;
    info.port = location.port;
    info.keyType = keyTypeName(keyType);
    info.fingerprintSha256 = sha256Fingerprint(session);
    info.mismatch = (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH);

    // The fingerprint is public by definition — it is what a user is asked to compare
    // against the server's own — so recording it is safe, and it is exactly what somebody
    // reading this file after a refusal needs in order to check it by hand.
    diagLog("ssh", QStringLiteral("host key %1: %2 (%3 %4)")
                       .arg(location.target(),
                            QString::fromLatin1(info.mismatch ? "MISMATCH against known_hosts"
                                                              : "not in known_hosts"),
                            keyTypeName(keyType),
                            info.fingerprintSha256));

    if (info.mismatch) {
        // A DIFFERENT key is on record for this host. That is the man-in-the-middle
        // signature, and there is no "accept anyway" here — offering one would be the
        // single most dangerous convenience in the application. Tell the prompter so
        // it can explain, but refuse regardless of what it answers.
        if (prompter)
            prompter->confirmHostKey(info);
        libssh2_knownhost_free(hosts);
        *error = Tr::tr(
            "The host key for %1 has CHANGED since it was recorded (%2). This may be a "
            "server rebuild, or it may be an interception. loftail will not connect. "
            "Verify the key, then remove the stale entry from ~/.ssh/known_hosts.")
                     .arg(location.host, info.fingerprintSha256);
        return false;
    }

    if (!prompter) {
        // An unattended retry cannot accept a key on the user's behalf, and must never
        // be made to — this is the one decision that has to be a person's (§6.5).
        *failure = SshSession::Failure::NeedsPerson;
        libssh2_knownhost_free(hosts);
        *error = Tr::tr(
            "%1 is not in ~/.ssh/known_hosts and there is no way to ask about it here. "
            "Connect once with ssh to record its key, then reopen.")
                     .arg(location.host);
        return false;
    }

    const SshPrompter::HostKeyChoice choice = prompter->confirmHostKey(info);
    if (choice == SshPrompter::HostKeyChoice::Reject) {
        libssh2_knownhost_free(hosts);
        *error = Tr::tr("Host key for %1 was not accepted.").arg(location.host);
        return false;
    }

    if (choice == SshPrompter::HostKeyChoice::AcceptAndRemember && !knownHosts.isEmpty()) {
        libssh2_knownhost_addc(hosts, host.constData(), nullptr, key, keyLength, nullptr, 0,
                               knownHostTypeMask(keyType), nullptr);
        // Append the ONE new line rather than rewriting the file: writefile() emits
        // only what libssh2 managed to parse, so it can silently drop comments,
        // markers and entry types it did not understand — someone else's file.
        char line[4096];
        size_t written = 0;
        if (known == nullptr) {
            struct libssh2_knownhost *added = nullptr;
            libssh2_knownhost_checkp(hosts, host.constData(), location.port, key, keyLength,
                                     knownHostTypeMask(keyType), &added);
            known = added;
        }
        if (known
            && libssh2_knownhost_writeline(hosts, known, line, sizeof(line), &written,
                                           LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0
            && written > 0) {
            QDir().mkpath(QFileInfo(knownHosts).absolutePath());
            QFile out(knownHosts);
            if (out.open(QIODevice::Append | QIODevice::WriteOnly)) {
                out.write(line, static_cast<qint64>(written));
                out.close();
                out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            }
        }
    }

    libssh2_knownhost_free(hosts);
    return true;
}

bool SshSession::Impl::tryAgent() const
{
    LIBSSH2_AGENT *agent = libssh2_agent_init(session);
    if (!agent)
        return false;

    bool ok = false;
    if (libssh2_agent_connect(agent) == 0 && libssh2_agent_list_identities(agent) == 0) {
        const QByteArray user = location.user.toUtf8();
        struct libssh2_agent_publickey *identity = nullptr;
        struct libssh2_agent_publickey *previous = nullptr;
        while (libssh2_agent_get_identity(agent, &identity, previous) == 0) {
            if (libssh2_agent_userauth(agent, user.constData(), identity) == 0) {
                ok = true;
                break;
            }
            previous = identity;
        }
        libssh2_agent_disconnect(agent);
    }
    libssh2_agent_free(agent);
    return ok;
}

bool SshSession::Impl::tryDefaultKeys() const
{
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (home.isEmpty())
        return false;

    // The same files, in the same order, that OpenSSH would try by default. No
    // passphrase is supplied: a passphrase-protected key belongs in the agent, and
    // prompting for one here would be a second, differently-shaped password dialog.
    static const char *const names[] = {"id_ed25519", "id_ecdsa", "id_rsa"};
    const QByteArray user = location.user.toUtf8();
    // This is an auth LADDER, not a predicate — each step talks to the server and the
    // one that succeeds has already authenticated the session. std::ranges::any_of over
    // a side-effecting ten-line lambda would read as a question about the file names.
    // NOLINTNEXTLINE(readability-use-anyofallof)
    for (const char *name : names) {
        const QString priv = home + QStringLiteral("/.ssh/") + QLatin1String(name);
        if (!QFile::exists(priv))
            continue;
        const QString pub = priv + QStringLiteral(".pub");
        const QByteArray privPath = QFile::encodeName(priv);
        const QByteArray pubPath = QFile::encodeName(pub);
        if (libssh2_userauth_publickey_fromfile(
                session, user.constData(), QFile::exists(pub) ? pubPath.constData() : nullptr,
                privPath.constData(), "")
            == 0) {
            return true;
        }
    }
    return false;
}

bool SshSession::Impl::tryPassword(const QByteArray &password) const
{
    const QByteArray user = location.user.toUtf8();
    if (libssh2_userauth_password(session, user.constData(), password.constData()) == 0)
        return true;

    // Plenty of servers offer only keyboard-interactive, where a password is just the
    // answer to the first challenge. Park it on the session abstract for the callback.
    QByteArray scratch = password;
    void **abstract = libssh2_session_abstract(session);
    void *previous = abstract ? *abstract : nullptr;
    if (abstract)
        *abstract = &scratch;
    const int rc = libssh2_userauth_keyboard_interactive(session, user.constData(),
                                                         &kbdIntCallback);
    if (abstract)
        *abstract = previous;
    scratch.fill('\0');
    return rc == 0;
}

bool SshSession::Impl::authenticate(SshPrompter *prompter, QString *error,
                                    SshSession::Failure *failure)
{
    *failure = SshSession::Failure::Refused;
    if (location.user.isEmpty()) {
        // No user in the URL and no ~/.ssh/config parsing yet: fall back to the local
        // account name, which is what ssh does absent a User directive.
        location.user = QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                            .section(u'/', -1);
    }
    const QByteArray user = location.user.toUtf8();
    const QString target = location.target();

    // What the server will even consider. Calling this also completes the "none"
    // auth exchange, which some servers answer by granting access outright.
    const char *methods = libssh2_userauth_list(session, user.constData(),
                                                static_cast<unsigned int>(user.size()));
    if (!methods && libssh2_userauth_authenticated(session))
        return true;
    const QString available = methods ? QString::fromUtf8(methods) : QString();
    // The auth ladder, rung by rung. This is the single most useful thing in the
    // diagnostic log for a connect that "just does not work": which methods the server
    // was willing to consider, and how far down the ladder loftail got before it ran out
    // of things to try. The METHOD NAMES are the server's own and safe to record; what is
    // never recorded, here or anywhere below, is any secret that satisfies one.
    diagLog("ssh", QStringLiteral("auth %1: server offers %2")
                       .arg(target, available.isEmpty() ? QStringLiteral("(nothing)")
                                                        : available));

    if (available.contains(QLatin1String("publickey"))) {
        if (prompter)
            prompter->progress(Tr::tr("Trying SSH agent for %1…").arg(target));
        if (tryAgent()) {
            diagLog("ssh", QStringLiteral("auth %1: accepted by SSH agent").arg(target));
            return true;
        }
        if (tryDefaultKeys()) {
            diagLog("ssh", QStringLiteral("auth %1: accepted by a key file").arg(target));
            return true;
        }
        diagLog("ssh", QStringLiteral("auth %1: no agent identity or key file accepted")
                           .arg(target));
    }

    const bool passwordOffered = available.contains(QLatin1String("password"))
        || available.contains(QLatin1String("keyboard-interactive"));
    if (!passwordOffered) {
        *error = Tr::tr(
            "Could not authenticate to %1 with an SSH agent or key, and the server "
            "offers no password method (it allows: %2).")
                     .arg(target, available.isEmpty() ? Tr::tr("nothing") : available);
        return false;
    }

    // A password already accepted for this host this session — the reason opening a
    // second file on one host, or restoring a whole session, asks only once.
    if (SshCredentialCache::has(target)) {
        QByteArray cached = SshCredentialCache::password(target).toUtf8();
        const bool ok = tryPassword(cached);
        cached.fill('\0');
        if (ok) {
            diagLog("ssh", QStringLiteral("auth %1: accepted by the password already "
                                          "given this session").arg(target));
            return true;
        }
        diagLog("ssh", QStringLiteral("auth %1: the password given earlier this session "
                                      "is no longer accepted").arg(target));
        SshCredentialCache::forget(target); // stale; fall through and ask again
    }

    if (!prompter) {
        // Reached by an unattended retry with nothing usable cached. Retrying on a
        // timer would ask the same question forever; the caller surfaces this and waits
        // for the user to reconnect, which does have a prompter (§6.5).
        *failure = SshSession::Failure::NeedsPerson;
        *error = Tr::tr("%1 needs a password and there is no way to ask for one here.")
                     .arg(target);
        diagLog("ssh", QStringLiteral("auth %1: needs a password and this attempt is "
                                      "unattended").arg(target));
        return false;
    }

    // A password this machine's keychain is holding for this host (M14, §6.3.2).
    //
    // AFTER the agent and the key files above, so a host that signs in with a key never
    // causes a keychain read at all — which on KDE or macOS can mean an unlock dialog for
    // a credential that was not needed.
    //
    // AFTER the null-prompter bail just above, which is the whole threading rule: a
    // keychain read is non-interactive under CredRead but CAN raise a dialog on a locked
    // KWallet or for a macOS item whose ACL does not list loftail. A dialog on the fetcher
    // thread for a log opened hours ago is precisely what reconnect() forbids (§6.3,
    // §6.5), so the existing "is there anybody to ask" test guards this too.
    QString stored;
    if (secretStore()->read(sshSecretKey(target), &stored) == SecretStore::Result::Ok) {
        QByteArray raw = stored.toUtf8();
        const bool ok = tryPassword(raw);
        raw.fill('\0');
        if (ok) {
            SshCredentialCache::remember(target, stored);
            stored.fill(QChar(u'\0'));
            diagLog("ssh", QStringLiteral("auth %1: accepted by the stored password")
                               .arg(target));
            return true;
        }
        stored.fill(QChar(u'\0'));
        diagLog("ssh", QStringLiteral("auth %1: the stored password was rejected and has "
                                      "been forgotten").arg(target));
        // Erased, exactly as SshCredentialCache::forget() erases a stale cache entry, and
        // for a sharper reason than tidiness: sshd counts failed attempts against
        // MaxAuthTries (6 by default) and this chain already spends an agent identity,
        // several key files and up to three prompts. A stored password the server rejects
        // would burn one of those on every future connect, forever.
        forgetSshPassword(target);
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        QString password;
        bool remember = false;
        if (!prompter->askPassword(target, Tr::tr("Password:"), &password, &remember)) {
            *error = Tr::tr("Cancelled while authenticating to %1.").arg(target);
            return false;
        }
        QByteArray raw = password.toUtf8();
        const bool ok = tryPassword(raw);
        raw.fill('\0');
        if (ok) {
            diagLog("ssh", QStringLiteral("auth %1: accepted by a typed password "
                                          "(attempt %2)").arg(target).arg(attempt + 1));
            SshCredentialCache::remember(target, password);
            // Only now, with the server's yes in hand — and the prompter decides where it
            // goes, because it drew the checkbox that named the destination.
            prompter->passwordAccepted(target, password, remember);
            password.fill(QChar(u'\0'));
            return true;
        }
        password.fill(QChar(u'\0'));
    }

    diagLog("ssh", QStringLiteral("auth %1: all three typed passwords rejected")
                       .arg(target));
    *error = Tr::tr("Authentication to %1 failed.").arg(target);
    return false;
}

// --- SshSession ------------------------------------------------------------

SshSession::SshSession() : d(std::make_unique<Impl>())
{
    ensureLibraryInit();
}

SshSession::~SshSession() = default;

bool SshSession::isConnected() const
{
    // An exec session never has an SFTP handle, so "connected" has to mean the session
    // rather than the subsystem. Getting this wrong would make the reconnect loop treat
    // a perfectly healthy exec session as a dropped link, forever.
    return d->session != nullptr
        && (d->mode == Mode::Exec || d->sftp != nullptr);
}

bool SshSession::hasFile() const
{
    if (d->mode == Mode::Exec)
        return d->execFileOpen;
    return d->file != nullptr;
}

bool SshSession::fstatTracksHandle() const
{
    return d->fstatTracks;
}

void SshSession::setAbandonCheck(std::function<bool()> check)
{
    d->abandonCheck = std::move(check);
}

void SshSession::abort()
{
    // Everything this deliberately does not do is the point: no teardown, no free, no
    // touching the session. The thread inside libssh2 owns all of that and will do it
    // when its call returns — which is what this is for, and all it is for.
    std::scoped_lock lock(d->fdMutex);
    shutdownDetachedSocket(d->fd);
}

bool SshSession::connectTo(const RemoteLocation &location, SshPrompter *prompter, int timeoutMs,
                           QString *error, Failure *failure)
{
    QString scratch;
    QString &err = error ? *error : scratch;
    Failure ignored = Failure::None;
    Failure &kind = failure ? *failure : ignored;
    // Everything up to the host key is the host not answering, which fixes itself.
    kind = Failure::Unreachable;

    d->teardown();
    d->location = location;

    if (prompter)
        prompter->progress(Tr::tr("Connecting to %1…").arg(location.host));

    // In slices rather than one wait of `timeoutMs`, so that giving up on this connect
    // costs a quarter of a second rather than the whole timeout. A host that is simply
    // not answering is the dominant case of a slow open and the one that used to make
    // closing its tab take twenty seconds. QAbstractSocket keeps connecting through a
    // SocketTimeoutError, so looping is the documented way to do this.
    //
    // BEFORE the detach below, and that ordering is not negotiable: waitForConnected()
    // runs Qt's own machinery on this socket, which is exactly what must stop happening
    // once SSH bytes start moving (SocketDetach.h).
    d->socket.connectToHost(location.host, static_cast<quint16>(location.port));
    {
        QElapsedTimer elapsed;
        elapsed.start();
        while (!d->socket.waitForConnected(kConnectSliceMs)) {
            if (d->socket.error() != QAbstractSocket::SocketTimeoutError
                || elapsed.elapsed() >= timeoutMs) {
                err = Tr::tr("Cannot reach %1:%2 — %3")
                          .arg(location.host).arg(location.port).arg(d->socket.errorString());
                return false;
            }
            if (d->abandonCheck && d->abandonCheck()) {
                err = Tr::tr("Cancelled while connecting to %1.").arg(location.host);
                d->teardown();
                return false;
            }
        }
    }

    if (d->abandonCheck && d->abandonCheck()) {
        err = Tr::tr("Cancelled while connecting to %1.").arg(location.host);
        d->teardown();
        return false;
    }

    // Connected — now take the socket off Qt before a single SSH byte moves, because
    // from here on libssh2 must be its only reader (detachFromQt).
    {
        std::scoped_lock lock(d->fdMutex);
        d->fd = detachSocketFromQt(d->socket);
    }
    if (d->fd < 0) {
        err = Tr::tr("Cannot take over the connection to %1.").arg(location.host);
        d->teardown();
        return false;
    }

    d->session = libssh2_session_init();
    if (!d->session) {
        err = Tr::tr("Cannot start an SSH session.");
        d->teardown();
        return false;
    }
    libssh2_session_set_blocking(d->session, 1);
    // Bounds every subsequent call, so a wedged server cannot hang the caller
    // indefinitely — this runs on the thread that opened the document.
    libssh2_session_set_timeout(d->session, timeoutMs);

    if (libssh2_session_handshake(d->session, static_cast<libssh2_socket_t>(d->fd))) {
        err = sessionError(d->session, Tr::tr("SSH handshake with %1 failed")
                                           .arg(location.host));
        d->teardown();
        return false;
    }

    if (!d->verifyHostKey(prompter, &err, &kind)) {
        d->teardown();
        return false;
    }

    if (prompter)
        prompter->progress(Tr::tr("Authenticating to %1…").arg(location.host));
    if (!d->authenticate(prompter, &err, &kind)) {
        d->teardown();
        return false;
    }

    d->mode = Mode::Sftp;
    d->sftp = libssh2_sftp_init(d->session);
    if (!d->sftp) {
        // Signed in, and then no SFTP. There are two shapes of that and THE ERROR CODE
        // DOES NOT TELL THEM APART, which is worth spelling out because the obvious
        // reading of the code is wrong:
        //
        //  - the server answers the subsystem request with a refusal — sshd with no
        //    `Subsystem sftp` line, or an account confined to a shell that cannot start
        //    one. libssh2 reports a channel failure, promptly.
        //  - the server ACCEPTS the channel and then nothing on the far end ever speaks
        //    SFTP, because the `sftp-server` binary the config names is not installed.
        //    Common on embedded systems, where the sshd config is generic and the
        //    filesystem is not. libssh2 has nothing to report: it waits for the version
        //    packet that is never coming and gives up with a plain TIMEOUT, "Timed out
        //    waiting on socket", after a perfectly successful login.
        //
        // Both mean the same thing to us — this server will not do SFTP, and will not
        // start doing it in five minutes' time — so neither is classified from the code.
        // ASK INSTEAD: if a command runs on this session, the session is healthy and it
        // is SFTP that is missing, whatever the code said. Fall back to reading the log
        // with ordinary shell commands over a plain exec channel (§6.3.1).
        const int code = libssh2_session_last_errno(d->session);
        const bool timedOut = (code == LIBSSH2_ERROR_TIMEOUT
                               || code == LIBSSH2_ERROR_SOCKET_TIMEOUT);

        // A session that has just gone silent for the whole timeout gets a shorter leash
        // for the probe: it has already cost the user that wait once, connecting blocks
        // the thread that opened the document, and a box that will answer a one-line
        // command answers it in a round trip rather than in twenty seconds.
        if (timedOut)
            libssh2_session_set_timeout(d->session, qMax(2000, timeoutMs / 4));
        QByteArray probe;
        int exitCode = -1;
        const bool ran = d->runCommand(probeCommand(), &probe, &exitCode);
        libssh2_session_set_timeout(d->session, timeoutMs);

        // Read on the LAST non-empty line, for the reason parseStatOutput() is: a login
        // banner on stdout is common, and on the small images this fallback exists for it
        // is close to universal.
        const ExecTools tools = parseProbeOutput(probe);
        if (ran && tools.ok && tools.anySizeTool()) {
            // WHICH of the two shapes of "no SFTP" this was is not knowable from the
            // error code (see above), but the code and whether a command ran are both
            // worth recording: together they are what distinguishes a stripped-down image
            // from an sshd that refuses the subsystem outright.
            diagLog("ssh", QStringLiteral("%1: no SFTP (libssh2 code %2%3) — falling back "
                                          "to shell commands")
                               .arg(location.target())
                               .arg(code)
                               .arg(QString::fromLatin1(timedOut ? ", timed out" : "")));
            d->mode = Mode::Exec;
            d->execTools = tools;
            // No handle exists in this mode, so the inode substitute is unavailable and
            // SshFetcher's weaker mtime/head-compare rotation check is what applies.
            d->fstatTracks = false;
            kind = Failure::None;
            return true;
        }

        // Neither subsystem nor command got us a transport. `ran` is asked FIRST and the
        // error code second, for the same reason the probe outranked the code above: a
        // command that ran is direct evidence the session is alive, and it beats an
        // earlier timeout that only ever said something about the SFTP subsystem.
        if (ran && tools.ok) {
            // The shell is fine and it can READ the log — what it cannot do is MEASURE
            // it, and the transport needs both. Said separately from the case below
            // because the remedy is different and much smaller: any one of three
            // utilities is enough, and one of them is on practically every system.
            kind = Failure::Refused;
            err = Tr::tr(
                      "%1 does not offer SFTP, and its shell can read the log but "
                      "cannot measure it: none of `stat`, `ls` or `wc` is on the "
                      "account's PATH. Any one of the three is enough.")
                      .arg(location.host);
        } else if (ran) {
            // A command RAN and did not print the marker: there is a shell, it is the
            // two utilities that are missing. Worth saying separately — it is the usual
            // answer from a stripped-down embedded image, and it names something the
            // user can actually go and install.
            kind = Failure::Refused;
            err = Tr::tr(
                      "%1 does not offer SFTP, and its shell has no `tail` and `head`, "
                      "which is the only other way loftail can read a remote log. "
                      "sshd needs a `Subsystem sftp` line pointing at an sftp-server "
                      "that is actually installed, or the account needs those two "
                      "commands on its PATH.")
                      .arg(location.host);
        } else if (timedOut) {
            // Nothing ran and nothing replied: the link itself went quiet, which is the
            // one case here that comes back on its own and is therefore worth retrying.
            kind = Failure::Unreachable;
            err = Tr::tr("%1 signed in and then stopped answering: neither SFTP "
                                 "nor a shell command replied before the timeout.")
                      .arg(location.host);
        } else {
            kind = Failure::Refused;
            err = Tr::tr(
                      "%1 signed in but offers neither SFTP nor a shell that can run "
                      "`tail` and `head`, which are the two ways loftail can read a "
                      "remote log. sshd needs a `Subsystem sftp` line, or the account "
                      "needs to be able to run commands.")
                      .arg(location.host);
        }
        d->teardown();
        return false;
    }
    kind = Failure::None;
    return true;
}

SshSession::Mode SshSession::mode() const
{
    return d->mode;
}

SizeSource SshSession::sizeSource() const
{
    return d->execSize;
}

void SshSession::close()
{
    d->teardown();
}

bool SshSession::openFile(QString *error, Failure *failure)
{
    Failure ignored = Failure::None;
    Failure &kind = failure ? *failure : ignored;

    if (d->mode == Mode::Exec) {
        // Nothing to open: every read runs its own command. "Opening" therefore means
        // settling on a way to measure this file and confirming it answers — which is
        // the same question the SFTP branch answers by opening a handle, and it is
        // asked the way the poll will ask it, so a path that measures now measures later.
        //
        // The ladder runs FIRST and its success is the existence proof, rather than the
        // other way round: statPath() has nothing to dispatch on until a rung is
        // settled. That is not a compromise — "no rung answered" and "the file is not
        // there" are the same observation from out here, and they want the same answer.
        //
        // Settled on EVERY open, which means on every rotation too. Settling once per
        // session would let a rotated-to file the chosen rung cannot parse make
        // statPath() invalid for good, and the fetcher would then report the log as
        // waiting on every poll — indistinguishable from one that was deleted.
        closeFile();
        d->execSize = SizeSource::None;

        ExecSizeProbe probe = d->sizeProbe();
        const SizeSource source = probe.settle();
        if (source == SizeSource::None) {
            // A dead channel is not a missing file. Both are worth retrying, but only
            // one of them is fixed by reconnecting, and SshFetcher tells them apart by
            // this code alone.
            if (probe.channelDied()) {
                kind = Failure::Unreachable;
                if (error) {
                    *error = Tr::tr("Lost the connection to %1 while opening %2.")
                                 .arg(d->location.host, d->location.path);
                }
                return false;
            }
            // Indistinguishable from here: absent, or present and unreadable. Both are
            // things that change on their own, so both wait (§6.5) — the same answer
            // the SFTP branch gives for NO_SUCH_FILE and PERMISSION_DENIED.
            kind = Failure::NoSuchFile;
            if (error) {
                *error = Tr::tr("Cannot read %1 on %2 — it is missing, or the "
                                        "account cannot read it.")
                             .arg(d->location.path, d->location.host);
            }
            return false;
        }
        d->execSize = source;
        d->execFileOpen = true;
        d->fstatTracks = false; // no handle exists to compare against the path
        return true;
    }

    if (!d->sftp) {
        kind = Failure::Unreachable;
        if (error)
            *error = Tr::tr("Not connected.");
        return false;
    }
    closeFile();

    const QByteArray path = d->location.path.toUtf8();
    d->file = libssh2_sftp_open_ex(d->sftp, path.constData(),
                                   static_cast<unsigned int>(path.size()), LIBSSH2_FXF_READ, 0,
                                   LIBSSH2_SFTP_OPENFILE);
    if (!d->file) {
        const unsigned long sftpError = libssh2_sftp_last_error(d->sftp);
        // "Not there" and "not readable by me" are both things that change on their own
        // — a log gets written, a permission gets fixed — and both are what a LOCAL
        // path answers "unavailable" to, so they wait for the same reason (§6.5).
        // Anything else is the server saying something we did not ask about.
        kind = (sftpError == LIBSSH2_FX_NO_SUCH_FILE || sftpError == LIBSSH2_FX_NO_SUCH_PATH
                || sftpError == LIBSSH2_FX_PERMISSION_DENIED)
            ? Failure::NoSuchFile
            : Failure::Refused;
        if (error) {
            *error = Tr::tr("Cannot open %1 on %2 (%3)")
                         .arg(d->location.path, d->location.host)
                         .arg(sftpError);
        }
        return false;
    }

    // Probe once whether this server's FSTAT follows the handle. On OpenSSH it does,
    // which is what substitutes for an inode when detecting a rotation; a server that
    // re-resolves the name instead needs the weaker fallback in SshFetcher.
    const Attrs byName = statPath();
    const Attrs byHandle = statHandle();
    d->fstatTracks = byName.valid && byHandle.valid && byName.size == byHandle.size;
    return true;
}

// --- Whole-file operations at an arbitrary path (SPEC.md §4) ----------------

bool SshSession::readFileAt(const QString &path, QByteArray *out, bool *existed, QString *error)
{
    const auto fail = [error](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
    if (out)
        out->clear();
    if (existed)
        *existed = false;

    if (d->mode == Mode::Exec) {
        // Existence FIRST and on its own round trip: an empty file that is there and a
        // file that is not are the same empty stdout, and which of the two it is decides
        // whether the editor says "new file" and whether saving creates something.
        QByteArray probe;
        int code = 0;
        if (!d->runCommand(configExistsCommand(path), &probe, &code))
            return fail(Tr::tr("The server would not answer whether %1 is there.").arg(path));
        bool there = false;
        if (!parseConfigExistsOutput(probe, &there)) {
            return fail(Tr::tr("Could not tell whether %1 is there on %2.")
                            .arg(path, d->location.host));
        }
        if (existed)
            *existed = there;
        if (!there)
            return true; // not there is a SUCCESS: the editor opens empty on it

        QByteArray body;
        if (!d->runCommand(configReadCommand(path), &body, &code))
            return fail(Tr::tr("The server would not run the command to read %1.").arg(path));
        if (code != 0)
            return fail(Tr::tr("Cannot read %1 on %2.").arg(path, d->location.host));
        if (out)
            *out = body;
        return true;
    }

    if (!d->sftp)
        return fail(Tr::tr("Not connected."));

    const QByteArray raw = path.toUtf8();
    LIBSSH2_SFTP_HANDLE *handle =
        libssh2_sftp_open_ex(d->sftp, raw.constData(), static_cast<unsigned int>(raw.size()),
                             LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!handle) {
        const unsigned long code = libssh2_sftp_last_error(d->sftp);
        if (code == LIBSSH2_FX_NO_SUCH_FILE || code == LIBSSH2_FX_NO_SUCH_PATH)
            return true; // not there, and that is a supported answer
        // "There and shut" is a DIFFERENT sentence from "not there", and this is where
        // the two part company: a config whose mode is 000 must not be described as one
        // that has not been created yet, because saving would then overwrite it.
        if (code == LIBSSH2_FX_PERMISSION_DENIED) {
            return fail(Tr::tr("%1 on %2 is there but cannot be read.")
                            .arg(path, d->location.host));
        }
        return fail(Tr::tr("Cannot open %1 on %2 (%3)").arg(path, d->location.host).arg(code));
    }

    QByteArray body;
    char buffer[32768];
    forever {
        const ssize_t n = libssh2_sftp_read(handle, buffer, sizeof(buffer));
        if (n > 0) {
            body.append(buffer, int(n));
            continue;
        }
        if (n == 0)
            break; // end of file
        libssh2_sftp_close(handle);
        return fail(Tr::tr("The connection dropped while reading %1.").arg(path));
    }
    libssh2_sftp_close(handle);
    if (existed)
        *existed = true;
    if (out)
        *out = body;
    return true;
}

bool SshSession::writeFileAt(const QString &path, const QByteArray &bytes, QString *error)
{
    const auto fail = [error](const QString &text) {
        if (error)
            *error = text;
        return false;
    };

    if (d->mode == Mode::Exec) {
        // `cat > 'path'` truncates the existing inode rather than replacing it, so the
        // owner, the group and the mode all survive without anything here having to read
        // or restore them — see configWriteCommand().
        int code = 0;
        QString why;
        if (!d->runCommandWithInput(configWriteCommand(path), bytes, &code, &why))
            return fail(why);
        if (code != 0) {
            return fail(Tr::tr("%1 could not be written on %2 — the server refused it.")
                            .arg(path, d->location.host));
        }
        // VERIFY THE SIZE. This transport has no atomic replace, so a write that died
        // halfway leaves a short file; the whole point of checking is that the reader is
        // told rather than left with a truncated config that still looks saved.
        QByteArray probe;
        int sizeCode = 0;
        if (d->runCommand(wcSizeCommand(path), &probe, &sizeCode)) {
            const ExecAttrs attrs = parseWcSizeOutput(probe);
            if (attrs.ok && attrs.size != bytes.size()) {
                return fail(Tr::tr("%1 was written short on %2 — %3 bytes of %4 arrived.")
                                .arg(path, d->location.host)
                                .arg(attrs.size)
                                .arg(bytes.size()));
            }
        }
        return true;
    }

    if (!d->sftp)
        return fail(Tr::tr("Not connected."));

    const QByteArray raw = path.toUtf8();

    // What is there now, so the mode can be put back if the server applies the create
    // mode to an existing file. WRITE|TRUNC is not supposed to, but this is one round
    // trip against silently widening the permissions on somebody's configuration.
    LIBSSH2_SFTP_ATTRIBUTES before{};
    const bool existed = libssh2_sftp_stat_ex(d->sftp, raw.constData(),
                                              static_cast<unsigned int>(raw.size()),
                                              LIBSSH2_SFTP_STAT, &before)
        == 0;

    // TRUNC, NOT a temp-and-rename: the existing inode is kept, and with it the owner
    // and group a rename could not preserve. 0644 is the mode for a file being CREATED
    // and is ignored when one is already there.
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open_ex(
        d->sftp, raw.constData(), static_cast<unsigned int>(raw.size()),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, 0644, LIBSSH2_SFTP_OPENFILE);
    if (!handle) {
        const unsigned long code = libssh2_sftp_last_error(d->sftp);
        if (code == LIBSSH2_FX_PERMISSION_DENIED)
            return fail(Tr::tr("%1 on %2 cannot be written.").arg(path, d->location.host));
        if (code == LIBSSH2_FX_NO_SUCH_PATH || code == LIBSSH2_FX_NO_SUCH_FILE) {
            // The FILE was allowed not to exist; a missing DIRECTORY is a different
            // thing and is named rather than created, exactly as the local path does.
            return fail(Tr::tr("The directory for %1 does not exist on %2, so it cannot "
                               "be saved. Create it first, or correct the path in "
                               "File ▸ Preferences.")
                            .arg(path, d->location.host));
        }
        return fail(Tr::tr("Cannot write %1 on %2 (%3)").arg(path, d->location.host).arg(code));
    }

    qint64 written = 0;
    while (written < bytes.size()) {
        const ssize_t n = libssh2_sftp_write(handle, bytes.constData() + written,
                                             size_t(bytes.size() - written));
        if (n < 0) {
            libssh2_sftp_close(handle);
            return fail(Tr::tr("The connection dropped while writing %1 — it may now be "
                               "incomplete.")
                            .arg(path));
        }
        if (n == 0)
            break;
        written += qint64(n);
    }
    libssh2_sftp_close(handle);

    if (written != bytes.size()) {
        return fail(Tr::tr("%1 was written short on %2 — %3 bytes of %4 arrived.")
                        .arg(path, d->location.host)
                        .arg(written)
                        .arg(bytes.size()));
    }

    if (existed && (before.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)) {
        LIBSSH2_SFTP_ATTRIBUTES restore{};
        restore.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
        restore.permissions = before.permissions;
        // Not reported on failure: the file IS saved, and a server that refuses SETSTAT
        // is usually one that never applied the create mode either. Widening would be
        // worth shouting about; being unable to re-set what is already right is not.
        libssh2_sftp_setstat(d->sftp, raw.constData(), &restore);
    }
    return true;
}

void SshSession::closeFile()
{
    d->execFileOpen = false;
    if (d->file) {
        libssh2_sftp_close(d->file);
        d->file = nullptr;
    }
}

SshSession::Attrs SshSession::statPath() const
{
    Attrs out;
    if (d->mode == Mode::Exec) {
        // Whichever rung openFile() settled on. No re-validation here: that question was
        // answered once, and asking it again would double the cost of every poll.
        if (d->execSize == SizeSource::None)
            return out;
        ExecSizeProbe probe = d->sizeProbe();
        const ExecAttrs parsed = probe.query(d->execSize);
        out.valid = parsed.ok;
        out.size = parsed.size;
        out.mtime = parsed.mtime;
        return out;
    }
    if (!d->sftp)
        return out;
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    const QByteArray path = d->location.path.toUtf8();
    if (libssh2_sftp_stat_ex(d->sftp, path.constData(), static_cast<unsigned int>(path.size()),
                             LIBSSH2_SFTP_STAT, &attrs)
        != 0) {
        return out;
    }
    out.valid = true;
    out.size = static_cast<qint64>(attrs.filesize);
    out.mtime = static_cast<qint64>(attrs.mtime);
    return out;
}

SshSession::Attrs SshSession::statHandle() const
{
    Attrs out;
    // Mode::Exec has no handle to stat, and must not pretend otherwise: reporting the
    // path's attributes here would make the two agree by construction and silently
    // defeat the rotation check that compares them.
    if (d->mode == Mode::Exec || !d->file)
        return out;
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    if (libssh2_sftp_fstat_ex(d->file, &attrs, 0) != 0)
        return out;
    out.valid = true;
    out.size = static_cast<qint64>(attrs.filesize);
    out.mtime = static_cast<qint64>(attrs.mtime);
    return out;
}

bool SshSession::runScript(
    const QString &command,
    const std::function<void(const QByteArray &bytes, bool isStdErr)> &onChunk,
    int *exitCode, QString *error)
{
    // NO MODE BRANCH, alone among the operations on this class. An exec channel needs the
    // session and nothing else, so a server that does SFTP perfectly well runs a restart
    // script exactly the way one that refused it does — the fallback and this are two uses
    // of the same facility rather than two paths through one.
    return d->runScriptStreaming(command, onChunk, exitCode, error);
}

qint64 SshSession::readAt(qint64 offset, char *buffer, qint64 length, QString *error)
{
    if (length <= 0)
        return 0;
    if (d->mode == Mode::Exec)
        return d->execFileOpen ? d->execRead(offset, buffer, length, error) : 0;
    if (!d->file)
        return 0;

    libssh2_sftp_seek64(d->file, static_cast<libssh2_uint64_t>(offset));

    qint64 total = 0;
    while (total < length) {
        const ssize_t n = libssh2_sftp_read(d->file, buffer + total,
                                            static_cast<size_t>(length - total));
        if (n == 0)
            break; // EOF: the remote file has no more bytes right now
        if (n < 0) {
            if (error) {
                *error = sessionError(d->session, Tr::tr("Reading %1 from %2 failed")
                                                      .arg(d->location.path, d->location.host));
            }
            return total > 0 ? total : -1;
        }
        total += n;
    }
    return total;
}

} // namespace loftail
