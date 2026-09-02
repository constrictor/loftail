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

#include <QString>

#include <memory>
#include <mutex>
#include <vector>

namespace loftail {

// The idle connections an SSH ERRAND may take instead of connecting again
// (ARCHITECTURE.md §6.3, §6.8, §6.9).
//
// Every errand `withSshSession()` runs used to build a session of its own: a TCP connect,
// a key exchange, a host-key check and the whole authentication ladder, per errand. So
// opening a remote log's config file, saving it, and then bouncing the service was three
// full connects to a machine loftail was already talking to — the log's own `SshFetcher`
// holds a live authenticated session throughout. On a link with any latency that is most
// of the time each of those three gestures takes.
//
// WHY NOT SIMPLY SHARE THE FETCHER'S SESSION, which is the obvious answer and the wrong
// one. `SshSession` is one-thread-at-a-time by contract (SshSession.h) and the fetcher's
// is owned and driven by its own `tailLoop()` thread, which is inside libssh2 for most of
// every poll; the only call safe to make on it from elsewhere is `abort()`, and that is
// safe precisely because it shuts the socket and touches nothing else. Handing that
// session to a config or restart worker would be a plain data race on a `LIBSSH2_SESSION`,
// and it would also couple the two in the direction nobody wants: an errand that fails on
// it latches `SessionHealth` (SshSessionHealth.h), and the log's tail would then be told
// its link had dropped because a config save went wrong. The connection is what is worth
// reusing; the SESSION OBJECT is not shareable, so this holds sessions that belong to
// nobody instead.
//
// ALWAYS COMPILED, and it names no libssh2 type — the sessions go in behind
// `CachedSshSession` below. That is the rule `SshRetryPolicy`, `ExecSizeProbe` and
// `SshSessionHealth` beside it already follow: the transport is reachable in one build
// configuration on one kind of server, so a decision compiled only there is a decision
// tested only there. Here the whole of the bookkeeping — the checkout, the single
// ownership, the idle deadline, the cap and the shutdown latch — is exercised by
// `tst_sshsessioncache` with no libssh2, no network and no waiting.
//
// A CACHE HIT DOES NOT RE-VERIFY THE HOST KEY, AND THAT IS NOT AN OVERSIGHT. The key was
// verified when this very connection was established, by `SshSession::connectTo()`, and
// what is being reused is that connection rather than the address it was made from — the
// bytes still travel inside the session those checks admitted. A host whose key CHANGES
// has to restart its sshd to do it, which drops every established connection, which makes
// the cached session unusable and sends the next errand through a fresh connect and a
// fresh check. The same sentence covers authentication: a hit prompts for nothing because
// it is already signed in, so "one prompt per host" gets strictly quieter rather than
// weaker, and `SshCredentialCache` is untouched by any of this.

// What a cached connection is entitled to be handed to.
//
// The two are NOT interchangeable and the asymmetry is the point. A `Need::ExecOnly`
// connect stops after the login: it never asked the server which transport it offers, so
// it has no SFTP handle and no settled size rung, and every operation that would want one
// refuses it by name (SshSession.h). A `Need::LogTransport` connect settled all of that.
// So a Transport session can serve an ExecOnly errand — `runScript()` opens a plain exec
// channel, which needs the session and nothing else, and works in either mode — while the
// reverse would hand `readFileAt()` a session it must refuse.
//
// Which is not a curiosity: "save the config, then restart the service" is the errand pair
// this whole file exists for, and the borrow is what makes the second one free.
enum class SshSessionRole {
    // Need::ExecOnly. The login and nothing else.
    ExecOnly,
    // Need::LogTransport. A settled transport, SFTP wherever the server offers it.
    Transport,
};

// The face the cache holds a session by, so that none of the bookkeeping names libssh2.
//
// Three questions, and each one is asked at a moment the cache decides rather than left to
// a caller to remember.
class CachedSshSession
{
public:
    virtual ~CachedSshSession() = default;

    CachedSshSession(const CachedSshSession &) = delete;
    CachedSshSession &operator=(const CachedSshSession &) = delete;

    // Is this connection still worth handing to somebody? Asked on the way in AND on the
    // way out: a session can latch itself dead during an errand (which is what keeps a
    // failed one out of here), and it can be found dead later without anything having
    // touched it in between.
    virtual bool healthy() const = 0;

    // Make every later write on this connection fail at once instead of blocking.
    //
    // Called immediately before the session is destroyed, EVERYWHERE, and the reason is
    // that destroying one is a socket write: `libssh2_session_disconnect()` sends a
    // farewell packet, bounded only by the session timeout, which is the twenty seconds
    // of `kSshWorkerConnectTimeoutMs`. Two of the three places a cached session is
    // destroyed cannot afford that — a checkout sweeping an expired entry is on the
    // thread the user is waiting on, and `close()` runs on the APPLICATION thread inside
    // the shutdown drain, where a twenty-second write is a quit that appears to hang.
    // Cutting the socket first makes destruction prompt in every case for the price of the
    // far end seeing the connection drop rather than being said goodbye to, which is what
    // it sees whenever a laptop lid closes and what it has to cope with regardless.
    virtual void cutLoose() = 0;

    // Forget the errand that just finished with it.
    //
    // A worker gives its session an abandon check that captures that worker's shared
    // block (SshWorkerPool.h). Left in place, a stored session would keep a finished
    // errand's block alive for as long as it sat here and then hand the NEXT errand a
    // check that answers for somebody else's cancellation. The cache clears it on the way
    // in rather than trusting the caller to, because forgetting costs nothing visible.
    virtual void detachOwner() = 0;

protected:
    CachedSshSession() = default;
};

// A monotonic millisecond clock for the callers that have no reason to keep one. The cache
// itself takes the time as a parameter — the `ReconnectGrace`/`AlertPolicy` rule, so that
// every deadline in here is walked in microseconds by a test with no server and no wait.
qint64 sshMonotonicMs();

class SshSessionCache
{
public:
    // How long a returned connection stays reusable.
    //
    // Sixty seconds, and the number is picked from BOTH ends. It has to cover the errand
    // sequence this exists for — open a log's config, edit it, save it, restart the
    // service — which is a person clicking through three dialogs, i.e. seconds. It must
    // NOT be long enough to make a stale hit likely: what closes an idle SSH connection is
    // sshd's own idle policy (usually none), a NAT or firewall table dropping an idle flow
    // (minutes at the very least), or the host going away (in which case a fresh connect
    // would fail too). At a minute the first is comfortably covered and the second two are
    // not yet in play.
    //
    // It is also a bound on holding somebody else's socket open for nothing, which is
    // invariant #5's spirit one step out from the log itself: loftail has no business
    // keeping a connection to a machine it has finished with.
    static constexpr qint64 kDefaultIdleMs = 60000;

    // How many may be held at once. A person reads a config or restarts a service on a
    // handful of machines, and each entry is a real socket on one of them; past that the
    // oldest is let go rather than the pool being allowed to grow with the session.
    static constexpr int kDefaultCapacity = 4;

    // What a checkout answers with. Null `session` is a miss.
    //
    // `role` IS THE ROLE THE SESSION ACTUALLY HAS, not the one that was asked for — they
    // differ exactly when an ExecOnly errand borrows a Transport session — so that the
    // caller files it back under what it is and the next reader of this cache is not lied
    // to about what that connection can do.
    struct Checkout
    {
        std::unique_ptr<CachedSshSession> session;
        SshSessionRole                    role = SshSessionRole::ExecOnly;

        explicit operator bool() const { return session != nullptr; }
    };

    explicit SshSessionCache(qint64 idleMs = kDefaultIdleMs, int capacity = kDefaultCapacity);
    ~SshSessionCache();

    SshSessionCache(const SshSessionCache &) = delete;
    SshSessionCache &operator=(const SshSessionCache &) = delete;

    // Take a connection to `target` out, if there is one that can serve `need`.
    //
    // TAKING IT OUT IS THE MUTUAL EXCLUSION, and there is no other. `SshSession` is
    // one-thread-at-a-time, and `SshConnectHold` — which is what serialises two workers
    // aiming at one host today — is taken around a CONNECT, which a hit does not perform.
    // So the only thing standing between two errands and one session is that this removes
    // the entry: the second worker misses and connects, exactly as it would have before
    // any of this existed.
    //
    // Sweeps expired entries as it goes, which is the whole of how the deadline is
    // enforced on a worker thread with no event loop to hang a timer on.
    Checkout checkOut(const QString &target, SshSessionRole need, qint64 nowMs);

    // Give one back. Takes ownership either way: a session that is not `healthy()`, or
    // that arrives after `close()`, is destroyed here rather than handed back for the
    // caller to wonder about.
    void checkIn(const QString &target, SshSessionRole role,
                 std::unique_ptr<CachedSshSession> session, qint64 nowMs);

    // Let every connection go and refuse to hold any more, for good.
    //
    // Called from `drainSshWorkers()`, i.e. by the window on its way out, and the latch is
    // as load-bearing as the release: a worker that finishes after the drain has given up
    // waiting for it would otherwise check its session back in, leaving a live socket and
    // a `QTcpSocket` to be torn down after the application object has gone — which is the
    // exact SEGV `SshWorkerPool.h` records. Closed, this cache answers every checkout with
    // a miss and destroys everything checked into it, so a straggler behaves like a build
    // with no cache at all.
    void close();

    // For tests and for the diagnostic log. Neither is a promise about ordering.
    int  size() const;
    bool closed() const;

private:
    struct Entry
    {
        QString                           target;
        SshSessionRole                    role = SshSessionRole::ExecOnly;
        std::unique_ptr<CachedSshSession> session;
        qint64                            idleSinceMs = 0;
    };

    // Cut loose and destroy, which is the only way a session leaves this class. Takes the
    // entries by value so the work happens with `m_mutex` released — destroying one is a
    // socket write, and holding a lock across it would let one wedged connection block
    // every other errand in the process.
    static void discard(std::vector<Entry> going);

    // Move out everything that has sat here past its deadline. Caller holds m_mutex.
    void takeExpiredLocked(qint64 nowMs, std::vector<Entry> *going);

    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries; // in check-in order, so the front is the oldest
    qint64             m_idleMs;
    int                m_capacity;
    bool               m_closed = false;
};

// The one cache for the process, in the shape `SshCredentialCache` and the worker list
// already use. A function-local static rather than a namespace-scope one so that its
// construction is ordered by first use; by the time its destructor runs at exit,
// `drainSshWorkers()` has already closed it and it holds nothing.
SshSessionCache &sshSessionCache();

} // namespace loftail
