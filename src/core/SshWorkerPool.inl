// Included from SshWorkerPool.h, and only in a build with SSH support.
//
// A template, so it cannot live in the .cpp; its own file rather than an inline blob in
// the header, because it is the only thing there that needs libssh2's headers in scope.

#pragma once

#include "RemoteLocation.h"
#include "SshPrompter.h"
#include "SshSession.h"

#include <QCoreApplication>

namespace loftail {

// The bound a connect gets. A config read or a restart is a DELIBERATE gesture with
// somebody waiting, so it takes the attended timeout rather than the shorter unattended
// one a background retry uses.
inline constexpr int kSshWorkerConnectTimeoutMs = 20000;

template <class Body>
QString withSshSession(const QString &address, SshPrompter *prompter,
                       const std::shared_ptr<SshWorkerShared> &shared, Body body)
{
    const auto location = RemoteLocation::parse(address);
    if (!location || !location->isValid()) {
        return QCoreApplication::translate("loftail::SshWorkerPool",
                                           "Not a valid remote address: %1")
            .arg(RemoteLocation::withoutPassword(address));
    }

    // ONE CONNECT AT A TIME PER HOST, which is what keeps "one prompt per host" true now
    // that this can be in flight beside a log's own reconnect.
    SshConnectHold hold(location->target(), [shared]() { return shared->abandoned.load(); });
    if (!hold.held() || shared->abandoned)
        return {}; // asked to stop; the caller reports nothing

    auto session = std::make_unique<SshSession>();
    session->setAbandonCheck([shared]() { return shared->abandoned.load(); });
    {
        std::scoped_lock lock(shared->mutex);
        shared->session = session.get();
    }
    // Whatever happens below, the pointer must stop being publishable before the session
    // is destroyed, or a late abort() would aim at freed memory.
    struct Unpublish
    {
        std::shared_ptr<SshWorkerShared> shared;
        ~Unpublish()
        {
            std::scoped_lock lock(shared->mutex);
            shared->session = nullptr;
        }
    } unpublish{shared};

    QString error;
    if (!session->connectTo(*location, prompter, kSshWorkerConnectTimeoutMs, &error, nullptr))
        return error;
    if (shared->abandoned)
        return {};
    return body(*session, location->path);
}

} // namespace loftail
