#include "SshSession.h"

#include "SshPrompter.h"
#include "SocketDetach.h"
#include "SshExecCommands.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QStandardPaths>
#include <QTcpSocket>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <cstring>
#include <mutex>

namespace loftail {

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
        return QString();
    return home + QStringLiteral("/.ssh/known_hosts");
}

// The OpenSSH spelling of a fingerprint: "SHA256:" + unpadded base64. Shown to the
// user so it can be compared against what `ssh-keygen -lf` prints on the server.
QString sha256Fingerprint(LIBSSH2_SESSION *session)
{
    const char *hash = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!hash)
        return QString();
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

struct SshSession::Impl
{
    QTcpSocket        socket;   // resolves, connects, times out — then hands the fd over
    qintptr           fd = -1;  // OURS, not Qt's; see SocketDetach.h
    LIBSSH2_SESSION  *session = nullptr;
    LIBSSH2_SFTP     *sftp = nullptr;
    LIBSSH2_SFTP_HANDLE *file = nullptr;
    RemoteLocation    location;
    bool              fstatTracks = false;
    SshSession::Mode  mode = SshSession::Mode::Sftp;
    bool              execFileOpen = false; // Mode::Exec has no handle, only this flag

    ~Impl() { teardown(); }

    // Run `command` on the server and collect its stdout. Blocking, bounded by the
    // session timeout like everything else here. Returns false when the channel could
    // not be opened or the command could not be started; a command that RAN and failed
    // returns true with whatever it printed, because "stat said nothing" and "stat
    // could not be launched" want different handling upstream.
    bool runCommand(const QString &command, QByteArray *stdOut, int *exitCode);

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
        closeDetachedSocket(fd);
        fd = -1;
        if (socket.state() != QAbstractSocket::UnconnectedState)
            socket.disconnectFromHost();
    }

    // Host-key verification, before a single credential is sent.
    bool verifyHostKey(SshPrompter *prompter, QString *error, SshSession::Failure *failure);
    // Read `length` bytes at `offset` by running a command. Mode::Exec's readAt().
    qint64 execRead(qint64 offset, char *buffer, qint64 length, QString *error);
    bool authenticate(SshPrompter *prompter, QString *error, SshSession::Failure *failure);
    bool tryAgent();
    bool tryDefaultKeys();
    bool tryPassword(const QByteArray &password);
};

bool SshSession::Impl::runCommand(const QString &command, QByteArray *stdOut, int *exitCode)
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

qint64 SshSession::Impl::execRead(qint64 offset, char *buffer, qint64 length, QString *error)
{
    QByteArray out;
    int exitCode = 0;
    if (!runCommand(readCommand(location.path, offset, length), &out, &exitCode)) {
        if (error) {
            *error = QStringLiteral("Reading %1 from %2 failed: the server would not run a "
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
        *error = QStringLiteral("%1 offered no host key.").arg(location.host);
        return false;
    }

    LIBSSH2_KNOWNHOSTS *hosts = libssh2_knownhost_init(session);
    if (!hosts) {
        *error = QStringLiteral("Cannot read known hosts.");
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
        return true;
    }

    SshPrompter::HostKeyInfo info;
    info.host = location.host;
    info.port = location.port;
    info.keyType = keyTypeName(keyType);
    info.fingerprintSha256 = sha256Fingerprint(session);
    info.mismatch = (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH);

    if (info.mismatch) {
        // A DIFFERENT key is on record for this host. That is the man-in-the-middle
        // signature, and there is no "accept anyway" here — offering one would be the
        // single most dangerous convenience in the application. Tell the prompter so
        // it can explain, but refuse regardless of what it answers.
        if (prompter)
            prompter->confirmHostKey(info);
        libssh2_knownhost_free(hosts);
        *error = QStringLiteral(
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
        *error = QStringLiteral(
            "%1 is not in ~/.ssh/known_hosts and there is no way to ask about it here. "
            "Connect once with ssh to record its key, then reopen.")
                     .arg(location.host);
        return false;
    }

    const SshPrompter::HostKeyChoice choice = prompter->confirmHostKey(info);
    if (choice == SshPrompter::HostKeyChoice::Reject) {
        libssh2_knownhost_free(hosts);
        *error = QStringLiteral("Host key for %1 was not accepted.").arg(location.host);
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

bool SshSession::Impl::tryAgent()
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

bool SshSession::Impl::tryDefaultKeys()
{
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (home.isEmpty())
        return false;

    // The same files, in the same order, that OpenSSH would try by default. No
    // passphrase is supplied: a passphrase-protected key belongs in the agent, and
    // prompting for one here would be a second, differently-shaped password dialog.
    static const char *const names[] = {"id_ed25519", "id_ecdsa", "id_rsa"};
    const QByteArray user = location.user.toUtf8();
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

bool SshSession::Impl::tryPassword(const QByteArray &password)
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

    if (available.contains(QLatin1String("publickey"))) {
        if (prompter)
            prompter->progress(QStringLiteral("Trying SSH agent for %1…").arg(target));
        if (tryAgent() || tryDefaultKeys())
            return true;
    }

    const bool passwordOffered = available.contains(QLatin1String("password"))
        || available.contains(QLatin1String("keyboard-interactive"));
    if (!passwordOffered) {
        *error = QStringLiteral(
            "Could not authenticate to %1 with an SSH agent or key, and the server "
            "offers no password method (it allows: %2).")
                     .arg(target, available.isEmpty() ? QStringLiteral("nothing") : available);
        return false;
    }

    // A password already accepted for this host this session — the reason opening a
    // second file on one host, or restoring a whole session, asks only once.
    if (SshCredentialCache::has(target)) {
        QByteArray cached = SshCredentialCache::password(target).toUtf8();
        const bool ok = tryPassword(cached);
        cached.fill('\0');
        if (ok)
            return true;
        SshCredentialCache::forget(target); // stale; fall through and ask again
    }

    if (!prompter) {
        // Reached by an unattended retry with nothing usable cached. Retrying on a
        // timer would ask the same question forever; the caller surfaces this and waits
        // for the user to reconnect, which does have a prompter (§6.5).
        *failure = SshSession::Failure::NeedsPerson;
        *error = QStringLiteral("%1 needs a password and there is no way to ask for one here.")
                     .arg(target);
        return false;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        QString password;
        bool remember = false;
        if (!prompter->askPassword(target, QStringLiteral("Password:"), &password, &remember)) {
            *error = QStringLiteral("Cancelled while authenticating to %1.").arg(target);
            return false;
        }
        QByteArray raw = password.toUtf8();
        const bool ok = tryPassword(raw);
        raw.fill('\0');
        if (ok) {
            SshCredentialCache::remember(target, password);
            password.fill(QChar(u'\0'));
            return true;
        }
        password.fill(QChar(u'\0'));
    }

    *error = QStringLiteral("Authentication to %1 failed.").arg(target);
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
        prompter->progress(QStringLiteral("Connecting to %1…").arg(location.host));

    d->socket.connectToHost(location.host, static_cast<quint16>(location.port));
    if (!d->socket.waitForConnected(timeoutMs)) {
        err = QStringLiteral("Cannot reach %1:%2 — %3")
                  .arg(location.host).arg(location.port).arg(d->socket.errorString());
        return false;
    }

    // Connected — now take the socket off Qt before a single SSH byte moves, because
    // from here on libssh2 must be its only reader (detachFromQt).
    d->fd = detachSocketFromQt(d->socket);
    if (d->fd < 0) {
        err = QStringLiteral("Cannot take over the connection to %1.").arg(location.host);
        d->teardown();
        return false;
    }

    d->session = libssh2_session_init();
    if (!d->session) {
        err = QStringLiteral("Cannot start an SSH session.");
        d->teardown();
        return false;
    }
    libssh2_session_set_blocking(d->session, 1);
    // Bounds every subsequent call, so a wedged server cannot hang the caller
    // indefinitely — this runs on the thread that opened the document.
    libssh2_session_set_timeout(d->session, timeoutMs);

    if (libssh2_session_handshake(d->session, static_cast<libssh2_socket_t>(d->fd))) {
        err = sessionError(d->session, QStringLiteral("SSH handshake with %1 failed")
                                           .arg(location.host));
        d->teardown();
        return false;
    }

    if (!d->verifyHostKey(prompter, &err, &kind)) {
        d->teardown();
        return false;
    }

    if (prompter)
        prompter->progress(QStringLiteral("Authenticating to %1…").arg(location.host));
    if (!d->authenticate(prompter, &err, &kind)) {
        d->teardown();
        return false;
    }

    d->mode = Mode::Sftp;
    d->sftp = libssh2_sftp_init(d->session);
    if (!d->sftp) {
        // Signed in, and then no SFTP. A TIMEOUT is transient — a loaded server, a slow
        // subsystem launch — and is worth retrying as it stands, so it is not a reason
        // to change transport.
        const int code = libssh2_session_last_errno(d->session);
        if (code == LIBSSH2_ERROR_TIMEOUT || code == LIBSSH2_ERROR_SOCKET_TIMEOUT) {
            kind = Failure::Unreachable;
            err = sessionError(d->session,
                               QStringLiteral("Cannot start SFTP on %1").arg(location.host));
            d->teardown();
            return false;
        }

        // Anything else means this server will not do SFTP — sshd with no `Subsystem
        // sftp` line, or an account confined to a shell that cannot start one. That
        // never becomes true later, so rather than retry it, fall back to reading the
        // log with `stat` and `tail` over a plain exec channel (§6.3.1).
        QByteArray probe;
        int exitCode = -1;
        const bool ran = d->runCommand(probeCommand(), &probe, &exitCode);
        if (ran && probe.trimmed() == probeMarker()) {
            d->mode = Mode::Exec;
            // No handle exists in this mode, so the inode substitute is unavailable and
            // SshFetcher's weaker mtime/head-compare rotation check is what applies.
            d->fstatTracks = false;
            kind = Failure::None;
            return true;
        }

        kind = Failure::Refused;
        err = QStringLiteral(
                  "%1 signed in but offers neither SFTP nor a shell that can run `stat` "
                  "and `tail`, which are the two ways loftail can read a remote log. "
                  "sshd needs a `Subsystem sftp` line, or the account needs to be able "
                  "to run commands.")
                  .arg(location.host);
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
        // confirming the log is there and readable, which is the same question the
        // SFTP branch answers by opening a handle — and it is asked the same way the
        // poll will ask it, so a path that stats now will stat later.
        closeFile();
        const Attrs attrs = statPath();
        if (!attrs.valid) {
            // Indistinguishable from here: absent, or present and unreadable. Both are
            // things that change on their own, so both wait (§6.5) — the same answer
            // the SFTP branch gives for NO_SUCH_FILE and PERMISSION_DENIED.
            kind = Failure::NoSuchFile;
            if (error) {
                *error = QStringLiteral("Cannot read %1 on %2 — it is missing, or the "
                                        "account cannot read it.")
                             .arg(d->location.path, d->location.host);
            }
            return false;
        }
        d->execFileOpen = true;
        d->fstatTracks = false; // no handle exists to compare against the path
        return true;
    }

    if (!d->sftp) {
        kind = Failure::Unreachable;
        if (error)
            *error = QStringLiteral("Not connected.");
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
            *error = QStringLiteral("Cannot open %1 on %2 (%3)")
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
        QByteArray printed;
        int exitCode = 0;
        if (!d->runCommand(statCommand(d->location.path), &printed, &exitCode))
            return out;
        const ExecAttrs parsed = parseStatOutput(printed);
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
                *error = sessionError(d->session, QStringLiteral("Reading %1 from %2 failed")
                                                      .arg(d->location.path, d->location.host));
            }
            return total > 0 ? total : -1;
        }
        total += n;
    }
    return total;
}

} // namespace loftail
