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

#include "SessionStore.h"

#include <QJsonDocument>
#include <QSettings>

namespace loftail {

namespace {
constexpr auto kGroup = "session";
constexpr auto kSchema = "schemaVersion";
constexpr auto kGeometry = "geometry";
constexpr auto kWindowState = "windowState";
constexpr auto kActiveView = "activeView";
constexpr auto kActiveTab = "activeTab";
constexpr auto kEditors = "editors";
constexpr auto kActiveDocumentV1 = "activeDocument";
constexpr auto kDocuments = "documents";
constexpr auto kViews = "views";

QJsonObject stringToJson(const QString &s)
{
    return QJsonDocument::fromJson(s.toUtf8()).object();
}
} // namespace

Session SessionStore::load(QSettings &settings)
{
    Session session;
    settings.beginGroup(QLatin1String(kGroup));

    // A schema version from the future (or an absent one) yields an empty session
    // rather than a half-read one — a clean first launch (§8). Versions 1 and 2 are
    // read and migrated; anything else is discarded.
    const int version = settings.value(QLatin1String(kSchema), 0).toInt();
    if (version != kSchemaVersion && version != 1 && version != 2 && version != 3) {
        settings.endGroup();
        return session;
    }
    const bool v1 = version == 1;

    session.schemaVersion = kSchemaVersion;
    session.geometry = settings.value(QLatin1String(kGeometry)).toByteArray();
    // Only a windowState describing THIS shell is usable: a v1 blob describes a
    // different window entirely, and a v2 one records the collapsed central widget of
    // the all-docks shell, which would squeeze the document well to zero. Geometry
    // (position and size) is still good, so only the pane layout is dropped.
    //
    // `>= 3`, NOT `== kSchemaVersion`. v3 is the same shell as v4 — the bump added an
    // array, not a layout — so testing for the current version alone would silently
    // throw away every existing user's pane arrangement on the first launch after the
    // upgrade, for no reason at all.
    if (version >= 3)
        session.windowState = settings.value(QLatin1String(kWindowState)).toByteArray();

    const int n = settings.beginReadArray(QLatin1String(kDocuments));
    session.documents.reserve(n);
    for (int i = 0; i < n; ++i) {
        settings.setArrayIndex(i);
        SessionDocument d;
        d.path = settings.value(QStringLiteral("path")).toString();
        // The format keys a pre-M20 session wrote are simply not read: the settings
        // tree answers for this path now. They are left in place rather than removed,
        // so rolling back to an earlier build still finds them.
        d.runAll = settings.value(QStringLiteral("runAll"), false).toBool();
        d.selectedRunStartOffset =
            settings.value(QStringLiteral("selectedRunOffset"), qint64(-1)).toLongLong();
        d.selectedRunStartTimestamp = settings
                                          .value(QStringLiteral("selectedRunTs"),
                                                 qint64(Record::kNoTimestamp))
                                          .toLongLong();
        d.filters = stringToJson(settings.value(QStringLiteral("filters")).toString());
        d.highlighters =
            stringToJson(settings.value(QStringLiteral("highlighters")).toString());
        session.documents.append(d);

        // v1 had exactly one view per document, with the column state on the
        // document. Synthesize that view.
        if (v1) {
            SessionView v;
            v.documentIndex = i;
            v.columnState = settings.value(QStringLiteral("columnState")).toByteArray();
            session.views.append(v);
        }
    }
    settings.endArray();

    if (v1) {
        session.activeView = settings.value(QLatin1String(kActiveDocumentV1), 0).toInt();
    } else {
        session.activeView = settings.value(QLatin1String(kActiveView), 0).toInt();
        const int viewCount = settings.beginReadArray(QLatin1String(kViews));
        session.views.reserve(viewCount);
        for (int i = 0; i < viewCount; ++i) {
            settings.setArrayIndex(i);
            // A v2 view also carried a `dockName`; the tab order it used to
            // disambiguate is now just this array's order, so it is read past.
            SessionView v;
            v.documentIndex = settings.value(QStringLiteral("document"), 0).toInt();
            v.columnState = settings.value(QStringLiteral("columnState")).toByteArray();
            v.wrapMode = settings.value(QStringLiteral("wrapMode"), 0).toInt();
            session.views.append(v);
        }
        settings.endArray();

        // v3 knew only log pages, so the tab in front was the active view and there are
        // no editors to place. Both fall out of the defaults, which is what makes this
        // migration a copy rather than a conversion.
        session.activeTab = settings.value(QLatin1String(kActiveTab), session.activeView).toInt();
        const int editorCount = settings.beginReadArray(QLatin1String(kEditors));
        session.editors.reserve(editorCount);
        for (int i = 0; i < editorCount; ++i) {
            settings.setArrayIndex(i);
            SessionEditor e;
            e.address = settings.value(QStringLiteral("address")).toString();
            e.tabIndex = settings.value(QStringLiteral("tab"), 0).toInt();
            // PRESENCE, not value: a stored 0 is PlainText and is indistinguishable from
            // "nothing was chosen", so reading it as a choice brings every restored tab
            // back uncoloured. Only a syntax the user actually picked is written.
            e.syntaxChosen = settings.contains(QStringLiteral("syntax"));
            e.syntax = settings.value(QStringLiteral("syntax"), 0).toInt();
            if (!e.address.isEmpty())
                session.editors.append(e);
        }
        settings.endArray();
    }

    settings.endGroup();
    return session;
}

void SessionStore::save(QSettings &settings, const Session &session)
{
    settings.beginGroup(QLatin1String(kGroup));

    // Clear the arrays first so a shrunk list leaves no stale indices
    // (QSettings::beginWriteArray does not remove entries beyond the new size).
    settings.remove(QLatin1String(kDocuments));
    settings.remove(QLatin1String(kViews));
    settings.remove(QLatin1String(kEditors));
    settings.remove(QLatin1String(kActiveDocumentV1)); // superseded by activeView

    settings.setValue(QLatin1String(kSchema), kSchemaVersion);
    settings.setValue(QLatin1String(kGeometry), session.geometry);
    settings.setValue(QLatin1String(kWindowState), session.windowState);
    settings.setValue(QLatin1String(kActiveView), session.activeView);
    settings.setValue(QLatin1String(kActiveTab), session.activeTab);

    settings.beginWriteArray(QLatin1String(kDocuments), int(session.documents.size()));
    for (int i = 0; i < session.documents.size(); ++i) {
        settings.setArrayIndex(i);
        const SessionDocument &d = session.documents.at(i);
        // THE PATH, AND NOTHING ELSE. A log's filters, its highlight rules and its run
        // are per-FILE state and live one record per log (M21, LogFileStore.h), so this
        // array is now only "which logs were open, in which order". The five keys that
        // used to be here are still READ by load(), once, to be migrated across.
        //
        // Removing them earns no schema bump — a removed key is exactly what a backward
        // read handles, the same reasoning M20 used when it dropped the format group —
        // and the remove() above is what makes it once-only: the first quit after the
        // upgrade takes them off the disk, so nothing has to remember the drain ran.
        settings.setValue(QStringLiteral("path"), d.path);
    }
    settings.endArray();

    settings.beginWriteArray(QLatin1String(kViews), int(session.views.size()));
    for (int i = 0; i < session.views.size(); ++i) {
        settings.setArrayIndex(i);
        const SessionView &v = session.views.at(i);
        settings.setValue(QStringLiteral("document"), v.documentIndex);
        settings.setValue(QStringLiteral("columnState"), v.columnState);
        settings.setValue(QStringLiteral("wrapMode"), v.wrapMode);
    }
    settings.endArray();

    settings.beginWriteArray(QLatin1String(kEditors), int(session.editors.size()));
    for (int i = 0; i < session.editors.size(); ++i) {
        settings.setArrayIndex(i);
        const SessionEditor &e = session.editors.at(i);
        settings.setValue(QStringLiteral("address"), e.address);
        settings.setValue(QStringLiteral("tab"), e.tabIndex);
        // Written ONLY when the user chose it. The key's presence is the whole signal
        // (see load()), so an unconditional write would make every guess look like a
        // decision and freeze it against a file that may since have changed.
        if (e.syntaxChosen)
            settings.setValue(QStringLiteral("syntax"), e.syntax);
        else
            settings.remove(QStringLiteral("syntax"));
    }
    settings.endArray();

    settings.endGroup();
    settings.sync();
}

} // namespace loftail
