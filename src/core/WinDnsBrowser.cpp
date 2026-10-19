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

// Bonjour discovery on Windows, through the system's own mDNS client — DnsServiceBrowse and
// DnsServiceResolve in dnsapi, Windows 10 1809 and later (ARCHITECTURE.md §6.13). No Bonjour
// SDK and no extra runtime: the build probes for the API and leaves this file out where the
// Windows SDK does not declare it.
//
// THE CALLBACKS ARRIVE ON A SYSTEM THREAD-POOL THREAD, never on the thread that started the
// browse. Nothing is done there but copying the answer out and posting it, under a mutex, to
// the browser on its own thread — and a browser being destroyed takes that mutex to unhook
// itself first, so a post either lands before the destructor (and Qt discards it with the
// object) or never happens at all.

#include "DiscoveryBackends.h"

#include <QCoreApplication>
#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

// DnsServiceBrowse is declared for Windows 10 1809 (RS5) and later only; Qt 6 targets Windows
// 10 already, so this raises nothing a supported build does not have — it only makes the
// declarations visible. The root CMakeLists probes with the same two values.
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#  undef _WIN32_WINNT
#  define _WIN32_WINNT 0x0A00
#endif
#if !defined(NTDDI_VERSION) || NTDDI_VERSION < 0x0A000006
#  undef NTDDI_VERSION
#  define NTDDI_VERSION 0x0A000006
#endif
#include <windows.h>
#include <windns.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace loftail {
namespace {

struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::WinDnsBrowser)
};

class WinDnsBrowser;

// What the system's callbacks hold: the browser, while there is one. Shared so it outlives
// whichever of the browser and the last callback goes first.
struct Hook
{
    std::mutex     mutex;
    WinDnsBrowser *owner = nullptr;
    QString        query;   // "_ssh._tcp.local", to cut instance names out of full names
};

// One resolve's context: the hook, and the full name it is resolving. Deleted by its callback.
struct ResolveContext
{
    std::shared_ptr<Hook> hook;
    std::wstring          fullName;
    DNS_SERVICE_CANCEL    cancel{};
};

QString fromWide(PCWSTR s)
{
    return s ? QString::fromWCharArray(s) : QString();
}

class WinDnsBrowser final : public DiscoveryBrowser
{
public:
    explicit WinDnsBrowser(QObject *parent) : DiscoveryBrowser(parent) {}
    ~WinDnsBrowser() override { stop(); }

    void start(const RemoteType &type) override
    {
        stop();
        m_type = type;
        QString domain = type.domain.trimmed();
        if (domain.isEmpty())
            domain = QStringLiteral("local");
        if (domain.endsWith(u'.'))
            domain.chop(1);
        m_hook = std::make_shared<Hook>();
        m_hook->owner = this;
        m_hook->query = type.serviceType.trimmed() + QLatin1Char('.') + domain;
        m_query = m_hook->query.toStdWString();

        // The context the browse callback receives: a heap copy of the shared hook, released
        // by the callback that reports the cancel.
        auto *context = new std::shared_ptr<Hook>(m_hook);
        DNS_SERVICE_BROWSE_REQUEST request{};
        request.Version = DNS_QUERY_REQUEST_VERSION1;
        request.InterfaceIndex = 0;
        request.QueryName = m_query.c_str();
        request.pBrowseCallback = &WinDnsBrowser::onBrowse;
        request.pQueryContext = context;
        m_cancel = DNS_SERVICE_CANCEL{};
        const DNS_STATUS status = DnsServiceBrowse(&request, &m_cancel);
        if (status != DNS_REQUEST_PENDING) {
            delete context;
            m_hook.reset();
            const QString reason =
                Tr::tr("Bonjour discovery could not start (error %1).").arg(qulonglong(status));
            QTimer::singleShot(0, this, [this, reason] { emit failed(reason); });
            return;
        }
        m_browsing = true;
    }

    void stop() override
    {
        if (m_hook) {
            const std::scoped_lock lock(m_hook->mutex);
            m_hook->owner = nullptr;
        }
        if (m_browsing) {
            DnsServiceBrowseCancel(&m_cancel);
            m_browsing = false;
        }
        m_hook.reset();
        m_seen.clear();
    }

private:
    // Posted from a pool thread, run on the browser's own.
    void onInstanceSeen(const QString &fullName, bool removed)
    {
        // Posted before a stop() and delivered after it: the browse it answers is over.
        if (!m_hook)
            return;
        const QString suffix = QLatin1Char('.') + m_hook->query;
        QString instance = fullName;
        if (instance.endsWith(u'.'))
            instance.chop(1);
        if (instance.endsWith(suffix, Qt::CaseInsensitive))
            instance.chop(suffix.size());
        if (removed) {
            if (m_seen.remove(instance))
                emit lost(m_type.name, instance);
            return;
        }
        if (m_seen.contains(instance))
            return;
        m_seen.insert(instance, true);
        auto *context = new ResolveContext;
        context->hook = m_hook;
        context->fullName = fullName.toStdWString();
        DNS_SERVICE_RESOLVE_REQUEST request{};
        request.Version = DNS_QUERY_REQUEST_VERSION1;
        request.InterfaceIndex = 0;
        request.QueryName = const_cast<PWSTR>(context->fullName.c_str());
        request.pResolveCompletionCallback = &WinDnsBrowser::onResolve;
        request.pQueryContext = context;
        if (DnsServiceResolve(&request, &context->cancel) != DNS_REQUEST_PENDING)
            delete context;
    }

    static void post(const std::shared_ptr<Hook> &hook, std::function<void(WinDnsBrowser *)> fn)
    {
        const std::scoped_lock lock(hook->mutex);
        if (!hook->owner)
            return;
        WinDnsBrowser *owner = hook->owner;
        QMetaObject::invokeMethod(
            owner, [owner, fn = std::move(fn)] { fn(owner); }, Qt::QueuedConnection);
    }

    static VOID WINAPI onBrowse(DWORD status, PVOID context, PDNS_RECORD records)
    {
        auto *hook = static_cast<std::shared_ptr<Hook> *>(context);
        if (status == ERROR_CANCELLED) {
            delete hook;
            if (records)
                DnsRecordListFree(records, DnsFreeRecordList);
            return;
        }
        for (auto *r = reinterpret_cast<PDNS_RECORDW>(records); r; r = r->pNext) {
            if (r->wType != DNS_TYPE_PTR)
                continue;
            const QString full = fromWide(r->Data.PTR.pNameHost);
            // A record with a TTL of zero is the "goodbye" an mDNS responder sends on leaving.
            const bool removed = r->dwTtl == 0;
            post(*hook, [full, removed](WinDnsBrowser *b) { b->onInstanceSeen(full, removed); });
        }
        if (records)
            DnsRecordListFree(records, DnsFreeRecordList);
    }

    static VOID WINAPI onResolve(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
    {
        std::unique_ptr<ResolveContext> owned(static_cast<ResolveContext *>(context));
        if (status != ERROR_SUCCESS || !instance) {
            if (instance)
                DnsServiceFreeInstance(instance);
            return;
        }
        DiscoveredDevice device;
        device.id = fromWide(instance->pszInstanceName);
        if (instance->ip4Address) {
            const DWORD a = *instance->ip4Address;
            // Network byte order, as an IN_ADDR holds it.
            device.host = QStringLiteral("%1.%2.%3.%4")
                              .arg(a & 0xff)
                              .arg((a >> 8) & 0xff)
                              .arg((a >> 16) & 0xff)
                              .arg((a >> 24) & 0xff);
        } else {
            device.host = fromWide(instance->pszHostName);
        }
        device.port = instance->wPort;
        for (DWORD i = 0; i < instance->dwPropertyCount; ++i) {
            QString item = fromWide(instance->keys[i]);
            if (instance->values && instance->values[i])
                item += QLatin1Char('=') + fromWide(instance->values[i]);
            device.details.append(item);
        }
        DnsServiceFreeInstance(instance);
        const QString full = QString::fromStdWString(owned->fullName);
        post(owned->hook, [device, full](WinDnsBrowser *b) mutable {
            if (!b->m_hook)
                return;   // stopped between the post and its delivery
            // The instance name as the BROWSE spelled it is the id, for symmetry with removal.
            QString instanceName = full;
            const QString suffix = QLatin1Char('.') + b->m_hook->query;
            if (instanceName.endsWith(u'.'))
                instanceName.chop(1);
            if (instanceName.endsWith(suffix, Qt::CaseInsensitive))
                instanceName.chop(suffix.size());
            if (!instanceName.isEmpty())
                device.id = instanceName;
            device.displayName = device.id;
            b->report(b->m_type, device);
        });
    }

    RemoteType            m_type;
    std::shared_ptr<Hook> m_hook;
    std::wstring          m_query;
    DNS_SERVICE_CANCEL    m_cancel{};
    bool                  m_browsing = false;
    QHash<QString, bool>  m_seen;
};

} // namespace

std::unique_ptr<DiscoveryBrowser> makeBonjourBrowser(QObject *parent)
{
    return std::make_unique<WinDnsBrowser>(parent);
}

} // namespace loftail
