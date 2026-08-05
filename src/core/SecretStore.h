#pragma once

#include <QString>

namespace loftail {

// Where a password the user asked loftail to KEEP actually goes (M14, SPEC.md §3).
//
// Free of QtKeychain types and ALWAYS compiled, exactly like SshPrompter and
// RemoteLocation: the wording of the checkbox, the ordering of the auth chain and the
// consent rule below must be identical in every build configuration. Only the answer to
// available() may differ.
class SecretStore
{
public:
    virtual ~SecretStore() = default;

    // Why an operation did not happen. The split that matters is NotFound vs NoBackend:
    // "the keychain answered and has nothing for you" and "there is no keychain here"
    // lead to different user-facing text and different fallbacks, and conflating them is
    // how a user gets told their password was saved somewhere it was not.
    enum class Result {
        Ok,
        NotFound,   // a backend answered: nothing stored under this key
        NoBackend,  // nothing to ask — no library, no daemon, no session bus, or it timed out
        Denied,     // a backend answered: the user or the OS refused
        Failed,     // anything else; `error` carries the backend's own wording
    };

    // Whether a keychain will ACTUALLY ANSWER — not whether one was linked.
    //
    // These are different questions and the difference is load-bearing. QtKeychain's own
    // QKeychain::isAvailable() answers the first: on Linux it is true as soon as
    // libsecret-1 can be dlopen()ed, whether or not any daemon is running — upstream says
    // so itself in keychain_unix.cpp ("In the future there should be a difference between
    // 'API available' and 'keychain available'"). A headless CI runner and a KDE box with
    // kwalletd not started are the same case, and both would pass that test. So an
    // implementation must ROUND-TRIP: ask the backend for something and see whether an
    // answer comes back. May block; call it where blocking is already expected.
    virtual bool available() = 0;

    // What to call it in a sentence shown to a person: "KWallet", "GNOME Keyring",
    // "Windows Credential Manager", "the macOS Keychain". Never empty when available() is
    // true — a warning that cannot name the destination is not much of a warning, which is
    // the same rule GuiSshPrompter names the fallback FILE for.
    virtual QString backendName() = 0;

    virtual Result read(const QString &key, QString *secret, QString *error = nullptr) = 0;
    virtual Result store(const QString &key, const QString &secret, QString *error = nullptr) = 0;
    virtual Result erase(const QString &key, QString *error = nullptr) = 0;
};

// The store that answers "no" to everything. What a build with no keychain support uses,
// and what a test installs to exercise the plain-text path.
class NullSecretStore final : public SecretStore
{
public:
    bool available() override { return false; }
    QString backendName() override { return QString(); }
    // No default arguments repeated here: the base declares them, and a default that
    // differs between a base and an override is resolved statically and silently.
    Result read(const QString &, QString *, QString *) override { return Result::NoBackend; }
    Result store(const QString &, const QString &, QString *) override { return Result::NoBackend; }
    Result erase(const QString &, QString *) override { return Result::NoBackend; }
};

// The store in use. NEVER NULL — and that is the one deliberate difference from
// sshPrompter(), which may be.
//
// For a prompter, null IS a policy: "never prompt", and every question then answers itself
// in the safe direction. Here "no keychain" is not a policy, only which backend was
// compiled in or found at runtime, and it already has a first-class spelling the object
// can give about itself — available() == false. A nullable store would put a null check at
// every call site guarding a condition the store already states.
//
// Absent an installed one this is the QtKeychain-backed store where it was compiled in,
// and a NullSecretStore otherwise.
SecretStore *secretStore();
void setSecretStore(SecretStore *store); // null restores the process default

// The keychain SERVICE loftail's entries live under, as it appears in kwalletmanager or
// seahorse, and the key within it.
//
// The key is RemoteLocation::target() with a prefix — the same string SshCredentialCache
// is keyed on, so one host is one entry in the keychain for the same reason it is one
// prompt. The prefix keeps a later kind of loftail secret from colliding with an SSH one.
constexpr auto kSecretService = "loftail";
QString sshSecretKey(const QString &target); // "ssh/deploy@web1.example.com:22"

// WHERE A REMEMBERED PASSWORD GOES, as one decision in one place.
//
// In core and always compiled, because it is a contract about CONSENT rather than a piece
// of UI: the keychain when one will answer, the caller's own file fallback when none will,
// and NEVER the file after the user was shown the keychain's name.
enum class RememberOutcome {
    StoredInKeychain,
    UseFileFallback, // no keychain here; the caller's plain-text path applies
    Failed,          // there IS a keychain and it refused — do NOT substitute a file
};

// Never returns UseFileFallback once available() has said yes. That single rule is what
// stops a secret landing in a file the user was never told about, in the one dialog whose
// whole job is to say where the secret goes; tst_hostbookmarks asserts it.
// `message` receives the backend's own wording on Failed.
RememberOutcome rememberSshPassword(const QString &target, const QString &password,
                                    QString *message);

// Drop a keychain entry the server has just rejected, so a password it no longer accepts
// does not burn one of sshd's MaxAuthTries on every future connect
// (SshSession::authenticate).
//
// The KEYCHAIN only, deliberately. The plain-text fallback lives in hosts.json, and
// reaching for AppConfigLocation from inside a network auth routine is exactly the hidden
// dependency the bookmark lookup was kept out of core to avoid (ARCHITECTURE.md §6.3.2) —
// a stale entry there is cleared where it was set, in the Open Remote dialog. The
// asymmetry is real and is the price of that placement.
void forgetSshPassword(const QString &target);

} // namespace loftail
