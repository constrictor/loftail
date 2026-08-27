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

#include <QString>

#include <functional>

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
//
// Read and written from every fetcher thread since M17, hence the lock inside.
namespace SshCredentialCache {
bool has(const QString &target);
QString password(const QString &target);
void remember(const QString &target, const QString &password);
void forget(const QString &target);
void clear();
} // namespace SshCredentialCache

// One connect at a time to any given target, held for as long as this object lives
// (ARCHITECTURE.md §6.3.3).
//
// WHAT IT PROTECTS is the promise above it: one host costs one prompt. That used to be
// free, because every connect ran on the GUI thread and they were therefore serialised
// by construction. Once each fetcher connects on its own thread, restoring five files
// from one host means five workers reaching authentication at the same instant, all
// missing the still-empty credential cache, and all asking — five stacked dialogs for
// one host, and five of sshd's six default MaxAuthTries spent to produce them.
//
// So the first fetcher for a host connects and prompts, and the rest wait here; when it
// releases, they find the password in SshCredentialCache and sign in without asking.
// DIFFERENT hosts still connect in parallel, which is where the time is actually saved.
//
// Coalescing the prompts themselves would be the wrong fix: keyboard-interactive is a
// conversation whose wording differs per prompt, so one host's answer is not in general
// another's question.
//
// Never taken on the application thread, and never while holding a fetcher's own mutex.
class SshConnectHold
{
public:
    // Blocks until this target's slot is free, `abandon` returns true, or the process is
    // shutting down. Check held() before relying on it.
    SshConnectHold(QString target, const std::function<bool()> &abandon);
    ~SshConnectHold();

    SshConnectHold(const SshConnectHold &) = delete;
    SshConnectHold &operator=(const SshConnectHold &) = delete;

    // False when the wait was abandoned. The caller should give up rather than connect:
    // it was asked to stop.
    bool held() const { return m_held; }

private:
    QString m_target;
    bool    m_held = false;
};

} // namespace loftail
