#include "AlertPolicy.h"

namespace loftail {

AlertPolicy::Decision AlertPolicy::recordBatch(qint64 nowMs, int matchCount)
{
    if (matchCount <= 0)
        return {};

    m_pending += matchCount;

    // The first notification for a log is never delayed: waiting out an interval that
    // has not started yet would make the feature look broken on the one match a user is
    // most likely to be testing it with.
    if (m_everNotified && nowMs - m_lastNotifyMs < m_intervalMs)
        return {}; // suppressed; m_pending carries it to the next admitted decision

    Decision d;
    d.notify = true;
    d.count = m_pending;
    m_pending = 0;
    m_lastNotifyMs = nowMs;
    m_everNotified = true;
    return d;
}

AlertPolicy::Decision AlertPolicy::poll(qint64 nowMs)
{
    if (m_pending <= 0)
        return {};
    if (m_everNotified && nowMs - m_lastNotifyMs < m_intervalMs)
        return {};

    Decision d;
    d.notify = true;
    d.count = m_pending;
    m_pending = 0;
    m_lastNotifyMs = nowMs;
    m_everNotified = true;
    return d;
}

void AlertPolicy::reset()
{
    m_pending = 0;
    m_lastNotifyMs = 0;
    m_everNotified = false;
}

} // namespace loftail
