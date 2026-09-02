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

#include "PromptRelay.h"

#if defined(LOFTAIL_HAVE_SSH)
// For SshSession::Need, which withSshSession() takes by value. The header names no libssh2
// type (SshSession.h says so and it is load-bearing), so including it costs a build with SSH
// nothing and a build without it is not compiling any of this.
#include "SshSession.h"
#endif

#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace loftail {

// Forward-declared for SshWorkerShared::session below, which is a POINTER and is present
// in every build — the abandon rule it serves must read the same with and without libssh2.
class SshSession;
class SshPrompter;

// Running SSH work off the application thread, and the lifetime rules that makes
// necessary (ARCHITECTURE.md §6.3.3, §6.8, §6.9).
//
// Two features need it — reading and writing a log's config file, and running its restart
// script — and both need it for the same reason: a connect is up to twenty seconds and may
// stop to ask for a password, so it cannot run where the window lives. Every rule below
// was learned from a crash, which is why there is one copy of them rather than one per
// caller.

// What a worker and its owner share.
struct SshWorkerShared
{
    // Set when the owner goes, or when the work is cancelled. The worker checks it before
    // doing anything expensive and again before reporting, so abandoned work stops as soon
    // as it can and never reports to a destroyed object.
    std::atomic<bool> abandoned{false};

    // GUARDED because abort() is reached from the OWNER's thread while the worker is
    // inside libssh2 — the one call on SshSession that is safe to make concurrently, and
    // the reason it is safe is that it shuts the socket and touches nothing else.
    std::mutex        mutex;
    SshSession       *session = nullptr;

    // OWNED HERE, and that placement is the whole point: the worker holds a strong
    // reference to this block, so the relay it asks through cannot be destroyed while a
    // connect is still using it. A relay owned by the WINDOW and handed to a detached
    // thread is how this crashed — "pure virtual method called", from the next virtual
    // call after the owner went. PromptRelay holds no prompter of its own (it resolves
    // sshPrompter() inside each marshalled call), so one per worker costs nothing.
    PromptRelay       relay;

    // Abandon the work and make any blocking libssh2 call return now. Safe from any
    // thread, and idempotent — which is what lets a destructor and an explicit cancel
    // share one implementation.
    void abandon();
};

// Start `body` on a thread of its own, reaping any that have already finished.
//
// A QThread, NOT a std::thread, and that is not a style choice: SshSession::connectTo()
// ends in QTcpSocket::waitForConnected(), and a QAbstractSocket needs the Qt event
// dispatcher of the thread it is used on. A raw std::thread has no QThreadData and
// therefore no dispatcher, so that call dereferenced a null one and crashed inside
// libQt6Network — a SEGV on the worker, which is what AddressSanitizer caught in CI.
void startSshWorker(std::function<void()> body);

// Stop waiting for workers, up to `budgetMs`, and let every cached connection go. Called
// by the window on its way out.
//
// Work is ABANDONED rather than joined while the process is alive — see SshWorkerShared —
// but at shutdown that is not enough: Qt's own globals go with the application object, and
// a worker still inside QTcpSocket then writes through a pointer that has just become
// null. Bounded, because a quit that can hang is worse than a budget that can be exceeded.
//
// It also CLOSES the idle session cache, and that is not housekeeping: a session sitting
// there past this point is a live socket and a QTcpSocket to be torn down after the
// application object has gone, which is the same SEGV in different clothing. Closing
// latches, so a worker that finishes after the budget expires cannot put one back.
void drainSshWorkers(int budgetMs = 3000);

#if defined(LOFTAIL_HAVE_SSH)
// Whether this errand may be RUN TWICE.
//
// A session taken out of the idle cache (SshSessionCache.h) was proven when it was made,
// not when it was handed over, so it can turn out to have been closed by the far end while
// it sat there. For an errand that can simply be done again, the honest answer is to throw
// that session away and repeat it on a fresh connect, so a stale hit costs a moment rather
// than a failure the user has to understand. For one that cannot, it is to report what
// happened and let the person decide.
//
// GETTING THIS BACKWARDS IS SILENT IN BOTH DIRECTIONS. Say `Allowed` of something that is
// not repeatable and a link that dies mid-errand runs it a second time — for File ▸ Restart
// App that is the user's restart script executed twice, which is exactly the situation
// where a half-run script must not be re-run behind their back, since nothing here can know
// how far the first attempt got. Say `Never` of something that is repeatable and a stale
// hit turns into a config save that failed for no reason the user can act on.
enum class SshErrandRepeat {
    // Whole-file and idempotent: reading a config, and writing one (which truncates and
    // rewrites, so a second attempt lands the same bytes whatever the first one managed).
    Allowed,
    // Runs at most once, whatever happens. Anything with a side effect on the far end.
    Never,
};

// Get a session for `address` and hand it to `body`, which does the one operation the
// caller is for. Everything every caller shares lives here.
//
// The session is TAKEN FROM THE IDLE CACHE where one is there and connected otherwise, and
// returned to the cache afterwards if it is still healthy — so a run of errands against one
// machine pays one connect rather than one each (§6.3). Nothing about `body` changes: it is
// handed a connected `SshSession` and the remote path, exactly as before.
//
// Returns an error string for the caller to report; EMPTY means either success or "asked
// to stop", which the caller tells apart by whether its own result was filled in.
template <class Body>
QString withSshSession(const QString &address, SshPrompter *prompter,
                       const std::shared_ptr<SshWorkerShared> &shared, SshSession::Need need,
                       SshErrandRepeat repeat, Body body);
#endif

} // namespace loftail

#if defined(LOFTAIL_HAVE_SSH)
#include "SshWorkerPool.inl"
#endif
