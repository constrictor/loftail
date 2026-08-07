#include "PromptRelay.h"

#include "GuiCallGate.h"

namespace loftail {

SshPrompter::HostKeyChoice PromptRelay::confirmHostKey(const HostKeyInfo &info)
{
    if (!m_target)
        return HostKeyChoice::Reject;

    HostKeyChoice choice = HostKeyChoice::Reject;
    SshPrompter *target = m_target;
    // Reject stands if the question never gets asked, which is the same answer a null
    // prompter gives: accepting a host key is the one decision that has to be a
    // person's, and nobody answered.
    guiCallGate().call([target, &info, &choice]() { choice = target->confirmHostKey(info); });
    return choice;
}

bool PromptRelay::askPassword(const QString &target, const QString &promptText,
                              QString *password, bool *remember)
{
    if (!m_target)
        return false;

    bool answered = false;
    SshPrompter *prompter = m_target;
    const bool asked = guiCallGate().call([&]() {
        answered = prompter->askPassword(target, promptText, password, remember);
    });
    return asked && answered;
}

void PromptRelay::passwordAccepted(const QString &target, const QString &password,
                                   bool remember)
{
    if (!m_target)
        return;
    // Storing a password can raise a keychain unlock dialog, so this goes across too —
    // and it is worth stating why it is not merely bookkeeping: this is the call that
    // acts on the consent the checkbox collected, and the object that drew that checkbox
    // is the one that has to decide where the secret lands (SshPrompter.h).
    SshPrompter *prompter = m_target;
    guiCallGate().call(
        [prompter, &target, &password, remember]() {
            prompter->passwordAccepted(target, password, remember);
        });
}

void PromptRelay::progress(const QString &message)
{
    // Advisory: a few words between the steps of a connect. It crosses like everything
    // else — one event-loop turn, three times per connect, against a recorder that only
    // stores a string — which is cheap enough not to be worth a second mechanism, and
    // waiting is what keeps `message` alive for the far side to read.
    if (!m_target || guiCallGate().cancelled())
        return;
    SshPrompter *prompter = m_target;
    guiCallGate().call([prompter, &message]() { prompter->progress(message); });
}

} // namespace loftail
