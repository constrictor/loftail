#include "SshPrompter.h"

#include "GuiCallGate.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QWaitCondition>

namespace loftail {

namespace {

SshPrompter *g_prompter = nullptr;

// In-memory only, and cleared when the process ends. A password the user asked to
// KEEP is a separate, deliberate act with its own warning (HostBookmarkStore); this
// cache exists purely so that one host costs one prompt rather than one per file.
QHash<QString, QString> &cache()
{
    static QHash<QString, QString> store;
    return store;
}

// Guards cache(). Every fetcher thread reads it during authentication now, where before
// M17 the only caller was whichever thread happened to be opening a document — which,
// connects being serialised on the GUI thread, was always the same one.
QMutex &cacheMutex()
{
    static QMutex mutex;
    return mutex;
}

// --- SshConnectHold's bookkeeping ------------------------------------------

QMutex &connectMutex()
{
    static QMutex mutex;
    return mutex;
}

QWaitCondition &connectFreed()
{
    static QWaitCondition condition;
    return condition;
}

QSet<QString> &connecting()
{
    static QSet<QString> targets;
    return targets;
}

// How long a waiter sleeps before rechecking whether it has been asked to give up. The
// holder may be sitting on a modal password dialog, so there is nothing to wake us.
constexpr unsigned long kConnectSliceMs = 100;

} // namespace

void setSshPrompter(SshPrompter *prompter)
{
    g_prompter = prompter;

    // "Null means never prompt" is this seam's existing policy (see the header), and
    // once a question can be waiting on the application thread that policy has to reach
    // the questions already in flight as well — otherwise a window on its way out leaves
    // a fetcher blocked on a prompter it has just withdrawn. Cancelling answers every one
    // of them in the safe direction, which is the same answer a null prompter gives.
    if (prompter)
        guiCallGate().reopen();
    else
        guiCallGate().cancel();
}

SshPrompter *sshPrompter()
{
    return g_prompter;
}

namespace SshCredentialCache {

bool has(const QString &target)
{
    QMutexLocker lock(&cacheMutex());
    return cache().contains(target);
}

QString password(const QString &target)
{
    QMutexLocker lock(&cacheMutex());
    return cache().value(target);
}

void remember(const QString &target, const QString &password)
{
    QMutexLocker lock(&cacheMutex());
    cache().insert(target, password);
}

void forget(const QString &target)
{
    QMutexLocker lock(&cacheMutex());
    if (auto it = cache().find(target); it != cache().end()) {
        it.value().fill(QChar(u'\0')); // overwrite before releasing the buffer
        cache().erase(it);
    }
}

void clear()
{
    QMutexLocker lock(&cacheMutex());
    for (auto it = cache().begin(); it != cache().end(); ++it)
        it.value().fill(QChar(u'\0'));
    cache().clear();
}

} // namespace SshCredentialCache

SshConnectHold::SshConnectHold(const QString &target, const std::function<bool()> &abandon)
    : m_target(target)
{
    QMutexLocker lock(&connectMutex());
    while (connecting().contains(m_target)) {
        if (abandon && abandon())
            return; // asked to stop while waiting; m_held stays false
        // Timed rather than indefinite: the holder may be sitting on a modal dialog for
        // as long as the user takes, and nothing wakes us in the meantime. The slice is
        // how quickly this notices it has been asked to give up.
        connectFreed().wait(&connectMutex(), QDeadlineTimer(kConnectSliceMs));
    }
    connecting().insert(m_target);
    m_held = true;
}

SshConnectHold::~SshConnectHold()
{
    if (!m_held)
        return;
    {
        QMutexLocker lock(&connectMutex());
        connecting().remove(m_target);
    }
    connectFreed().wakeAll();
}

} // namespace loftail
