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

#include "SecretStore.h"

namespace loftail {

// The SecretStore backed by the operating system's own credential store, through
// QtKeychain (M14, ARCHITECTURE.md §6.3.2): KWallet or GNOME Keyring over the freedesktop
// Secret Service on Linux, the Credential Manager on Windows, the Keychain on macOS.
//
// This header names no QtKeychain type, and KeychainSecretStore.cpp is the only
// translation unit that includes one — the same discipline SshSession.cpp keeps for
// libssh2 and ArchiveReader.cpp for libarchive. It is compiled only when
// LOFTAIL_HAVE_KEYCHAIN is set; SecretStore.cpp picks it or NullSecretStore there.
//
// EVERY METHOD BLOCKS, and every one must be called on the thread that owns the GUI.
// Both properties are deliberate and are explained where they are enforced in the .cpp;
// the short version is that QtKeychain's jobs are asynchronous while the auth chain needs
// an answer now, and that a keychain read can raise an unlock dialog.
class KeychainSecretStore final : public SecretStore
{
public:
    bool available() override;
    QString backendName() override;

    Result read(const QString &key, QString *secret, QString *error = nullptr) override;
    Result store(const QString &key, const QString &secret, QString *error = nullptr) override;
    Result erase(const QString &key, QString *error = nullptr) override;

private:
    // The round trip behind available(), split out so available() can run it with a short
    // leash while the ordinary operations keep a generous one.
    static Result probe();

    QString m_backendName;
    bool    m_probed = false;
    bool    m_available = false;
};

} // namespace loftail
