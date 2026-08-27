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

#include "SecretStore.h"

#include "GuiCallGate.h"

#if defined(LOFTAIL_HAVE_KEYCHAIN)
#include "KeychainSecretStore.h"
#endif

#include <QCoreApplication>

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr() — and these strings are user-facing all the same: they travel up to
// the status bar through Document::lastError() and LiveController::sourceStatusChanged.
// Q_DECLARE_TR_FUNCTIONS is what lets lupdate file them under a name that means
// something rather than under the file they happen to sit in.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::SecretStore)
};
} // namespace


namespace {

// The #if lives at the call site rather than in the header, the same shape
// SourceSpool::defaultFetcher() uses for makeSshFetcher: everything above sees one
// SecretStore type in every configuration, and only which object it is differs.
SecretStore &defaultStore()
{
#if defined(LOFTAIL_HAVE_KEYCHAIN)
    static KeychainSecretStore store;
#else
    static NullSecretStore store;
#endif
    return store;
}

SecretStore *g_store = nullptr;

SecretStore &installedStore()
{
    return g_store ? *g_store : defaultStore();
}

// Every keychain call, run on the application thread (ARCHITECTURE.md §6.3.3).
//
// A keychain read happens inside SshSession's authentication ladder, which after M17
// runs on a fetcher's own thread. QtKeychain needs the application thread for two
// reasons that outlive any one backend: its jobs are asynchronous and are bridged with a
// nested event loop, and on Unix that loop wants QDBusConnection::sessionBus(). A worker
// runs no event loop at all.
//
// WHAT THIS DOES NOT CHANGE is the more important half. The keychain rung still sits
// BELOW authenticate()'s "is there anybody to ask" test, and marshalling is not a reason
// to move it: that test is about whether a PERSON is there, not about which thread is
// running, and an unattended retry — which passes no prompter — must not raise an unlock
// dialog for a log the user opened hours ago.
//
// Resolves the installed store inside each call rather than holding a pointer, so a test
// swapping stores mid-flight cannot be caught between the two.
class MarshalledSecretStore final : public SecretStore
{
public:
    bool available() override
    {
        bool answer = false;
        guiCallGate().call([&answer]() { answer = installedStore().available(); });
        return answer;
    }

    QString backendName() override
    {
        QString name;
        guiCallGate().call([&name]() { name = installedStore().backendName(); });
        return name;
    }

    Result read(const QString &key, QString *secret, QString *error) override
    {
        Result result = Result::NoBackend;
        guiCallGate().call(
            [&]() { result = installedStore().read(key, secret, error); });
        return result;
    }

    Result store(const QString &key, const QString &secret, QString *error) override
    {
        Result result = Result::NoBackend;
        guiCallGate().call(
            [&]() { result = installedStore().store(key, secret, error); });
        return result;
    }

    Result erase(const QString &key, QString *error) override
    {
        Result result = Result::NoBackend;
        guiCallGate().call([&]() { result = installedStore().erase(key, error); });
        return result;
    }
};

} // namespace

void setSecretStore(SecretStore *store)
{
    g_store = store;
}

SecretStore *secretStore()
{
    // Always the marshalling view, never the store itself. On the application thread the
    // gate runs the work inline, so this costs one indirection and nothing else — which
    // is what keeps the rule from being something each caller has to remember.
    static MarshalledSecretStore marshalled;
    return &marshalled;
}

QString sshSecretKey(const QString &target)
{
    return QStringLiteral("ssh/") + target;
}

RememberOutcome rememberSshPassword(const QString &target, const QString &password,
                                    QString *message)
{
    SecretStore *store = secretStore();
    if (!store->available()) {
        // No keychain on this machine, or none that will answer. The caller's plain-text
        // file applies — and the caller said so, by name, before the box could be ticked.
        return RememberOutcome::UseFileFallback;
    }

    QString error;
    const SecretStore::Result result = store->store(sshSecretKey(target), password, &error);
    if (result == SecretStore::Result::Ok)
        return RememberOutcome::StoredInKeychain;

    // There IS a keychain, the user was shown its name, and it refused. Reported, never
    // substituted: falling back to the file here would put a secret somewhere the user was
    // never told about. This is the whole reason UseFileFallback is not returned below.
    if (message) {
        *message = error.isEmpty()
            ? Tr::tr("%1 would not store the password.").arg(store->backendName())
            : Tr::tr("%1 would not store the password: %2")
                  .arg(store->backendName(), error);
    }
    return RememberOutcome::Failed;
}

void forgetSshPassword(const QString &target)
{
    SecretStore *store = secretStore();
    if (store->available())
        store->erase(sshSecretKey(target));
}

} // namespace loftail
