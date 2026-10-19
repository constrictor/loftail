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

// A telnetd with a login and a real /bin/sh behind it, on loopback, from a std::thread — the
// far end tst_telnetsession and tst_discoveredfetcher read logs from. Its rules are
// tst_serialdevice's: a std::thread and never a QThread or QTimer (a timer fires only if the
// code under test turns an event loop), popen() for the shell, and PATH deciding which tools
// the machine has.

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QPair>
#include <QString>
#include <QThread>

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

namespace fake_telnetd {

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
        m_raw = false;
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
                // LIKE A REAL PTY BEHIND A REAL telnetd. Until `stty raw` a line ends at CR or LF
                // (the terminal's icrnl turns a CR into a newline) and CR LF / CR NUL count once.
                // AFTER it, only LF ends a line and a CR is an ordinary byte of it — which is what
                // made a client sending CR LF put a stray `\r` on every command's last word, a
                // fault this fixture could not show while it treated the two alike.
                if (m_raw) {
                    if (c == '\n') {
                        const QByteArray typed = line;
                        line.clear();
                        handleLine(typed);
                        continue;
                    }
                    line.append(char(c));
                    continue;
                }
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
        if (typed.contains("stty raw"))
            m_raw = true;   // the terminal's line discipline is gone from the next line on
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
        // The newline is the ECHO of the line just typed, and a terminal that has had
        // `stty raw -echo` echoes nothing — so the prompt this machine printed last is still
        // on the line the answer starts on, exactly as on a real console. A fixture that
        // always wrote it hid a framing defect that hung every real login (SerialFraming.cpp).
        if (!m_raw)
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
    bool               m_raw = false;   // `stty raw` has been run on the session
    bool               m_loggedIn = false;
    bool               m_awaitingPassword = false;
    std::atomic<bool>  m_running{false};
    std::atomic<int>   m_connections{0};
    mutable std::mutex m_mutex;
    QList<QPair<int, int>> m_received;
    std::thread        m_thread;
};

} // namespace fake_telnetd

using fake_telnetd::FakeTelnetd;
