#include <QtTest>

#include <QThread>

#include <atomic>

#include "GuiCallGate.h"
#include "PromptRelay.h"

using namespace loftail;

namespace {

// A prompter that answers from a script and records which thread it was asked on —
// which is the whole question here.
class RecordingPrompter final : public SshPrompter
{
public:
    HostKeyChoice confirmHostKey(const HostKeyInfo &) override
    {
        answeredOn = QThread::currentThread();
        ++calls;
        return hostKeyAnswer;
    }

    bool askPassword(const QString &target, const QString &, QString *password,
                     bool *remember) override
    {
        answeredOn = QThread::currentThread();
        ++calls;
        askedFor = target;
        if (password)
            *password = passwordToGive;
        if (remember)
            *remember = rememberToGive;
        return acceptPassword;
    }

    void progress(const QString &) override { ++progressCalls; }

    QThread *answeredOn = nullptr;
    int      calls = 0;
    int      progressCalls = 0;
    QString  askedFor;

    HostKeyChoice hostKeyAnswer = HostKeyChoice::AcceptOnce;
    bool          acceptPassword = true;
    QString       passwordToGive = QStringLiteral("hunter2");
    bool          rememberToGive = false;
};

// Runs one lambda off the main thread and reports when it is done.
class Asker : public QThread
{
public:
    explicit Asker(std::function<void()> body) : m_body(std::move(body)) {}
    void run() override
    {
        m_body();
        done = true;
    }
    std::atomic_bool done{false};

private:
    std::function<void()> m_body;
};

} // namespace

// M17 — a question asked on a fetcher's thread is answered on the application's
// (ARCHITECTURE.md §6.3.3).
//
// This exists because the alternative shape does not work: keyboard-interactive
// authentication is a conversation libssh2 drives through a callback, so a prompt cannot
// be restructured into fail-ask-retry and has to travel instead. What is pinned here is
// the part that is easy to get wrong — where the answer runs, that only one question is
// in flight at a time, and that a cancel unblocks a waiter rather than stranding it.
class TestPromptRelay : public QObject
{
    Q_OBJECT

private slots:
    void cleanup() { guiCallGate().reopen(); }

    void aQuestionFromAWorkerIsAnsweredOnTheMainThread();
    void aQuestionFromTheMainThreadRunsInline();
    void twoWorkersAreAnsweredOneAtATime();
    void cancelUnblocksAWaitingWorker();
    void cancelledMeansRefused();
    void aNullPrompterRefusesWithoutAsking();
};

void TestPromptRelay::aQuestionFromAWorkerIsAnsweredOnTheMainThread()
{
    RecordingPrompter prompter;
    PromptRelay relay(&prompter);

    QString password;
    bool remember = false;
    bool answered = false;

    Asker asker([&]() {
        answered = relay.askPassword(QStringLiteral("deploy@web1:22"),
                                     QStringLiteral("Password:"), &password, &remember);
    });
    asker.start();

    // The main thread has to keep turning for the question to be delivered — which is
    // the point: it is doing so because it is the application thread, not because
    // anything special was arranged.
    QTRY_VERIFY_WITH_TIMEOUT(asker.done.load(), 5000);
    asker.wait();

    QVERIFY(answered);
    QCOMPARE(password, QStringLiteral("hunter2"));
    QCOMPARE(prompter.askedFor, QStringLiteral("deploy@web1:22"));
    QCOMPARE(prompter.answeredOn, QThread::currentThread()); // NOT the asker's thread
}

void TestPromptRelay::aQuestionFromTheMainThreadRunsInline()
{
    // The ordinary interactive open, where the connect is already on the application
    // thread. Posting to ourselves and waiting for a queue we are the only one draining
    // would deadlock, so the gate must run it directly.
    RecordingPrompter prompter;
    PromptRelay relay(&prompter);

    QString password;
    bool remember = false;
    QVERIFY(relay.askPassword(QStringLiteral("deploy@web1:22"), QStringLiteral("Password:"),
                              &password, &remember));
    QCOMPARE(prompter.calls, 1);
    QCOMPARE(prompter.answeredOn, QThread::currentThread());
}

void TestPromptRelay::twoWorkersAreAnsweredOneAtATime()
{
    // Rule 3. Two fetchers connecting at once must not stack two modal dialogs, and the
    // gate is what makes that true without either of them knowing about the other.
    std::atomic_int inFlight{0};
    std::atomic_int highWater{0};

    class CountingPrompter final : public SshPrompter
    {
    public:
        CountingPrompter(std::atomic_int &in, std::atomic_int &high)
            : m_in(in), m_high(high) {}
        HostKeyChoice confirmHostKey(const HostKeyInfo &) override
        {
            const int now = ++m_in;
            m_high = qMax(m_high.load(), now);
            QThread::msleep(50); // as a modal dialog would, only briefly
            --m_in;
            return HostKeyChoice::AcceptOnce;
        }
        bool askPassword(const QString &, const QString &, QString *, bool *) override
        {
            return false;
        }
        void progress(const QString &) override {}

    private:
        std::atomic_int &m_in;
        std::atomic_int &m_high;
    };

    CountingPrompter prompter(inFlight, highWater);
    PromptRelay relay(&prompter);

    SshPrompter::HostKeyInfo info;
    info.host = QStringLiteral("web1");

    Asker one([&]() { relay.confirmHostKey(info); });
    Asker two([&]() { relay.confirmHostKey(info); });
    one.start();
    two.start();

    QTRY_VERIFY_WITH_TIMEOUT(one.done.load() && two.done.load(), 5000);
    one.wait();
    two.wait();

    QCOMPARE(highWater.load(), 1);
}

void TestPromptRelay::cancelUnblocksAWaitingWorker()
{
    // A fetcher asked to stop while it is blocked on a question must not hold up its own
    // teardown — which, since nothing joins a fetcher any more, is what keeps a cancel
    // that arrives late from costing anything.
    RecordingPrompter prompter;
    PromptRelay relay(&prompter);

    std::atomic_bool asked{false};
    SshPrompter::HostKeyInfo info;
    info.host = QStringLiteral("web1");

    // Cancel BEFORE the question can be delivered: this thread is the one that would
    // deliver it, and it is here instead.
    guiCallGate().cancel();

    Asker asker([&]() {
        asked = (relay.confirmHostKey(info) != SshPrompter::HostKeyChoice::Reject);
    });
    asker.start();

    QVERIFY2(asker.wait(5000), "the worker was left blocked on a cancelled gate");
    QVERIFY(!asked.load());
    QCOMPARE(prompter.calls, 0); // never asked at all
}

void TestPromptRelay::cancelledMeansRefused()
{
    // The safe direction, and the same answer a null prompter gives: "nobody is going to
    // answer" and "there is nobody to ask" are the same situation.
    RecordingPrompter prompter;
    prompter.hostKeyAnswer = SshPrompter::HostKeyChoice::AcceptAndRemember;
    PromptRelay relay(&prompter);

    guiCallGate().cancel();

    SshPrompter::HostKeyInfo info;
    info.host = QStringLiteral("web1");
    QCOMPARE(relay.confirmHostKey(info), SshPrompter::HostKeyChoice::Reject);

    QString password = QStringLiteral("not cleared");
    bool remember = false;
    QVERIFY(!relay.askPassword(QStringLiteral("deploy@web1:22"), QStringLiteral("Password:"),
                               &password, &remember));
    QCOMPARE(prompter.calls, 0);
}

void TestPromptRelay::aNullPrompterRefusesWithoutAsking()
{
    PromptRelay relay(nullptr);

    SshPrompter::HostKeyInfo info;
    info.host = QStringLiteral("web1");
    QCOMPARE(relay.confirmHostKey(info), SshPrompter::HostKeyChoice::Reject);

    QString password;
    bool remember = false;
    QVERIFY(!relay.askPassword(QStringLiteral("deploy@web1:22"), QStringLiteral("Password:"),
                               &password, &remember));
}

QTEST_MAIN(TestPromptRelay)
#include "tst_promptrelay.moc"
