// Included from SshWorkerPool.h, and only in a build with SSH support.
//
// A template, so it cannot live in the .cpp; its own file rather than an inline blob in
// the header, because it is the only thing there that needs libssh2's headers in scope.

#pragma once

#include "RemoteLocation.h"
#include "SshPrompter.h"
#include "SshSession.h"
#include "SshSessionCache.h"

#include <QCoreApplication>

#include <memory>
#include <utility>

namespace loftail {

// The bound a connect gets. A config read or a restart is a DELIBERATE gesture with
// somebody waiting, so it takes the attended timeout rather than the shorter unattended
// one a background retry uses.
inline constexpr int kSshWorkerConnectTimeoutMs = 20000;

namespace detail {

// An SshSession wearing the face the cache holds one by (SshSessionCache.h).
//
// The cache is deliberately ignorant of libssh2 so that all of its bookkeeping is testable
// with no server; this is the four lines that make the ignorance affordable. Nothing else
// in the process ever puts anything into `sshSessionCache()`, which is what makes the
// static_cast on the way back out safe — and it is a static_cast rather than a dynamic one
// because that fact is a property of this file, not something to be re-checked at runtime.
class PooledSshSession final : public CachedSshSession
{
public:
    explicit PooledSshSession(std::unique_ptr<SshSession> session)
        : m_session(std::move(session))
    {
    }

    SshSession                *get() { return m_session.get(); }
    std::unique_ptr<SshSession> release() { return std::move(m_session); }

    bool healthy() const override { return m_session && m_session->isConnected(); }

    // abort(), which shuts the socket and touches nothing else — the one call on
    // SshSession that is safe from another thread, and here it is being used for its other
    // property: it makes the disconnect packet ~SshSession is about to write fail
    // immediately instead of waiting out the session timeout.
    void cutLoose() override
    {
        if (m_session)
            m_session->abort();
    }

    void detachOwner() override
    {
        if (m_session)
            m_session->setAbandonCheck(nullptr);
    }

private:
    std::unique_ptr<SshSession> m_session;
};

} // namespace detail

// `need` is REQUIRED and comes before `body` on purpose, where SshSession::connectTo()
// defaults it. There it has to default, or every fetcher call site would have to be
// touched to say what it already means; here there are a handful of callers, each one is
// a whole errand rather than a step in one, and the wrong value fails in the direction
// that costs twenty seconds or silently changes transport (SshSession.h). Making the next
// caller answer the question is worth the four words. `repeat` is there on exactly the
// same argument and fails in exactly the same way — silently, in both directions
// (SshWorkerPool.h).
template <class Body>
QString withSshSession(const QString &address, SshPrompter *prompter,
                       const std::shared_ptr<SshWorkerShared> &shared, SshSession::Need need,
                       SshErrandRepeat repeat, Body body)
{
    const auto location = RemoteLocation::parse(address);
    if (!location || !location->isValid()) {
        return QCoreApplication::translate("loftail::SshWorkerPool",
                                           "Not a valid remote address: %1")
            .arg(RemoteLocation::withoutPassword(address));
    }

    const QString        target = location->target();
    const SshSessionRole wantRole = need == SshSession::Need::ExecOnly
                                        ? SshSessionRole::ExecOnly
                                        : SshSessionRole::Transport;

    // Declared out here so that a session survives the scope the abort pointer is
    // published in: it has to be UNPUBLISHED before it is handed to the cache, or an
    // abandon() arriving a moment later would abort a session this worker no longer owns
    // — and in the worst case one another worker has already checked out and is using.
    std::unique_ptr<SshSession> session;
    SshSessionRole              heldRole = wantRole;
    QString                     error;

    {
        // Whatever happens below, the pointer must stop being publishable before the
        // session is destroyed OR given away, or a late abort() would aim at memory this
        // worker does not own any more.
        struct Unpublish
        {
            std::shared_ptr<SshWorkerShared> shared;
            ~Unpublish()
            {
                std::scoped_lock lock(shared->mutex);
                shared->session = nullptr;
            }
        } unpublish{shared};

        const auto publish = [&shared](SshSession *raw) {
            std::scoped_lock lock(shared->mutex);
            shared->session = raw;
        };
        const auto abandonCheck = [shared]() { return shared->abandoned.load(); };

        enum class Connect { Ok, Stopped, Failed };

        // A fresh connection, discarding whatever we were holding. Written once because
        // the stale-hit retry below needs the identical thing a first attempt does — a
        // second hand-written copy is a second place for the connect hold, the abandon
        // check or the publication to be forgotten.
        const auto connectFresh = [&]() -> Connect {
            publish(nullptr);
            session.reset();
            heldRole = wantRole;
            error.clear();

            // ONE CONNECT AT A TIME PER HOST, which is what keeps "one prompt per host"
            // true now that this can be in flight beside a log's own reconnect. A cache
            // HIT deliberately takes no hold at all: it asks nobody anything, so there is
            // no prompt to serialise, and the exclusion two errands actually need is the
            // checkout itself (SshSessionCache.h).
            //
            // THE HOLD NOW ENDS WITH THE CONNECT, WHERE IT USED TO SPAN THE WHOLE ERRAND,
            // and that narrowing is deliberate rather than incidental. It is forced: the
            // stale-hit retry below connects a second time, and a hold still held by this
            // very thread would make the second constructor wait for a slot only this
            // thread could free — a deadlock, ended only by whoever eventually abandons
            // the work. It is also right on its own terms. What the hold protects is the
            // PROMPT, which happens inside connectTo() and so is still entirely inside it;
            // what it was additionally costing was head-of-line blocking on the errand, so
            // a five-minute restart script kept a config read on the same host from even
            // beginning to connect.
            SshConnectHold hold(target, abandonCheck);
            if (!hold.held() || shared->abandoned)
                return Connect::Stopped; // asked to stop; the caller reports nothing

            auto fresh = std::make_unique<SshSession>();
            fresh->setAbandonCheck(abandonCheck);
            publish(fresh.get());
            // `userAsked = true` DELIBERATELY, and it is not a bug. An errand moves
            // kilobytes and the deflating would be done by the machine holding the log, so
            // it does not compress even for a host whose FETCHES do — and passing the
            // strongest possible request here is what makes that a statement rather than
            // an omission somebody could quietly reverse by wiring the option through
            // (SshSession::compressionFor, pinned without a server in tst_sshoptions).
            //
            // It is also what keeps the idle cache's key honest at target+role: no
            // compressed session is ever created on this path, so none can be checked in
            // and handed to an errand that did not ask for one (SshSessionCache.h).
            if (!fresh->connectTo(*location, prompter, kSshWorkerConnectTimeoutMs, &error,
                                  nullptr, need,
                                  SshSession::compressionFor(SshSession::Purpose::Errand,
                                                             /*userAsked=*/true))) {
                publish(nullptr);
                return Connect::Failed;
            }
            session = std::move(fresh);
            return Connect::Ok;
        };

        // Take one out of the idle cache first. A hit skips the TCP connect, the key
        // exchange, the host-key check and the whole authentication ladder — which is the
        // point of the whole file — and skips them because this very connection already
        // passed them, not because they were waived (SshSessionCache.h says so at length).
        bool fromCache = false;
        if (auto taken = sshSessionCache().checkOut(target, wantRole, sshMonotonicMs())) {
            auto *pooled = static_cast<detail::PooledSshSession *>(taken.session.get());
            session = pooled->release();
            heldRole = taken.role;
            fromCache = true;
            // The abandon check goes back on: the cache took the last errand's off, and
            // this worker's cancellation has to reach a connect that may yet be made from
            // the retry below.
            session->setAbandonCheck(abandonCheck);
            publish(session.get());
        }

        if (!session) {
            const Connect connected = connectFresh();
            if (connected != Connect::Ok)
                return connected == Connect::Stopped ? QString() : error;
        }
        if (shared->abandoned)
            return {};

        error = body(*session, location->path);

        // A cached session can have been closed by the far end while it sat idle, and
        // nothing short of using it finds that out — an idle TCP connection looks exactly
        // like a live one from this end. So an errand that failed on a session whose
        // TRANSPORT has since latched dead (SshSessionHealth.h) is given one more go on a
        // connection made from scratch.
        //
        // THE HEALTH TEST IS WHAT KEEPS THIS FROM BEING A BLIND RETRY. A config file that
        // is missing, too large, or refused by its permissions all fail on a perfectly
        // healthy link, and repeating any of them would get the same answer twice as
        // slowly. Only a session that has declared itself finished is worth a second
        // attempt — and only for an errand that says it may be repeated at all, because
        // "the link died partway" is indistinguishable from "the link died before
        // anything happened" and one of those must not be run again (SshWorkerPool.h).
        if (!error.isEmpty() && fromCache && repeat == SshErrandRepeat::Allowed
            && !shared->abandoned && !session->isConnected()) {
            const Connect connected = connectFresh();
            if (connected != Connect::Ok)
                return connected == Connect::Stopped ? QString() : error;
            if (shared->abandoned)
                return {};
            error = body(*session, location->path);
        }
    }

    // Out of the publication scope, so this session is this thread's alone again.
    //
    // KEEP ONLY WHAT IS STILL GOOD. An abandoned errand has usually had abort() called on
    // its session, so the socket is already shut; and a session that has latched dead is
    // the thing the retry above just proved is not worth passing on. Everything else is
    // destroyed right here on the worker, exactly as every session was before this cache
    // existed — so the cost of not keeping one has not moved.
    if (session) {
        if (!shared->abandoned && session->isConnected()) {
            sshSessionCache().checkIn(
                target, heldRole, std::make_unique<detail::PooledSshSession>(std::move(session)),
                sshMonotonicMs());
        } else {
            session.reset();
        }
    }
    return error;
}

} // namespace loftail
