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

#include <QHash>
#include <QString>

#include "SecretStore.h"

namespace loftail {

// A SecretStore the test drives by hand (M14), the same device FakeFetcher.h is for the
// remote transport: everything above the seam — the auth chain's ordering, the checkbox's
// three destinations, the consent rule — is exercised for real, and only the backend is
// fake. Which is why every test built on it is UNGATED: none of it needs QtKeychain, and a
// contract that is only tested in one build configuration is a contract that only holds in
// one build configuration.
class FakeSecretStore final : public SecretStore
{
public:
    // --- Controls, called from the test ------------------------------------

    // Whether a backend will answer at all. False is the machine with no keychain, the
    // headless runner and the build with no QtKeychain — all one case, by design.
    void setAvailable(bool available) { m_available = available; }
    void setBackendName(const QString &name) { m_backendName = name; }

    // Put a secret in without going through store(), so a test can start from "the
    // keychain already has one" without pretending to have saved it.
    void preload(const QString &key, const QString &secret) { m_entries.insert(key, secret); }

    // Make the NEXT operation fail with `result`, then behave normally again. This is
    // how a keychain that is present and refuses gets tested — the case the consent rule
    // turns on, and the one a real keyring will not produce on demand.
    void failNext(Result result, const QString &error = QString())
    {
        m_failNext = result;
        m_failError = error;
    }

    // What is actually in there, for asserting a secret went where the test expects and,
    // just as often, that it did NOT.
    QHash<QString, QString> contents() const { return m_entries; }
    bool holds(const QString &key) const { return m_entries.contains(key); }

    int readCount() const { return m_reads; }
    int storeCount() const { return m_stores; }
    int eraseCount() const { return m_erases; }

    // --- SecretStore -------------------------------------------------------

    bool available() override { return m_available; }
    QString backendName() override { return m_available ? m_backendName : QString(); }

    Result read(const QString &key, QString *secret, QString *error = nullptr) override
    {
        ++m_reads;
        if (const Result forced = takeForcedFailure(error); forced != Result::Ok)
            return forced;
        if (!m_available)
            return Result::NoBackend;
        const auto it = m_entries.constFind(key);
        if (it == m_entries.constEnd())
            return Result::NotFound;
        if (secret)
            *secret = it.value();
        return Result::Ok;
    }

    Result store(const QString &key, const QString &secret, QString *error = nullptr) override
    {
        ++m_stores;
        if (const Result forced = takeForcedFailure(error); forced != Result::Ok)
            return forced;
        if (!m_available)
            return Result::NoBackend;
        m_entries.insert(key, secret);
        return Result::Ok;
    }

    Result erase(const QString &key, QString *error = nullptr) override
    {
        ++m_erases;
        if (const Result forced = takeForcedFailure(error); forced != Result::Ok)
            return forced;
        if (!m_available)
            return Result::NoBackend;
        m_entries.remove(key);
        return Result::Ok;
    }

private:
    Result takeForcedFailure(QString *error)
    {
        if (m_failNext == Result::Ok)
            return Result::Ok;
        const Result forced = m_failNext;
        m_failNext = Result::Ok;
        if (error)
            *error = m_failError;
        m_failError.clear();
        return forced;
    }

    QHash<QString, QString> m_entries;
    QString m_backendName = QStringLiteral("Test Keychain");
    Result  m_failNext = Result::Ok;
    QString m_failError;
    bool    m_available = true;
    int     m_reads = 0;
    int     m_stores = 0;
    int     m_erases = 0;
};

// Installs a store for the lifetime of the object and restores the process default after,
// so a test function cannot leak its fake into the next one. Construct one per test
// function. It restores the DEFAULT rather than whatever was installed before, because
// setSecretStore() has no getter to read one back with — and it needs none: nesting these
// would mean a test with two keychains, which is not a thing.
class InstalledSecretStore
{
public:
    explicit InstalledSecretStore(SecretStore *store) { setSecretStore(store); }
    ~InstalledSecretStore() { setSecretStore(nullptr); }

    InstalledSecretStore(const InstalledSecretStore &) = delete;
    InstalledSecretStore &operator=(const InstalledSecretStore &) = delete;
};

} // namespace loftail
