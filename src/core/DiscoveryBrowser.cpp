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

#include "DiscoveryBrowser.h"

#include "DiagnosticLog.h"
#include "DiscoveryBackends.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QPointer>
#include <QTimer>

#include <mutex>

namespace loftail {
namespace {

struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::DiscoveryBrowser)
};

std::mutex &factoryMutex()
{
    static std::mutex m;
    return m;
}

DiscoveryBrowserFactory &factory()
{
    static DiscoveryBrowserFactory f;
    return f;
}

std::mutex &cacheMutex()
{
    static std::mutex m;
    return m;
}

QHash<QString, DiscoveredDevice> &cache()
{
    static QHash<QString, DiscoveredDevice> c;
    return c;
}

QString cacheKey(const QString &typeName, const QString &id)
{
    // Type names are case-insensitive identities; device ids are the device's own and kept.
    return typeName.trimmed().toCaseFolded() + QLatin1Char('\n') + id;
}

// A browser that can do nothing but say why, for a kind of discovery this build lacks.
class RefusingBrowser final : public DiscoveryBrowser
{
public:
    RefusingBrowser(QString reason, QObject *parent)
        : DiscoveryBrowser(parent), m_reason(std::move(reason))
    {
    }
    void start(const RemoteType &) override
    {
        // Queued, so a caller that connects after start() still hears it.
        QPointer<RefusingBrowser> self(this);
        QTimer::singleShot(0, this, [self] {
            if (self)
                emit self->failed(self->m_reason);
        });
    }
    void stop() override {}

private:
    QString m_reason;
};

} // namespace

#if !defined(LOFTAIL_HAVE_MDNS)
std::unique_ptr<DiscoveryBrowser> makeBonjourBrowser(QObject *parent)
{
    QString why;
    bonjourDiscoveryAvailable(&why);
    return std::make_unique<RefusingBrowser>(why, parent);
}
#endif

bool bonjourDiscoveryAvailable(QString *why)
{
#if defined(LOFTAIL_HAVE_MDNS)
    Q_UNUSED(why);
    return true;
#else
    if (why) {
        *why = Tr::tr("Bonjour discovery is not built into this copy of loftail. ONVIF discovery "
                      "and typing an address still work.");
    }
    return false;
#endif
}

QString bonjourBackendName()
{
#if defined(LOFTAIL_MDNS_BACKEND)
    return QStringLiteral(LOFTAIL_MDNS_BACKEND);
#else
    return {};
#endif
}

void setDiscoveryBrowserFactory(DiscoveryBrowserFactory f)
{
    const std::scoped_lock lock(factoryMutex());
    factory() = std::move(f);
}

std::unique_ptr<DiscoveryBrowser> makeDiscoveryBrowser(const RemoteType &type, QObject *parent)
{
    {
        const std::scoped_lock lock(factoryMutex());
        if (factory())
            return factory()(type, parent);
    }
    switch (type.discovery) {
    case RemoteType::Discovery::Bonjour:
        return makeBonjourBrowser(parent);
    case RemoteType::Discovery::Onvif:
        return makeOnvifBrowser(parent);
    }
    return std::make_unique<RefusingBrowser>(QString(), parent);
}

void DiscoveryBrowser::report(const RemoteType &type, DiscoveredDevice device)
{
    device.typeName = type.name;
    if (device.displayName.isEmpty())
        device.displayName = device.id;
    // `addresses` always holds `host`, and `host` is one of `addresses`: a backend that knows
    // only one fills one, and every reader may then ask either.
    if (device.host.isEmpty() && !device.addresses.isEmpty())
        device.host = device.addresses.first();
    if (!device.host.isEmpty() && !device.addresses.contains(device.host))
        device.addresses.prepend(device.host);
    if (!deviceMatchesType(type, device))
        return;
    rememberDiscoveredDevice(device);
    emit found(device);
}

void rememberDiscoveredDevice(const DiscoveredDevice &device)
{
    const std::scoped_lock lock(cacheMutex());
    cache().insert(cacheKey(device.typeName, device.id), device);
}

std::optional<DiscoveredDevice> lastSeenDevice(const QString &typeName, const QString &id)
{
    const std::scoped_lock lock(cacheMutex());
    const auto it = cache().constFind(cacheKey(typeName, id));
    if (it == cache().constEnd())
        return std::nullopt;
    return *it;
}

void clearDiscoveredDevices()
{
    const std::scoped_lock lock(cacheMutex());
    cache().clear();
}

std::optional<DiscoveredDevice> resolveDiscoveredDevice(const RemoteType &type, const QString &id,
                                                        int timeoutMs,
                                                        const std::function<bool()> &abandoned,
                                                        QString *why)
{
    // A browser made HERE, on the calling thread, so its notifiers and timers belong to the
    // event loop this function turns — the fetcher's worker QThread, or the application thread.
    std::unique_ptr<DiscoveryBrowser> browser = makeDiscoveryBrowser(type);
    std::optional<DiscoveredDevice> answer;
    QString failure;

    QEventLoop loop;
    QObject::connect(browser.get(), &DiscoveryBrowser::found, &loop,
                     [&](const DiscoveredDevice &device) {
                         if (device.id == id) {
                             answer = device;
                             loop.quit();
                         }
                     });
    QObject::connect(browser.get(), &DiscoveryBrowser::failed, &loop,
                     [&](const QString &reason) {
                         failure = reason;
                         loop.quit();
                     });
    browser->start(type);

    // SLICED, so a tab closing mid-lookup costs a quarter of a second rather than the timeout —
    // the M17 rule every connect in the tree keeps.
    QElapsedTimer clock;
    clock.start();
    while (!answer && failure.isEmpty() && clock.elapsed() < timeoutMs) {
        if (abandoned && abandoned())
            break;
        QTimer::singleShot(int(qMin<qint64>(250, timeoutMs - clock.elapsed())), &loop,
                           &QEventLoop::quit);
        loop.exec();
    }
    browser->stop();

    if (answer) {
        rememberDiscoveredDevice(*answer);
        return answer;
    }
    if (why) {
        *why = !failure.isEmpty()
            ? failure
            : Tr::tr("%1 (%2) is not on the network right now").arg(id, type.name);
    }
    return std::nullopt;
}

} // namespace loftail
