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

#include "DiagnosticLog.h"

#include "Version.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStandardPaths>

#include <mutex>

namespace loftail {

namespace {

// Everything the log owns, behind one lock. A struct with a function-local static rather
// than namespace-scope objects, so nothing is constructed before main() and nothing is
// destroyed while a fetcher thread might still be writing — a retired fetcher outlives
// the window (SourceSpool.h), and a static QFile destroyed at exit would be a
// use-after-free on exactly that path. A leaked-on-purpose singleton has neither problem.
struct LogState
{
    std::mutex mutex;
    QString    directory;      // empty == resolve the default on first use
    QFile      file;
    bool       tried = false;  // an open was attempted; do not retry it every line
    bool       startWritten = false;

    // The monotonic clock every throttle key measures against. One timer for the
    // process rather than one per key, so the map holds two ints and not a QElapsedTimer.
    QElapsedTimer clock;
    struct Throttle
    {
        // A FLAG rather than a sentinel value in lastMs. Overloading 0 for "never
        // emitted" and then storing max(now, 1) to avoid it puts the first entry one
        // millisecond in the FUTURE, so `now - lastMs` is negative and compares below any
        // window at all — including a zero one. Every subsequent line is then suppressed
        // for as long as the clock reads 0, which on a fast machine is the whole of the
        // first millisecond of the process.
        bool   emitted = false;
        qint64 lastMs = 0;
        int    suppressed = 0;
    };
    QHash<QString, Throttle> throttles;
};

LogState &state()
{
    // Never destroyed, deliberately — see LogState above.
    static LogState *s = [] {
        auto *created = new LogState;
        created->clock.start();
        return created;
    }();
    return *s;
}

QString defaultDirectory()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    // AppLocalDataLocation is empty only where the platform has no such notion at all.
    // The working directory is a poor answer but a working one, and a diagnostic log
    // that silently writes nowhere is worse than one in an awkward place.
    return dir.isEmpty() ? QDir::currentPath() : dir;
}

// Caller holds the lock.
QString resolvedPathLocked(LogState &s)
{
    const QString dir = s.directory.isEmpty() ? defaultDirectory() : s.directory;
    return dir + QStringLiteral("/loftail.log");
}

// Caller holds the lock. Returns false when there is nowhere to write, which is not an
// error anybody is told about: loftail's own log failing must never become a second
// fault on top of the one being diagnosed.
bool ensureOpenLocked(LogState &s)
{
    if (s.file.isOpen())
        return true;
    if (s.tried)
        return false; // a directory that cannot be made will not become makeable
    s.tried = true;

    const QString path = resolvedPathLocked(s);
    QDir().mkpath(QFileInfo(path).absolutePath());
    s.file.setFileName(path);
    return s.file.open(QIODevice::WriteOnly | QIODevice::Append);
}

// Caller holds the lock. ONE rollover, so the total on disk is bounded by twice the cap
// rather than growing a numbered series nobody prunes. The previous .1 is replaced: the
// interesting evidence is the recent kind, and a log about connection attempts that kept
// the oldest megabyte and dropped the newest would be exactly backwards.
void rollOverIfFullLocked(LogState &s)
{
    if (!s.file.isOpen() || s.file.size() <= kDiagLogMaxBytes)
        return;
    const QString path = s.file.fileName();
    const QString previous = path + QStringLiteral(".1");
    s.file.close();
    QFile::remove(previous);
    QFile::rename(path, previous);
    s.tried = false;
    s.startWritten = false; // the fresh file identifies its own run again
    ensureOpenLocked(s);
}

// Caller holds the lock.
void writeLineLocked(LogState &s, const char *area, const QString &message)
{
    if (!ensureOpenLocked(s))
        return;
    // ISO-8601 UTC with milliseconds. UTC because the one thing this file is correlated
    // against is a log4cplus log, whose own zone is whatever the writing machine's was,
    // and a diagnostic timestamp that needs its own zone explained is no use at all —
    // the same reasoning invariant #10 applies to Record::timestamp.
    const QString line = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
        + QStringLiteral(" [") + QString::fromLatin1(area) + QStringLiteral("] ")
        + message + QLatin1Char('\n');
    s.file.write(line.toUtf8());
    // Flushed per line, not per buffer. The faults this exists to explain include the
    // ones where loftail does not get to exit tidily, and a buffered last line is the
    // line that would have said why.
    s.file.flush();
    rollOverIfFullLocked(s);
}

// Caller holds the lock.
void writeSessionStartLocked(LogState &s)
{
    if (s.startWritten)
        return;
    s.startWritten = true;
    writeLineLocked(s, "app",
                    QStringLiteral("loftail %1 starting (build %2)")
                        .arg(applicationVersionString(),
                             applicationBuildId().isEmpty() ? QStringLiteral("local")
                                                            : applicationBuildId()));
}

} // namespace

QString diagLogPath()
{
    LogState &s = state();
    std::scoped_lock lock(s.mutex);
    return resolvedPathLocked(s);
}

void diagLogSetDirectory(const QString &dir)
{
    LogState &s = state();
    std::scoped_lock lock(s.mutex);
    if (s.file.isOpen())
        s.file.close();
    s.directory = dir;
    s.tried = false;
    s.startWritten = false;
    s.throttles.clear();
}

void diagLog(const char *area, const QString &message)
{
    LogState &s = state();
    std::scoped_lock lock(s.mutex);
    writeSessionStartLocked(s);
    writeLineLocked(s, area, message);
}

void diagLogEvery(qint64 everyMs, const char *area, const QString &key,
                  const QString &message)
{
    LogState &s = state();
    std::scoped_lock lock(s.mutex);

    const qint64 now = s.clock.elapsed();
    int suppressed = 0;
    {
        LogState::Throttle &t = s.throttles[key];
        if (t.emitted && now - t.lastMs < everyMs) {
            ++t.suppressed;
            return;
        }
        // Read out BEFORE the map may be cleared below — that clear invalidates every
        // reference into it, and taking the count afterwards would silently report 0 on
        // exactly the line whose job is to say how many were skipped.
        suppressed = t.suppressed;
    }

    // The map is keyed by host, path or document, so it is bounded by what the user has
    // open — except that a log opened, closed and reopened under a new name would grow
    // it forever. Cheaper to forget everything occasionally than to track lifetimes: the
    // only cost of forgetting is one un-throttled line per surviving key.
    if (s.throttles.size() > 256)
        s.throttles.clear();

    LogState::Throttle &entry = s.throttles[key];
    entry.suppressed = 0;
    entry.emitted = true;
    entry.lastMs = now;

    writeSessionStartLocked(s);
    writeLineLocked(s, area,
                    suppressed > 0
                        ? message + QStringLiteral(" (+%1 since the last)").arg(suppressed)
                        : message);
}

void diagLogSessionStart()
{
    LogState &s = state();
    std::scoped_lock lock(s.mutex);
    writeSessionStartLocked(s);
}

} // namespace loftail
