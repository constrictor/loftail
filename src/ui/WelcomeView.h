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

#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QLayout;
class QListWidget;
class QPushButton;

namespace loftail {

class MessageLabel;

// What a window with no log open shows (SPEC.md §3).
//
// It used to be one centred sentence — "No file open. Open a log file to begin." — which
// is the first thing a new user sees, the thing every user sees after closing their last
// tab, and which offers no way out of itself: both open commands and the ten remembered
// logs are behind menus. This is the same two lists the File menu already keeps, on the
// page, openable from it.
//
// IT HOLDS STRINGS AND NOTHING ELSE, and answers with signals naming an address. That is
// CopyHighlightersDialog's split and it is here for the same reasons: the window owns the
// enumeration, so HostBookmarkStore::all() — which is a file read, not a cached model —
// stays on the window's schedule and is read once for the menu and this page together;
// and setSshFetchOptions(), which carries a saved host's poll cadence, tail-start and
// compression into the fetcher about to be built, stays at the one call site that already
// makes it rather than being duplicated into a widget with no business knowing about it.
//
// Nothing here may ask whether a log is actually THERE. logSourcePresence() is
// deliberately optimistic for a remote address — always Present — so greying a row on it
// would be a lie for exactly the entries where it matters, and anything that really
// looked would do I/O in the MainWindow constructor, before show(), on a host that may
// be down.
class WelcomeView : public QWidget
{
    Q_OBJECT

public:
    // One row of either list. A row of the recent list carries `address`; a row of the
    // remote list carries `hostName` and `path`, and an EMPTY `path` is what tells a
    // saved host with no remembered log from a remembered log on one.
    //
    // A remote row names its host rather than its ssh:// address BECAUSE THE ADDRESS IS
    // NOT ENOUGH: opening it also has to carry that host's poll cadence, tail-start and
    // compression into the fetcher about to be built (setSshFetchOptions), and only the
    // bookmark knows those. A row that answered with the URL alone would open the log
    // with every one of them silently at its default.
    struct Entry
    {
        QString label;    // what the row shows; already shortened by the caller
        QString tooltip;  // the whole address, which is what makes shortening safe
        QString address;  // recent rows: the log to open
        QString hostName; // remote rows: which saved host
        QString path;     // remote rows: the log on it; empty => it has none remembered
    };

    explicit WelcomeView(QWidget *parent = nullptr);

    void setRecent(const QVector<Entry> &entries);
    void setRemotes(const QVector<Entry> &entries);

    // False where SSH was not compiled in, which takes the whole section away rather
    // than greying it. This deliberately differs from the File menu, which keeps a
    // disabled Open Remote... carrying the "built without SSH support" tooltip: that is
    // where the explanation lives and it is not lost, while an empty disabled column on
    // a landing page is dead space that explains nothing twice.
    void setRemotesVisible(bool on);

    // The "these logs could not be reopened" sentence a session restore may leave
    // behind (SPEC.md §10). Empty hides the strip.
    void setMessage(const QString &text);

signals:
    void recentActivated(const QString &address);
    // An empty `path` is a saved host with no remembered log: there is nothing to open,
    // so what it asks for is the Open Remote dialog preset to that host.
    void remoteActivated(const QString &hostName, const QString &path);
    void browseRequested();     // the Open Log... button
    void openRemoteRequested(); // the Open Remote... button
    void clearRecentRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    // One section: its action button in a column on the LEFT, its heading and its list on
    // the right, with the list bounded in height. Returns the row for the caller to add.
    static QLayout *buildSection(QWidget *parent, const QString &heading,
                                 QPushButton *action, QPushButton *listAction,
                                 QListWidget *list);
    static QLabel *makeEmptyLabel(QListWidget *list, const QString &text,
                                  const QString &objectName);
    void applyThemeColours();

    QLabel       *m_title = nullptr;
    QLabel       *m_tagline = nullptr;
    MessageLabel *m_message = nullptr;

    QListWidget *m_recent = nullptr;
    QLabel      *m_recentEmpty = nullptr;
    QPushButton *m_clearRecent = nullptr;

    QWidget     *m_remoteSection = nullptr;
    QListWidget *m_remotes = nullptr;
    QLabel      *m_remotesEmpty = nullptr;
};

} // namespace loftail
