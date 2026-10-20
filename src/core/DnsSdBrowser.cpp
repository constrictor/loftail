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

// Bonjour discovery on macOS (and wherever else dns_sd.h is the system's own responder API),
// through mDNSResponder (ARCHITECTURE.md §6.13).
//
// Every DNSServiceRef is driven by a QSocketNotifier on its own socket, so the browser runs on
// whichever thread's event loop made it — Avahi's arrangement, through a different API.
//
// The host is the record's own `hosttarget` (`cam-7.local.`) rather than an address looked up
// here: on this platform the system resolver answers `.local` names itself, so QTcpSocket can
// connect to it directly, and a DNSServiceGetAddrInfo round trip would buy nothing.

#include "DiscoveryBackends.h"

#include <QCoreApplication>
#include <QHash>
#include <QSocketNotifier>
#include <QTimer>

#include <dns_sd.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace loftail {
namespace {

struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::DnsSdBrowser)
};

class DnsSdBrowser;

// One DNSServiceRef and the notifier that drives it. `dead` is set the moment the operation is
// let go of, and every callback checks it: the ref is deallocated on a later turn of the event
// loop (retire()), and its notifier may fire once more in between.
struct Operation
{
    DnsSdBrowser    *owner = nullptr;
    DNSServiceRef    ref = nullptr;
    QSocketNotifier *notifier = nullptr;
    QString          instance;
    uint32_t         iface = 0;
    bool             dead = false;
};

// What is known of one instance: a resolve per interface it was seen on, KEPT RUNNING so a
// change to its SRV or TXT is reported, and one address query on its host name, kept running
// so a change of ADDRESS is — the two things the standing watch needs (DiscoveryWatch.h).
struct Instance
{
    QHash<uint32_t, Operation *> resolves;
    Operation                   *addresses = nullptr;
    QString                      hostTarget;
    int                          port = 0;
    QStringList                  txt;
    QStringList                  v4;
    QStringList                  v6;
};

class DnsSdBrowser final : public DiscoveryBrowser
{
public:
    explicit DnsSdBrowser(QObject *parent) : DiscoveryBrowser(parent) {}
    ~DnsSdBrowser() override { stop(); }

    void start(const RemoteType &type) override
    {
        stop();
        m_type = type;
        const QByteArray service = type.serviceType.trimmed().toUtf8();
        const QByteArray domain = type.domain.trimmed().toUtf8();
        auto op = std::make_unique<Operation>();
        op->owner = this;
        const DNSServiceErrorType err =
            DNSServiceBrowse(&op->ref, 0, 0, service.constData(),
                             domain.isEmpty() ? nullptr : domain.constData(),
                             &DnsSdBrowser::onBrowse, op.get());
        if (err != kDNSServiceErr_NoError) {
            const QString reason =
                Tr::tr("Bonjour discovery could not start (error %1).").arg(int(err));
            QTimer::singleShot(0, this, [this, reason] { emit failed(reason); });
            return;
        }
        watch(op.get());
        m_ops.push_back(std::move(op));
    }

    void stop() override
    {
        for (auto &op : m_ops)
            release(op.get());
        m_ops.clear();
        m_instances.clear();
        m_lastReported.clear();
    }

private:
    void watch(Operation *op)
    {
        op->notifier = new QSocketNotifier(DNSServiceRefSockFD(op->ref), QSocketNotifier::Read,
                                           this);
        connect(op->notifier, &QSocketNotifier::activated, this, [op] {
            if (op->ref && !op->dead)
                DNSServiceProcessResult(op->ref);
        });
    }

    static void release(Operation *op)
    {
        op->dead = true;
        if (op->notifier) {
            op->notifier->setEnabled(false);
            op->notifier->deleteLater();
            op->notifier = nullptr;
        }
        if (op->ref) {
            DNSServiceRefDeallocate(op->ref);
            op->ref = nullptr;
        }
    }

    Operation *adopt(std::unique_ptr<Operation> op)
    {
        watch(op.get());
        Operation *raw = op.get();
        m_ops.push_back(std::move(op));
        return raw;
    }

    // Let an operation go OUTSIDE any callback that may be on the stack for it: marked dead
    // now, deallocated on the next turn.
    void retire(Operation *done)
    {
        if (!done || done->dead)
            return;
        done->dead = true;
        if (done->notifier)
            done->notifier->setEnabled(false);
        QTimer::singleShot(0, this, [this, done] {
            for (auto it = m_ops.begin(); it != m_ops.end(); ++it) {
                if (it->get() == done) {
                    release(done);
                    m_ops.erase(it);
                    return;
                }
            }
        });
    }

    static void DNSSD_API onBrowse(DNSServiceRef, DNSServiceFlags flags, uint32_t iface,
                                   DNSServiceErrorType err, const char *name, const char *regtype,
                                   const char *domain, void *context)
    {
        auto *browse = static_cast<Operation *>(context);
        if (browse->dead)
            return;
        DnsSdBrowser *self = browse->owner;
        if (err != kDNSServiceErr_NoError || !name)
            return;
        const QString instance = QString::fromUtf8(name);
        if (flags & kDNSServiceFlagsAdd) {
            Instance &state = self->m_instances[instance];
            if (state.resolves.contains(iface))
                return;
            auto op = std::make_unique<Operation>();
            op->owner = self;
            op->instance = instance;
            op->iface = iface;
            if (DNSServiceResolve(&op->ref, 0, iface, name, regtype, domain,
                                  &DnsSdBrowser::onResolve, op.get())
                == kDNSServiceErr_NoError) {
                state.resolves.insert(iface, self->adopt(std::move(op)));
            }
            return;
        }
        // Gone from this interface; gone altogether when it is gone from the last one.
        auto it = self->m_instances.find(instance);
        if (it == self->m_instances.end())
            return;
        self->retire(it->resolves.take(iface));
        if (!it->resolves.isEmpty())
            return;
        self->retire(it->addresses);
        self->m_instances.erase(it);
        self->m_lastReported.remove(instance);
        emit self->lost(self->m_type.name, instance);
    }

    static void DNSSD_API onResolve(DNSServiceRef, DNSServiceFlags, uint32_t,
                                    DNSServiceErrorType err, const char *,
                                    const char *hostTarget, uint16_t port, uint16_t txtLen,
                                    const unsigned char *txt, void *context)
    {
        auto *op = static_cast<Operation *>(context);
        if (op->dead)
            return;
        DnsSdBrowser *self = op->owner;
        auto it = self->m_instances.find(op->instance);
        if (it == self->m_instances.end() || err != kDNSServiceErr_NoError || !hostTarget)
            return;
        Instance &state = *it;
        QString target = QString::fromUtf8(hostTarget);
        if (target.endsWith(u'.'))
            target.chop(1);
        state.port = ntohs(port);
        state.txt.clear();
        const uint16_t count = TXTRecordGetCount(txtLen, txt);
        for (uint16_t i = 0; i < count; ++i) {
            char key[256] = {0};
            uint8_t valueLen = 0;
            const void *value = nullptr;
            if (TXTRecordGetItemAtIndex(txtLen, txt, i, sizeof key, key, &valueLen, &value)
                != kDNSServiceErr_NoError) {
                continue;
            }
            QString item = QString::fromUtf8(key);
            if (value) {
                item += QLatin1Char('=')
                    + QString::fromUtf8(static_cast<const char *>(value), valueLen);
            }
            state.txt.append(item);
        }
        // A NEW HOST NAME starts a new address query; the same one keeps the running query,
        // whose answers are what report a change of address.
        if (target != state.hostTarget || !state.addresses) {
            self->retire(state.addresses);
            state.addresses = nullptr;
            state.hostTarget = target;
            state.v4.clear();
            state.v6.clear();
            auto query = std::make_unique<Operation>();
            query->owner = self;
            query->instance = op->instance;
            if (DNSServiceGetAddrInfo(&query->ref, kDNSServiceFlagsLongLivedQuery, 0,
                                      kDNSServiceProtocol_IPv4 | kDNSServiceProtocol_IPv6,
                                      hostTarget, &DnsSdBrowser::onAddress, query.get())
                == kDNSServiceErr_NoError) {
                state.addresses = self->adopt(std::move(query));
            }
        }
        self->reportInstance(op->instance);
    }

    static void DNSSD_API onAddress(DNSServiceRef, DNSServiceFlags flags, uint32_t iface,
                                    DNSServiceErrorType err, const char *,
                                    const struct sockaddr *address, uint32_t, void *context)
    {
        auto *op = static_cast<Operation *>(context);
        if (op->dead || err != kDNSServiceErr_NoError || !address)
            return;
        DnsSdBrowser *self = op->owner;
        auto it = self->m_instances.find(op->instance);
        if (it == self->m_instances.end())
            return;
        char text[INET6_ADDRSTRLEN] = {0};
        QStringList *list = nullptr;
        QString host;
        if (address->sa_family == AF_INET) {
            const auto *in = reinterpret_cast<const sockaddr_in *>(address);
            inet_ntop(AF_INET, &in->sin_addr, text, sizeof text);
            host = QString::fromLatin1(text);
            list = &it->v4;
        } else if (address->sa_family == AF_INET6) {
            const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(address);
            inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof text);
            host = QString::fromLatin1(text);
            // A link-local IPv6 address is meaningless without the interface it is on.
            if (host.startsWith(u"fe80")) {
                char ifname[IF_NAMESIZE] = {0};
                if (if_indextoname(iface, ifname))
                    host += QLatin1Char('%') + QString::fromLatin1(ifname);
            }
            list = &it->v6;
        }
        if (!list || host.isEmpty())
            return;
        if (flags & kDNSServiceFlagsAdd) {
            if (!list->contains(host))
                list->append(host);
        } else {
            list->removeAll(host);
        }
        // More answers queued behind this one: report once they are all in.
        if (flags & kDNSServiceFlagsMoreComing)
            return;
        self->reportInstance(op->instance);
    }

    // The instance as it stands: every numeric address, IPv4 preferred for `host`; the host
    // name while no address has been answered yet. Reported only when it changed.
    void reportInstance(const QString &instance)
    {
        const auto it = m_instances.constFind(instance);
        if (it == m_instances.constEnd() || it->hostTarget.isEmpty())
            return;
        DiscoveredDevice device;
        device.id = instance;
        device.displayName = instance;
        QStringList v4 = it->v4;
        QStringList v6 = it->v6;
        std::sort(v4.begin(), v4.end());
        std::sort(v6.begin(), v6.end());
        device.addresses = v4 + v6;
        if (device.addresses.isEmpty())
            device.addresses.append(it->hostTarget);
        device.host = device.addresses.first();
        device.port = it->port;
        device.details = it->txt;
        const auto last = m_lastReported.constFind(instance);
        if (last != m_lastReported.constEnd() && *last == device)
            return;
        m_lastReported.insert(instance, device);
        report(m_type, device);
    }

    RemoteType                              m_type;
    std::vector<std::unique_ptr<Operation>> m_ops;
    QHash<QString, Instance>                m_instances;
    QHash<QString, DiscoveredDevice>        m_lastReported;
};

} // namespace

std::unique_ptr<DiscoveryBrowser> makeBonjourBrowser(QObject *parent)
{
    return std::make_unique<DnsSdBrowser>(parent);
}

} // namespace loftail
