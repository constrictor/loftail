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

#include "SerialProfile.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QRegularExpression>
#include <QString>
#include <QtGlobal>

namespace loftail {

// Where the conversation with a device's serial console has got to (SPEC.md §3,
// ARCHITECTURE.md §6.11).
//
// A tty is not an authenticated channel: before loftail can run `tail`, somebody has to
// log in, and the only way to know whether that has happened is to read what the device
// prints and recognise it. This is that recognition, as a pure decision — no QSerialPort,
// no QObject, no I/O, and the clock is a parameter — so the whole of it is testable
// without a device, without a port and without waiting (tst_serialloginmachine).
//
// ALWAYS COMPILED, for the reason SshExecCommands, ExecSizeProbe, SshRetryPolicy,
// PathTrouble and SshSessionHealth are: this is the only judgement the serial transport
// makes on its own, and a decision reachable in one build configuration is a decision
// tested in one build configuration. That is the sixth time that argument is made in
// src/core/CMakeLists.txt.
enum class LoginStep {
    Idle,          // nothing sent yet
    Probing,       // nudged the line; deciding WHAT is on the other end
    ConfirmShell,  // asked for the marker; a shell is there if it comes back
    SentUser,      // the user name has gone; expecting a password prompt or a shell
    SentPassword,  // the password has gone; expecting a shell
    RunStartup,    // working through the preset's startup commands, one at a time
    Ready,         // logged in, console quietened: the transport may run commands
    Failed,        // gave up; `reason` says why
};

// The untranslated name of a step, for the diagnostic log. Deliberately not routed
// through waitingText(): this is the token somebody greps a bug report for, so it means
// one thing in every build and every locale (fetchStateName()'s own rule).
const char *loginStepName(LoginStep step);

// What the machine wants done next.
struct LoginAction
{
    enum class Kind {
        Wait,    // nothing to do; read more and feed it back
        Write,   // put `bytes` on the line
        Done,    // Ready: the console is loftail's to run commands on
        Fail,    // give up; `reason` is user-facing
        Reboot,  // the device restarted. Outranks everything else in the same feed.
    };

    Kind       kind = Kind::Wait;
    QByteArray bytes;

    // THE PASSWORD STEP, AND ONLY IT. SPEC.md §3's "Diagnostics" promises by name that
    // loftail's own log "never records a password, a passphrase, a key" — and the whole
    // point of a transcript of this conversation is that it is attachable to a bug report
    // as it stands. So the transcript logger asks this flag rather than reasoning about
    // which step it is in, and a logger that writes `bytes` unconditionally breaks that
    // promise in the one file it was made about.
    bool       sensitive = false;

    QString    reason;   // Fail only, and user-facing
};

// The conversation, as a state machine over the bytes a device prints.
//
// FOUR RULES ARE LOAD-BEARING AND EACH IS SILENT WHEN BROKEN.
//
// (a) MATCHING IS AGAINST A SLIDING WINDOW OF BYTES, NEVER AGAINST THE ARRIVING CHUNK. A
//     `login: ` prompt carries no trailing newline and a prompt split across two reads is
//     the ORDINARY case at 115200 baud — so a machine that matched per chunk would miss
//     most prompts, silently, and only on a slow link. The window is also what makes the
//     garbage tolerance free: kernel messages between the prompt and the answer simply
//     sit in it and match nothing.
//
// (b) THE WINDOW IS BOUNDED. A device that chatters for an hour before answering is
//     exactly the input this exists to tolerate, so an accumulating buffer is an OOM on
//     the one case that matters. kWindowBytes is generous enough for any prompt plus a
//     burst of noise and no more.
//
// (c) THE WINDOW IS CLEARED ON EVERY TRANSITION. The prompts are unanchored, so the
//     `login:` that started the conversation would otherwise match again after the user
//     name is sent, and the machine would send it for ever.
//
// (c2) FEED RECOGNISES; TICK GUESSES. feed() acts only on what it can positively
//     identify — a login prompt, a password prompt, the marker — and never on "something
//     arrived, so this might be a shell". Guessing there loses a prompt that arrives in two
//     reads: the first half is "something", so the machine sends the marker probe, the
//     transition clears the window by rule (c), and the second half — `in: ` — matches
//     nothing. The prompt is gone and the login stalls, on a slow link, which is every
//     link this is for. The shell guess therefore belongs to tick(), where the step's
//     timeout has already established that nothing recognisable is coming.
//
// (d) A SHELL IS CONFIRMED BY A MARKER ROUND TRIP, NEVER BY A PROMPT PATTERN. Every
//     board's PS1 differs, and a pattern for `#` or `$` matches any line of any log the
//     device happens to be printing. `echo <marker>` and a line whose ENTIRE content is
//     that marker is the one test that cannot be fooled — whole-line equality being what
//     makes it survive the device echoing the command back, which contains the marker
//     (the framing layer's BEGIN rule, same trap one layer up).
class SerialLoginMachine
{
public:
    // Enough for any prompt plus a burst of noise. Not larger: see rule (b).
    static constexpr int kWindowBytes = 4096;

    // `deviceName` appears in the sentences, which is the whole reason it is here —
    // "waiting for the login prompt" says nothing on a window with four tabs.
    // `password` is a QByteArray rather than a QString because it goes on a wire.
    SerialLoginMachine(SerialProfile profile, QString deviceName, QString loginUser,
                       QByteArray password, qint64 nowMs);

    // Bytes just read off the port. The reboot pattern is asked FIRST and its answer
    // outranks everything else in this feed (rule below).
    LoginAction feed(QByteArrayView arrived, qint64 nowMs);

    // No bytes arrived. Nudges a silent device, and gives up once the step's attempts are
    // spent. A device already sitting at a shell prints NOTHING until spoken to, so
    // without this the commonest real case waits for ever.
    LoginAction tick(qint64 nowMs);

    // Start over, which is what a reboot and a dropped port both mean.
    void reset(qint64 nowMs);

    LoginStep step() const { return m_step; }
    bool isReady() const { return m_step == LoginStep::Ready; }
    bool hasFailed() const { return m_step == LoginStep::Failed; }

    // THE REBOOT PATTERN IS ASKED IN EVERY STEP, `Ready` INCLUDED — which is the only
    // step a reboot actually happens in, so a machine that only looked during the login
    // would be a machine that never noticed one. Latched here so the caller can ask after
    // the fact as well as read it off the action.
    bool sawReboot() const { return m_sawReboot; }

    // What the tab says while this is running (SPEC.md §3). One sentence per step, naming
    // the device and the step, and it reaches the placeholder, the tab tooltip and the
    // status bar through FetchStatus::error and the existing republishWaitReason()
    // machinery — a reason that CHANGES while the wait stands is exactly what that was
    // built for.
    QString waitingText() const;

    // The marker this instance confirms a shell with. Public for the tests and for the
    // scripted device in tst_serialdevice, which has to answer it.
    QByteArray marker() const { return m_marker; }

private:
    LoginAction enter(LoginStep step, qint64 nowMs);
    LoginAction write(const QByteArray &bytes, LoginStep next, qint64 nowMs,
                      bool sensitive = false);
    LoginAction fail(const QString &reason);
    void noteProgress();
    LoginAction classify(qint64 nowMs);
    LoginAction runNextStartupCommand(qint64 nowMs);
    bool windowHasMarker() const;
    bool windowMatches(const QRegularExpression &re) const;

    SerialProfile m_profile;
    QString       m_deviceName;
    QString       m_loginUser;
    QByteArray    m_password;
    QByteArray    m_marker;

    // Compiled once. AN INVALID PATTERN MATCHES NOTHING, never everything — which for
    // these three is the difference between a half-typed pattern doing nothing and one
    // that tears the session down and logs in again once a second
    // (LogPatternNode::matches()'s rule, and it bites harder here).
    QRegularExpression m_loginRe;
    QRegularExpression m_passwordRe;
    QRegularExpression m_rebootRe;

    LoginStep  m_step = LoginStep::Idle;
    QByteArray m_window;
    qint64     m_stepSinceMs = 0;
    int        m_attemptsSpent = 0;
    int        m_startupIndex = 0;
    bool       m_sawReboot = false;
    QString    m_failReason;
};

} // namespace loftail
