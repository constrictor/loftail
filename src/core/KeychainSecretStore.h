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
    Result probe();

    QString m_backendName;
    bool    m_probed = false;
    bool    m_available = false;
};

} // namespace loftail
