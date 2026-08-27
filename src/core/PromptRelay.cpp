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

#include "PromptRelay.h"

#include "GuiCallGate.h"

namespace loftail {

SshPrompter::HostKeyChoice PromptRelay::confirmHostKey(const HostKeyInfo &info)
{
    HostKeyChoice choice = HostKeyChoice::Reject;
    // Reject stands if the question never gets asked, which is the same answer a null
    // prompter gives: accepting a host key is the one decision that has to be a
    // person's, and nobody answered.
    guiCallGate().call([&info, &choice]() {
        if (SshPrompter *target = sshPrompter())
            choice = target->confirmHostKey(info);
    });
    return choice;
}

bool PromptRelay::askPassword(const QString &target, const QString &promptText,
                              QString *password, bool *remember)
{
    bool answered = false;
    const bool asked = guiCallGate().call([&]() {
        if (SshPrompter *prompter = sshPrompter())
            answered = prompter->askPassword(target, promptText, password, remember);
    });
    return asked && answered;
}

void PromptRelay::passwordAccepted(const QString &target, const QString &password,
                                   bool remember)
{
    // Storing a password can raise a keychain unlock dialog, so this goes across too —
    // and it is worth stating why it is not merely bookkeeping: this is the call that
    // acts on the consent the checkbox collected, and the object that drew that checkbox
    // is the one that has to decide where the secret lands (SshPrompter.h).
    guiCallGate().call([&target, &password, remember]() {
        if (SshPrompter *prompter = sshPrompter())
            prompter->passwordAccepted(target, password, remember);
    });
}

void PromptRelay::progress(const QString &message)
{
    // Advisory: a few words between the steps of a connect. It crosses like everything
    // else — one event-loop turn, three times per connect, against a recorder that only
    // stores a string — which is cheap enough not to be worth a second mechanism, and
    // waiting is what keeps `message` alive for the far side to read.
    if (guiCallGate().cancelled())
        return;
    guiCallGate().call([&message]() {
        if (SshPrompter *prompter = sshPrompter())
            prompter->progress(message);
    });
}

} // namespace loftail
