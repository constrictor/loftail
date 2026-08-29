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

#include "GuiCallGate.h"

#include <QCoreApplication>
#include <QObject>
#include <QThread>

#include <condition_variable>
#include <deque>
#include <mutex>

namespace loftail {

// One question, owned jointly by the thread that asked it and the thread that answers it
// (rule 2). A cancelled asker returns while the application thread may still be inside
// the dialog, so the object has to outlive whichever of the two lets go first.
struct GuiCallGate::Call
{
    std::function<void()> work;
    bool                  done = false;
    bool                  abandoned = false; // the asker gave up BEFORE it started

    // Set under the mutex the instant the application thread commits to running this,
    // and it is what makes `abandoned` a decision rather than a race. Once this is true
    // the asker MUST wait for done, cancelled or not: `work` holds references into the
    // asker's stack frame (&password, &choice, &message), so returning early would let
    // that frame die underneath a call already executing.
    bool                  started = false;
};

// Lives on the application thread and drains the queue there.
//
// No Q_OBJECT and no signals of its own, so this needs no moc: everything arrives
// through QMetaObject::invokeMethod's functor form, which needs only a context object.
class GuiCallGate::Pump : public QObject
{
public:
    mutable std::mutex                      mutex;
    std::condition_variable                 answered;
    std::deque<std::shared_ptr<Call>>       queue;
    bool                                    cancelled = false;
    bool                                    running = false; // rule 3
    std::function<void()>                   interrupt;

    // Rule 3's bookkeeping, in one place because the queued path and the inline path
    // must agree about it exactly.
    //
    // `owner` is what tells a SECOND ASKER — refuse, or two modal dialogs stack — from
    // the running work RE-ENTERING the gate on the very thread already inside it, which
    // is not a stack at all and must run. `depth` counts those re-entries and is the
    // ONLY thing that clears `running`: a nested return that cleared it would unblock
    // the queue while the outer modal is still on screen, which is the stack again by
    // the other door.
    Qt::HANDLE                              owner = nullptr;
    int                                     depth = 0;

    // Both called with `mutex` held. leaveLocked() answers whether this was the
    // outermost entry, i.e. whether the gate is free again.
    void enterLocked()
    {
        running = true;
        owner = QThread::currentThreadId();
        ++depth;
    }

    bool leaveLocked()
    {
        if (--depth > 0)
            return false;
        running = false;
        owner = nullptr;
        return true;
    }

    // Application thread. Takes at most one call, runs it OUTSIDE the mutex (rule 1),
    // and re-arms itself if more arrived meanwhile.
    void drain()
    {
        std::shared_ptr<Call> next;
        bool run = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (running || queue.empty())
                return;
            next = queue.front();
            queue.pop_front();
            // DECIDED HERE, under the mutex, and not re-read afterwards. Reading
            // `abandoned` outside the lock is a race on the one bit that says whether the
            // asker's stack is still alive — and losing it means running a lambda over a
            // dead frame, which is a crash a long way from its cause.
            run = !next->abandoned && next->work != nullptr;
            if (run) {
                next->started = true;
                // Records the application thread as the owner, which is what lets the
                // work reach back through the gate — GuiSshPrompter's body asking the
                // marshalled SecretStore whether there is a keychain — instead of being
                // refused and handed a default (bugs.md 31).
                enterLocked();
            }
        }

        if (run)
            next->work();

        bool more = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (run)
                leaveLocked();
            next->done = true;
            more = !queue.empty();
            answered.notify_all();
        }

        if (more)
            rearm();
    }

    void rearm()
    {
        QMetaObject::invokeMethod(this, [this]() { drain(); }, Qt::QueuedConnection);
    }
};

GuiCallGate::GuiCallGate() : d(std::make_unique<Pump>())
{
    // The pump must belong to the application thread, because that is the thread whose
    // event loop delivers to it. A gate built before the application exists — a static,
    // most likely — is moved across when the first call discovers one.
    if (QCoreApplication *app = QCoreApplication::instance())
        d->moveToThread(app->thread());
}

GuiCallGate::~GuiCallGate()
{
    cancel();
}

void GuiCallGate::setInterrupt(std::function<void()> interrupt)
{
    std::unique_lock<std::mutex> lock(d->mutex);
    d->interrupt = std::move(interrupt);
}

bool GuiCallGate::cancelled() const
{
    std::unique_lock<std::mutex> lock(d->mutex);
    return d->cancelled;
}

void GuiCallGate::reopen()
{
    std::unique_lock<std::mutex> lock(d->mutex);
    d->cancelled = false;
}

void GuiCallGate::cancel()
{
    std::function<void()> interrupt;
    {
        std::unique_lock<std::mutex> lock(d->mutex);
        d->cancelled = true;
        // Only the ones that have not started. A started call is the application thread's
        // business until it returns, and its asker stays parked until then.
        for (auto &call : d->queue) {
            if (!call->started)
                call->abandoned = true;
        }
        // Only worth interrupting if the application thread is actually inside something.
        if (d->running)
            interrupt = d->interrupt;
        d->answered.notify_all();
    }

    // Outside the mutex (rule 1): an interrupt closes a modal dialog, which returns
    // control to the drain() frame that will want this very mutex.
    if (interrupt)
        interrupt();
}

bool GuiCallGate::call(const std::function<void()> &work)
{
    {
        std::unique_lock<std::mutex> lock(d->mutex);
        if (d->cancelled)
            return false;
    }

    QCoreApplication *app = QCoreApplication::instance();

    // Rule 5. With no application there is no other thread "the application thread"
    // could mean, so running the work here is not a compromise, it is the only reading —
    // and it is what the hang this rule guards against actually requires avoiding:
    // posting to a queue that nobody will ever drain. A guiless test's scripted prompter
    // and fake secret store both work exactly as they did before any of this existed.
    if (!app || QThread::currentThread() == app->thread()) {
        // Already there. Run it directly rather than posting to ourselves and waiting
        // for a queue we are the only one draining — that would deadlock, and it is also
        // the ordinary path for an interactive open, where the connect and the dialog
        // are on the same thread to begin with.
        //
        // Rule 3 still holds, and the whole of what it is for is WHOSE question this is.
        // A call arriving from a DIFFERENT thread while one runs is the second asker the
        // rule refuses — its dialog would stack on the one already up. A call from the
        // thread that is already inside the running work is not that: there is no second
        // asker, and refusing it does not prevent a dialog, it silently skips the work
        // and hands the caller its default. Testing `running` alone did exactly that to
        // GuiSshPrompter, whose body runs under the gate and asks the marshalled
        // SecretStore whether a keychain is there — answered "no" on a machine running
        // one, so the remembered password went to plain text (bugs.md 31).
        {
            std::unique_lock<std::mutex> lock(d->mutex);
            if (d->running && d->owner != QThread::currentThreadId())
                return false;
            d->enterLocked();
        }
        work();
        bool more = false;
        {
            std::unique_lock<std::mutex> lock(d->mutex);
            more = d->leaveLocked() && !d->queue.empty();
            d->answered.notify_all();
        }

        // Re-arm, because drain() early-returns without re-posting while a call is in
        // flight: a queued request whose posted drain() was delivered INTO this call's
        // nested event loop was dropped, and nothing else will ever come back for it —
        // the asker parks on answered.wait() until some unrelated call or the teardown
        // cancel releases it (bugs.md 35). It has to be AFTER `running` is cleared and
        // only at depth 0, or the posted drain() is dropped a second time for the same
        // reason and the fix reproduces the bug exactly.
        if (more)
            d->rearm();
        return true;
    }

    if (d->thread() != app->thread()) {
        // The pump was built by a thread that is not the application's and cannot be
        // moved from here — only its current owner may do that. In practice this cannot
        // happen: the window touches the gate while installing its prompter, long before
        // any fetcher exists. Refuse rather than post to a queue nobody drains, because
        // the failure mode of the alternative is a worker that waits forever.
        return false;
    }

    auto call = std::make_shared<Call>();
    call->work = work;

    {
        std::unique_lock<std::mutex> lock(d->mutex);
        d->queue.push_back(call);
    }
    d->rearm();

    std::unique_lock<std::mutex> lock(d->mutex);
    // A cancel releases a call that has NOT STARTED. One that has must be waited out —
    // `work` holds references into this frame, so returning here would let the frame die
    // while the application thread is still inside the lambda. That is what cancel()'s
    // interrupt is for: it ends the dialog, so the wait this keeps is a moment, not the
    // user's own time.
    d->answered.wait(lock, [&] { return call->done || (d->cancelled && !call->started); });

    if (call->done)
        return !call->abandoned;

    // Cancelled with the question still queued and untouched. Mark it so the application
    // thread skips it, and leave: the shared_ptr keeps the object alive for whichever
    // side is still holding one (rule 2).
    call->abandoned = true;
    return false;
}

GuiCallGate &guiCallGate()
{
    static GuiCallGate gate;
    return gate;
}

} // namespace loftail
