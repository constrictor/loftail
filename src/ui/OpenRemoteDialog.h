#pragma once

#include "HostBookmarkStore.h"

#include <QDialog>
#include <QVector>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;
QT_END_NAMESPACE

namespace loftail {

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
// separately (HostBookmarkStore::save).
//
// There is deliberately NO "Address" row showing the assembled URL. It used to be the
// first field in the form, and it was every other field concatenated — it could show
// nothing the rows below it did not already say, so it cost a row of the dialog to
// repeat them. What it was actually for is PASTE, and paste needs no field of its own.
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
    void absorbPastedUrl(QLineEdit *field);
    void saveCurrentAsBookmark();
    void removeCurrentBookmark();
    void accept() override;

    HostBookmarkStore    *m_store = nullptr;
    QVector<HostBookmark> m_bookmarks;

    QListWidget *m_list = nullptr;
    QLineEdit   *m_label = nullptr;
    QLineEdit   *m_user = nullptr;
    QLineEdit   *m_host = nullptr;
    QSpinBox    *m_port = nullptr;
    QLineEdit   *m_path = nullptr;
    QComboBox   *m_auth = nullptr;
    QSpinBox    *m_poll = nullptr;
    QCheckBox   *m_tailOnly = nullptr;
    QSpinBox    *m_tailMb = nullptr;
    QCheckBox   *m_remember = nullptr;
    QLabel      *m_warning = nullptr;  // the plain-text password caution
    QFormLayout *m_form = nullptr;     // owns the row m_warning lives in
    QDialogButtonBox *m_buttons = nullptr;

    QString m_chosenUrl;
};

} // namespace loftail
