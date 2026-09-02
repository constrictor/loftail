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

#include "HostBookmarkStore.h"

#include <QDialog>
#include <QVector>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
QT_END_NAMESPACE

namespace loftail {

class CollapsibleSection;

// Open a log on another machine (SPEC.md §3, M11).
//
// Saved hosts live HERE and in the File ▸ Remote Hosts submenu rather than in a dock
// pane of their own. Every existing pane binds to the active Document (invariant #7,
// ARCHITECTURE.md §12) and a host list has no document to bind to; it would be the
// only pane in the window with no such binding, and the pane docking arrangement is
// deliberately narrow after M9.
//
// The dialog accepts a pasted ssh:// URL as readily as filled-in fields: paste one
// into User, Host or Path and it is split across all of them.
//
// One name, one saved host: Save replaces the entry of that name silently, because the
// list is a list of names and two rows reading the same cannot be told apart or removed
// separately (HostBookmarkStore::save). The button says "Update" rather than "Save" when
// the name in the form already names a row, so the replacement is visible before it
// happens rather than inferred afterwards from the selection moving.
//
// There is deliberately NO "Address" row showing the assembled URL. It used to be the
// first field in the form, and it was every other field concatenated — it could show
// nothing the rows below it did not already say, so it cost a row of the dialog to
// repeat them. What it was actually for is PASTE, and paste needs no field of its own —
// only the one line of prose under the group title saying so, since a placeholder
// disappears at the first keystroke and cannot be what teaches a feature.
//
// FORM ORDER FOLLOWS THE ORDER THINGS ARE KNOWN IN: user, host, port, path, and only
// then the name. A saved host is named after you have decided what it is; Name was the
// first field for three milestones and it was the last thing anybody could fill in.
class OpenRemoteDialog : public QDialog
{
    Q_OBJECT

public:
    explicit OpenRemoteDialog(HostBookmarkStore *store, QWidget *parent = nullptr);

    // The chosen location in normal form, valid once exec() returned Accepted.
    QString chosenUrl() const { return m_chosenUrl; }

    // Prefill from an existing bookmark and path (the Remote Hosts submenu route).
    void preset(const HostBookmark &bookmark, const QString &path);

private:
    void reloadBookmarks();
    void showBookmark(int row);
    HostBookmark currentFields() const;
    void setPasswordAuth(bool password);
    void updateConsent();
    void updateActions();
    void setPathChoices(const QStringList &paths, const QString &current);
    void dropPathChoicesIfHostChanged();
    QString currentTarget() const;
    void showPathMenu(const QPoint &where);
    void absorbPastedUrl(QLineEdit *field);
    void saveCurrentAsBookmark();
    void removeCurrentBookmark();
    void accept() override;

    HostBookmarkStore    *m_store = nullptr;
    QVector<HostBookmark> m_bookmarks;

    QListWidget *m_list = nullptr;
    QLabel      *m_listEmptyHint = nullptr; // shown over the list while it has no rows
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_removeButton = nullptr;

    QLineEdit *m_label = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_host = nullptr;
    QSpinBox  *m_port = nullptr;
    QComboBox *m_path = nullptr; // editable; lists the paths remembered for this host
    QComboBox *m_auth = nullptr;
    QCheckBox *m_remember = nullptr;
    QLabel    *m_consent = nullptr; // where a remembered password would go — always shown
    QSpinBox  *m_poll = nullptr;
    QCheckBox *m_tailOnly = nullptr;
    QSpinBox  *m_tailMb = nullptr;
    QCheckBox *m_compress = nullptr;

    CollapsibleSection *m_advanced = nullptr;
    QDialogButtonBox   *m_buttons = nullptr;
    QPushButton        *m_openButton = nullptr;

    // Which machine the paths currently listed in m_path belong to, as user@host:port.
    // A remembered path list is a property of a host, so pointing the form at a
    // different one must not carry it across (see dropPathChoicesIfHostChanged).
    QString m_pathsTarget;

    QString m_chosenUrl;
};

} // namespace loftail
