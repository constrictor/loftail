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

// A log read over a real TCP telnet connection into a real /bin/sh (ARCHITECTURE.md §6.12).
//
// tst_serialdevice's twin, and built to its rules: the far end is a std::thread and never a
// QThread or QTimer (a timer fires only if the code under test turns an event loop), the shell
// is reached through popen() for the same reason, and "this machine has no base64" is said by
// PATH rather than by intercepting a command. What it adds is the one thing a pty cannot be: a
// socket with the telnet protocol on it — IAC negotiation, a doubled 0xFF in a log's bytes,
// binary mode granted or refused, and a host that is not there at all.

#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

#include <atomic>
#include <mutex>
#include <thread>

#include "ConsoleSession.h"
#include "SerialFetchOptions.h"
#include "SerialProfile.h"
#include "SshPrompter.h"
#include "TelnetProtocol.h"
#include "TelnetSession.h"

using namespace loftail;

namespace {

constexpr unsigned char IAC = 255, WILL = 251, WONT = 252, DO = 253, DONT = 254;
constexpr unsigned char BINARY = 0, ECHO_ = 1, SGA = 3, TTYPE = 24;

// A telnetd with a login and a shell, on loopback, from a std::thread.
class FakeTelnetd
{
public:
    FakeTelnetd(QString toolsDir, QString scriptDir)
        : m_toolsDir(std::move(toolsDir)), m_scriptDir(std::move(scriptDir))
    {
        m_listen = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        ::bind(m_listen, reinterpret_cast<sockaddr *>(&a), sizeof a);
        ::listen(m_listen, 4);
        socklen_t len = sizeof a;
        ::getsockname(m_listen, reinterpret_cast<sockaddr *>(&a), &len);
        m_port = ntohs(a.sin_port);
    }
    ~FakeTelnetd()
    {
        stop();
        ::close(m_listen);
    }

    int port() const { return m_port; }
    void setPassword(const QByteArray &p) { m_password = p; }
    void setAllowBinary(bool b) { m_allowBinary = b; }

    // Every option command the client sent, as `verb option` pairs — for asserting what it
    // agreed to and what it refused.
    QList<QPair<int, int>> received() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_received;
    }
    int connections() const { return m_connections; }

    void start()
    {
        m_running = true;
        m_thread = std::thread([this] { serve(); });
    }
    void stop()
    {
        m_running = false;
        if (m_thread.joinable())
            m_thread.join();
    }

private:
    void sendRaw(const QByteArray &bytes)
    {
        qsizetype at = 0;
        while (at < bytes.size() && m_running && m_client >= 0) {
            const ssize_t n = ::send(m_client, bytes.constData() + at, size_t(bytes.size() - at),
                                     MSG_NOSIGNAL);
            if (n > 0) {
                at += n;
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                QThread::msleep(2);
                continue;
            }
            return;
        }
    }

    // Data to the client: IAC doubled always, and — outside binary mode — a bare LF sent as
    // CR LF, the NVT rule a real telnetd's pty output gets. That is the "mangling" here: a raw
    // read through a non-binary connection does not come back as the file's bytes.
    void say(const QByteArray &text)
    {
        QByteArray out;
        for (const char c : text) {
            if (static_cast<unsigned char>(c) == IAC) {
                out.append(char(IAC));
                out.append(char(IAC));
            } else if (c == '\n' && !m_binaryOut) {
                out.append("\r\n");
            } else {
                out.append(c);
            }
        }
        sendRaw(out);
    }

    void option(unsigned char verb, unsigned char opt)
    {
        QByteArray b;
        b.append(char(IAC));
        b.append(char(verb));
        b.append(char(opt));
        sendRaw(b);
    }

    void serve()
    {
        while (m_running) {
            pollfd p{m_listen, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0)
                continue;
            m_client = ::accept(m_listen, nullptr, nullptr);
            if (m_client < 0)
                continue;
            ++m_connections;
            ::fcntl(m_client, F_SETFL, ::fcntl(m_client, F_GETFL, 0) | O_NONBLOCK);
            session();
            ::close(m_client);
            m_client = -1;
        }
    }

    void session()
    {
        m_binaryOut = false;
        m_loggedIn = false;
        m_awaitingPassword = false;
        // What a real telnetd opens with: echo and go-ahead suppression offered, and a question
        // about the terminal type the client is expected to refuse.
        option(WILL, ECHO_);
        option(WILL, SGA);
        option(DO, TTYPE);
        say("\nfakeboard login: ");

        enum { Data, Iac, Opt } state = Data;
        unsigned char verb = 0;
        bool lastCr = false;
        QByteArray line;
        while (m_running) {
            pollfd p{m_client, POLLIN, 0};
            if (::poll(&p, 1, 20) <= 0)
                continue;
            char buf[8192];
            const ssize_t n = ::recv(m_client, buf, sizeof buf, 0);
            if (n == 0)
                return;
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                return;
            }
            for (ssize_t i = 0; i < n; ++i) {
                const auto c = static_cast<unsigned char>(buf[i]);
                if (state == Iac) {
                    if (c == IAC) {
                        line.append(char(c));
                        state = Data;
                    } else if (c >= WILL && c <= DONT) {
                        verb = c;
                        state = Opt;
                    } else {
                        state = Data;
                    }
                    continue;
                }
                if (state == Opt) {
                    state = Data;
                    {
                        const std::scoped_lock lock(m_mutex);
                        m_received.append({verb, c});
                    }
                    if (c == BINARY) {
                        if (verb == DO) {
                            // The client asks us to send binary.
                            option(m_allowBinary ? WILL : WONT, BINARY);
                            m_binaryOut = m_allowBinary;
                        } else if (verb == WILL) {
                            option(m_allowBinary ? DO : DONT, BINARY);
                        }
                    }
                    continue;
                }
                if (c == IAC) {
                    state = Iac;
                    continue;
                }
                // A line ends at CR or LF, and CR LF / CR NUL count once.
                if (lastCr && (c == '\n' || c == 0)) {
                    lastCr = false;
                    continue;
                }
                lastCr = c == '\r';
                if (c == '\r' || c == '\n') {
                    const QByteArray typed = line;
                    line.clear();
                    handleLine(typed);
                    continue;
                }
                line.append(char(c));
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
                    say("\nWelcome\n# ");
                } else {
                    say("\nLogin incorrect\nfakeboard login: ");
                }
                return;
            }
            if (typed.isEmpty()) {
                say("\nfakeboard login: ");
                return;
            }
            m_awaitingPassword = true;
            say("\nPassword: ");
            return;
        }
        if (typed.isEmpty()) {
            say("\n# ");
            return;
        }
        // A REAL SHELL RUNS IT, from a file, with stdin from /dev/null — tst_serialdevice's
        // two rules, for the reason that cost that fixture three hung runs.
        const QString scriptPath = m_scriptDir + QStringLiteral("/tline.sh");
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
            while ((got = ::fread(chunk, 1, sizeof chunk, pipe)) > 0)
                out.append(chunk, int(got));
            ::pclose(pipe);
        }
        say("\n");
        say(out);
        say("# ");
    }

    QString            m_toolsDir;
    QString            m_scriptDir;
    int                m_listen = -1;
    int                m_client = -1;
    int                m_port = 0;
    QByteArray         m_password = QByteArrayLiteral("hunter2");
    bool               m_allowBinary = true;
    bool               m_binaryOut = false;
    bool               m_loggedIn = false;
    bool               m_awaitingPassword = false;
    std::atomic<bool>  m_running{false};
    std::atomic<int>   m_connections{0};
    mutable std::mutex m_mutex;
    QList<QPair<int, int>> m_received;
    std::thread        m_thread;
};

} // namespace

class TestTelnetSession : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString makeToolsDir(const QStringList &tools)
    {
        const QString dir = m_dir.filePath(QStringLiteral("tools-%1").arg(tools.join(u'-')));
        QDir().mkpath(dir);
        for (const QString &tool : tools + QStringList{QStringLiteral("stty")}) {
            for (const QString &from : {QStringLiteral("/bin/"), QStringLiteral("/usr/bin/")}) {
                const QString real = from + tool;
                if (QFileInfo::exists(real)) {
                    QFile::link(real, dir + u'/' + tool);
                    break;
                }
            }
        }
        // `stty` has no terminal under popen(); a shim that exits 0 stands in, exactly as in
        // tst_serialdevice. Whether bytes survive is decided by what the connection does to
        // them, which is the byte-safety probe's own question.
        QFile::remove(dir + QStringLiteral("/stty"));
        QFile shim(dir + QStringLiteral("/stty"));
        if (shim.open(QIODevice::WriteOnly)) {
            shim.write("#!/bin/sh\nexit 0\n");
            shim.close();
            shim.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
        return dir;
    }

    QStringList fullTools() const
    {
        return {QStringLiteral("stat"), QStringLiteral("tail"), QStringLiteral("head"),
                QStringLiteral("base64"), QStringLiteral("printf"), QStringLiteral("test"),
                QStringLiteral("ls"), QStringLiteral("wc")};
    }

    QString writeLog(const QString &name, const QByteArray &bytes)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return {};
        f.write(bytes);
        return path;
    }

    static SerialFetchOptions optionsFor()
    {
        SerialFetchOptions o;
        o.profile = SerialProfile::builtIn();
        o.profile.name = QStringLiteral("telnet-box");
        o.profile.promptTimeoutMs = 700;
        o.profile.attempts = 4;
        return o;
    }

    static RemoteLocation locationFor(int port, const QString &logPath)
    {
        return RemoteLocation::parse(
                   QStringLiteral("telnet://root@127.0.0.1:%1%2").arg(port).arg(logPath))
            .value_or(RemoteLocation{});
    }

    // ensureReady() until it settles, on a worker QThread: awaitSocketConnected() needs the
    // calling thread's event dispatcher, and the fetcher's worker is a QThread for that reason.
    static ConsoleSession::Ready readyOn(ConsoleSession *session, const QString &log,
                                         const RemoteLocation &location, const QByteArray &pw)
    {
        ConsoleSession::Ready ready;
        QThread *worker = QThread::create([&] {
            QElapsedTimer clock;
            clock.start();
            while (clock.elapsed() < 20000) {
                ready = session->ensureReady(log, optionsFor(), location, pw);
                if (ready.ok || ready.refused)
                    return;
                QThread::msleep(50);
            }
        });
        worker->start();
        worker->wait();
        delete worker;
        return ready;
    }

private slots:
    void initTestCase();
    void cleanup();

    void aLogIsReadByteForByteOverBinaryTelnet();
    void theClientRefusesWhatItDoesNotDoAndAsksForBinary();
    void aServerThatRefusesBinaryStillGivesTheFilesBytes();
    void aWrongPasswordIsRefused();
    void aHostThatIsNotListeningIsAWaitNotARefusal();
    void twoLogsOnOneHostShareOneLogin();
};

void TestTelnetSession::initTestCase()
{
    QVERIFY(m_dir.isValid());
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("no /bin/sh");
}

void TestTelnetSession::cleanup()
{
    TelnetSessionRegistry::instance().clear();
    clearSerialFetchOptions();
    SshCredentialCache::clear();
}

void TestTelnetSession::aLogIsReadByteForByteOverBinaryTelnet()
{
    // 0xFF IS IN THE FILE ON PURPOSE: it is the telnet IAC byte, so it reaches this side doubled
    // and must arrive single. A CR NUL is there too, which is two bytes of the file in binary
    // mode and a folded CR outside it.
    QByteArray content("2026-01-01 10:00:00 INFO [main] started\n");
    content.append(char(0xff));
    content.append("\r");
    content.append('\0');
    content.append("tail of the record\n");
    const QString log = writeLog(QStringLiteral("app.log"), content);

    FakeTelnetd server(makeToolsDir(fullTools()), m_dir.path());
    server.start();
    const RemoteLocation location = locationFor(server.port(), log);
    auto session = TelnetSessionRegistry::instance().acquire(location);
    const auto ready = readyOn(session.get(), log, location, QByteArrayLiteral("hunter2"));
    QVERIFY2(ready.ok, qUtf8Printable(ready.reason));
    QCOMPARE(session->byteSafety(), ByteSafety::Raw);

    QByteArray got(content.size(), '\0');
    const qint64 n = session->readBytes(log, 0, got.data(), content.size());
    QCOMPARE(n, qint64(content.size()));
    QCOMPARE(got, content);

    session.reset();
    TelnetSessionRegistry::instance().clear();
    server.stop();
}

void TestTelnetSession::theClientRefusesWhatItDoesNotDoAndAsksForBinary()
{
    const QString log = writeLog(QStringLiteral("neg.log"), "x\n");
    FakeTelnetd server(makeToolsDir(fullTools()), m_dir.path());
    server.start();
    const RemoteLocation location = locationFor(server.port(), log);
    auto session = TelnetSessionRegistry::instance().acquire(location);
    const auto ready = readyOn(session.get(), log, location, QByteArrayLiteral("hunter2"));
    QVERIFY2(ready.ok, qUtf8Printable(ready.reason));

    const auto got = server.received();
    QVERIFY(got.contains(qMakePair(int(DO), int(BINARY))));
    QVERIFY(got.contains(qMakePair(int(WILL), int(BINARY))));
    QVERIFY(got.contains(qMakePair(int(WONT), int(TTYPE))));
    QVERIFY(got.contains(qMakePair(int(DO), int(ECHO_))));
    // And the server's agreement to our binary request is not answered again.
    QCOMPARE(got.count(qMakePair(int(DO), int(BINARY))), 1);

    session.reset();
    TelnetSessionRegistry::instance().clear();
    server.stop();
}

void TestTelnetSession::aServerThatRefusesBinaryStillGivesTheFilesBytes()
{
    // A telnetd that will not do binary sends each LF as CR LF — the console mangles, and the
    // byte-safety probe, which never believes anything it has not round-tripped, has to notice
    // and fall back to base64. The log still arrives byte for byte.
    const QByteArray content("one\ntwo\nthree\n");
    const QString log = writeLog(QStringLiteral("nvt.log"), content);
    FakeTelnetd server(makeToolsDir(fullTools()), m_dir.path());
    server.setAllowBinary(false);
    server.start();
    const RemoteLocation location = locationFor(server.port(), log);
    auto session = TelnetSessionRegistry::instance().acquire(location);
    const auto ready = readyOn(session.get(), log, location, QByteArrayLiteral("hunter2"));
    QVERIFY2(ready.ok, qUtf8Printable(ready.reason));
    QCOMPARE(session->byteSafety(), ByteSafety::Base64);

    QByteArray got(content.size(), '\0');
    QCOMPARE(session->readBytes(log, 0, got.data(), content.size()), qint64(content.size()));
    QCOMPARE(got, content);

    session.reset();
    TelnetSessionRegistry::instance().clear();
    server.stop();
}

void TestTelnetSession::aWrongPasswordIsRefused()
{
    const QString log = writeLog(QStringLiteral("locked.log"), "x\n");
    FakeTelnetd server(makeToolsDir(fullTools()), m_dir.path());
    server.setPassword("correct-horse");
    server.start();
    const RemoteLocation location = locationFor(server.port(), log);
    auto session = TelnetSessionRegistry::instance().acquire(location);
    const auto ready = readyOn(session.get(), log, location, QByteArrayLiteral("wrong"));
    QVERIFY(!ready.ok);
    QVERIFY2(ready.refused, qUtf8Printable(ready.reason));

    session.reset();
    TelnetSessionRegistry::instance().clear();
    server.stop();
}

void TestTelnetSession::aHostThatIsNotListeningIsAWaitNotARefusal()
{
    // A machine that is down is a log that has not turned up (M13): the next poll tries again.
    int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(probe, reinterpret_cast<sockaddr *>(&a), sizeof a);
    socklen_t len = sizeof a;
    ::getsockname(probe, reinterpret_cast<sockaddr *>(&a), &len);
    const int port = ntohs(a.sin_port);
    ::close(probe);   // bound and released: nothing listens there now

    const RemoteLocation location = locationFor(port, QStringLiteral("/var/log/x.log"));
    auto session = TelnetSessionRegistry::instance().acquire(location);
    ConsoleSession::Ready ready;
    QThread *worker = QThread::create([&] {
        ready = session->ensureReady(location.path, optionsFor(), location, {});
    });
    worker->start();
    worker->wait();
    delete worker;
    QVERIFY(!ready.ok);
    QVERIFY(!ready.refused);
    QVERIFY(ready.waiting);
    QVERIFY2(ready.reason.contains(QString::number(port)), qUtf8Printable(ready.reason));
}

void TestTelnetSession::twoLogsOnOneHostShareOneLogin()
{
    const QString a = writeLog(QStringLiteral("a.log"), "aaaa\n");
    const QString b = writeLog(QStringLiteral("b.log"), "bbbbbb\n");
    FakeTelnetd server(makeToolsDir(fullTools()), m_dir.path());
    server.start();
    const RemoteLocation la = locationFor(server.port(), a);
    const RemoteLocation lb = locationFor(server.port(), b);
    auto first = TelnetSessionRegistry::instance().acquire(la);
    auto second = TelnetSessionRegistry::instance().acquire(lb);
    QCOMPARE(first.get(), second.get());

    QVERIFY(readyOn(first.get(), a, la, QByteArrayLiteral("hunter2")).ok);
    QVERIFY(readyOn(second.get(), b, lb, QByteArrayLiteral("hunter2")).ok);
    QCOMPARE(server.connections(), 1);

    QByteArray got(6, '\0');
    QCOMPARE(second->readBytes(b, 0, got.data(), 6), qint64(6));
    QCOMPARE(got, QByteArray("bbbbbb"));

    first.reset();
    second.reset();
    TelnetSessionRegistry::instance().clear();
    server.stop();
}

QTEST_GUILESS_MAIN(TestTelnetSession)
#include "tst_telnetsession.moc"
