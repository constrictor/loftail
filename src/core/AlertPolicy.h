#pragma once

#include <QtGlobal>

namespace loftail {

// M19 — how often a log is allowed to interrupt (SPEC.md §7, ARCHITECTURE.md §7.5.3).
//
// A highlight rule carrying HighlightAction::Notify raises a desktop notification when
// it matches a record that arrives while the log is not being looked at. Left ungated
// that is a stream: a chatty ERROR rule on a polled log would fire every tick, and one
// ingest batch can carry ten thousand matching records.
//
// The rate limit is therefore structural rather than advisory, and in two layers:
//
//   1. ONE decision per ingest batch. recordBatch() is called once per tick with the
//      number of matches in it, never once per record — so "ten thousand matches in one
//      tick is one notification" is true by construction and needs no special case.
//   2. A minimum interval between notifications for one log. Matches suppressed inside
//      it are COALESCED, not dropped: they accumulate, and the next admitted decision
//      reports how many there were. Dropping them would make the notification lie about
//      what happened while it was quiet.
//
// No QObject, no widgets, no timer, and the clock is a parameter — so the whole policy
// is testable without a QApplication and without waiting (tst_alertpolicy). One instance
// per open document, because the interval is per log; it is transient window state, not
// a property of the file, so it lives beside the tab marker rather than on the Document.
//
// A backlog needs a PUMP. `ingested` only fires on a tick that produced records, so a
// burst followed by silence would leave suppressed matches unreported forever; the
// window polls every document on a timer while any rule wants notifications.
class AlertPolicy
{
public:
    struct Decision
    {
        bool notify = false; // raise a notification now
        int  count = 0;      // how many matches it stands for, including coalesced ones
    };

    // Ten seconds: long enough that a one-second poll on a busy rule cannot become a
    // stream, short enough that a notification still describes something recent.
    static constexpr qint64 kDefaultIntervalMs = 10000;

    explicit AlertPolicy(qint64 minIntervalMs = kDefaultIntervalMs)
        : m_intervalMs(minIntervalMs)
    {
    }

    // One ingest batch carrying `matchCount` matching records arrived at `nowMs`.
    // `nowMs` is any monotonic millisecond clock (QElapsedTimer in the window; a plain
    // counter in tests).
    Decision recordBatch(qint64 nowMs, int matchCount);

    // Release a backlog that recordBatch() suppressed, once the interval has passed.
    // Returns a no-op decision when there is nothing pending or it is still too soon.
    Decision poll(qint64 nowMs);

    // Forget the backlog and the last-notified time — a rescan, a rotation, or the log
    // being closed. What was suppressed described records that no longer exist.
    void reset();

    // Matches suppressed and not yet reported. For tests and for the window's decision
    // about whether polling this document can do anything.
    int pending() const { return m_pending; }

private:
    qint64 m_intervalMs;
    qint64 m_lastNotifyMs = 0;
    bool   m_everNotified = false;
    int    m_pending = 0;
};

} // namespace loftail
