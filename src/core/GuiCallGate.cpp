#include "GuiCallGate.h"

#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QThread>
#include <QWaitCondition>

#include <deque>

namespace loftail {

// One question, owned jointly by the thread that asked it and the thread that answers it
// (rule 2). A cancelled asker returns while the application thread may still be inside
// the dialog, so the object has to outlive whichever of the two lets go first.
struct GuiCallGate::Call
{
    std::function<void()> work;
    bool                  done = false;
    bool                  abandoned = false; // the asker gave up; run nothing
};

// Lives on the application thread and drains the queue there.
//
// No Q_OBJECT and no signals of its own, so this needs no moc: everything arrives
// through QMetaObject::invokeMethod's functor form, which needs only a context object.
class GuiCallGate::Pump : public QObject
{
public:
    mutable QMutex                          mutex;
    QWaitCondition                          answered;
    std::deque<std::shared_ptr<Call>>       queue;
    bool                                    cancelled = false;
    bool                                    running = false; // rule 3
    std::function<void()>                   interrupt;

    // Application thread. Takes at most one call, runs it OUTSIDE the mutex (rule 1),
    // and re-arms itself if more arrived meanwhile.
    void drain()
    {
        std::shared_ptr<Call> next;
        {
            QMutexLocker lock(&mutex);
            if (running || queue.empty())
                return;
            next = queue.front();
            queue.pop_front();
            running = true;
        }

        if (!next->abandoned && next->work)
            next->work();

        bool more = false;
        {
            QMutexLocker lock(&mutex);
            running = false;
            next->done = true;
            more = !queue.empty();
            answered.wakeAll();
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
    QMutexLocker lock(&d->mutex);
    d->interrupt = std::move(interrupt);
}

bool GuiCallGate::cancelled() const
{
    QMutexLocker lock(&d->mutex);
    return d->cancelled;
}

void GuiCallGate::reopen()
{
    QMutexLocker lock(&d->mutex);
    d->cancelled = false;
}

void GuiCallGate::cancel()
{
    std::function<void()> interrupt;
    {
        QMutexLocker lock(&d->mutex);
        d->cancelled = true;
        for (auto &call : d->queue)
            call->abandoned = true;
        // Only worth interrupting if the application thread is actually inside something.
        if (d->running)
            interrupt = d->interrupt;
        d->answered.wakeAll();
    }

    // Outside the mutex (rule 1): an interrupt closes a modal dialog, which returns
    // control to the drain() frame that will want this very mutex.
    if (interrupt)
        interrupt();
}

bool GuiCallGate::call(const std::function<void()> &work)
{
    {
        QMutexLocker lock(&d->mutex);
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
        // Rule 3 still holds: reaching here while running is true would mean a dialog's
        // nested event loop delivered a second question, which is the stack this refuses.
        {
            QMutexLocker lock(&d->mutex);
            if (d->running)
                return false;
            d->running = true;
        }
        work();
        QMutexLocker lock(&d->mutex);
        d->running = false;
        d->answered.wakeAll();
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
        QMutexLocker lock(&d->mutex);
        d->queue.push_back(call);
    }
    d->rearm();

    QMutexLocker lock(&d->mutex);
    while (!call->done && !d->cancelled)
        d->answered.wait(&d->mutex);

    if (call->done)
        return !call->abandoned;

    // Cancelled with the question still outstanding. Mark it so the application thread
    // does not run it if it gets there first, and leave: the shared_ptr keeps the object
    // alive for whichever side is still holding one (rule 2).
    call->abandoned = true;
    return false;
}

GuiCallGate &guiCallGate()
{
    static GuiCallGate gate;
    return gate;
}

} // namespace loftail
