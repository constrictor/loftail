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

#include <memory>
#include <vector>

namespace loftail {
namespace {

struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::DnsSdBrowser)
};

class DnsSdBrowser;

// One DNSServiceRef and the notifier that drives it.
struct Operation
{
    DnsSdBrowser    *owner = nullptr;
    DNSServiceRef    ref = nullptr;
    QSocketNotifier *notifier = nullptr;
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
        m_seen.clear();
    }

private:
    void watch(Operation *op)
    {
        op->notifier = new QSocketNotifier(DNSServiceRefSockFD(op->ref), QSocketNotifier::Read,
                                           this);
        connect(op->notifier, &QSocketNotifier::activated, this, [op] {
            if (op->ref)
                DNSServiceProcessResult(op->ref);
        });
    }

    static void release(Operation *op)
    {
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

    // Retire a finished resolve OUTSIDE its own callback, which is still on the stack.
    void retire(Operation *done)
    {
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
        auto *self = static_cast<Operation *>(context)->owner;
        if (err != kDNSServiceErr_NoError || !name)
            return;
        const QString instance = QString::fromUtf8(name);
        if (flags & kDNSServiceFlagsAdd) {
            ++self->m_seen[instance];
            auto op = std::make_unique<Operation>();
            op->owner = self;
            if (DNSServiceResolve(&op->ref, 0, iface, name, regtype, domain,
                                  &DnsSdBrowser::onResolve, op.get())
                == kDNSServiceErr_NoError) {
                self->watch(op.get());
                self->m_ops.push_back(std::move(op));
            }
        } else if (--self->m_seen[instance] <= 0) {
            self->m_seen.remove(instance);
            emit self->lost(self->m_type.name, instance);
        }
    }

    static void DNSSD_API onResolve(DNSServiceRef, DNSServiceFlags, uint32_t,
                                    DNSServiceErrorType err, const char *fullName,
                                    const char *hostTarget, uint16_t port, uint16_t txtLen,
                                    const unsigned char *txt, void *context)
    {
        auto *op = static_cast<Operation *>(context);
        DnsSdBrowser *self = op->owner;
        if (err == kDNSServiceErr_NoError && fullName && hostTarget) {
            DiscoveredDevice device;
            // The full name is "<instance>.<type>.<domain>", with dots in the instance escaped;
            // the instance is what the browse reported, and the first unescaped label.
            const char *rest = fullName;
            QByteArray label;
            while (*rest && *rest != '.') {
                if (*rest == '\\' && rest[1]) {
                    if (rest[1] >= '0' && rest[1] <= '9' && rest[2] && rest[3]) {
                        label.append(char((rest[1] - '0') * 100 + (rest[2] - '0') * 10
                                          + (rest[3] - '0')));
                        rest += 4;
                    } else {
                        label.append(rest[1]);
                        rest += 2;
                    }
                    continue;
                }
                label.append(*rest++);
            }
            device.id = QString::fromUtf8(label);
            device.displayName = device.id;
            device.host = QString::fromUtf8(hostTarget);
            if (device.host.endsWith(u'.'))
                device.host.chop(1);
            device.port = ntohs(port);
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
                device.details.append(item);
            }
            self->report(self->m_type, device);
        }
        self->retire(op);
    }

    RemoteType                              m_type;
    std::vector<std::unique_ptr<Operation>> m_ops;
    QHash<QString, int>                     m_seen;
};

} // namespace

std::unique_ptr<DiscoveryBrowser> makeBonjourBrowser(QObject *parent)
{
    return std::make_unique<DnsSdBrowser>(parent);
}

} // namespace loftail
