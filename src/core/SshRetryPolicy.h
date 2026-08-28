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

#include <QtGlobal>

namespace loftail {

// How long an UNATTENDED reconnect keeps trying after the far end suddenly wants a
// person (SPEC.md §3, ARCHITECTURE.md §6.5).
//
// SshFetcher::reconnect() runs with no prompter, so anything needing an answer from
// somebody comes back as SshSession::Failure::NeedsPerson — a password with nobody to
// ask, or a host key that is not in known_hosts. The loop used to latch on that and stop
// reconnecting for the life of the tab, on the reasoning that the next hundred attempts
// get the same answer. That is true of a host that simply wants a password loftail does
// not have. It is NOT true of the case Failure::Unreachable was written for and names in
// its own comment — "a machine reboots" — because a reboot does not go straight from
// unreachable to signed in:
//
//   sshd answers before /home is mounted, so authorized_keys is not readable yet;
//   sshd answers before PAM is up;
//   an embedded box regenerates its host key, so it is briefly not in known_hosts.
//
// Every one of those is a NeedsPerson that fixes itself seconds later, and every one of
// them permanently killed the tab: loftail never tried again, and the user's next launch
// was the first thing that would connect — which is where the password prompt they had
// never been asked for before came from.
//
// So a fetcher that HAS signed in to this host keeps trying through a grace window, and
// only then gives up and waits to be asked. Two properties make that safe rather than
// merely persistent:
//
//   * it is bounded, so a server that has genuinely changed its mind still ends up in
//     the "loftail has stopped trying" state the diagnostic log calls out, rather than
//     connecting every five seconds forever;
//   * a retry inside the window is unattended by construction — reconnect() consumes
//     m_wantsPrompter before it gets here — so it can never put a dialog on screen for
//     a log somebody opened hours ago, which is the rule the latch was protecting.
//
// It is deliberately gated on having signed in ONCE. A tab that has never connected has
// no evidence its credentials work at all, so "needs a person" there is a standing state
// and latching immediately is right: that is the session-restore-onto-an-unknown-host
// case, and it must keep costing one attempt rather than a window of them.
//
// No QObject, no timer, and the clock is a parameter, exactly as AlertPolicy does it —
// so the whole decision is testable without a QApplication, without a server and without
// waiting (tst_sshretry). Always compiled, like SshExecCommands and ExecSizeProbe beside
// it, because a decision reachable in one build configuration is a decision tested in
// one build configuration.
class ReconnectGrace
{
public:
    // Five minutes. Longer than any boot this is meant to survive, and short enough that
    // a host which really does want a password is left alone rather than probed for the
    // rest of the day.
    static constexpr qint64 kDefaultGraceMs = 300000;

    explicit ReconnectGrace(qint64 graceMs = kDefaultGraceMs) : m_graceMs(graceMs) {}

    // A connect that got all the way in. Clears the window, so the NEXT outage on this
    // fetcher gets a fresh one — which is what makes a log that has been up for a week
    // survive its second reboot as well as its first.
    void signedIn();

    // An unattended attempt that could not authenticate, at `nowMs` on any monotonic
    // millisecond clock. True while loftail should keep trying; false once it should
    // stop and wait for File ▸ Reconnect.
    bool keepTrying(qint64 nowMs);

    // Whether this fetcher has ever authenticated. The evidence the window is granted on.
    bool signedInOnce() const { return m_signedInOnce; }

private:
    qint64 m_graceMs;
    bool   m_signedInOnce = false;
    bool   m_waiting = false;   // a window is open
    qint64 m_firstFailureMs = 0;
};

} // namespace loftail
