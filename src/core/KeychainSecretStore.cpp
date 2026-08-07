#include "KeychainSecretStore.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QPointer>
#include <QProcessEnvironment>
#include <QThread>
#include <QTimer>

#include <memory>
#include <type_traits>

// QtKeychain installs its header under a directory named after the Qt major it was built
// for (qt6keychain/ on Debian and Ubuntu, qtkeychain/ elsewhere), and a FetchContent build
// exports the source tree, where it is simply keychain.h. Probing beats picking one and
// making the Windows CI job — which has no package manager and builds from source — the
// odd one out.
#if __has_include(<qt6keychain/keychain.h>)
#include <qt6keychain/keychain.h>
#elif __has_include(<qtkeychain/keychain.h>)
#include <qtkeychain/keychain.h>
#else
#include <keychain.h>
#endif

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr() — and these strings are user-facing all the same: they travel up to
// the status bar through Document::lastError() and LiveController::sourceStatusChanged.
// Q_DECLARE_TR_FUNCTIONS is what lets lupdate file them under a name that means
// something rather than under the file they happen to sit in.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::KeychainSecretStore)
};
} // namespace


namespace {

// An ordinary operation may legitimately involve a PERSON: a locked KWallet raises
// kwalletd's unlock dialog, and macOS asks before letting a binary at an item whose ACL
// does not list it. Waiting a minute for that is right; waiting forever is not, for the
// same reason libssh2_session_set_timeout() bounds every SSH call — a wedged service must
// not take the thread that opened the document with it.
constexpr int kOperationTimeoutMs = 60000;

// The availability probe gets a much shorter leash, because by the time it runs the
// interactive case is already behind us: the auth chain reads the keychain (rung 6) before
// it builds any dialog, so a wallet that was going to ask has asked. What is left for the
// probe to discover is whether anything answers at all, and that is fast or never.
constexpr int kProbeTimeoutMs = 2000;

// The key the probe asks for. It is not expected to exist — EntryNotFound is this probe's
// SUCCESS, because only a backend that is actually there can say so.
constexpr auto kProbeKey = "loftail-availability-probe";

struct JobOutcome
{
    bool    finished = false; // false means the timer won, not the job
    int     error = 0;        // QKeychain::Error
    QString errorString;
    QString text;
};

// QtKeychain's jobs are asynchronous and everything above authenticate() wants an answer
// now. A nested QEventLoop is the bridge — and it is safe here for exactly the reason the
// modal password dialog is safe, which is already the load-bearing paragraph of
// ARCHITECTURE.md §6.3:
//
//   connectTo() duplicates the connected descriptor away from Qt (SocketDetach.h, called
//   at SshSession.cpp:572) BEFORE the handshake and long before authenticate(). No
//   QSocketNotifier is armed on the SSH socket by then, so no turn of any Qt event loop
//   can drain bytes into a buffer libssh2 cannot see. That is the failure the whole
//   detach exists for — "Timed out waiting on socket" after a SUCCESSFUL login — and the
//   modal prompt was one of the two loops that caused it. A keychain job's loop is the
//   same kind of loop in the same window, so it needs no new argument.
//
// It gets no new PROTECTION either, hence the two things this does that the dialog got for
// free from modality and from being a dialog:
//
//   ExcludeUserInputEvents, because modality is what stops a second open starting on top
//   of a connect in progress (SshPromptDialogs.h) and a bare nested loop has none. Timers
//   still fire, which is the exposure the modal dialog already has, not a new one.
//
//   A timer bound, above.
//
// The outcome is a shared_ptr and the loop is held through a QPointer because the job may
// outlive this frame: on timeout it is still in flight, and writing its eventual answer
// into a dead stack frame is the one genuinely dangerous move available here. The job is
// left with QtKeychain's default auto-delete, so whenever it does finish it tidies itself
// up and the connection — whose context object is the job — dies with it.
template <typename JobT>
std::shared_ptr<JobOutcome> runJob(JobT *job, int timeoutMs)
{
    auto outcome = std::make_shared<JobOutcome>();

    QEventLoop loop;
    QPointer<QEventLoop> loopGuard(&loop);

    QObject::connect(job, &QKeychain::Job::finished, job, [job, outcome, loopGuard]() {
        outcome->finished = true;
        outcome->error = job->error();
        outcome->errorString = job->errorString();
        if constexpr (std::is_same_v<JobT, QKeychain::ReadPasswordJob>)
            outcome->text = job->textData();
        if (loopGuard)
            loopGuard->quit();
    });

    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    job->start();
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    return outcome;
}

SecretStore::Result mapOutcome(const std::shared_ptr<JobOutcome> &outcome, QString *error)
{
    if (!outcome->finished) {
        // Nothing answered inside the leash. Indistinguishable from "there is no keychain
        // here" as far as any caller is concerned, and it must NOT read as a refusal —
        // a refusal is a thing a backend does, and no backend did anything.
        if (error)
            *error = Tr::tr("the keychain did not answer in time");
        return SecretStore::Result::NoBackend;
    }

    if (error)
        *error = outcome->errorString;

    switch (outcome->error) {
    case QKeychain::NoError:
        return SecretStore::Result::Ok;
    case QKeychain::EntryNotFound:
        return SecretStore::Result::NotFound;
    case QKeychain::NoBackendAvailable:
    case QKeychain::NotImplemented:
        return SecretStore::Result::NoBackend;
    case QKeychain::AccessDenied:
    case QKeychain::AccessDeniedByUser:
        return SecretStore::Result::Denied;
    default:
        return SecretStore::Result::Failed;
    }
}

QString service()
{
    return QString::fromLatin1(kSecretService);
}

// QtKeychain exposes no accessor for the backend it chose, so this is a heuristic — and it
// is allowed to be one, because nothing branches on it. Its only job is to name a
// destination in a sentence a person reads before deciding whether to save a password, and
// the fallback wording is true everywhere.
QString detectBackendName()
{
#if defined(Q_OS_WIN)
    return Tr::tr("the Windows Credential Manager");
#elif defined(Q_OS_MACOS)
    return Tr::tr("the macOS Keychain");
#else
    const QString desktop =
        QProcessEnvironment::systemEnvironment()
            .value(QStringLiteral("XDG_CURRENT_DESKTOP"))
            .toUpper();
    if (desktop.contains(QStringLiteral("KDE")) || desktop.contains(QStringLiteral("PLASMA")))
        return Tr::tr("KWallet");
    if (desktop.contains(QStringLiteral("GNOME")) || desktop.contains(QStringLiteral("UNITY"))
        || desktop.contains(QStringLiteral("CINNAMON")))
        return Tr::tr("GNOME Keyring");
    return Tr::tr("your system keychain");
#endif
}

// A keychain is consulted ONLY on the thread that has a prompter — the thread that opened
// the document (ARCHITECTURE.md §6.3.2). Three reasons, the first sufficient alone:
//
//   1. A keychain read can PROMPT. SshFetcher::reconnect() already forbids exactly this,
//      in those words: a dialog appearing while the user is doing something else, for a
//      log they may have opened hours ago. A keychain unlock is that dialog by another
//      route.
//   2. The fetcher's worker runs no event loop — Worker::run() is tailLoop(), with no
//      exec() — and QtKeychain's Unix path wants QDBusConnection::sessionBus().
//   3. reconnect() needs none of this anyway: the password went into SshCredentialCache
//      when the document was opened, and the chain finds it a rung earlier.
//
// The auth chain enforces this structurally, by putting the keychain rung below its
// "is there anybody to ask" test. This makes it a runtime fact as well, so a later caller
// cannot quietly reintroduce a keychain read on the fetcher thread.
bool onTheRightThread()
{
    QCoreApplication *app = QCoreApplication::instance();
    return !app || QThread::currentThread() == app->thread();
}

} // namespace

bool KeychainSecretStore::available()
{
    if (m_probed)
        return m_available;

    // BEFORE m_probed is latched, and that ordering is the whole of it. Latching first
    // meant a single off-thread call — a marshalling wrapper that forgot to route this
    // one method, or a test — turned the keychain off for the rest of the process, and
    // M14's remembered passwords then stopped working with every test above this seam
    // still green. Which is exactly how the two M14 wires broke the first time.
    //
    // Not reachable through secretStore(), which marshals; kept as the structural
    // assertion it has always been, so a later caller cannot quietly reintroduce a
    // keychain read on a fetcher thread.
    if (!onTheRightThread())
        return false;

    m_probed = true;

    // The cheap negative first — but ONLY as a negative. QKeychain::isAvailable() answers
    // "was a backend library found", which on Linux is true as soon as libsecret-1 can be
    // dlopen()ed, running daemon or not. Upstream says so itself in keychain_unix.cpp:
    // "In the future there should be a difference between 'API available' and 'keychain
    // available'." A headless CI runner and a KDE box with kwalletd not started both pass
    // it, so a yes here means nothing on its own.
    if (!QKeychain::isAvailable())
        return false;

    // So actually ask, and see whether anything answers.
    const Result result = probe();
    m_available = (result == Result::Ok || result == Result::NotFound
                   || result == Result::Denied);
    if (m_available)
        m_backendName = detectBackendName();
    return m_available;
}

QString KeychainSecretStore::backendName()
{
    return available() ? m_backendName : QString();
}

SecretStore::Result KeychainSecretStore::probe()
{
    auto *job = new QKeychain::ReadPasswordJob(service());
    job->setKey(QString::fromLatin1(kProbeKey));
    return mapOutcome(runJob(job, kProbeTimeoutMs), nullptr);
}

SecretStore::Result KeychainSecretStore::read(const QString &key, QString *secret,
                                              QString *error)
{
    if (!onTheRightThread()) {
        if (error)
            *error = Tr::tr("a keychain is only consulted on the thread that "
                                    "opened the log");
        return Result::NoBackend;
    }

    auto *job = new QKeychain::ReadPasswordJob(service());
    job->setKey(key);
    const auto outcome = runJob(job, kOperationTimeoutMs);
    const Result result = mapOutcome(outcome, error);
    if (result == Result::Ok && secret)
        *secret = outcome->text;
    return result;
}

SecretStore::Result KeychainSecretStore::store(const QString &key, const QString &secret,
                                               QString *error)
{
    if (!onTheRightThread()) {
        if (error)
            *error = Tr::tr("a keychain is only written on the thread that "
                                    "opened the log");
        return Result::NoBackend;
    }

    auto *job = new QKeychain::WritePasswordJob(service());
    job->setKey(key);
    job->setTextData(secret);
    return mapOutcome(runJob(job, kOperationTimeoutMs), error);
}

SecretStore::Result KeychainSecretStore::erase(const QString &key, QString *error)
{
    if (!onTheRightThread()) {
        if (error)
            *error = Tr::tr("a keychain is only written on the thread that "
                                    "opened the log");
        return Result::NoBackend;
    }

    auto *job = new QKeychain::DeletePasswordJob(service());
    job->setKey(key);
    const Result result = mapOutcome(runJob(job, kOperationTimeoutMs), error);
    // Deleting something that is not there is not a failure — it is the state the caller
    // asked for. forgetSshPassword() runs on every rejected stored password, and a chain
    // that treated this as an error would report one on the common path.
    return result == Result::NotFound ? Result::Ok : result;
}

} // namespace loftail
