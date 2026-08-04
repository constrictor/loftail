#pragma once

#include "HostBookmarkStore.h"

#include <QDialog>
#include <QVector>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
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
// The dialog accepts a pasted ssh:// URL as readily as filled-in fields, and keeps
// the two in step in both directions — pasting a URL fills the fields, editing a
// field rewrites the URL.
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
    void syncUrlFromFields();
    void syncFieldsFromUrl();
    void saveCurrentAsBookmark();
    void removeCurrentBookmark();
    void accept() override;

    HostBookmarkStore    *m_store = nullptr;
    QVector<HostBookmark> m_bookmarks;

    QListWidget *m_list = nullptr;
    QLineEdit   *m_url = nullptr;
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
    QDialogButtonBox *m_buttons = nullptr;

    bool    m_syncing = false; // guards the two-way URL/field binding
    QString m_chosenUrl;
};

} // namespace loftail
