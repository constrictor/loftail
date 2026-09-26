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

// A log on a device, read over a real QSerialPort — with no hardware, no docker and no root
// (ARCHITECTURE.md §6.11).
//
// openpty() gives a master/slave pair; loftail opens the slave as an ordinary serial port and
// this test IS the device on the other end. What it runs is not an imitation: the framed line
// loftail writes is handed to a REAL /bin/sh, so the markers, the exit status, `stat`, `tail`,
// `head` and `base64` are all the genuine article. The only thing faked is which tools the
// board HAS, and that is done by controlling PATH rather than by intercepting commands.
//
// That makes serial the first transport in this tree with automated coverage of its own
// conversation, where the SSH exec transport needed six containers and shipped three milestones
// of code that had only ever been compiled.
//
// THE CAVEAT, worded as tst_sshexec/tst_sshlive's split already is: A PTY IS NOT A UART. Baud,
// parity and flow control are no-ops on one. So this proves the CONVERSATION, the FRAMING, the
// LADDERS, the byte safety, the reboot recovery and the shared session — and proves nothing
// whatever about the line settings, which need a board.

#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

#include <cstdio>

#include <atomic>
#include <thread>

#include "SerialDevice.h"
#include "SerialFetchOptions.h"
#include "SerialProfile.h"
#include "SshExecCommands.h"
#include "SshPrompter.h"

using namespace loftail;

namespace {

// The far end, driven from a std::thread and NEVER a QTimer: a timer fires only if the code
// under test turns an event loop, which is the very thing being established rather than
// something a fixture may assume (tst_sshconnect's stall-slot rule).
class FakeBoard
{
public:
    FakeBoard(int master, QString toolsDir, QString scriptDir)
        : m_master(master)
        , m_toolsDir(std::move(toolsDir))
        , m_scriptDir(std::move(scriptDir))
    {
    }

    void setPassword(const QByteArray &p) { m_password = p; }
    void setAlreadyLoggedIn(bool in) { m_loggedIn = in; }
    void setMangles(bool m) { m_mangles = m; }
    void setChatty(bool c) { m_chatty = c; }
    void rebootNow() { m_reboot = true; }
    void setStickyMangling(bool sticky) { m_stickyMangling = sticky; }

    void start()
    {
        m_running = true;
        m_thread = std::thread([this] { loop(); });
    }

    void stop()
    {
        m_running = false;
        if (m_thread.joinable())
            m_thread.join();
    }

private:
    void say(QByteArray text)
    {
        if (m_mangles)
            text.replace('\n', "\r\n");
        ::write(m_master, text.constData(), size_t(text.size()));
    }

    void loop()
    {
        QByteArray line;
        QElapsedTimer chatter;
        chatter.start();
        while (m_running) {
            if (m_reboot.exchange(false)) {
                // The pattern the preset looks for, and then back to the login prompt: a board
                // that restarts under a tab is the case the whole reboot rule exists for.
                say(QByteArrayLiteral("\nU-Boot 2023.04\n[    0.000000] Linux version 6.1.0\n"
                                      "myboard login: "));
                m_loggedIn = false;
                m_awaitingPassword = false;
                line.clear();
                continue;
            }
            if (m_chatty && chatter.elapsed() > 120) {
                chatter.restart();
                // Requirement 7: a device runs other things and they print to the same console.
                say(QByteArrayLiteral("[   12.345678] usb 1-1: new high-speed device\n"));
            }

            char buf[8192];
            const ssize_t n = ::read(m_master, buf, sizeof(buf));
            if (n <= 0) {
                QThread::msleep(2);
                continue;
            }
            for (ssize_t i = 0; i < n; ++i) {
                const char c = buf[i];
                if (c != '\n' && c != '\r') {
                    line.append(c);
                    continue;
                }
                const QByteArray typed = line;
                line.clear();
                handleLine(typed);
            }
        }
    }

    void handleLine(const QByteArray &typed)
    {
        if (!m_loggedIn) {
            if (m_awaitingPassword) {
                m_awaitingPassword = false;
                if (typed == m_password) {
                    m_loggedIn = true;
                    say(QByteArrayLiteral("\nWelcome to myboard\n# "));
                } else {
                    say(QByteArrayLiteral("\nLogin incorrect\nmyboard login: "));
                }
                return;
            }
            if (typed.isEmpty()) {
                say(QByteArrayLiteral("\nmyboard login: "));
                return;
            }
            if (m_password.isEmpty()) {
                m_loggedIn = true;
                say(QByteArrayLiteral("\nWelcome to myboard\n# "));
            } else {
                m_awaitingPassword = true;
                say(QByteArrayLiteral("\nPassword: "));
            }
            return;
        }

        if (typed.isEmpty()) {
            say(QByteArrayLiteral("\n# "));
            return;
        }

        // A REAL SHELL RUNS IT. The frame, both markers, the exit status and every tool in the
        // pipeline are therefore genuine — which is what makes this test able to fail when the
        // commands the transport composes are wrong, rather than only when the fixture's idea
        // of them is.
        // popen() AND NOT QProcess, and that is forced rather than chosen: QProcess drives its
        // child through socket notifiers, which require a QThread — and this thread must stay a
        // std::thread, because a QTimer or a QThread here would fire only if the code under test
        // turns an event loop, which is the very thing being established. CLAUDE.md records the
        // same trade one level over: "a test whose own load generator needs joining uses
        // std::thread".
        //
        // PATH IS HOW "THIS BOARD HAS NO base64" IS EXPRESSED. Nothing intercepts a command; the
        // board simply does not have the tool, exactly as a stripped-down image does not.
        // THE SCRIPT GOES TO A FILE AND STDIN COMES FROM /dev/null, and both halves are there
        // because of how this failed. Passed as `sh -c '<quoted>'`, any quoting slip leaves the
        // shell reading standard input — which it INHERITS from the test — and it then blocks
        // for ever: a HUNG fixture rather than a failing one, which is the worst shape a test can
        // take. A file needs no quoting at all, and `</dev/null` makes the failure impossible
        // rather than merely unlikely.
        const QString scriptPath = m_scriptDir + QStringLiteral("/line.sh");
        {
            QFile f(scriptPath);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return;
            f.write(typed);
            f.write("\n");
        }
        const QByteArray command = "PATH=" + m_toolsDir.toLocal8Bit() + " /bin/sh "
            + scriptPath.toLocal8Bit() + " </dev/null 2>/dev/null";
        QByteArray out;
        if (FILE *pipe = ::popen(command.constData(), "r")) {
            char chunk[4096];
            size_t got = 0;
            while ((got = ::fread(chunk, 1, sizeof(chunk), pipe)) > 0)
                out.append(chunk, int(got));
            ::pclose(pipe);
        }

        // `stty raw -echo` is what stops the mangling, and it is honoured HERE rather than in
        // the shim, because a tty's line discipline is the tty's — the shim exists only so the
        // command does not fail for want of a terminal.
        if (typed.contains("stty raw -echo") && !m_stickyMangling)
            m_mangles = false;

        say("\n");
        say(out);
        say(QByteArrayLiteral("# "));
    }

    int               m_master;
    QString           m_toolsDir;
    QString           m_scriptDir;
    QByteArray        m_password = QByteArrayLiteral("hunter2");
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_reboot{false};
    bool              m_loggedIn = false;
    bool              m_awaitingPassword = false;
    bool              m_mangles = false;
    bool              m_stickyMangling = false;
    bool              m_chatty = false;
    std::thread       m_thread;
};

} // namespace

class TestSerialDevice : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int           m_master = -1;
    int           m_slave = -1;
    QString       m_slaveNode;

    // A PATH holding exactly the tools this board is supposed to have. Real binaries, found
    // where they really are, so nothing about the commands is mocked.
    QString makeToolsDir(const QStringList &tools, bool withStty = true)
    {
        const QString dir = m_dir.filePath(QStringLiteral("tools-%1").arg(tools.join(u'-')
                                                                         + (withStty ? u'y' : u'n')));
        QDir().mkpath(dir);
        QStringList wanted = tools;
        if (withStty)
            wanted.append(QStringLiteral("stty"));
        for (const QString &tool : wanted) {
            for (const QString &from : {QStringLiteral("/bin/"), QStringLiteral("/usr/bin/")}) {
                const QString real = from + tool;
                if (QFileInfo::exists(real)) {
                    QFile::link(real, dir + u'/' + tool);
                    break;
                }
            }
        }
        // `stty` on a pipe fails for want of a terminal, which would make the probe read the
        // failure as "raw mode did not take". A one-line shim that exits 0 stands in for the
        // real thing; whether the mangling actually STOPS is the board's own business above.
        if (withStty) {
            QFile::remove(dir + QStringLiteral("/stty"));
            QFile shim(dir + QStringLiteral("/stty"));
            if (shim.open(QIODevice::WriteOnly)) {
                shim.write("#!/bin/sh\nexit 0\n");
                shim.close();
                shim.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            }
        }
        return dir;
    }

    QString writeLog(const QString &name, const QByteArray &bytes)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return {};
        f.write(bytes);
        f.close();
        return path;
    }

    static SerialFetchOptions optionsFor()
    {
        SerialFetchOptions o;
        o.profile = SerialProfile::builtIn();
        o.profile.name = QStringLiteral("board");
        o.profile.promptTimeoutMs = 700;
        o.profile.attempts = 4;
        o.profile.rebootRe = QStringLiteral("U-Boot|Linux version");
        return o;
    }

    RemoteLocation locationFor(const QString &logPath) const
    {
        const auto loc = RemoteLocation::parse(
            QStringLiteral("serial://root@%1%2").arg(QStringLiteral("ttyTEST0"), logPath));
        return loc.value_or(RemoteLocation{});
    }

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void aLogIsReadThroughAPtyPair();
    void aLoginConversationSurvivesKernelNoise();
    void aDeviceAlreadyAtAShellNeedsNoPassword();
    void aWrongPasswordIsRefusedRatherThanRetriedForEver();
    void aMangingLineFallsBackToBase64AndTheBytesStillMatch();
    void aBoardWithNeitherRawNorBase64IsRefusedByName();
    void aRebootIsNoticedAndSignedInToAgain();
    void twoLogsOnOneDeviceShareOneLogin();
    void aRequestFailureOnOneLogLeavesTheDeviceUsable();
    void theChunkAndTheDeadlineComeFromTheBaud();
};

void TestSerialDevice::initTestCase()
{
    QVERIFY(m_dir.isValid());
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("no /bin/sh");
}

void TestSerialDevice::init()
{
    char name[256] = {0};
    if (::openpty(&m_master, &m_slave, name, nullptr, nullptr) != 0)
        QSKIP("openpty() is not available here");
    m_slaveNode = QString::fromLocal8Bit(name);
    // NON-BLOCKING ON THE MASTER, and this is not a nicety: a blocking ::read() there never
    // returns on a quiet line, so the board thread sits in it and stop() joins for ever — a HUNG
    // test rather than a failing one, which is the worst shape a fixture can take. (It was
    // exactly that for three runs.)
    ::fcntl(m_master, F_SETFL, ::fcntl(m_master, F_GETFL, 0) | O_NONBLOCK);
    // RAW ON THIS END, so the pair carries bytes rather than interpreting them: what the board
    // does to the bytes is the board's business, and a second line discipline in the middle
    // would make the mangling case test the fixture instead of the ladder.
    struct termios t{};
    ::tcgetattr(m_slave, &t);
    ::cfmakeraw(&t);
    ::tcsetattr(m_slave, TCSANOW, &t);
    ::tcgetattr(m_master, &t);
    ::cfmakeraw(&t);
    ::tcsetattr(m_master, TCSANOW, &t);

    clearSerialFetchOptions();
    SshCredentialCache::clear();
    // THE PTY REACHED THROUGH THE REGISTRY'S OWN SEAM, which is what makes the device NAME an
    // ordinary one: the address under test is `serial://root@ttyTEST0/...`, so it parses, keys
    // and displays exactly as a real device's would, and only where the node lives is faked.
    const QString node = m_slaveNode;
    SerialDeviceRegistry::instance().setNodeResolver([node](const QString &name) {
        return name == QStringLiteral("ttyTEST0") ? node : QString();
    });
}

void TestSerialDevice::cleanup()
{
    // The registry first, so the device closes the port before the pair goes — and the resolver
    // with it, or the next case's device is wired to a pty that has been closed.
    SerialDeviceRegistry::instance().setNodeResolver(nullptr);
    SerialDeviceRegistry::instance().clear();
    clearSerialFetchOptions();
    SshCredentialCache::clear();
    if (m_slave >= 0)
        ::close(m_slave);
    if (m_master >= 0)
        ::close(m_master);
    m_slave = m_master = -1;
}

void TestSerialDevice::aLogIsReadThroughAPtyPair()
{
    const QByteArray content = QByteArrayLiteral(
        "2026-01-01 10:00:00 INFO  [main] started\n"
        "2026-01-01 10:00:01 WARN  [main] something\n"
        "2026-01-01 10:00:02 ERROR [main] and then this\n");
    const QString log = writeLog(QStringLiteral("app.log"), content);
    QVERIFY(!log.isEmpty());

    FakeBoard board(m_master, makeToolsDir({QStringLiteral("stat"), QStringLiteral("tail"),
                                           QStringLiteral("head"), QStringLiteral("base64"),
                                           QStringLiteral("printf"), QStringLiteral("test"),
                                           QStringLiteral("ls"), QStringLiteral("wc")}), m_dir.path());
    board.start();

    auto device = SerialDeviceRegistry::instance().acquire(QStringLiteral("ttyTEST0"));
    SshCredentialCache::remember(locationFor(log).target(), QStringLiteral("hunter2"));

    SerialDevice::Ready ready;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 20000) {
        ready = device->ensureReady(log, optionsFor(), locationFor(log),
                                    QByteArrayLiteral("hunter2"));
        if (ready.ok || ready.refused)
            break;
        QTest::qWait(50);
    }
    QVERIFY2(ready.ok, qUtf8Printable(ready.reason));

    // The log's bytes, byte for byte, through a real framed read over a real port.
    QByteArray got;
    got.resize(int(content.size()));
    const qint64 n = device->readBytes(log, 0, got.data(), content.size());
    QCOMPARE(n, qint64(content.size()));
    QCOMPARE(got, content);

    board.stop();
}

void TestSerialDevice::aLoginConversationSurvivesKernelNoise()
{
    // Requirement 7, end to end: the board prints kernel messages throughout, and the login has
    // to find its prompts in amongst them.
    const QString log = writeLog(QStringLiteral("noisy.log"), QByteArrayLiteral("a line\n"));
    FakeBoard board(m_master, makeToolsDir({QStringLiteral("stat"), QStringLiteral("tail"),
                                           QStringLiteral("head"), QStringLiteral("base64"),
                                           QStringLiteral("printf"), QStringLiteral("test")}), m_dir.path());
    board.setChatty(true);
    board.start();

    auto device = SerialDeviceRegistry::instance().acquire(QStringLiteral("ttyTEST0"));
    SerialDevice::Ready ready;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 25000) {
        ready = device->ensureReady(log, optionsFor(), locationFor(log),
                                    QByteArrayLiteral("hunter2"));
        if (ready.ok || ready.refused)
            break;
        QTest::qWait(50);
    }
    QVERIFY2(ready.ok, qUtf8Printable(ready.reason));

    // And a command still answers correctly with the noise continuing underneath it — which is
    // the framing's half of the same requirement.
    QByteArray out;
    QVERIFY(device->run(QStringLiteral("printf 'the-answer\\n'"), &out));
    QVERIFY2(out.contains(QByteArrayLiteral("the-answer")), out.constData());
    QVERIFY2(!out.contains(QByteArrayLiteral("usb 1-1")), out.constData());

    board.stop();
}

void TestSerialDevice::aDeviceAlreadyAtAShellNeedsNoPassword()
{
    // THE COMMONEST REAL CASE: a board left logged in prints nothing at all until spoken to.
    const QString log = writeLog(QStringLiteral("shell.log"), QByteArrayLiteral("x\n"));
    FakeBoard board(m_master, makeToolsDir({QStringLiteral("stat"), QStringLiteral("tail"),
                                           QStringLiteral("head"), QStringLiteral("base64"),
                                           QStringLiteral("printf"), QStringLiteral("test")}), m_dir.path());
    board.setAlreadyLoggedIn(true);
    board.start();

    auto device = SerialDeviceRegistry::instance().acquire(QStringLiteral("ttyTEST0"));
    SerialDevice::Ready ready;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 20000) {
        // NO PASSWORD AT ALL is passed, which is the point: the marker round trip is what
        // establishes there is a shell, and nothing had to be sent to find out.
        ready = device->ensureReady(log, optionsFor(), locationFor(log), QByteArray());
        if (ready.ok || ready.refused)
            break;
        QTest::qWait(50);
    }
    QVERIFY2(ready.ok, qUtf8Printable(ready.reason));
    board.stop();
}

void TestSerialDevice::aWrongPasswordIsRefusedRatherThanRetriedForEver()
{
    // The likeliest configuration mistake there is, and a getty answers it by printing the login
    // prompt again — which has to read as a refusal and not as another turn of the conversation.
    const QString log = writeLog(QStringLiteral("locked.log"), QByteArrayLiteral("x\n"));
    FakeBoard board(m_master, makeToolsDir({QStringLiteral("stat"), QStringLiteral("tail"),
                                           QStringLiteral("head"), QStringLiteral("printf"),
                                           QStringLiteral("test")}), m_dir.path());
    board.setPassword(QByteArrayLiteral("correct-horse"));
    board.start();

    auto device = SerialDeviceRegistry::instance().acquire(QStringLiteral("ttyTEST0"));
    SerialDevice::Ready ready;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 20000) {
        ready = device->ensureReady(log, optionsFor(), locationFor(log),
                                    QByteArrayLiteral("wrong"));
        if (ready.refused)
            break;
        QTest::qWait(50);
    }
    QVERIFY2(ready.refused, qUtf8Printable(ready.reason));
    QVERIFY(!ready.reason.isEmpty());
    // And the sentence never carries the credential it was refused over.
    QVERIFY(!ready.reason.contains(QStringLiteral("wrong")));
    board.stop();
}

void TestSerialDevice::aMangingLineFallsBackToBase64AndTheBytesStillMatch()
{
    // THE CASE THE BYTE-SAFETY LADDER EXISTS FOR, end to end. The board's line turns every LF
    // into CR-LF and refuses to stop doing it, so raw cannot survive — and the log still has to
    // come back byte for byte.
    const QByteArray content = QByteArrayLiteral("one\ntwo\nthree\n");
    const QString log = writeLog(QStringLiteral("crlf.log"), content);
    FakeBoard board(m_master, makeToolsDir({QStringLiteral("stat"), QStringLiteral("tail"),
                                           QStringLiteral("head"), QStringLiteral("base64"),
                                           QStringLiteral("printf"), QStringLiteral("test")}), m_dir.path());
    board.setMangles(true);
    board.setStickyMangling(true);   // `stty raw` is accepted and does nothing, as busybox may
    board.start();

    auto device = SerialDeviceRegistry::instance().acquire(QStringLiteral("ttyTEST0"));
    SerialDevice::Ready ready;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 25000) {
        ready = device->ensureReady(log, optionsFor(), locationFor(log),
                                    QByteArrayLiteral("hunter2"));
        if (ready.ok || ready.refused)
            break;
        QTest::qWait(50);
    }
    QVERIFY2(ready.ok, qUtf8Printable(ready.reason));
    // Settled on the encoder, because raw demonstrably did not survive.
    QCOMPARE(device->byteSafety(), ByteSafety::Base64);

    QByteArray got;
    got.resize(int(content.size()));
    const qint64 n = device->readBytes(log, 0, got.data(), content.size());
    QCOMPARE(n, qint64(content.size()));
    QCOMPARE(got, content);   // byte for byte, over a line that corrupts everything raw
    board.stop();
}

void TestSerialDevice::aBoardWithNeitherRawNorBase64IsRefusedByName()
{
    // Nothing to be done, so it says so rather than reading the log wrongly — a refusal that
    // keeps its tab and explains itself (M17). Reading a log through a mangling line is worse
    // than not reading it, because nothing about the result says it was mangled.
    const QString log = writeLog(QStringLiteral("hopeless.log"), QByteArrayLiteral("one\ntwo\n"));
    FakeBoard board(m_master, makeToolsDir({QStringLiteral("stat"), QStringLiteral("tail"),
                                           QStringLiteral("head"), QStringLiteral("printf"),
                                           QStringLiteral("test")}), m_dir.path());
    board.setMangles(true);
    board.setStickyMangling(true);
    board.start();

    auto device = SerialDeviceRegistry::instance().acquire(QStringLiteral("ttyTEST0"));
    SerialDevice::Ready ready;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 25000) {
        ready = device->ensureReady(log, optionsFor(), locationFor(log),
                                    QByteArrayLiteral("hunter2"));
        if (ready.ok || ready.refused)
            break;
        QTest::qWait(50);
    }
    QVERIFY2(ready.refused, qUtf8Printable(ready.reason));
    QVERIFY2(ready.reason.contains(QStringLiteral("base64")), qUtf8Printable(ready.reason));
    board.stop();
}

void TestSerialDevice::aRebootIsNoticedAndSignedInToAgain()
{
    // A board that restarts under a tab, which is what the reboot pattern is for — and the case
    // the user's own correction settled: it behaves like an SSH reconnect.
    const QString log = writeLog(QStringLiteral("boot.log"), QByteArrayLiteral("before\n"));
    FakeBoard board(m_master, makeToolsDir({QStringLiteral("stat"), QStringLiteral("tail"),
                                           QStringLiteral("head"), QStringLiteral("base64"),
                                           QStringLiteral("printf"), QStringLiteral("test")}), m_dir.path());
    board.start();

    auto device = SerialDeviceRegistry::instance().acquire(QStringLiteral("ttyTEST0"));
    SerialDevice::Ready ready;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 20000) {
        ready = device->ensureReady(log, optionsFor(), locationFor(log),
                                    QByteArrayLiteral("hunter2"));
        if (ready.ok || ready.refused)
            break;
        QTest::qWait(50);
    }
    QVERIFY2(ready.ok, qUtf8Printable(ready.reason));
    QVERIFY(!device->takeReboot());

    board.rebootNow();
    // The next command sees it. Which command notices is not the point — that it is noticed at
    // all is, because Ready is the state a reboot actually happens in.
    bool noticed = false;
    clock.restart();
    while (clock.elapsed() < 15000 && !noticed) {
        QByteArray out;
        device->run(QStringLiteral("printf 'alive\\n'"), &out);
        noticed = device->takeReboot();
        if (!noticed)
            QTest::qWait(50);
    }
    QVERIFY2(noticed, "the reboot was never noticed");

    // And it signs in again on its own, which is the whole of "like an SSH reconnect".
    clock.restart();
    ready = SerialDevice::Ready{};
    while (clock.elapsed() < 25000) {
        ready = device->ensureReady(log, optionsFor(), locationFor(log),
                                    QByteArrayLiteral("hunter2"));
        if (ready.ok || ready.refused)
            break;
        QTest::qWait(50);
    }
    QVERIFY2(ready.ok, qUtf8Printable(ready.reason));
    board.stop();
}

void TestSerialDevice::twoLogsOnOneDeviceShareOneLogin()
{
    // ONE DEVICE, ONE SESSION, N LOGS. A serial line permits exactly one opener, so the
    // alternative to sharing is two QSerialPort::open() calls on one node — either the second
    // fails, or both succeed and two command streams interleave on one line.
    const QByteArray a = QByteArrayLiteral("log a line one\nlog a line two\n");
    const QByteArray b = QByteArrayLiteral("log b only line\n");
    const QString logA = writeLog(QStringLiteral("a.log"), a);
    const QString logB = writeLog(QStringLiteral("b.log"), b);

    FakeBoard board(m_master, makeToolsDir({QStringLiteral("stat"), QStringLiteral("tail"),
                                           QStringLiteral("head"), QStringLiteral("base64"),
                                           QStringLiteral("printf"), QStringLiteral("test")}), m_dir.path());
    board.start();

    // The SAME handle for both, which is what the registry guarantees.
    auto first = SerialDeviceRegistry::instance().acquire(QStringLiteral("ttyTEST0"));
    auto second = SerialDeviceRegistry::instance().acquire(QStringLiteral("ttyTEST0"));
    QCOMPARE(first.get(), second.get());

    SerialDevice::Ready ready;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 20000) {
        ready = first->ensureReady(logA, optionsFor(), locationFor(logA),
                                   QByteArrayLiteral("hunter2"));
        if (ready.ok || ready.refused)
            break;
        QTest::qWait(50);
    }
    QVERIFY2(ready.ok, qUtf8Printable(ready.reason));

    // The second log needs NO second login: it is already signed in, because the session
    // belongs to the device rather than to a log.
    const SerialDevice::Ready again =
        second->ensureReady(logB, optionsFor(), locationFor(logB), QByteArray());
    QVERIFY2(again.ok, qUtf8Printable(again.reason));

    // And both logs read their OWN bytes, which is the thing interleaved streams would destroy.
    QByteArray gotA;
    gotA.resize(int(a.size()));
    QCOMPARE(first->readBytes(logA, 0, gotA.data(), a.size()), qint64(a.size()));
    QCOMPARE(gotA, a);

    QByteArray gotB;
    gotB.resize(int(b.size()));
    QCOMPARE(second->readBytes(logB, 0, gotB.data(), b.size()), qint64(b.size()));
    QCOMPARE(gotB, b);

    board.stop();
}

void TestSerialDevice::aRequestFailureOnOneLogLeavesTheDeviceUsable()
{
    // THE WHOLE RISK THE SHARED SESSION TAKES ON, and the answer is the tree's existing split
    // between the LINK and the REQUEST. One log being missing is a fact about that log; it must
    // not condemn a session the other log is tailing, or a reader watching two logs on one board
    // loses both whenever either goes away.
    const QByteArray content = QByteArrayLiteral("still here\n");
    const QString present = writeLog(QStringLiteral("present.log"), content);
    const QString missing = m_dir.filePath(QStringLiteral("not-there.log"));

    FakeBoard board(m_master, makeToolsDir({QStringLiteral("stat"), QStringLiteral("tail"),
                                           QStringLiteral("head"), QStringLiteral("base64"),
                                           QStringLiteral("printf"), QStringLiteral("test")}), m_dir.path());
    board.start();

    auto device = SerialDeviceRegistry::instance().acquire(QStringLiteral("ttyTEST0"));
    SerialDevice::Ready ready;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 20000) {
        ready = device->ensureReady(present, optionsFor(), locationFor(present),
                                    QByteArrayLiteral("hunter2"));
        if (ready.ok || ready.refused)
            break;
        QTest::qWait(50);
    }
    QVERIFY2(ready.ok, qUtf8Printable(ready.reason));

    // A stat of a log that is not there: the command RAN and printed nothing, which is
    // RunCommand's "true with empty output" — a request failure.
    QByteArray out;
    QVERIFY2(device->run(statCommand(missing), &out),
             "a missing log was reported as a dead channel");
    QVERIFY(lastNonEmptyLine(out).isEmpty());

    // And the device is still perfectly usable for the log that IS there.
    QByteArray got;
    got.resize(int(content.size()));
    QCOMPARE(device->readBytes(present, 0, got.data(), content.size()), qint64(content.size()));
    QCOMPARE(got, content);
    QVERIFY(!device->takeReboot());

    board.stop();
}

void TestSerialDevice::theChunkAndTheDeadlineComeFromTheBaud()
{
    // DERIVED, NOT WRITTEN DOWN. SSH reads 1 MB at a time because at network speed that is about
    // a second; a chunk is one step of committedSize, so it is also how coarsely a catch-up
    // fills the view. At 115200 baud one second is eleven kilobytes, so borrowing 1 MB here
    // would freeze the record count for a minute and a half per chunk.
    QVERIFY(serialChunkBytes(115200) < 65536);
    QVERIFY(serialChunkBytes(115200) > 4096);
    // Slower line, smaller chunk — the relation is the claim, never a particular number.
    QVERIFY(serialChunkBytes(9600) < serialChunkBytes(115200));
    // And both are bounded, so neither a 300-baud line nor a 4-megabaud one is absurd.
    QCOMPARE(serialChunkBytes(300), 4096LL);
    QCOMPARE(serialChunkBytes(4000000), 65536LL);

    // The deadline scales with the bytes AND with the line, because a fixed timeout cannot work:
    // at 9600 baud a chunk legitimately takes a minute, and a deadline shorter than the transfer
    // turns every read into a retry that will also time out.
    QVERIFY(serialReadDeadlineMs(115200, 65536) > serialReadDeadlineMs(115200, 4096));
    QVERIFY(serialReadDeadlineMs(9600, 65536) > serialReadDeadlineMs(115200, 65536));
    // A zero-byte read still gets a round trip's worth.
    QVERIFY(serialReadDeadlineMs(115200, 0) >= 5000);
}

QTEST_MAIN(TestSerialDevice)
#include "tst_serialdevice.moc"
