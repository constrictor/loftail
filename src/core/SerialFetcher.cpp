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

#include "SerialFetcher.h"

#include "DiagnosticLog.h"
#include "ExecTransport.h"
#include "RetryCountdown.h"
#include "SerialDevice.h"
#include "SerialFetchOptions.h"
#include "SshExecCommands.h"
#include "SshPrompter.h"
#include "SshRetryPolicy.h"

#include <QCoreApplication>
#include <QFile>
#include <QThread>

#include <condition_variable>
#include <mutex>
#include <utility>

namespace loftail {
namespace {

struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::SerialFetcher)
};

// The slow cadence, shared with the network transport so two tabs on one machine do not count
// down to different instants.
constexpr int kSlowPollMs = 5000;

} // namespace

#if !defined(LOFTAIL_HAVE_SERIAL)

std::unique_ptr<SourceFetcher> makeSerialFetcher(const RemoteLocation &location, QString *error)
{
    Q_UNUSED(location);
    if (error) {
        // A REFUSAL DECIDED WITH NO I/O, so it fails the open outright and says why (M17) —
        // the same sentence shape a build with no SSH or no archive support gives.
        *error = Tr::tr("Serial support is not built into this copy of loftail, so a log on a "
                        "device cannot be opened.");
    }
    return nullptr;
}

#else

namespace {

class SerialFetcher final : public SourceFetcher
{
public:
    explicit SerialFetcher(RemoteLocation location) : m_location(std::move(location)) {}

    ~SerialFetcher() override
    {
        // The device handle goes LAST, and only once the worker has stopped touching it — the
        // port is what the next open of this device needs back, and handing it over while a
        // read is still in flight is what would make a reopen race the wind-down.
        requestStop();
    }

    bool start(const QString &spoolDir, QString *error) override
    {
        Q_UNUSED(error);   // nothing here can fail any more (M17): the tab goes up, then asks
        m_spoolDir = spoolDir;
        m_device = SerialDeviceRegistry::instance().acquire(m_location.host);
        // Decided on the thread that would be ASKED, exactly as SshFetcher decides it: a
        // permission, never a pointer (PromptRelay.h).
        m_wantsPrompter = sshPrompter() != nullptr;
        setState(FetchStatus::State::Connecting);
        m_worker = std::make_unique<Worker>(this);
        m_worker->start();
        return true;
    }

    void requestStop() override
    {
        {
            const std::scoped_lock lock(m_mutex);
            if (m_stopping)
                return;
            m_stopping = true;
        }
        m_wake.notify_all();
        if (m_device)
            m_device->abort();   // latched, never acted on: the thread inside the port owns it
        setState(FetchStatus::State::Disconnected);
    }

    bool isStopped() const override
    {
        return !m_worker || m_worker->isFinished();
    }

    FetchStatus status() const override
    {
        const std::scoped_lock lock(m_mutex);
        return m_status;
    }

    QString spoolPath(quint64 generation) const override
    {
        return m_spoolDir + QStringLiteral("/gen-%1.log").arg(generation);
    }

    void pokeNow() override
    {
        {
            const std::scoped_lock lock(m_mutex);
            // Clearing the latch is the whole of what File ▸ Reconnect does, and it grants the
            // next attempt a prompter: the reader is here now, which is what makes asking them
            // something reasonable where an unattended retry must not.
            m_refused = false;
            m_wantsPrompter = sshPrompter() != nullptr;
        }
        m_wake.notify_all();
    }

private:
    class Worker : public QThread
    {
    public:
        explicit Worker(SerialFetcher *owner) : m_owner(owner) {}
        void run() override { m_owner->tailLoop(); }

    private:
        SerialFetcher *m_owner;
    };

    void tailLoop();
    void pollOnce();
    bool establish();
    bool beginGeneration(qint64 size);
    bool fetchForward(qint64 from, qint64 to);
    void setState(FetchStatus::State state);
    void setError(const QString &message);
    void setWaiting(const QString &message);
    bool stopping() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_stopping;
    }

    RemoteLocation                 m_location;
    QString                        m_spoolDir;
    std::shared_ptr<SerialDevice>  m_device;
    std::unique_ptr<ExecTransport> m_exec;
    std::unique_ptr<Worker>        m_worker;
    SerialFetchOptions             m_options;
    ReconnectGrace                 m_grace;

    mutable std::mutex      m_mutex;
    std::condition_variable m_wake;
    FetchStatus             m_status;
    bool                    m_stopping = false;
    bool                    m_refused = false;
    bool                    m_wantsPrompter = false;
    qint64                  m_lastSize = 0;
    qint64                  m_lastMtime = kUnknownMtime;
};

void SerialFetcher::setState(FetchStatus::State state)
{
    const std::scoped_lock lock(m_mutex);
    if (m_status.state == state)
        return;
    m_status.state = state;
    // Cleared for every state that is not Error or Waiting, and the retry deadline with it: a
    // published deadline belongs to the sleep that is about to happen and to no other.
    if (state != FetchStatus::State::Error && state != FetchStatus::State::Waiting)
        m_status.error.clear();
    m_status.retryAtMs = 0;
    diagLog("serial", QStringLiteral("%1: %2").arg(m_location.toString(),
                                                  QString::fromLatin1(fetchStateName(state))));
}

void SerialFetcher::setError(const QString &message)
{
    const std::scoped_lock lock(m_mutex);
    m_status.state = FetchStatus::State::Error;
    m_status.error = message;
    m_status.retryAtMs = 0;
}

void SerialFetcher::setWaiting(const QString &message)
{
    // NOT AN ERROR: the device or the log is not there, and this fetcher is still trying.
    // SpooledLogSource::originVanished() reads exactly this state.
    const std::scoped_lock lock(m_mutex);
    m_status.state = FetchStatus::State::Waiting;
    m_status.error = message;
    m_status.retryAtMs = 0;
}

bool SerialFetcher::beginGeneration(qint64 size)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    const quint64 next = m_status.generation + 1;
    lock.unlock();

    qint64 base = 0;
    if (m_options.tailStartBytes > 0 && size > m_options.tailStartBytes)
        base = size - m_options.tailStartBytes;

    const QString path = spoolPath(next);
    QFile spool(path);
    if (!spool.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(Tr::tr("Cannot write the local cache file %1.").arg(path));
        return false;
    }
    spool.close();

    lock.lock();
    m_status.baseOffset = base;
    m_status.committedSize = 0;
    m_status.totalSize = size;
    m_status.generation = next;   // published LAST, once its file exists and is empty
    return true;
}

bool SerialFetcher::fetchForward(qint64 from, qint64 to)
{
    const quint64 generation = status().generation;
    const QString path = spoolPath(generation);
    QFile spool(path);
    if (!spool.open(QIODevice::Append)) {
        setError(Tr::tr("Cannot append to the local cache file %1.").arg(path));
        return false;
    }

    const qint64 chunk = serialChunkBytes(m_options.profile.baud);
    qint64 at = from;
    QByteArray buffer;
    while (at < to && !stopping()) {
        const qint64 want = qMin(chunk, to - at);
        buffer.resize(int(want));
        const qint64 got = m_device->readBytes(m_location.path, at, buffer.data(), want);
        if (got < 0) {
            setError(Tr::tr("Could not read %1 from %2.").arg(m_location.path, m_location.host));
            return false;
        }
        if (got == 0)
            break;   // nothing more to be had this turn; the next poll asks again
        if (spool.write(buffer.constData(), got) != got) {
            setError(Tr::tr("Cannot write to the local cache file %1.").arg(path));
            return false;
        }
        spool.flush();
        at += got;
        // PUBLISHED ONLY AFTER THE WRITE HAS LANDED, which is the entire synchronisation
        // between this thread and the reader: refreshSize() clamps to this, so a half-written
        // chunk is not merely unlikely to be observed, it is unobservable.
        const std::scoped_lock lock(m_mutex);
        m_status.committedSize = at - m_status.baseOffset;
    }
    return true;
}

bool SerialFetcher::establish()
{
    m_options = serialFetchOptions(m_location);

    // The password, if one is remembered, comes from the same cache and the same keychain the
    // network transport uses — keyed on target(), which for a device carries the `serial:`
    // prefix so a host of the same name cannot be handed this one's credential.
    QByteArray password;
    if (SshCredentialCache::has(m_location.target()))
        password = SshCredentialCache::password(m_location.target()).toUtf8();

    const SerialDevice::Ready ready =
        m_device->ensureReady(m_location.path, m_options, m_location, password);
    if (!ready.ok) {
        if (ready.refused) {
            // Told no, by the login or by the byte-safety ladder. A refusal that keeps its tab
            // and says why (M17) — and LATCHED, because a wrong password in a preset and a line
            // that mangles bytes are neither of them things another poll fixes.
            //
            // THE GRACE WINDOW IS WHAT MAKES THAT SAFE ON A DEVICE THAT HAS SIGNED IN BEFORE.
            // A board rebooting answers its console before its login is ready, so the first
            // refusal after an outage is a MOMENT rather than a standing fact — which is
            // exactly the distinction ReconnectGrace was written for (SshRetryPolicy.h), and
            // exactly the case a serial console meets more often than a network one.
            if (m_grace.signedInOnce() && m_grace.keepTrying(fetchMonotonicMs())) {
                setWaiting(ready.reason);
                return false;
            }
            const std::scoped_lock lock(m_mutex);
            m_refused = true;
            m_status.state = FetchStatus::State::Error;
            m_status.error = ready.reason;
            return false;
        }
        setWaiting(ready.reason);
        return false;
    }
    m_grace.signedIn();

    // The log-shaped half of the transport, bound to the device's two seams. Rebuilt on every
    // establish so that a reconnect settles its rungs afresh — settling once per session would
    // let one rotated-to file the chosen rung cannot parse strand the tab in "waiting".
    m_exec = std::make_unique<ExecTransport>(
        m_location.path, m_location.host,
        [this](const QString &command, QByteArray *out) { return m_device->run(command, out); },
        [this](qint64 offset, qint64 length) {
            QByteArray buffer;
            buffer.resize(int(length));
            return m_device->readBytes(m_location.path, offset, buffer.data(), length);
        });

    ExecTools tools;
    if (!m_exec->probe(&tools)) {
        setWaiting(Tr::tr("%1 answered, but `tail` and `head` are not there — loftail cannot "
                          "read a log without them.").arg(m_location.host));
        return false;
    }

    setState(FetchStatus::State::Priming);
    ExecTransport::Trouble trouble = ExecTransport::Trouble::None;
    QString why;
    if (!m_exec->openFile(&trouble, &why)) {
        if (trouble == ExecTransport::Trouble::Refused) {
            const std::scoped_lock lock(m_mutex);
            m_refused = true;
            m_status.state = FetchStatus::State::Error;
            m_status.error = why;
        } else {
            setWaiting(why);
        }
        return false;
    }

    const ExecAttrs attrs = m_exec->statPath();
    if (!attrs.ok) {
        setWaiting(Tr::tr("%1 is not readable on %2 right now.")
                       .arg(m_location.path, m_location.host));
        return false;
    }

    // BEGINGENERATION ANSWERS WHETHER IT WORKED AND THE CALLER ABORTS ON FALSE. A failure
    // leaves the OLD generation published, so carrying on would write this file's bytes onto
    // the end of the previous file's spool — which the reader is still indexing by offset.
    if (!beginGeneration(attrs.size))
        return false;

    if (!fetchForward(status().baseOffset, attrs.size))
        return false;

    m_lastSize = attrs.size;
    m_lastMtime = attrs.mtime;
    // Live only once bytes have landed: a Live carrying no message reads as a healthy tail
    // everywhere, which is what made a failed prime look like an ordinary empty log.
    setState(FetchStatus::State::Live);
    return true;
}

void SerialFetcher::pollOnce()
{
    // A REBOOT IS THE LINK, AND IT IS ASKED FIRST. The shell that was going to answer is gone,
    // so nothing else this turn means anything — and the device has already dropped the session
    // for every log on it, which is the shared-health split doing what it is for.
    if (m_device->takeReboot()) {
        diagLog("serial", QStringLiteral("%1: rebooted — signing in again")
                              .arg(m_location.toString()));
        m_exec.reset();
        setWaiting(Tr::tr("%1 restarted — signing in again").arg(m_location.host));
        return;
    }

    // The options may have been edited while this tab was open. ensureReady() is what decides
    // whether that costs a reopened port, a fresh login or nothing (serialChangeCost()), so the
    // comparison lives there rather than here — one device, several logs, one answer.
    const SerialFetchOptions current = serialFetchOptions(m_location);
    if (current.profile != m_options.profile || current.tailStartBytes != m_options.tailStartBytes) {
        diagLog("serial", QStringLiteral("%1: settings changed").arg(m_location.toString()));
        m_exec.reset();
        setWaiting(Tr::tr("Applying the new settings for %1").arg(m_location.host));
        return;
    }

    if (!m_exec) {
        establish();
        return;
    }

    const ExecAttrs attrs = m_exec->statPath();
    if (!attrs.ok) {
        // The stat is this transport's one round trip per turn, so it is where a device that
        // has gone away is noticed. Which of the two it is — gone, or the log gone — is what
        // classifyPath() answers, on the failure path only.
        const RemotePathReport report = m_exec->classifyPath();
        setWaiting(report.known ? m_exec->troubleText(report)
                                : Tr::tr("Lost the console on %1 — signing in again")
                                      .arg(m_location.host));
        if (!report.known) {
            m_exec.reset();
            m_device->dropSession();
        }
        return;
    }

    const qint64 consumed = status().baseOffset + status().committedSize;
    RemoteObservation seen;
    seen.size = attrs.size;
    seen.mtime = attrs.mtime;
    seen.lastSize = m_lastSize;
    seen.lastMtime = m_lastMtime;
    seen.consumed = consumed;
    // No handle exists on this transport, so the inode substitute is unavailable and the
    // weaker mtime comparison is what applies — the same position the exec fallback is in.
    seen.fstatTracksHandle = false;
    seen.handleValid = false;

    m_lastSize = attrs.size;
    m_lastMtime = attrs.mtime;

    switch (rotationVerdict(seen)) {
    case RotationVerdict::Rotated: {
        diagLog("serial", QStringLiteral("%1: replaced — re-reading").arg(m_location.toString()));
        setState(FetchStatus::State::Priming);
        ExecTransport::Trouble trouble = ExecTransport::Trouble::None;
        QString why;
        if (!m_exec->openFile(&trouble, &why)) {
            setWaiting(why);
            return;
        }
        const ExecAttrs fresh = m_exec->statPath();
        if (!beginGeneration(fresh.ok ? fresh.size : 0))
            return;
        // Live only if the fetch actually worked, exactly as the append branch below: a Live
        // carrying no message reads as a healthy tail everywhere (bugs.md 40).
        if (fetchForward(status().baseOffset, fresh.ok ? fresh.size : 0))
            setState(FetchStatus::State::Live);
        return;
    }
    case RotationVerdict::CompareNow:
    case RotationVerdict::ComparePaced:
        // A rewrite in place that grew would need the head compared, which costs a read over a
        // line moving 11 KB/s. Not done, and the cost is stated in SPEC.md rather than left to
        // be found: on a serial line such a rewrite is noticed when the size next shrinks.
        break;
    case RotationVerdict::Nothing:
        break;
    }

    if (attrs.size > consumed) {
        if (fetchForward(consumed, attrs.size))
            setState(FetchStatus::State::Live);
        return;
    }
    setState(FetchStatus::State::Live);
}

void SerialFetcher::tailLoop()
{
    forever {
        if (stopping())
            return;

        bool refused = false;
        {
            const std::scoped_lock lock(m_mutex);
            refused = m_refused;
        }
        if (!refused)
            pollOnce();

        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_stopping)
            return;
        const bool slow = m_status.state == FetchStatus::State::Error
            || m_status.state == FetchStatus::State::Waiting;
        const int wait = slow ? qMax(m_options.profile.pollMs, kSlowPollMs)
                              : qMax(200, m_options.profile.pollMs);
        // A DEADLINE AND NEVER A REMAINING DURATION, published in exactly one statement — the
        // one immediately before the sleep it describes. Read on the GUI's own tick, so a
        // duration would be stale by an unknown amount by the time anybody saw it. And nothing
        // is published for a fetcher that has given up: a tab must not promise an attempt that
        // is not coming.
        m_status.retryAtMs = (slow && !m_refused) ? fetchMonotonicMs() + wait : 0;
        m_wake.wait_for(lock, std::chrono::milliseconds(wait));
        m_status.retryAtMs = 0;
    }
}

} // namespace

std::unique_ptr<SourceFetcher> makeSerialFetcher(const RemoteLocation &location, QString *error)
{
    if (location.transport != RemoteLocation::Transport::Serial) {
        if (error)
            *error = Tr::tr("Not a serial log address: %1").arg(location.toString());
        return nullptr;
    }
    return std::make_unique<SerialFetcher>(location);
}

#endif // LOFTAIL_HAVE_SERIAL

} // namespace loftail
