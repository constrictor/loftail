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

#pragma once

#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace loftail {

class ConfigView;
class LiveWatcher;

// Notices that a config page's file changed on disk and hands what is there now to
// ConfigView::externalChange(), which decides — reload in silence, or ask (SPEC.md §4).
// This class never decides anything about the page: it only answers "what is in the
// file now", and only when that might have moved.
//
// Owned by the view, so closing the tab stops it.
//
// LOCAL: core's LiveWatcher, the same belt-and-braces watch a log tab uses — the file
// AND its directory through QFileSystemWatcher, which is what survives a QSaveFile's
// rename (ours or an editor's), with a 750 ms poll behind it for the mounts the watcher
// cannot hear. Every event is gated on a stat that moved before anything is read, so a
// busy directory costs an attribute query per event and never a read.
//
// REMOTE: a probe every kRemotePollMs, one at a time, through ConfigTransfer — the same
// errand the page's own read is, on the connection the session cache keeps warm. The
// whole file is read rather than stat'ed, which is the point and not a shortcut: the
// comparison is by CONTENT, and a config file is small. A probe that fails says nothing
// and the next tick asks again.
class ConfigChangeWatcher : public QObject
{
    Q_OBJECT

public:
    // The fetcher's own slow cadence and ConfigTransfer::kRetryMs, so a config tab and
    // the log beside it on one machine do not keep two different clocks.
    static constexpr int kRemotePollMs = 5000;

    explicit ConfigChangeWatcher(ConfigView *view);
    ~ConfigChangeWatcher() override;

    // Look now, off the timer. The tests' deterministic entry, and harmless anywhere.
    void checkNow();

private:
    struct Stat
    {
        bool      exists = false;
        qint64    size = -1;
        qint64    mtimeMs = -1;
        quint64   identity = 0;
        bool operator==(const Stat &) const = default;
    };
    Stat statNow() const;
    void checkLocal();
    void probeRemote();

    ConfigView  *m_view = nullptr;
    QString      m_address;
    bool         m_remote = false;
    LiveWatcher *m_watcher = nullptr;
    Stat         m_lastStat;
    QTimer      *m_poll = nullptr;
    bool         m_probing = false;
};

} // namespace loftail
