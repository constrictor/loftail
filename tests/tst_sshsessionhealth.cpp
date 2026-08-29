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

#include <QtTest>

#include "SshSessionHealth.h"

using namespace loftail;

// Whether a libssh2 error code means the SESSION is finished, and the latch that
// remembers the answer (bugs.md 30, ARCHITECTURE.md §6.3).
//
// UNGATED, and the situation it stands for is one nothing in CI can reach: a machine that
// goes away under a tab which is already connected. Staging that needs a server to
// connect to and then a server to take away, so the decision is a pure function with the
// error code as its parameter and the whole of it is walked here in microseconds — the
// same argument SshExecCommands, ExecSizeProbe and SshRetryPolicy are always compiled by.
//
// The constants are read from SshError, never written as bare numbers: they are libssh2's
// public ABI mirrored into a header that compiles without it, and SshSession.cpp
// static_asserts the mirror against the real thing. A test spelling -13 rather than
// kSocketDisconnect would pass just as well against a mirror that had drifted.
class TestSshSessionHealth : public QObject
{
    Q_OBJECT

private slots:
    void aTransportFailureEndsTheSession_data();
    void aTransportFailureEndsTheSession();
    void aRequestFailingLeavesTheLinkAlone_data();
    void aRequestFailingLeavesTheLinkAlone();
    void anUnknownCodeIsNotTakenAsADroppedLink();
    void aFreshSessionIsAlive();
    void aNonTerminalCodeDoesNotClearTheLatch();
    void aCallThatGotAnAnswerClearsTheLatch();
};

// The codes that mean the socket or the encrypted stream underneath is gone. Each of
// these has to answer true or SshFetcher::pollOnce() goes on polling a dead session, which
// is the whole of the defect: the tab reported the log as unreadable once per session
// timeout, for ever, and did not recover when the host came back.
void TestSshSessionHealth::aTransportFailureEndsTheSession_data()
{
    QTest::addColumn<int>("code");

    QTest::newRow("socket none") << SshError::kSocketNone;
    QTest::newRow("socket send") << SshError::kSocketSend;
    QTest::newRow("socket recv") << SshError::kSocketRecv;
    QTest::newRow("socket disconnect") << SshError::kSocketDisconnect;
    QTest::newRow("bad socket") << SshError::kBadSocket;
    QTest::newRow("timeout") << SshError::kTimeout;
    QTest::newRow("socket timeout") << SshError::kSocketTimeout;
    QTest::newRow("banner recv") << SshError::kBannerRecv;
    QTest::newRow("banner send") << SshError::kBannerSend;
    QTest::newRow("kex failure") << SshError::kKexFailure;
    QTest::newRow("key exchange failure") << SshError::kKeyExchangeFailure;
    QTest::newRow("invalid mac") << SshError::kInvalidMac;
    QTest::newRow("mac failure") << SshError::kMacFailure;
    QTest::newRow("decrypt") << SshError::kDecrypt;
    QTest::newRow("encrypt") << SshError::kEncrypt;
    QTest::newRow("proto") << SshError::kProto;
    QTest::newRow("zlib") << SshError::kZlib;
    QTest::newRow("compress") << SshError::kCompress;
}

void TestSshSessionHealth::aTransportFailureEndsTheSession()
{
    QFETCH(int, code);
    QVERIFY(sshErrorEndsSession(code));

    SessionHealth health;
    health.noteError(code);
    QVERIFY(health.dead());
}

// The other half, and the one that costs something if it is got wrong: everything the far
// end says about a REQUEST arrives on a healthy link. A log that is merely missing answers
// LIBSSH2_ERROR_SFTP_PROTOCOL (the FX code is fetched separately), and condemning the
// session for it would tear a working connection down once a second on a path that is
// simply not there yet — which is the state M13 exists to wait in.
void TestSshSessionHealth::aRequestFailingLeavesTheLinkAlone_data()
{
    QTest::addColumn<int>("code");

    QTest::newRow("none") << SshError::kNone;
    QTest::newRow("sftp protocol") << SshError::kSftpProtocol;
    QTest::newRow("eagain") << SshError::kEagain;
    QTest::newRow("authentication failed") << SshError::kAuthenticationFailed;
    QTest::newRow("channel failure") << SshError::kChannelFailure;
    QTest::newRow("channel request denied") << SshError::kChannelRequestDenied;
    QTest::newRow("channel closed") << SshError::kChannelClosed;
    QTest::newRow("channel eof sent") << SshError::kChannelEofSent;
    QTest::newRow("request denied") << SshError::kRequestDenied;
    QTest::newRow("alloc") << SshError::kAlloc;
    QTest::newRow("bad use") << SshError::kBadUse;
}

void TestSshSessionHealth::aRequestFailingLeavesTheLinkAlone()
{
    QFETCH(int, code);
    QVERIFY(!sshErrorEndsSession(code));

    SessionHealth health;
    health.noteError(code);
    QVERIFY(!health.dead());
}

// The default sits on the "still alive" side deliberately. A code this classification has
// never heard of is far likelier to be a new thing a server can say about a file than a
// new way for a socket to die, and guessing the other way answers it by dropping a working
// connection — on a poll that runs once a second.
void TestSshSessionHealth::anUnknownCodeIsNotTakenAsADroppedLink()
{
    QVERIFY(!sshErrorEndsSession(-9999));
    QVERIFY(!sshErrorEndsSession(12345));
}

void TestSshSessionHealth::aFreshSessionIsAlive()
{
    SessionHealth health;
    QVERIFY(!health.dead());
}

// Clearing is markAlive()'s alone. After the link has gone, a call that merely FAILED
// DIFFERENTLY — a channel read that returned zero, an SFTP status code, a would-block —
// says nothing about the far end being there and must not un-condemn the session: every
// one of those arrives just as readily on a socket that is no longer connected to
// anything. What does clear it is the next case.
void TestSshSessionHealth::aNonTerminalCodeDoesNotClearTheLatch()
{
    SessionHealth health;
    health.noteError(SshError::kSocketRecv);
    QVERIFY(health.dead());

    health.noteError(SshError::kNone);
    health.noteError(SshError::kSftpProtocol);
    QVERIFY(health.dead());
}

// A call that got a whole answer back out of the far end, which is two sites in
// SshSession.cpp and both matter. connectTo() probes for SFTP, reads a plain TIMEOUT from
// a server that accepts the subsystem channel with no sftp-server behind it, then runs a
// shell command on that same session, finds it perfectly healthy and falls back to the
// exec transport (§6.3.1) — without the clear, the probe would condemn the session the
// fallback just settled on, leaving isConnected() false on exactly the servers the
// fallback is for. And statPath(), the one round trip pollOnce() makes every turn: the
// flag is set from calls that latch WITHOUT failing their caller, so an unclearable one
// would sit there unnoticed and then fire on the next benign stat failure — a full
// reconnect, and a re-fetch from offset 0, charged to a link that never dropped.
//
// It cannot re-open bugs.md 30, because when the link is genuinely gone nothing succeeds
// and so nothing calls this. A latch is only meaningful while nothing can get an answer.
void TestSshSessionHealth::aCallThatGotAnAnswerClearsTheLatch()
{
    SessionHealth health;
    health.noteError(SshError::kTimeout);
    QVERIFY(health.dead());

    health.markAlive();
    QVERIFY(!health.dead());

    // And the latch still works afterwards: the link that comes back can go away again.
    health.noteError(SshError::kSocketDisconnect);
    QVERIFY(health.dead());
}

QTEST_GUILESS_MAIN(TestSshSessionHealth)
#include "tst_sshsessionhealth.moc"
