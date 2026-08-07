#include "SshPrompter.h"

#include "GuiCallGate.h"

#include <QHash>

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
    return cache().contains(target);
}

QString password(const QString &target)
{
    return cache().value(target);
}

void remember(const QString &target, const QString &password)
{
    cache().insert(target, password);
}

void forget(const QString &target)
{
    if (auto it = cache().find(target); it != cache().end()) {
        it.value().fill(QChar(u'\0')); // overwrite before releasing the buffer
        cache().erase(it);
    }
}

void clear()
{
    for (auto it = cache().begin(); it != cache().end(); ++it)
        it.value().fill(QChar(u'\0'));
    cache().clear();
}

} // namespace SshCredentialCache

} // namespace loftail
