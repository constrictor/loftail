#include <QtTest>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "GuiCallGate.h"

using namespace loftail;

namespace {

// How long each hammering case runs, in wall clock asked for rather than delivered: the
// application thread spends most of its time inside work, so the event loop is saturated
// and a case takes several times this. Long enough to enter the window below thousands
// of times, short enough not to be the slowest file in the suite. The refusal count is
// asserted, so cutting this too far makes the case say so rather than quietly pass on no
// coverage.
constexpr int kRunMs = 400;

// A call's frame, watched from the outside.
//
// The bug this file exists for (M17, "Fix two lifetime bugs in the call gate") was
// GuiCallGate::call() returning while its work was still RUNNING on the application
// thread. The work holds references into the caller's frame — &password, &choice,
// &message — so the frame then died underneath a live lambda: a use-after-free a long
// way from its cause, and one that needed a 1-in-28 timing window to become a crash.
//
// Rather than wait for the memory symptom, watch the LOGICAL invariant: work that has
// been entered and not left, at the moment call() returns. That is rule 2 stated
// directly, and it fails on the first violation instead of on the twenty-eighth run.
struct Frame
{
    std::atomic_bool entered{false};
    std::atomic_bool left{false};
};

// Rule 3: one call runs at a time, so two modal dialogs can never stack. Global because
// the gate is.
std::atomic_int  g_inWork{0};
std::atomic_bool g_twoAtOnce{false};

// Rule 2: nobody was released while their work was mid-flight.
std::atomic_bool g_releasedWhileRunning{false};

std::atomic_llong g_answered{0};
std::atomic_llong g_refused{0};

// Set for the case that models a modal dialog: every Nth piece of work spins a nested
// event loop and lets a cancel arrive inside it.
std::atomic_bool g_cancelInsideWork{false};
std::atomic_int  g_workCount{0};

// WHY A NESTED EVENT LOOP, AND NOT JUST A TIMER CALLING cancel().
//
// cancel() and drain() both run on the application thread, so a cancel posted to that
// thread can never land WHILE work is executing — it waits its turn, and the window the
// M17 bug lived in never opens. (A first draft of this file did exactly that and passed
// against the broken gate five times out of five.)
//
// What actually happens in the product is that the work IS a modal dialog. Its nested
// event loop delivers everything the outer one would, including the window going away
// and taking the gate's callers with it — so cancel() runs re-entrantly, on the
// application thread, with the work still on the stack. That is the shape reproduced
// here, and it is the only shape in which rule 2 has anything to say.
void maybeCancelFromInsideTheDialog()
{
    if (!g_cancelInsideWork.load())
        return;
    if ((g_workCount.fetch_add(1) % 4) != 0)
        return;

    QEventLoop nested;
    QTimer::singleShot(0, &nested, [&nested]() {
        guiCallGate().cancel();
        nested.quit();
    });
    nested.exec();

    // Still inside the work. A gate that releases this call's own caller on the strength
    // of `cancelled` alone has just handed the caller's frame back while these very
    // references are live — which is what the window below is long enough to catch.
    QElapsedTimer t;
    t.start();
    while (t.nsecsElapsed() < 300000) { }
}

// Asks questions in a loop, exactly the way PromptRelay does: a lambda holding
// references into this frame, and no lock of its own held across call() (rule 4).
//
// std::thread rather than QThread, which is the one place this file cares which it is:
// QThread::wait() joins through Qt's own condition variable inside an uninstrumented
// libQt6Core, so TSan cannot see the join and reports the asker's teardown as a race
// against its own loop. std::thread::join() goes through pthread_join, which TSan
// intercepts. A fetcher is a QThread for good reasons; a test's load generator is not.
class Asker
{
public:
    explicit Asker(std::atomic_bool *stop) : m_stop(stop) {}

    void run()
    {
        while (!m_stop->load()) {
            Frame   frame;
            QString answer;

            const bool ok = guiCallGate().call([&frame, &answer]() {
                frame.entered.store(true);
                if (g_inWork.fetch_add(1) != 0)
                    g_twoAtOnce.store(true);
                answer = QStringLiteral("secret");
                maybeCancelFromInsideTheDialog();
                g_inWork.fetch_sub(1);
                frame.left.store(true);
            });

            // The frame is about to die. If the work is inside it, the gate broke rule 2
            // — and it broke it here, not three seconds later in an allocator.
            if (frame.entered.load() && !frame.left.load())
                g_releasedWhileRunning.store(true);

            (ok ? g_answered : g_refused).fetch_add(1);
        }
    }

private:
    std::atomic_bool *m_stop;
};

// Runs `threads` askers against the gate for kRunMs while `arrange` drives whatever the
// case needs from the application thread, then stops everything. Leaves the gate open.
void hammer(int threads, const std::function<void(QTimer *)> &arrange)
{
    g_inWork.store(0);
    g_twoAtOnce.store(false);
    g_releasedWhileRunning.store(false);
    g_answered.store(0);
    g_refused.store(0);
    g_workCount.store(0);

    guiCallGate().reopen();

    std::atomic_bool         stop{false};
    std::vector<Asker>       askers;
    askers.reserve(static_cast<size_t>(threads));
    for (int i = 0; i < threads; ++i)
        askers.emplace_back(&stop);

    std::vector<std::thread> threadsRunning;
    threadsRunning.reserve(askers.size());
    for (Asker &a : askers)
        threadsRunning.emplace_back([&a]() { a.run(); });

    QTimer driver;
    arrange(&driver);

    QEventLoop loop;
    QTimer::singleShot(kRunMs, &loop, [&]() {
        driver.stop();
        stop.store(true);
        // Release anyone parked on a shut gate, or their threads never finish.
        guiCallGate().cancel();
        loop.quit();
    });
    loop.exec();

    for (std::thread &t : threadsRunning)
        t.join();
    g_cancelInsideWork.store(false);
    guiCallGate().reopen();
}

} // namespace

// M17 — the concurrent window the rest of the suite does not open.
//
// tst_promptrelay covers what the gate MEANS: which thread answers, one question at a
// time, a cancel unblocks a waiter. It drives each of those once, in isolation, which is
// how a behaviour is pinned — and is exactly why it did not catch the two lifetime bugs
// fixed in "Fix two lifetime bugs in the call gate". Those needed a cancel to land while
// a call was in one particular one of its three states, raised from inside the very work
// that call was running.
//
// So this file adds no new meaning. It adds PRESSURE, aimed at one place, with the
// invariants checked from outside the gate rather than through its return value. It is
// also the test a sanitizer needs in order to have anything to find: neither ASan nor
// TSan reported these bugs against the suite as it stood, because nothing in the suite
// went near the window — 60 twelve-way-parallel ASan runs of tst_remoteopen against the
// pre-fix tree reproduced nothing at all.
//
// Ungated and network-free: the gate is core machinery with no transport under it.
class TestGateStress : public QObject
{
    Q_OBJECT

private slots:
    void aCancelInsideTheDialogDoesNotReleaseTheRunningCaller();
    void onlyOneCallRunsAtATimeUnderLoad();
    void everyCallIsEitherAnsweredOrRefused();
};

// RULE 2, and the bug itself. A cancel raised from inside the work — the modal dialog
// closing under the caller — must not release the caller whose frame that work is
// reading. Verified to FAIL against the pre-fix gate.
void TestGateStress::aCancelInsideTheDialogDoesNotReleaseTheRunningCaller()
{
    g_cancelInsideWork.store(true);
    hammer(6, [](QTimer *driver) {
        // Reopen so the askers keep coming; the cancels are raised inside the work.
        QObject::connect(driver, &QTimer::timeout, []() { guiCallGate().reopen(); });
        driver->start(1);
    });

    QVERIFY2(!g_releasedWhileRunning.load(),
             "call() returned while its work was still running on the application "
             "thread; the caller's frame died underneath a live lambda (rule 2)");
    // A run that refused nothing proves nothing — it would mean no cancel ever landed
    // anywhere interesting and the case had silently stopped testing what it names.
    QVERIFY2(g_refused.load() > 0,
             qPrintable(QStringLiteral("no call was ever refused (%1 answered): the "
                                       "cancel window was not entered")
                            .arg(g_answered.load())));
}

// RULE 3, under the same pressure. Two dialogs must never stack, and the inline path —
// a caller already on the application thread — must not be a way around it.
void TestGateStress::onlyOneCallRunsAtATimeUnderLoad()
{
    hammer(6, [](QTimer *driver) {
        // The application thread places its OWN calls, taking the inline branch of
        // call() while queued ones are draining.
        QObject::connect(driver, &QTimer::timeout, []() {
            Frame   frame;
            QString answer;
            guiCallGate().call([&frame, &answer]() {
                frame.entered.store(true);
                if (g_inWork.fetch_add(1) != 0)
                    g_twoAtOnce.store(true);
                answer = QStringLiteral("inline");
                g_inWork.fetch_sub(1);
                frame.left.store(true);
            });
        });
        driver->start(0);
    });

    QVERIFY2(!g_twoAtOnce.load(), "two calls ran at once (rule 3)");
    QVERIFY2(!g_releasedWhileRunning.load(), "a caller was released mid-work (rule 2)");
}

// A caller must always get one answer or the other. A gate that neither runs the work
// nor refuses is the hang this machinery exists to avoid, and it shows up here as
// threads that never finish rather than as a wrong answer.
void TestGateStress::everyCallIsEitherAnsweredOrRefused()
{
    hammer(4, [](QTimer *driver) {
        QObject::connect(driver, &QTimer::timeout, []() {
            guiCallGate().cancel();
            // Shut for a moment, so callers parked on the wait actually observe it and
            // take the abandon decision, rather than the flag flickering past them.
            QThread::msleep(2);
            guiCallGate().reopen();
        });
        driver->start(1);
    });

    QVERIFY(g_answered.load() > 0);
    QVERIFY(g_refused.load() > 0);
    QVERIFY(!g_releasedWhileRunning.load());
    QVERIFY(!g_twoAtOnce.load());
}

QTEST_MAIN(TestGateStress)
#include "tst_gatestress.moc"
