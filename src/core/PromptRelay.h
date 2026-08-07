#pragma once

#include "SshPrompter.h"

namespace loftail {

// An SshPrompter that asks its questions on the application thread, whoever is asking
// (ARCHITECTURE.md §6.3.3).
//
// A connect runs on a fetcher's own thread, and a prompt is a modal dialog. This carries
// each question across through GuiCallGate and brings the answer back, so that
// SshSession's authentication ladder stays exactly as it was — including
// keyboard-interactive, which is a conversation libssh2 drives through a callback and
// which therefore CANNOT be turned into fail-ask-retry.
//
// The gate's cancel() is what makes the question refusable: a cancelled or unanswerable
// question is answered the safe way, which for a host key is Reject and for a password
// is "the user said no". That is the same answer a null prompter gives, and deliberately
// so — "there is nobody to ask" and "nobody is going to answer" are the same situation.
//
// HOLDS NO PROMPTER. It resolves sshPrompter() inside each marshalled call, on the
// application thread — the only thread that ever writes it — and refuses if there is
// none by then.
//
// Holding one was a dangling pointer waiting to happen, and it happened: a fetcher's
// relay outlives the window whose prompter it captured, because a fetcher is retired
// rather than joined and its thread may still be mid-connect when the window goes. The
// gate's cancel covers the questions in flight at that moment, but a *later* window
// reopens the gate, and the stale relay would then call a destroyed prompter. Resolving
// late means the answer is always the current prompter or nobody, and "nobody" is a case
// every caller already handles.
class PromptRelay final : public SshPrompter
{
public:
    HostKeyChoice confirmHostKey(const HostKeyInfo &info) override;
    bool askPassword(const QString &target, const QString &promptText,
                     QString *password, bool *remember) override;
    void passwordAccepted(const QString &target, const QString &password,
                          bool remember) override;
    void progress(const QString &message) override;
};

} // namespace loftail
