#pragma once

#include <QString>

namespace loftail {

// What loftail must ask a person before it can read a remote log, expressed without
// any reference to widgets so that loftail_core keeps linking QtCore only
// (CLAUDE.md conventions). The UI supplies the implementation; tests supply a
// scripted one; a headless run supplies none, and every question then answers itself
// in the safe direction — refuse.
class SshPrompter
{
public:
    virtual ~SshPrompter() = default;

    enum class HostKeyChoice {
        Reject,             // do not connect
        AcceptOnce,         // connect, remember nothing
        AcceptAndRemember,  // connect and append to ~/.ssh/known_hosts
    };

    struct HostKeyInfo
    {
        QString host;
        int     port = 22;
        QString keyType;            // "ssh-ed25519", "ssh-rsa", …
        QString fingerprintSha256;  // OpenSSH spelling: "SHA256:<base64, no padding>"

        // True when a DIFFERENT key is already recorded for this host. This is the
        // man-in-the-middle case, and the only honest answer is to refuse: an
        // implementation must not offer to accept, and the session will not proceed
        // even if it does.
        bool mismatch = false;
    };

    // Unknown (or changed) host key. Called before any credential is sent, which is
    // the whole point — a password must never reach an unverified server.
    virtual HostKeyChoice confirmHostKey(const HostKeyInfo &info) = 0;

    // Ask for a password. `promptText` is the server's own wording where it supplied
    // one (keyboard-interactive), otherwise a generic prompt. Returns false if the
    // user cancelled. `*remember` asks to store it — see HostBookmarkStore for what
    // that costs; an implementation MUST make that cost visible before ticking it.
    virtual bool askPassword(const QString &target, const QString &promptText,
                             QString *password, bool *remember) = 0;

    // The password the server has just ACCEPTED for `target`, with `remember` exactly as
    // this prompter answered it from askPassword(). Called once, and only after the server
    // said yes, so a rejected password is never written anywhere.
    //
    // The decision of WHERE it goes belongs here rather than in SshSession, because this
    // object drew the checkbox and wrote the label naming the destination — it is the only
    // one that knows what the user actually consented to. See SecretStore.h for the two
    // destinations and the rule that keeps them from being swapped.
    //
    // Non-pure with a no-op default, the shape LogSource::wasReplaced() and isComplete()
    // both took: a scripted prompter that stores nothing should not have to say so.
    virtual void passwordAccepted(const QString &target, const QString &password,
                                  bool remember)
    {
        Q_UNUSED(target);
        Q_UNUSED(password);
        Q_UNUSED(remember);
    }

    // Progress narration for a connect that is taking a while ("Connecting to …",
    // "Authenticating…"). Advisory; an implementation may ignore it.
    virtual void progress(const QString &message) = 0;
};

// The prompter used for connects from here on. Not owned; must outlive every open.
// Null means "never prompt": an unknown host key is refused and a password-only host
// fails with an explanation, rather than blocking a headless or scripted run forever.
void setSshPrompter(SshPrompter *prompter);
SshPrompter *sshPrompter();

// Credentials accepted for a target ("user@host:port"), for this process only.
//
// This is what makes one host cost ONE password prompt no matter how many of its
// files are open — including at session restore, which reopens everything at once.
// It deliberately caches per target rather than sharing a connection: a LIBSSH2_SESSION
// is not thread-safe, and each fetcher runs its own thread, so a shared session would
// need a mutex around every read and would serialize them for no user-visible gain.
namespace SshCredentialCache {
bool has(const QString &target);
QString password(const QString &target);
void remember(const QString &target, const QString &password);
void forget(const QString &target);
void clear();
} // namespace SshCredentialCache

} // namespace loftail
