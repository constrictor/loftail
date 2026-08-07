#pragma once

#include <QString>

#include <functional>
#include <memory>

namespace loftail {

// Runs a piece of work ON THE APPLICATION THREAD, from whatever thread asks, and waits
// for it to finish (ARCHITECTURE.md §6.3.3).
//
// WHY THIS EXISTS. Connecting to a host runs on a fetcher's own thread, and two things
// inside a connect can only happen on the application thread: a modal prompt for a
// password or a host key, and a keychain read, which spins a nested event loop and may
// raise an unlock dialog of its own. Neither can be restructured into "fail, ask, retry"
// — keyboard-interactive authentication is a conversation libssh2 drives through a
// callback, with the server's own wording per prompt — so the question has to travel to
// the application thread and the answer has to travel back.
//
// ONE GATE FOR THE WHOLE PROCESS, and that is a feature rather than a shortcut: it makes
// "one question at a time" true by construction. Two fetchers connecting at once cannot
// stack two modal password dialogs, because the second one waits for the first.
//
// THE DISCIPLINE, in one place so that it is not re-derived per user:
//
//   1. The gate's mutex is never held while the work runs, nor while a caller waits.
//   2. A call is a shared_ptr owned by both sides. A cancelled caller returns while the
//      application thread may still be inside a modal dialog, and writing that dialog's
//      eventual answer into a dead stack frame is the one genuinely dangerous move
//      available here.
//   3. One call runs at a time. A second request delivered inside the first dialog's
//      nested event loop would stack two modal dialogs.
//   4. A caller must not hold ITS OWN locks across call(). A fetcher that called this
//      under the mutex guarding its status would block every watch tick on the
//      application thread — the freeze, by another route.
//   5. With no QCoreApplication, run the work inline. There is no other thread the
//      application thread could mean, and what a headless or scripted run must not do is
//      post to a queue nobody will ever drain — which is the hang, not the running.
//
// CANCELLING is what keeps rule 5 from being the only escape. cancel() refuses every
// pending and future call and wakes everyone waiting, so a fetcher asked to stop while
// it is blocked on a question does not hold up its own teardown — and, because nothing
// joins a fetcher any more (SourceSpool.h, retire()), a cancel that arrived late costs
// nothing either. Anything the application thread is ITSELF blocked in — a modal dialog
// — is not the gate's to close, so the owner of that thing registers an interrupt.
class GuiCallGate
{
public:
    GuiCallGate();
    ~GuiCallGate();

    GuiCallGate(const GuiCallGate &) = delete;
    GuiCallGate &operator=(const GuiCallGate &) = delete;

    // Run `work` on the application thread and wait for it to return. Runs it inline
    // when the caller is already on that thread, which is the ordinary case for an
    // interactive open and costs nothing.
    //
    // Returns false, WITHOUT running `work`, when the gate is cancelled. A false return
    // means "no answer", never "the answer was no" — the caller decides what that means,
    // and for every current caller it means refuse.
    bool call(const std::function<void()> &work);

    // Refuse everything from here on and wake every waiter. Idempotent, callable from
    // any thread. Also fires the interrupt, if one is registered and a call is running.
    void cancel();

    // Accept calls again. For a fresh window, and for tests.
    void reopen();

    bool cancelled() const;

    // Called by cancel() when a call is in flight, so that whatever the application
    // thread is blocked in can be made to return — in practice, rejecting a modal
    // dialog. Registered by whoever owns that blocking thing; runs on the CALLER's
    // thread, so an implementation that touches widgets must check that for itself.
    void setInterrupt(std::function<void()> interrupt);

private:
    class Pump;
    struct Call;

    void deliver(const std::shared_ptr<Call> &call);

    std::unique_ptr<Pump> d;
};

// The process's gate. Built on first use.
GuiCallGate &guiCallGate();

} // namespace loftail
