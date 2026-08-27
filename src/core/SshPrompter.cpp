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

#include "SshPrompter.h"

#include "GuiCallGate.h"

#include <QHash>
#include <QSet>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

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
std::mutex &cacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

// --- SshConnectHold's bookkeeping ------------------------------------------

std::mutex &connectMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::condition_variable &connectFreed()
{
    static std::condition_variable condition;
    return condition;
}

QSet<QString> &connecting()
{
    static QSet<QString> targets;
    return targets;
}

// How long a waiter sleeps before rechecking whether it has been asked to give up. The
// holder may be sitting on a modal password dialog, so there is nothing to wake us.
constexpr auto kConnectSlice = std::chrono::milliseconds(100);

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
    std::scoped_lock lock(cacheMutex());
    return cache().contains(target);
}

QString password(const QString &target)
{
    std::scoped_lock lock(cacheMutex());
    return cache().value(target);
}

void remember(const QString &target, const QString &password)
{
    std::scoped_lock lock(cacheMutex());
    cache().insert(target, password);
}

void forget(const QString &target)
{
    std::scoped_lock lock(cacheMutex());
    if (auto it = cache().find(target); it != cache().end()) {
        it.value().fill(QChar(u'\0')); // overwrite before releasing the buffer
        cache().erase(it);
    }
}

void clear()
{
    std::scoped_lock lock(cacheMutex());
    for (auto it = cache().begin(); it != cache().end(); ++it)
        it.value().fill(QChar(u'\0'));
    cache().clear();
}

} // namespace SshCredentialCache

SshConnectHold::SshConnectHold(QString target, const std::function<bool()> &abandon)
    : m_target(std::move(target))
{
    std::unique_lock<std::mutex> lock(connectMutex());
    while (connecting().contains(m_target)) {
        if (abandon && abandon())
            return; // asked to stop while waiting; m_held stays false
        // Timed rather than indefinite: the holder may be sitting on a modal dialog for
        // as long as the user takes, and nothing wakes us in the meantime. The slice is
        // how quickly this notices it has been asked to give up.
        connectFreed().wait_for(lock, kConnectSlice);
    }
    connecting().insert(m_target);
    m_held = true;
}

SshConnectHold::~SshConnectHold()
{
    if (!m_held)
        return;
    {
        std::scoped_lock lock(connectMutex());
        connecting().remove(m_target);
    }
    connectFreed().notify_all();
}

} // namespace loftail
