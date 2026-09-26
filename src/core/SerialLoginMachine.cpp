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

#include "SerialLoginMachine.h"

#include <QCoreApplication>
#include <QRandomGenerator>

#include <utility>

namespace loftail {
namespace {

// Nothing in src/core is a QObject, so the sentences go through a shim named for the type
// rather than a hand-rolled tr(), which lupdate would not understand (§9.1).
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::SerialLoginMachine)
};

// A pattern that is empty or does not compile MATCHES NOTHING. Returning a default
// QRegularExpression would match EVERYTHING, which for the reboot pattern means tearing
// the session down on the first byte and for the prompts means sending the password to
// whatever the device said first.
QRegularExpression compileOrNothing(const QString &pattern)
{
    if (pattern.isEmpty())
        return QRegularExpression{};
    QRegularExpression re(pattern);
    if (!re.isValid())
        return QRegularExpression{};
    return re;
}

} // namespace

const char *loginStepName(LoginStep step)
{
    switch (step) {
    case LoginStep::Idle:         return "idle";
    case LoginStep::Probing:      return "probing";
    case LoginStep::ConfirmShell: return "confirm-shell";
    case LoginStep::SentUser:     return "sent-user";
    case LoginStep::SentPassword: return "sent-password";
    case LoginStep::RunStartup:   return "run-startup";
    case LoginStep::Ready:        return "ready";
    case LoginStep::Failed:       return "failed";
    }
    return "?";
}

SerialLoginMachine::SerialLoginMachine(SerialProfile profile, QString deviceName,
                                       QString loginUser, QByteArray password, qint64 nowMs)
    : m_profile(std::move(profile))
    , m_deviceName(std::move(deviceName))
    , m_loginUser(std::move(loginUser))
    , m_password(std::move(password))
    , m_stepSinceMs(nowMs)
{
    // Per instance, so a device replaying its scrollback — or a log holding an earlier
    // run's marker — cannot answer this one's question. Hex of 64 random bits: long
    // enough not to occur by accident, short enough to read in a transcript.
    m_marker = QByteArrayLiteral("LFTL-")
        + QByteArray::number(QRandomGenerator::global()->generate64(), 16);

    m_loginRe = compileOrNothing(m_profile.loginPromptRe);
    m_passwordRe = compileOrNothing(m_profile.passwordPromptRe);
    m_rebootRe = compileOrNothing(m_profile.rebootRe);
}

void SerialLoginMachine::reset(qint64 nowMs)
{
    m_step = LoginStep::Idle;
    m_window.clear();
    m_stepSinceMs = nowMs;
    m_attemptsSpent = 0;
    m_startupIndex = 0;
    m_sawReboot = false;
    m_failReason.clear();
}

LoginAction SerialLoginMachine::enter(LoginStep step, qint64 nowMs)
{
    // RULE (c): the window is cleared on every transition. The prompts are unanchored, so
    // without this the `login:` that started the conversation matches again after the user
    // name is sent and the machine sends it for ever.
    //
    // THE ATTEMPT COUNT IS NOT TOUCHED HERE, and that is load-bearing. Probing and
    // ConfirmShell ALTERNATE on a timeout — "say something" / "are you a shell" — so a
    // count reset by a step change would be reset by every turn of that alternation and
    // `attempts` would bound nothing: a device that never answers would be nudged for the
    // rest of the day, which is the one thing the bound exists to stop. Only
    // noteProgress() clears it, and only for something actually recognised.
    m_step = step;
    m_window.clear();
    m_stepSinceMs = nowMs;
    return LoginAction{};
}

LoginAction SerialLoginMachine::write(const QByteArray &bytes, LoginStep next, qint64 nowMs,
                                      bool sensitive)
{
    enter(next, nowMs);
    LoginAction a;
    a.kind = LoginAction::Kind::Write;
    a.bytes = bytes;
    a.sensitive = sensitive;
    return a;
}

void SerialLoginMachine::noteProgress()
{
    // Something was RECOGNISED — a prompt matched, the marker came back, a startup command
    // finished. That is the only thing that buys a fresh budget, which is what keeps the
    // Probing/ConfirmShell alternation bounded while still letting a slow but progressing
    // login take as long as it needs.
    m_attemptsSpent = 0;
}

LoginAction SerialLoginMachine::fail(const QString &reason)
{
    m_step = LoginStep::Failed;
    m_failReason = reason;
    LoginAction a;
    a.kind = LoginAction::Kind::Fail;
    a.reason = reason;
    return a;
}

bool SerialLoginMachine::windowMatches(const QRegularExpression &re) const
{
    if (!re.isValid() || re.pattern().isEmpty())
        return false;
    return re.match(QString::fromLatin1(m_window)).hasMatch();
}

bool SerialLoginMachine::windowHasMarker() const
{
    // RULE (d), and its whole-line half. The device echoes the command back, and that
    // echo CONTAINS the marker — `echo LFTL-abc` — so a substring test would confirm a
    // shell that is not there, on every device with echo on, and only on those. What is
    // looked for is a line whose entire content is the marker.
    const QList<QByteArray> lines = m_window.split('\n');
    for (const QByteArray &line : lines) {
        if (line.trimmed() == m_marker)
            return true;
    }
    return false;
}

LoginAction SerialLoginMachine::classify(qint64 nowMs)
{
    // What is on the other end? Only the two things that can be RECOGNISED are decided
    // here. Whether this is instead a shell is a guess, and guessing it belongs to tick()
    // rather than to feed() — see the rule below, which is the sharpest thing in this file.
    if (windowMatches(m_loginRe)) {
        noteProgress();
        return write(m_loginUser.toUtf8() + '\n', LoginStep::SentUser, nowMs);
    }
    if (windowMatches(m_passwordRe)) {
        noteProgress();
        return write(m_password + '\n', LoginStep::SentPassword, nowMs, /*sensitive=*/true);
    }
    return LoginAction{};
}

LoginAction SerialLoginMachine::runNextStartupCommand(qint64 nowMs)
{
    if (m_startupIndex >= m_profile.startupCommands.size()) {
        enter(LoginStep::Ready, nowMs);
        LoginAction a;
        a.kind = LoginAction::Kind::Done;
        return a;
    }
    // Each command carries its own completion marker, so "it finished" is an observation
    // rather than a timeout. The command's own output is noise as far as this machine is
    // concerned — the point of these commands is to make the device print LESS.
    const QString command = m_profile.startupCommands.at(m_startupIndex);
    ++m_startupIndex;
    const QByteArray line = command.toUtf8() + QByteArray("; echo ") + m_marker + '\n';
    return write(line, LoginStep::RunStartup, nowMs);
}

LoginAction SerialLoginMachine::feed(QByteArrayView arrived, qint64 nowMs)
{
    // RULE (a)/(b): append to a bounded sliding window rather than matching the chunk.
    m_window.append(arrived.data(), arrived.size());
    if (m_window.size() > kWindowBytes)
        m_window.remove(0, m_window.size() - kWindowBytes);

    // THE REBOOT PATTERN IS ASKED FIRST, IN EVERY STEP INCLUDING Ready, AND ITS ANSWER
    // OUTRANKS EVERYTHING ELSE IN THIS FEED. Ready is the step a reboot actually happens
    // in; a machine that only looked during the login would never notice one, which is
    // the whole of what SPEC.md §3 promises about a device that restarts.
    if (m_step != LoginStep::Failed && windowMatches(m_rebootRe)) {
        m_sawReboot = true;
        m_window.clear();
        LoginAction a;
        a.kind = LoginAction::Kind::Reboot;
        return a;
    }

    switch (m_step) {
    case LoginStep::Idle:
        // Something arrived before anything was sent — a device that talks unprompted.
        // Classify it rather than nudging a line that is plainly alive.
        return classify(nowMs);

    case LoginStep::Probing:
        return classify(nowMs);

    case LoginStep::ConfirmShell:
        if (windowHasMarker()) {
            noteProgress();
            return runNextStartupCommand(nowMs);
        }
        // The `echo` was eaten by a getty, which took it for a user name: the login
        // conversation is where we actually are. This is why ConfirmShell answers these
        // two as well, and why sending the marker early is self-correcting rather than
        // fatal.
        if (windowMatches(m_loginRe)) {
            noteProgress();
            return write(m_loginUser.toUtf8() + '\n', LoginStep::SentUser, nowMs);
        }
        if (windowMatches(m_passwordRe)) {
            noteProgress();
            return write(m_password + '\n', LoginStep::SentPassword, nowMs, /*sensitive=*/true);
        }
        return LoginAction{};

    case LoginStep::SentUser:
        if (windowMatches(m_passwordRe)) {
            noteProgress();
            return write(m_password + '\n', LoginStep::SentPassword, nowMs, /*sensitive=*/true);
        }
        // The login prompt came back instead: the user name was not accepted, or it went
        // out before the prompt had finished arriving. Either way, start the conversation
        // again rather than sitting on a dead step — bounded by attempts.
        if (windowMatches(m_loginRe)) {
            if (++m_attemptsSpent >= m_profile.attempts) {
                return fail(Tr::tr("%1 did not accept the user name %2.")
                                .arg(m_deviceName, m_loginUser));
            }
            return write(m_loginUser.toUtf8() + '\n', LoginStep::SentUser, nowMs);
        }
        return LoginAction{};

    case LoginStep::SentPassword:
        // The login prompt back again is a refusal, and it is the one failure here worth
        // its own sentence: it is what a wrong password in the preset looks like.
        if (windowMatches(m_loginRe)) {
            return fail(Tr::tr("%1 refused the user name or password.").arg(m_deviceName));
        }
        // Anything else may be a motd and a prompt, and it may equally be the first half
        // of a `Password:` the device is still printing. Guessing here is what the rule
        // below forbids, so the marker probe waits for tick().
        return LoginAction{};

    case LoginStep::RunStartup:
        if (windowHasMarker()) {
            noteProgress();
            return runNextStartupCommand(nowMs);
        }
        return LoginAction{};

    case LoginStep::Ready:
    case LoginStep::Failed:
        return LoginAction{};
    }
    return LoginAction{};
}

LoginAction SerialLoginMachine::tick(qint64 nowMs)
{
    if (m_step == LoginStep::Ready || m_step == LoginStep::Failed)
        return LoginAction{};

    if (m_step == LoginStep::Idle) {
        // A bare newline, which is harmless at a login prompt AND at a shell prompt. The
        // alternative — opening with `echo <marker>` — hands a getty a user name it will
        // then ask for a password for, spending a login attempt to learn nothing.
        return write(QByteArray("\n"), LoginStep::Probing, nowMs);
    }

    if (nowMs - m_stepSinceMs < m_profile.promptTimeoutMs)
        return LoginAction{};

    if (++m_attemptsSpent >= m_profile.attempts) {
        return fail(Tr::tr("%1 did not answer on its console — %2.")
                        .arg(m_deviceName, waitingText()));
    }
    m_stepSinceMs = nowMs;

    switch (m_step) {
    case LoginStep::Probing:
        // Nothing recognisable arrived within the step's timeout, so this may be a shell
        // prompt — every board's differs, and no pattern can match them all. Ask for the
        // marker, which is the only test that settles it.
        return write(QByteArray("echo ") + m_marker + '\n', LoginStep::ConfirmShell, nowMs);
    case LoginStep::ConfirmShell:
        // No marker either. Nudge the line and go round again; the two steps alternate,
        // bounded by the login-wide attempt count rather than a per-step one.
        return write(QByteArray("\n"), LoginStep::Probing, nowMs);
    case LoginStep::SentUser:
        // No password prompt came. A root account with no password goes straight to a
        // shell and prints nothing more, so ask for the marker rather than assume a
        // failure — which is what would make a passwordless board unopenable.
        return write(QByteArray("echo ") + m_marker + '\n', LoginStep::ConfirmShell, nowMs);
    case LoginStep::SentPassword:
        return write(QByteArray("echo ") + m_marker + '\n', LoginStep::ConfirmShell, nowMs);
    case LoginStep::RunStartup:
        // DELIBERATELY NOT RESENT. A startup command has side effects — that is what it is
        // for — so a second copy of it is worse than an honest failure. The attempts are
        // spent waiting, not retrying, and the sentence names the command.
        return LoginAction{};
    case LoginStep::Idle:
    case LoginStep::Ready:
    case LoginStep::Failed:
        break;
    }
    return LoginAction{};
}

QString SerialLoginMachine::waitingText() const
{
    switch (m_step) {
    case LoginStep::Idle:
        return Tr::tr("Opening the console on %1").arg(m_deviceName);
    case LoginStep::Probing:
        return Tr::tr("Waiting for %1 to say something").arg(m_deviceName);
    case LoginStep::ConfirmShell:
        return Tr::tr("Checking for a shell on %1").arg(m_deviceName);
    case LoginStep::SentUser:
        return Tr::tr("Signing in to %1 — waiting for the password prompt").arg(m_deviceName);
    case LoginStep::SentPassword:
        return Tr::tr("Signing in to %1 — waiting for a shell").arg(m_deviceName);
    case LoginStep::RunStartup: {
        const int at = m_startupIndex - 1;
        if (at >= 0 && at < m_profile.startupCommands.size()) {
            return Tr::tr("Running %1 on %2")
                .arg(m_profile.startupCommands.at(at), m_deviceName);
        }
        return Tr::tr("Setting %1 up").arg(m_deviceName);
    }
    case LoginStep::Ready:
        return QString();
    case LoginStep::Failed:
        return m_failReason;
    }
    return QString();
}

} // namespace loftail
