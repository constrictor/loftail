#include "SecretStore.h"

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

} // namespace

void setSecretStore(SecretStore *store)
{
    g_store = store;
}

SecretStore *secretStore()
{
    return g_store ? g_store : &defaultStore();
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
