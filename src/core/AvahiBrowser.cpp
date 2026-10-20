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

// Bonjour discovery on Linux, through the Avahi daemon (ARCHITECTURE.md §6.13).
//
// AVAHI AND NOT A RESPONDER OF loftail's OWN: the daemon already owns UDP 5353 on every Linux
// desktop that has mDNS at all, and a second responder would either fail to bind or fight it
// for the answers. avahi-client talks to the daemon over D-Bus and is driven through an
// AvahiPoll — implemented below onto QSocketNotifier and QTimer, so the browser runs on
// whichever thread's event loop made it, which is what lets a fetcher's worker thread look a
// device up with the same code the Open Remote dialog browses with.

#include "DiscoveryBackends.h"

#include "DiagnosticLog.h"

#include <QCoreApplication>
#include <QHash>
#include <QPointer>
#include <QSocketNotifier>
#include <QTimer>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/address.h>
#include <avahi-common/error.h>
#include <avahi-common/strlst.h>
#include <avahi-common/watch.h>

#include <algorithm>
#include <limits>

#include <net/if.h>
#include <sys/time.h>

// --- the AvahiPoll, onto Qt ---------------------------------------------------
//
// Avahi declares these two as opaque structs and lets the poll implementation define them.
// Both are QObjects so that a free from inside their own callback — which the API explicitly
// allows — can be a deleteLater() rather than a delete under the caller's feet.

struct AvahiWatch : public QObject
{
    int                fd = -1;
    AvahiWatchEvent    wanted{};
    AvahiWatchEvent    happened{};
    AvahiWatchCallback callback = nullptr;
    void              *userdata = nullptr;
    QSocketNotifier   *reader = nullptr;
    QSocketNotifier   *writer = nullptr;

    void fire(AvahiWatchEvent event)
    {
        happened = event;
        callback(this, fd, event, userdata);
        happened = AvahiWatchEvent{};
    }

    void apply()
    {
        if (reader)
            reader->setEnabled(wanted & AVAHI_WATCH_IN);
        if (writer)
            writer->setEnabled(wanted & AVAHI_WATCH_OUT);
    }
};

struct AvahiTimeout : public QObject
{
    QTimer               timer;
    AvahiTimeoutCallback callback = nullptr;
    void                *userdata = nullptr;

    void arm(const struct timeval *tv)
    {
        timer.stop();
        if (!tv)
            return;
        struct timeval now{};
        gettimeofday(&now, nullptr);
        const qint64 ms = (qint64(tv->tv_sec) - now.tv_sec) * 1000
            + (qint64(tv->tv_usec) - now.tv_usec) / 1000;
        timer.start(int(qBound<qint64>(0, ms, qint64(std::numeric_limits<int>::max()))));
    }
};

namespace loftail {
namespace {

struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::AvahiBrowser)
};

AvahiWatch *watchNew(const AvahiPoll *, int fd, AvahiWatchEvent event,
                     AvahiWatchCallback callback, void *userdata)
{
    auto *w = new AvahiWatch;
    w->fd = fd;
    w->wanted = event;
    w->callback = callback;
    w->userdata = userdata;
    w->reader = new QSocketNotifier(fd, QSocketNotifier::Read, w);
    w->writer = new QSocketNotifier(fd, QSocketNotifier::Write, w);
    QObject::connect(w->reader, &QSocketNotifier::activated, w,
                     [w] { w->fire(AVAHI_WATCH_IN); });
    QObject::connect(w->writer, &QSocketNotifier::activated, w,
                     [w] { w->fire(AVAHI_WATCH_OUT); });
    w->apply();
    return w;
}

void watchUpdate(AvahiWatch *w, AvahiWatchEvent event)
{
    w->wanted = event;
    w->apply();
}

AvahiWatchEvent watchGetEvents(AvahiWatch *w)
{
    return w->happened;
}

void watchFree(AvahiWatch *w)
{
    // Disarmed at once, destroyed later: this may be called from inside w's own callback.
    w->wanted = AvahiWatchEvent{};
    w->apply();
    w->callback = [](AvahiWatch *, int, AvahiWatchEvent, void *) {};
    w->deleteLater();
}

AvahiTimeout *timeoutNew(const AvahiPoll *, const struct timeval *tv,
                         AvahiTimeoutCallback callback, void *userdata)
{
    auto *t = new AvahiTimeout;
    t->callback = callback;
    t->userdata = userdata;
    t->timer.setSingleShot(true);
    QObject::connect(&t->timer, &QTimer::timeout, t, [t] { t->callback(t, t->userdata); });
    t->arm(tv);
    return t;
}

void timeoutUpdate(AvahiTimeout *t, const struct timeval *tv)
{
    t->arm(tv);
}

void timeoutFree(AvahiTimeout *t)
{
    t->timer.stop();
    t->callback = [](AvahiTimeout *, void *) {};
    t->deleteLater();
}

const AvahiPoll *qtPoll()
{
    static const AvahiPoll poll = {
        nullptr, watchNew, watchUpdate, watchGetEvents, watchFree,
        timeoutNew, timeoutUpdate, timeoutFree,
    };
    return &poll;
}

// --- the browser ----------------------------------------------------------------

class AvahiBrowser final : public DiscoveryBrowser
{
public:
    explicit AvahiBrowser(QObject *parent) : DiscoveryBrowser(parent) {}
    ~AvahiBrowser() override { stop(); }

    void start(const RemoteType &type) override
    {
        stop();
        m_type = type;
        int error = 0;
        m_client = avahi_client_new(qtPoll(), AvahiClientFlags(0), nullptr, nullptr, &error);
        if (!m_client) {
            // NO DAEMON is the case worth naming: it is how a minimal install and a container
            // look, and "Bonjour found nothing" would be a lie about the network.
            failLater(error == AVAHI_ERR_NO_DAEMON
                          ? Tr::tr("The Avahi daemon is not running, so Bonjour devices cannot "
                                   "be found. Start avahi-daemon, or type the address instead.")
                          : Tr::tr("Bonjour discovery could not start — %1")
                                .arg(QString::fromUtf8(avahi_strerror(error))));
            return;
        }
        const QByteArray service = type.serviceType.trimmed().toUtf8();
        const QByteArray domain = type.domain.trimmed().toUtf8();
        m_browser = avahi_service_browser_new(
            m_client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, service.constData(),
            domain.isEmpty() ? nullptr : domain.constData(), AvahiLookupFlags(0),
            &AvahiBrowser::onBrowse, this);
        if (!m_browser) {
            failLater(Tr::tr("Cannot browse for %1 — %2")
                          .arg(type.serviceType,
                               QString::fromUtf8(avahi_strerror(avahi_client_errno(m_client)))));
        }
    }

    void stop() override
    {
        // Resolvers belong to the client and go with it.
        if (m_browser) {
            avahi_service_browser_free(m_browser);
            m_browser = nullptr;
        }
        if (m_client) {
            avahi_client_free(m_client);
            m_client = nullptr;
        }
        // ...and so do the resolvers, which is why the pointers are only forgotten here.
        m_lookups.clear();
        m_lastReported.clear();
    }

private:
    void failLater(const QString &reason)
    {
        QTimer::singleShot(0, this, [this, reason] { emit failed(reason); });
    }

    // ONE LIVE RESOLVER PER (instance, interface, protocol), kept until that instance is
    // REMOVED there. A resolver freed after its first answer — which is what this was — reports
    // where a device was when it was first seen and never again, so a device whose service
    // stays registered while its address moves was invisible to the standing watch
    // (DiscoveryWatch.h). A live one re-emits FOUND whenever its SRV, TXT or address records
    // change, which is the whole of how an address change reaches an open log.
    struct Lookup
    {
        AvahiServiceResolver *resolver = nullptr;
        QString               address;   // empty until resolved
        int                   port = 0;
        QStringList           txt;
        bool                  v4 = false;
        // What the browse said, kept so a resolver that FAILED can be made again (onResolve()).
        AvahiIfIndex          iface = AVAHI_IF_UNSPEC;
        AvahiProtocol         protocol = AVAHI_PROTO_UNSPEC;
        QByteArray            type;
        QByteArray            domain;
    };

    // How long a failed resolver waits before it is made again.
    static constexpr int kResolveRetryMs = 3000;

    void startResolver(const QString &instance, Lookup &lookup)
    {
        const QByteArray name = instance.toUtf8();
        lookup.resolver = avahi_service_resolver_new(
            m_client, lookup.iface, lookup.protocol, name.constData(), lookup.type.constData(),
            lookup.domain.constData(), AVAHI_PROTO_UNSPEC, AvahiLookupFlags(0),
            &AvahiBrowser::onResolve, this);
    }

    static quint64 lookupKey(AvahiIfIndex iface, AvahiProtocol protocol)
    {
        return (quint64(quint32(iface)) << 8) | quint64(quint8(protocol));
    }

    static void onBrowse(AvahiServiceBrowser *, AvahiIfIndex iface, AvahiProtocol protocol,
                         AvahiBrowserEvent event, const char *name, const char *type,
                         const char *domain, AvahiLookupResultFlags, void *userdata)
    {
        auto *self = static_cast<AvahiBrowser *>(userdata);
        const QString instance = QString::fromUtf8(name ? name : "");
        switch (event) {
        case AVAHI_BROWSER_NEW: {
            Lookup &lookup = self->m_lookups[instance][lookupKey(iface, protocol)];
            lookup.iface = iface;
            lookup.protocol = protocol;
            lookup.type = QByteArray(type ? type : "");
            lookup.domain = QByteArray(domain ? domain : "");
            if (!lookup.resolver)
                self->startResolver(instance, lookup);
            break;
        }
        case AVAHI_BROWSER_REMOVE: {
            // An instance is seen once per interface and protocol; it is gone when the last
            // of those is. What is left of it is reported again, so a device that dropped off
            // one network but not another moves to the one it is still on.
            auto it = self->m_lookups.find(instance);
            if (it == self->m_lookups.end())
                break;
            const auto at = it->find(lookupKey(iface, protocol));
            if (at != it->end()) {
                if (at->resolver)
                    avahi_service_resolver_free(at->resolver);
                it->erase(at);
            }
            if (it->isEmpty()) {
                self->m_lookups.erase(it);
                self->m_lastReported.remove(instance);
                emit self->lost(self->m_type.name, instance);
            } else {
                self->reportInstance(instance);
            }
            break;
        }
        case AVAHI_BROWSER_FAILURE:
            self->failLater(Tr::tr("Bonjour browsing failed — %1")
                                .arg(QString::fromUtf8(
                                    avahi_strerror(avahi_client_errno(self->m_client)))));
            break;
        case AVAHI_BROWSER_CACHE_EXHAUSTED:
        case AVAHI_BROWSER_ALL_FOR_NOW:
            break;
        }
    }

    static void onResolve(AvahiServiceResolver *resolver, AvahiIfIndex iface, AvahiProtocol,
                          AvahiResolverEvent event, const char *name, const char *,
                          const char *, const char *hostName, const AvahiAddress *address,
                          uint16_t port, AvahiStringList *txt, AvahiLookupResultFlags,
                          void *userdata)
    {
        auto *self = static_cast<AvahiBrowser *>(userdata);
        const QString instance = QString::fromUtf8(name ? name : "");
        Lookup *lookup = self->lookupOf(instance, resolver);
        if (!lookup)
            return;   // removed while the answer was on its way; freed there
        if (event != AVAHI_RESOLVER_FOUND || !address) {
            // A resolver that failed is finished, and NOTHING ELSE WILL MAKE ANOTHER: the
            // service is still registered, so no NEW is coming. That is exactly what a device
            // off its network for a while looks like — its address record withdrawn, the
            // resolver timing out on it — and a resolver let go of here meant the address it
            // came back with was never reported, found by driving the real application. So it is
            // made again after a pause, for as long as the instance is still browsed on this
            // link. Nothing is reported for the failure itself: a failed lookup is an absence of
            // news, which is never a move (EndpointResolver.h).
            avahi_service_resolver_free(resolver);
            lookup->resolver = nullptr;
            const quint64 key = lookupKey(lookup->iface, lookup->protocol);
            QPointer<AvahiBrowser> guard(self);
            AvahiClient *client = self->m_client;
            QTimer::singleShot(kResolveRetryMs, self, [guard, client, instance, key] {
                if (!guard || guard->m_client != client)
                    return;   // stopped, or restarted with a client of its own, meanwhile
                auto it = guard->m_lookups.find(instance);
                if (it == guard->m_lookups.end())
                    return;   // removed meanwhile
                auto at = it->find(key);
                if (at == it->end() || at->resolver)
                    return;
                guard->startResolver(instance, *at);
            });
            return;
        }
        char text[AVAHI_ADDRESS_STR_MAX] = {0};
        avahi_address_snprint(text, sizeof text, address);
        QString host = QString::fromLatin1(text);
        // A link-local IPv6 address is meaningless without the interface it is on.
        if (address->proto == AVAHI_PROTO_INET6 && host.startsWith(u"fe80")) {
            char ifname[IF_NAMESIZE] = {0};
            if (if_indextoname(unsigned(iface), ifname))
                host += QLatin1Char('%') + QString::fromLatin1(ifname);
        }
        if (host.isEmpty() && hostName)
            host = QString::fromUtf8(hostName);
        lookup->address = host;
        lookup->port = port;
        lookup->v4 = address->proto == AVAHI_PROTO_INET;
        lookup->txt.clear();
        for (AvahiStringList *it = txt; it; it = avahi_string_list_get_next(it)) {
            lookup->txt.append(QString::fromUtf8(
                reinterpret_cast<const char *>(avahi_string_list_get_text(it)),
                int(avahi_string_list_get_size(it))));
        }
        self->reportInstance(instance);
    }

    Lookup *lookupOf(const QString &instance, AvahiServiceResolver *resolver)
    {
        auto it = m_lookups.find(instance);
        if (it == m_lookups.end())
            return nullptr;
        for (Lookup &l : *it) {
            if (l.resolver == resolver)
                return &l;
        }
        return nullptr;
    }

    // The instance as every interface and protocol currently has it: all of its addresses,
    // with IPv4 preferred for `host` where it has both. Reported only when it differs from what
    // was reported last, so a resolver restating itself is not news.
    void reportInstance(const QString &instance)
    {
        const auto it = m_lookups.constFind(instance);
        if (it == m_lookups.constEnd())
            return;
        DiscoveredDevice device;
        device.id = instance;
        device.displayName = instance;
        const Lookup *chosen = nullptr;
        for (const Lookup &l : *it) {
            if (l.address.isEmpty())
                continue;
            if (!device.addresses.contains(l.address))
                device.addresses.append(l.address);
            if (!chosen || (l.v4 && !chosen->v4))
                chosen = &l;
        }
        if (!chosen)
            return;
        device.host = chosen->address;
        device.port = chosen->port;
        device.details = chosen->txt;
        // Stable order, so the change test below compares sets rather than arrival order.
        std::sort(device.addresses.begin(), device.addresses.end());
        const auto last = m_lastReported.constFind(instance);
        if (last != m_lastReported.constEnd() && *last == device)
            return;
        m_lastReported.insert(instance, device);
        report(m_type, device);
    }

    RemoteType                                        m_type;
    AvahiClient                                      *m_client = nullptr;
    AvahiServiceBrowser                              *m_browser = nullptr;
    QHash<QString, QHash<quint64, Lookup>>            m_lookups;       // instance -> per link
    QHash<QString, DiscoveredDevice>                  m_lastReported;  // instance -> as reported
};

} // namespace

std::unique_ptr<DiscoveryBrowser> makeBonjourBrowser(QObject *parent)
{
    return std::make_unique<AvahiBrowser>(parent);
}

} // namespace loftail
