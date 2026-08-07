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
// Holds the target prompter but does not own it; both may outlive each other only
// because setSshPrompter() cancels the gate on the way out.
class PromptRelay final : public SshPrompter
{
public:
    explicit PromptRelay(SshPrompter *target) : m_target(target) {}

    SshPrompter *target() const { return m_target; }

    HostKeyChoice confirmHostKey(const HostKeyInfo &info) override;
    bool askPassword(const QString &target, const QString &promptText,
                     QString *password, bool *remember) override;
    void passwordAccepted(const QString &target, const QString &password,
                          bool remember) override;
    void progress(const QString &message) override;

private:
    SshPrompter *m_target;
};

} // namespace loftail
