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

#include "FakeTelnetd.h"

using namespace fake_telnetd;


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
