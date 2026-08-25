#include "SshWorkerPool.h"

#if defined(LOFTAIL_HAVE_SSH)
#include "SshSession.h"
#endif

#include <QElapsedTimer>
#include <QThread>

#include <vector>

namespace loftail {

void SshWorkerShared::abandon()
{
    // ABANDON, NEVER JOIN. Waiting here would make closing a tab on a host that is not
    // answering cost the whole connect timeout — and because the worker can be blocked
    // asking that very thread for a password, that wait can be a deadlock. The abort
    // makes the blocking call return now; the detached thread then finds `abandoned` set
    // and ends without reporting.
    abandoned = true;
#if defined(LOFTAIL_HAVE_SSH)
    std::scoped_lock lock(mutex);
    if (session)
        session->abort();
#endif
}

namespace {
#if defined(LOFTAIL_HAVE_SSH)
class WorkerThread : public QThread
{
public:
    std::function<void()> body;
    void run() override { body(); }
};

// Threads that have been started and not yet reaped.
//
// A finished one is deleted when the next worker starts. Anything STILL RUNNING when the
// process ends stays here — reachable, so the leak checker does not report it, which is
// the device SourceSpool::drainRetired() uses for an abandoned fetcher and for the same
// reason: the alternative is joining, and joining a worker that may be blocked asking the
// application thread for a password is a deadlock.
std::mutex g_workersMutex;
std::vector<QThread *> g_workers;

// Delete the ones that have already ended. Caller holds g_workersMutex.
void reapFinishedLocked()
{
    for (auto it = g_workers.begin(); it != g_workers.end();) {
        if ((*it)->isFinished()) {
            delete *it;
            it = g_workers.erase(it);
        } else {
            ++it;
        }
    }
}
#endif
} // namespace

void startSshWorker(std::function<void()> body)
{
#if !defined(LOFTAIL_HAVE_SSH)
    Q_UNUSED(body);
#else
    std::scoped_lock lock(g_workersMutex);
    reapFinishedLocked();
    auto *worker = new WorkerThread;
    worker->body = std::move(body);
    g_workers.push_back(worker);
    worker->start();
#endif
}

void drainSshWorkers(int budgetMs)
{
#if !defined(LOFTAIL_HAVE_SSH)
    Q_UNUSED(budgetMs);
#else
    // WAITING HERE IS THE POINT, and it is the one place this layer waits at all.
    //
    // Work is abandoned rather than joined when its owner goes, which is right while the
    // process is alive: the thread notices within a poll slice and ends on its own. At
    // SHUTDOWN that is not enough. Qt's own globals — the socket engine handlers, the
    // thread data — are torn down when the application object goes, and a worker still
    // inside QTcpSocket at that moment writes through a pointer that has just become null.
    // It crashed exactly there: a SEGV in QAbstractSocketPrivate::initSocketLayer(), on
    // the worker, under a leak-checking run whose exit timing loses that race every time.
    //
    // So the owner drains before it goes. The wait is BOUNDED because a budget that can be
    // exceeded is better than a quit that can hang: every worker has already been told to
    // stop by the time this runs, and the connect loop checks that between 250 ms slices,
    // so the budget is ample rather than tight.
    QElapsedTimer clock;
    clock.start();
    forever {
        {
            std::scoped_lock lock(g_workersMutex);
            reapFinishedLocked();
            if (g_workers.empty())
                return;
        }
        if (clock.elapsed() >= budgetMs)
            return; // left parked and reachable; better than blocking the quit
        QThread::msleep(10);
    }
#endif
}

} // namespace loftail
