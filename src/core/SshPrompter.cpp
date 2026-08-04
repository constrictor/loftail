#include "SshPrompter.h"

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
