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

// The serial login conversation (SPEC.md §3, ARCHITECTURE.md §6.11).
//
// This is the whole of what makes a serial log openable that has no analogue anywhere else
// in the tree: recognising a login prompt, a password prompt and a shell in a stream of
// somebody else's console output. It is a pure decision with an injected clock, so every
// case here runs with no port, no device, no QApplication and no waiting — which is the
// point, and the reason the machine was written this way round.

#include <QRegularExpression>
#include <QtTest>

#include "SerialLoginMachine.h"
#include "SerialProfile.h"

using namespace loftail;

namespace {

// A device that talks the way the fixtures below need it to, driven by hand. Nothing here
// is a port: the test IS the far end.
SerialProfile boardProfile()
{
    SerialProfile p;
    p.name = QStringLiteral("board");
    p.promptTimeoutMs = 1000;
    p.attempts = 3;
    return p;
}

SerialLoginMachine machineFor(const SerialProfile &p, qint64 nowMs = 0)
{
    return SerialLoginMachine(p, QStringLiteral("ttyUSB0"), QStringLiteral("root"),
                              QByteArrayLiteral("hunter2"), nowMs);
}

// Drive the machine to Ready against a co-operative device, answering whatever it asks.
// `transcript` collects every byte the machine wrote, which is what the password case
// inspects.
bool driveToReady(SerialLoginMachine &m, QList<QByteArray> *transcript, qint64 *clock,
                  const QByteArray &greeting = QByteArrayLiteral("\nmyboard login: "))
{
    QByteArray pending = greeting;
    for (int turn = 0; turn < 40; ++turn) {
        LoginAction a = pending.isEmpty() ? m.tick(*clock) : m.feed(pending, *clock);
        pending.clear();
        *clock += 10;

        switch (a.kind) {
        case LoginAction::Kind::Done:
            return true;
        case LoginAction::Kind::Fail:
        case LoginAction::Kind::Reboot:
            return false;
        case LoginAction::Kind::Wait:
            // Nothing asked for: let the clock reach the step's timeout so tick() nudges.
            *clock += 1000;
            continue;
        case LoginAction::Kind::Write:
            break;
        }

        if (transcript)
            transcript->append(a.bytes);

        // Answer as the device would.
        if (a.bytes == QByteArrayLiteral("root\n")) {
            pending = QByteArrayLiteral("root\r\nPassword: ");
        } else if (a.bytes == QByteArrayLiteral("hunter2\n")) {
            pending = QByteArrayLiteral("\r\nWelcome to myboard\r\n# ");
        } else if (a.bytes.startsWith(QByteArrayLiteral("echo "))) {
            pending = a.bytes + QByteArrayLiteral("\r\n") + m.marker() + QByteArrayLiteral("\r\n# ");
        } else if (a.bytes.contains(QByteArrayLiteral("; echo "))) {
            // A startup command: echo it, then its marker.
            pending = a.bytes + QByteArrayLiteral("\r\n") + m.marker() + QByteArrayLiteral("\r\n# ");
        } else {
            // A bare nudge.
            pending = QByteArrayLiteral("\r\nmyboard login: ");
        }
    }
    return false;
}

} // namespace

class TestSerialLoginMachine : public QObject
{
    Q_OBJECT

private slots:
    void aCleanLoginReachesReady();
    void aPromptSplitAcrossTwoReadsIsStillAPrompt();
    void kernelNoiseBetweenThePromptAndTheAnswerIsIgnored();
    void aWindowFullOfNoiseDoesNotGrowWithoutBound();
    void theLoginPromptIsNotMatchedTwiceRunning();
    void aDeviceAlreadyLoggedInReachesReadyWithNoPassword();
    void aShellIsNotConfirmedByTheEchoOfItsOwnCommand();
    void theRebootRegexpFiresFromTheReadyStateToo();
    void anInvalidRebootRegexpMatchesNothing();
    void aPasswordNeverReachesTheTranscriptUnmarked();
    void everyStepGivesUpAfterItsAttempts();
    void aRefusedPasswordSaysSoRatherThanRetryingForEver();
    void startupCommandsRunInOrderAndAreNeverResent();
    void everyWaitingSentenceNamesTheDeviceAndIsCapitalised();
};

void TestSerialLoginMachine::aCleanLoginReachesReady()
{
    SerialProfile p = boardProfile();
    SerialLoginMachine m = machineFor(p);
    qint64 clock = 0;
    QVERIFY(driveToReady(m, nullptr, &clock));
    QCOMPARE(m.step(), LoginStep::Ready);
    QVERIFY(m.isReady());
    // Ready says nothing: there is no wait to report.
    QVERIFY(m.waitingText().isEmpty());
}

void TestSerialLoginMachine::aPromptSplitAcrossTwoReadsIsStillAPrompt()
{
    // THE CASE THE SLIDING WINDOW EXISTS FOR. At 115200 baud a prompt arriving in two
    // reads is the ordinary event, not a corner, and a machine matching the arriving chunk
    // would miss it silently — and only on a slow link, which is every link this feature
    // is for.
    SerialProfile p = boardProfile();
    SerialLoginMachine m = machineFor(p);

    QCOMPARE(m.tick(0).kind, LoginAction::Kind::Write);      // the opening nudge
    QCOMPARE(m.step(), LoginStep::Probing);

    // Half a prompt. Nothing to do yet — and crucially NOT a failure.
    LoginAction a = m.feed(QByteArrayLiteral("\r\nmyboard log"), 10);
    QCOMPARE(a.kind, LoginAction::Kind::Wait);

    // The rest of it, in a second read. The window is what joins them.
    a = m.feed(QByteArrayLiteral("in: "), 20);
    QCOMPARE(a.kind, LoginAction::Kind::Write);
    QCOMPARE(a.bytes, QByteArrayLiteral("root\n"));
    QCOMPARE(m.step(), LoginStep::SentUser);
}

void TestSerialLoginMachine::kernelNoiseBetweenThePromptAndTheAnswerIsIgnored()
{
    // Requirement 7, and the reason the window is a window rather than a line reader: a
    // device runs other things, and they print to the same console.
    SerialProfile p = boardProfile();
    SerialLoginMachine m = machineFor(p);
    QCOMPARE(m.tick(0).kind, LoginAction::Kind::Write);

    QByteArray noisy;
    for (int i = 0; i < 12; ++i) {
        noisy += QByteArrayLiteral("[   12.3456] usb 1-1: new high-speed USB device number ")
            + QByteArray::number(i) + QByteArrayLiteral("\r\n");
    }
    noisy += QByteArrayLiteral("myboard login: ");

    const LoginAction a = m.feed(noisy, 10);
    QCOMPARE(a.kind, LoginAction::Kind::Write);
    QCOMPARE(a.bytes, QByteArrayLiteral("root\n"));
}

void TestSerialLoginMachine::aWindowFullOfNoiseDoesNotGrowWithoutBound()
{
    // A COST CONTRACT, COUNTED AND NEVER TIMED (tests/GUARDS.md's rule). A device that
    // chatters for an hour before answering is exactly the input this tolerates, so an
    // accumulating buffer is an OOM on the case that matters. What is asserted is that the
    // prompt is STILL found after far more noise than the window holds — which is the
    // observable consequence of the bound being applied at the right end.
    SerialProfile p = boardProfile();
    SerialLoginMachine m = machineFor(p);
    QCOMPARE(m.tick(0).kind, LoginAction::Kind::Write);

    const QByteArray chunk(SerialLoginMachine::kWindowBytes, 'x');
    for (int i = 0; i < 50; ++i) {
        const LoginAction a = m.feed(chunk, 10 + i);
        // Noise alone never matches a prompt, so the machine asks for the marker at most —
        // never the user name, and never a failure.
        QVERIFY(a.kind == LoginAction::Kind::Wait || a.kind == LoginAction::Kind::Write);
        QVERIFY(a.bytes != QByteArrayLiteral("root\n"));
    }
    // 200 KB of noise later, a prompt still lands.
    SerialLoginMachine fresh = machineFor(p);
    QCOMPARE(fresh.tick(0).kind, LoginAction::Kind::Write);
    for (int i = 0; i < 50; ++i)
        fresh.feed(chunk, 10 + i);
    const LoginAction a = fresh.feed(QByteArrayLiteral("\r\nmyboard login: "), 100);
    QCOMPARE(a.bytes, QByteArrayLiteral("root\n"));
}

void TestSerialLoginMachine::theLoginPromptIsNotMatchedTwiceRunning()
{
    // RULE (c). The prompts are unanchored, so without clearing the window on the
    // transition the `login:` that started the conversation matches again the moment any
    // byte arrives, and the machine sends the user name for ever.
    SerialProfile p = boardProfile();
    SerialLoginMachine m = machineFor(p);
    QCOMPARE(m.tick(0).kind, LoginAction::Kind::Write);

    LoginAction a = m.feed(QByteArrayLiteral("myboard login: "), 10);
    QCOMPARE(a.bytes, QByteArrayLiteral("root\n"));

    // The device echoes the user name. That echo does NOT carry a prompt, and the window
    // no longer holds the one that did — so nothing is resent.
    a = m.feed(QByteArrayLiteral("root\r\n"), 20);
    QVERIFY(a.bytes != QByteArrayLiteral("root\n"));
}

void TestSerialLoginMachine::aDeviceAlreadyLoggedInReachesReadyWithNoPassword()
{
    // THE COMMONEST REAL CASE, and the one that needs the opening nudge: a board left at a
    // shell prints nothing at all until it is spoken to, so a machine that waited for a
    // prompt would wait for ever.
    SerialProfile p = boardProfile();
    SerialLoginMachine m = machineFor(p);
    qint64 clock = 0;

    // Silence. The nudge is what breaks it.
    LoginAction a = m.tick(clock);
    QCOMPARE(a.kind, LoginAction::Kind::Write);
    QCOMPARE(a.bytes, QByteArrayLiteral("\n"));

    // A shell prompt matches neither pattern, because no pattern can match every board's
    // PS1 — so feed() recognises nothing and says so. THAT IS THE FIX, not a shortcoming:
    // guessing here would lose a prompt that arrives in two reads (rule c2).
    a = m.feed(QByteArrayLiteral("\r\n[root@myboard ~]# "), clock += 10);
    QCOMPARE(a.kind, LoginAction::Kind::Wait);

    // The step's timeout is what licenses the guess, and the marker is what settles it.
    a = m.tick(clock += p.promptTimeoutMs);
    QCOMPARE(a.kind, LoginAction::Kind::Write);
    QVERIFY(a.bytes.startsWith(QByteArrayLiteral("echo ")));
    QCOMPARE(m.step(), LoginStep::ConfirmShell);

    // It comes back, so there is a shell. No password was ever sent.
    a = m.feed(a.bytes + QByteArrayLiteral("\r\n") + m.marker() + QByteArrayLiteral("\r\n# "),
               clock += 10);
    QCOMPARE(a.kind, LoginAction::Kind::Done);
    QCOMPARE(m.step(), LoginStep::Ready);
}

void TestSerialLoginMachine::aShellIsNotConfirmedByTheEchoOfItsOwnCommand()
{
    // RULE (d)'s whole-line half, and the trap it exists against: the device echoes
    // `echo LFTL-abc` back, and that echo CONTAINS the marker. A substring test would
    // confirm a shell that is not there — on every device with echo on, and only on those.
    SerialProfile p = boardProfile();
    SerialLoginMachine m = machineFor(p);
    qint64 clock = 0;
    QCOMPARE(m.tick(clock).kind, LoginAction::Kind::Write);
    QCOMPARE(m.feed(QByteArrayLiteral("\r\n# "), clock += 10).kind, LoginAction::Kind::Wait);
    LoginAction a = m.tick(clock += p.promptTimeoutMs);
    QVERIFY(a.bytes.startsWith(QByteArrayLiteral("echo ")));

    // ONLY the echo of the command, with no answer after it.
    a = m.feed(QByteArrayLiteral("echo ") + m.marker() + QByteArrayLiteral("\r\n"), clock += 10);
    QCOMPARE(a.kind, LoginAction::Kind::Wait);
    QCOMPARE(m.step(), LoginStep::ConfirmShell);

    // The answer itself, on a line of its own, is what settles it.
    a = m.feed(m.marker() + QByteArrayLiteral("\r\n"), clock += 10);
    QCOMPARE(a.kind, LoginAction::Kind::Done);
}

void TestSerialLoginMachine::theRebootRegexpFiresFromTheReadyStateToo()
{
    // Ready IS the state a reboot happens in, so a machine that only looked during the
    // login would be a machine that never noticed one — which is the whole of what
    // SPEC.md §3 promises about a device that restarts.
    SerialProfile p = boardProfile();
    p.rebootRe = QStringLiteral("U-Boot|Linux version");
    SerialLoginMachine m = machineFor(p);
    qint64 clock = 0;
    QVERIFY(driveToReady(m, nullptr, &clock));
    QCOMPARE(m.step(), LoginStep::Ready);
    QVERIFY(!m.sawReboot());

    const LoginAction a = m.feed(QByteArrayLiteral("\r\nU-Boot 2023.04 (Apr 03 2023)\r\n"),
                                 clock += 10);
    QCOMPARE(a.kind, LoginAction::Kind::Reboot);
    QVERIFY(m.sawReboot());

    // And in the middle of a login, which is the easier half.
    SerialLoginMachine mid = machineFor(p);
    QCOMPARE(mid.tick(0).kind, LoginAction::Kind::Write);
    QCOMPARE(mid.feed(QByteArrayLiteral("[    0.000000] Linux version 6.1.0\r\n"), 10).kind,
             LoginAction::Kind::Reboot);
}

void TestSerialLoginMachine::anInvalidRebootRegexpMatchesNothing()
{
    // LogPatternNode::matches()' rule, and it bites harder here: a pattern that matched
    // everything would tear the session down and log in again on the first byte, once a
    // second, for ever.
    SerialProfile p = boardProfile();
    p.rebootRe = QStringLiteral("([unclosed");
    QVERIFY(!QRegularExpression(p.rebootRe).isValid());   // the premise

    SerialLoginMachine m = machineFor(p);
    qint64 clock = 0;
    QVERIFY(driveToReady(m, nullptr, &clock));
    QVERIFY(!m.sawReboot());
    QCOMPARE(m.feed(QByteArrayLiteral("anything at all\r\n"), clock += 10).kind,
             LoginAction::Kind::Wait);
    QVERIFY(!m.sawReboot());

    // An EMPTY pattern is the same answer, which is what makes the setting need no enable
    // flag beside it to fall out of step.
    SerialProfile off = boardProfile();
    SerialLoginMachine none = machineFor(off);
    QVERIFY(driveToReady(none, nullptr, &clock));
    QCOMPARE(none.feed(QByteArrayLiteral("U-Boot 2023.04\r\n"), clock += 10).kind,
             LoginAction::Kind::Wait);
    QVERIFY(!none.sawReboot());
}

void TestSerialLoginMachine::aPasswordNeverReachesTheTranscriptUnmarked()
{
    // SPEC.md §3's Diagnostics promises by name that loftail's own log "never records a
    // password, a passphrase, a key", and the whole point of a transcript of this
    // conversation is that it is attachable to a bug report as it stands. The flag is how
    // the logger knows, so what is asserted is that EXACTLY the write carrying the
    // password is marked, and no other.
    SerialProfile p = boardProfile();
    p.startupCommands = QStringList{QStringLiteral("dmesg -n 1")};
    SerialLoginMachine m = machineFor(p);
    qint64 clock = 0;

    QList<QByteArray> sensitive;
    QList<QByteArray> ordinary;
    QByteArray pending = QByteArrayLiteral("\nmyboard login: ");
    for (int turn = 0; turn < 40; ++turn) {
        LoginAction a = pending.isEmpty() ? m.tick(clock) : m.feed(pending, clock);
        pending.clear();
        clock += 10;
        if (a.kind == LoginAction::Kind::Done)
            break;
        if (a.kind == LoginAction::Kind::Wait) { clock += 1000; continue; }
        QVERIFY(a.kind == LoginAction::Kind::Write);
        (a.sensitive ? sensitive : ordinary).append(a.bytes);

        if (a.bytes == QByteArrayLiteral("root\n"))
            pending = QByteArrayLiteral("root\r\nPassword: ");
        else if (a.bytes == QByteArrayLiteral("hunter2\n"))
            pending = QByteArrayLiteral("\r\nWelcome\r\n# ");
        else if (a.bytes.contains(QByteArrayLiteral("echo ")))
            pending = a.bytes + QByteArrayLiteral("\r\n") + m.marker() + QByteArrayLiteral("\r\n# ");
        else
            pending = QByteArrayLiteral("\r\nmyboard login: ");
    }
    QVERIFY(m.isReady());

    // Exactly one sensitive write, and it is the password.
    QCOMPARE(sensitive.size(), 1);
    QCOMPARE(sensitive.first(), QByteArrayLiteral("hunter2\n"));
    // And the password appears in NO unmarked write — a logger honouring the flag then
    // cannot leak it however many steps the conversation took.
    for (const QByteArray &b : ordinary)
        QVERIFY(!b.contains(QByteArrayLiteral("hunter2")));
}

void TestSerialLoginMachine::everyStepGivesUpAfterItsAttempts()
{
    // Bounded for ReconnectGrace's reason: a device that will never answer must end in a
    // stated failure rather than a tab that says "signing in" for the rest of the day.
    SerialProfile p = boardProfile();
    p.attempts = 3;
    SerialLoginMachine m = machineFor(p);

    qint64 clock = 0;
    bool failed = false;
    for (int i = 0; i < 20 && !failed; ++i) {
        const LoginAction a = m.tick(clock);
        clock += p.promptTimeoutMs;
        failed = a.kind == LoginAction::Kind::Fail;
        if (failed)
            QVERIFY(!a.reason.isEmpty());
    }
    QVERIFY(failed);
    QCOMPARE(m.step(), LoginStep::Failed);
    QVERIFY(m.hasFailed());
    // The failure is what the tab then says, so it must survive as the sentence — asserted
    // as properties rather than as prose, which a rewording would break for no reason. It
    // opens on the device's own name, which is what every local wait reason already does
    // ("app.log has not appeared yet"), so it is not a capitalisation defect.
    QVERIFY(m.waitingText().startsWith(QStringLiteral("ttyUSB0")));
    QVERIFY(m.waitingText().contains(QStringLiteral("console")));
}

void TestSerialLoginMachine::aRefusedPasswordSaysSoRatherThanRetryingForEver()
{
    // A wrong password in the preset is the likeliest configuration mistake there is, and
    // what a getty does about it is print the login prompt again. That has to read as a
    // refusal and not as another turn of the conversation.
    SerialProfile p = boardProfile();
    SerialLoginMachine m = machineFor(p);
    qint64 clock = 0;
    QCOMPARE(m.tick(clock).kind, LoginAction::Kind::Write);
    LoginAction a = m.feed(QByteArrayLiteral("myboard login: "), clock += 10);
    QCOMPARE(a.bytes, QByteArrayLiteral("root\n"));
    a = m.feed(QByteArrayLiteral("root\r\nPassword: "), clock += 10);
    QCOMPARE(a.bytes, QByteArrayLiteral("hunter2\n"));
    QVERIFY(a.sensitive);

    a = m.feed(QByteArrayLiteral("\r\nLogin incorrect\r\nmyboard login: "), clock += 10);
    QCOMPARE(a.kind, LoginAction::Kind::Fail);
    QVERIFY(a.reason.contains(QStringLiteral("refused")));
    // And the reason never carries the credential it was refused over.
    QVERIFY(!a.reason.contains(QStringLiteral("hunter2")));
}

void TestSerialLoginMachine::startupCommandsRunInOrderAndAreNeverResent()
{
    // Requirement 4. Each command waits for its own completion marker, so "it finished" is
    // an observation rather than a timeout — and a command that does NOT answer is an
    // honest failure rather than a second copy of itself, because these have side effects
    // and that is what they are for.
    SerialProfile p = boardProfile();
    p.startupCommands = QStringList{QStringLiteral("dmesg -n 1"),
                                    QStringLiteral("stty -echo")};
    SerialLoginMachine m = machineFor(p);
    qint64 clock = 0;
    QList<QByteArray> written;
    QVERIFY(driveToReady(m, &written, &clock));

    QStringList commandsSeen;
    for (const QByteArray &b : written) {
        if (b.contains(QByteArrayLiteral("; echo ")))
            commandsSeen.append(QString::fromUtf8(b.left(b.indexOf(QByteArrayLiteral("; echo ")))));
    }
    QCOMPARE(commandsSeen, (QStringList{QStringLiteral("dmesg -n 1"), QStringLiteral("stty -echo")}));

    // Now the same two against a device that goes quiet after the first. The second must
    // NOT be resent, and the step must give up.
    SerialLoginMachine mute = machineFor(p);
    clock = 0;
    QByteArray pending = QByteArrayLiteral("\nmyboard login: ");
    int dmesgWrites = 0;
    bool gaveUp = false;
    for (int turn = 0; turn < 40 && !gaveUp; ++turn) {
        LoginAction a = pending.isEmpty() ? mute.tick(clock) : mute.feed(pending, clock);
        pending.clear();
        clock += 10;
        if (a.kind == LoginAction::Kind::Fail) { gaveUp = true; break; }
        if (a.kind == LoginAction::Kind::Wait) { clock += p.promptTimeoutMs; continue; }
        if (a.kind == LoginAction::Kind::Done) break;
        if (a.bytes.startsWith(QByteArrayLiteral("dmesg -n 1"))) {
            ++dmesgWrites;
            continue;       // the device says nothing at all about it
        }
        if (a.bytes == QByteArrayLiteral("root\n"))
            pending = QByteArrayLiteral("root\r\nPassword: ");
        else if (a.bytes == QByteArrayLiteral("hunter2\n"))
            pending = QByteArrayLiteral("\r\nWelcome\r\n# ");
        else if (a.bytes.startsWith(QByteArrayLiteral("echo ")))
            pending = a.bytes + QByteArrayLiteral("\r\n") + mute.marker() + QByteArrayLiteral("\r\n# ");
        else
            pending = QByteArrayLiteral("\r\nmyboard login: ");
    }
    QCOMPARE(dmesgWrites, 1);   // sent once, never again
    QVERIFY(gaveUp);
}

void TestSerialLoginMachine::everyWaitingSentenceNamesTheDeviceAndIsCapitalised()
{
    // These sentences go where LogView puts its placeholder, so the rule the tree already
    // keeps applies: a sentence over the log view starts with a capital letter — unless it
    // opens on the NAME of the thing it is about, which is what every local wait reason
    // already does ("app.log has not appeared yet"). And each one has to name the device,
    // or a window with four tabs says nothing about which.
    SerialProfile p = boardProfile();
    p.startupCommands = QStringList{QStringLiteral("dmesg -n 1")};

    const QList<LoginStep> spoken{LoginStep::Idle, LoginStep::Probing, LoginStep::ConfirmShell,
                                  LoginStep::SentUser, LoginStep::SentPassword,
                                  LoginStep::RunStartup};
    // Walk a real conversation and collect the sentence at each step it passes through,
    // rather than reaching into the machine to set one.
    QSet<int> seen;
    SerialLoginMachine m = machineFor(p);
    qint64 clock = 0;
    // Starts SILENT, which is both the realistic device and the only way the sweep passes
    // through Probing at all: a greeting already on the line takes Idle straight to
    // SentUser, so a fixture that opens with one would leave that step's sentence unasserted.
    QByteArray pending;
    for (int turn = 0; turn < 40; ++turn) {
        const QString text = m.waitingText();
        if (spoken.contains(m.step())) {
            seen.insert(int(m.step()));
            QVERIFY2(!text.isEmpty(), loginStepName(m.step()));
            QVERIFY2(text.contains(QStringLiteral("ttyUSB0")), qUtf8Printable(text));
            const bool opensOnAName = text.startsWith(QStringLiteral("ttyUSB0"))
                || text.startsWith(QStringLiteral("dmesg"));
            QVERIFY2(text.at(0).isUpper() || opensOnAName, qUtf8Printable(text));
            // Never a credential, in any of them.
            QVERIFY(!text.contains(QStringLiteral("hunter2")));
        }
        LoginAction a = pending.isEmpty() ? m.tick(clock) : m.feed(pending, clock);
        pending.clear();
        clock += 10;
        if (a.kind == LoginAction::Kind::Done)
            break;
        if (a.kind == LoginAction::Kind::Wait) { clock += p.promptTimeoutMs; continue; }
        if (a.bytes == QByteArrayLiteral("root\n"))
            pending = QByteArrayLiteral("root\r\nPassword: ");
        else if (a.bytes == QByteArrayLiteral("hunter2\n"))
            pending = QByteArrayLiteral("\r\nWelcome\r\n# ");
        else if (a.bytes.contains(QByteArrayLiteral("echo ")))
            pending = a.bytes + QByteArrayLiteral("\r\n") + m.marker() + QByteArrayLiteral("\r\n# ");
        else
            pending = QByteArrayLiteral("\r\nmyboard login: ");
    }
    // The conversation really did pass through every step that has something to say, or
    // the assertions above were vacuous for the ones it skipped.
    for (LoginStep step : spoken)
        QVERIFY2(seen.contains(int(step)), loginStepName(step));
}

QTEST_APPLESS_MAIN(TestSerialLoginMachine)
#include "tst_serialloginmachine.moc"
