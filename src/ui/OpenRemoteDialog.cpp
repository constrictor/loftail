#include "OpenRemoteDialog.h"

#include "RemoteLocation.h"
#include "UiColors.h"
#include "SshFetcher.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace loftail {

OpenRemoteDialog::OpenRemoteDialog(HostBookmarkStore *store, QWidget *parent)
    : QDialog(parent), m_store(store)
{
    setWindowTitle(QStringLiteral("Open Remote Log"));
    setObjectName(QStringLiteral("openRemoteDialog"));
    setModal(true);

    auto *outer = new QVBoxLayout(this);
    auto *columns = new QHBoxLayout;
    outer->addLayout(columns);

    // --- Saved hosts -------------------------------------------------------
    auto *savedBox = new QGroupBox(QStringLiteral("Saved hosts"), this);
    auto *savedLayout = new QVBoxLayout(savedBox);
    m_list = new QListWidget(savedBox);
    m_list->setObjectName(QStringLiteral("remoteBookmarkList"));
    m_list->setMinimumWidth(180);
    savedLayout->addWidget(m_list);

    auto *savedButtons = new QHBoxLayout;
    auto *saveButton = new QPushButton(QStringLiteral("Save"), savedBox);
    auto *removeButton = new QPushButton(QStringLiteral("Remove"), savedBox);
    saveButton->setToolTip(QStringLiteral("Remember the host on the right"));
    savedButtons->addWidget(saveButton);
    savedButtons->addWidget(removeButton);
    savedLayout->addLayout(savedButtons);
    columns->addWidget(savedBox);

    // --- The location ------------------------------------------------------
    auto *detailBox = new QGroupBox(QStringLiteral("Log"), this);
    auto *form = new QFormLayout(detailBox);

    m_label = new QLineEdit(detailBox);
    m_label->setPlaceholderText(QStringLiteral("optional, e.g. prod-web"));
    form->addRow(QStringLiteral("&Name:"), m_label);

    m_user = new QLineEdit(detailBox);
    m_user->setObjectName(QStringLiteral("remoteUserField"));
    m_user->setPlaceholderText(QStringLiteral("defaults to your SSH configuration"));
    form->addRow(QStringLiteral("&User:"), m_user);

    m_host = new QLineEdit(detailBox);
    m_host->setObjectName(QStringLiteral("remoteHostField"));
    m_host->setPlaceholderText(QStringLiteral("host name, or paste an ssh:// address"));
    form->addRow(QStringLiteral("&Host:"), m_host);

    m_port = new QSpinBox(detailBox);
    m_port->setRange(1, 65535);
    m_port->setValue(RemoteLocation::kDefaultPort);
    form->addRow(QStringLiteral("&Port:"), m_port);

    m_path = new QLineEdit(detailBox);
    m_path->setObjectName(QStringLiteral("remotePathField"));
    m_path->setPlaceholderText(QStringLiteral("/var/log/app.log"));
    form->addRow(QStringLiteral("Pa&th:"), m_path);

    // Qt's PlaceholderText role is unset by many themes, which leaves these four
    // hints black on a dark field — present, occupying space, and unreadable.
    for (QLineEdit *field : {m_label, m_user, m_host, m_path})
        ensureReadablePlaceholder(field);

    m_auth = new QComboBox(detailBox);
    m_auth->addItem(QStringLiteral("SSH agent or key (recommended)"),
                    int(HostBookmark::Auth::Agent));
    m_auth->addItem(QStringLiteral("Password"), int(HostBookmark::Auth::Password));
    form->addRow(QStringLiteral("&Sign in with:"), m_auth);

    m_remember = new QCheckBox(QStringLiteral("Remember the password when asked"), detailBox);
    m_remember->setObjectName(QStringLiteral("remoteRememberPassword"));
    form->addRow(QString(), m_remember);

    // Shown only while password authentication is selected — see setPasswordAuth().
    m_warning = new QLabel(
        QStringLiteral("<span style='color:%1'>⚠ A remembered password is stored as "
                       "<b>plain text</b> in %2 — not encrypted.</span>")
            .arg(warningColor(palette()).name(),
                 (store ? store->filePath() : QString()).toHtmlEscaped()),
        detailBox);
    m_warning->setTextFormat(Qt::RichText);
    m_warning->setWordWrap(true);
    form->addRow(QString(), m_warning);
    m_form = form;

    m_poll = new QSpinBox(detailBox);
    m_poll->setRange(200, 60000);
    m_poll->setSingleStep(250);
    m_poll->setValue(1000);
    m_poll->setSuffix(QStringLiteral(" ms"));
    m_poll->setToolTip(QStringLiteral("How often to ask the server whether the log grew"));
    form->addRow(QStringLiteral("Chec&k every:"), m_poll);

    // A remote log is fetched WHOLE by default, so it behaves exactly like a local
    // one. Starting mid-file is opt-in, because it silently hides the beginning.
    m_tailOnly = new QCheckBox(QStringLiteral("Only fetch the end of the file:"), detailBox);
    m_tailMb = new QSpinBox(detailBox);
    m_tailMb->setRange(1, 100000);
    m_tailMb->setValue(64);
    m_tailMb->setSuffix(QStringLiteral(" MB"));
    m_tailMb->setEnabled(false);
    auto *tailRow = new QHBoxLayout;
    tailRow->addWidget(m_tailOnly);
    tailRow->addWidget(m_tailMb);
    tailRow->addStretch();
    form->addRow(QString(), tailRow);

    columns->addWidget(detailBox, 1);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    outer->addWidget(m_buttons);

    // --- Wiring ------------------------------------------------------------
    connect(m_buttons, &QDialogButtonBox::accepted, this, &OpenRemoteDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &OpenRemoteDialog::reject);
    connect(saveButton, &QPushButton::clicked, this, &OpenRemoteDialog::saveCurrentAsBookmark);
    connect(removeButton, &QPushButton::clicked, this, &OpenRemoteDialog::removeCurrentBookmark);
    connect(m_list, &QListWidget::currentRowChanged, this, &OpenRemoteDialog::showBookmark);
    connect(m_tailOnly, &QCheckBox::toggled, m_tailMb, &QSpinBox::setEnabled);

    // A whole ssh:// address pasted into any of the three text fields splits itself
    // across them, so the form takes a URL from a colleague or a wiki page as readily
    // as it takes typing. Hung off textEdited, never textChanged: setText() below must
    // not re-enter this, and only a person can paste.
    for (QLineEdit *edit : {m_user, m_host, m_path})
        connect(edit, &QLineEdit::textEdited, this, [this, edit] { absorbPastedUrl(edit); });

    connect(m_auth, &QComboBox::currentIndexChanged, this, [this] {
        setPasswordAuth(m_auth->currentData().toInt() == int(HostBookmark::Auth::Password));
    });
    setPasswordAuth(false); // the combo starts on "SSH agent or key"

    reloadBookmarks();
    m_host->setFocus();
}

void OpenRemoteDialog::reloadBookmarks()
{
    m_bookmarks = m_store ? m_store->all() : QVector<HostBookmark>();
    const QSignalBlocker block(m_list);
    m_list->clear();
    for (const HostBookmark &b : m_bookmarks)
        m_list->addItem(b.displayName());
}

void OpenRemoteDialog::showBookmark(int row)
{
    if (row < 0 || row >= m_bookmarks.size())
        return;
    const HostBookmark &b = m_bookmarks.at(row);
    preset(b, b.paths.isEmpty() ? QString() : b.paths.first());
}

void OpenRemoteDialog::preset(const HostBookmark &bookmark, const QString &path)
{
    m_label->setText(bookmark.label);
    m_user->setText(bookmark.user);
    m_host->setText(bookmark.host);
    m_port->setValue(bookmark.port);
    m_path->setText(path);
    m_auth->setCurrentIndex(m_auth->findData(int(bookmark.auth == HostBookmark::Auth::Password
                                                     ? HostBookmark::Auth::Password
                                                     : HostBookmark::Auth::Agent)));
    m_poll->setValue(bookmark.pollMs > 0 ? bookmark.pollMs : 1000);
    // Only where there could be a password to remember: a bookmark carrying both
    // savePassword and agent auth would otherwise tick a disabled box, and saving the
    // host again would write that contradiction back out.
    m_remember->setChecked(bookmark.savePassword && m_remember->isEnabled());
    m_tailOnly->setChecked(bookmark.tailStartBytes > 0);
    if (bookmark.tailStartBytes > 0)
        m_tailMb->setValue(int(bookmark.tailStartBytes / (1024 * 1024)));
}

HostBookmark OpenRemoteDialog::currentFields() const
{
    HostBookmark b;
    b.label = m_label->text().trimmed();
    b.user = m_user->text().trimmed();
    b.host = m_host->text().trimmed();
    b.port = m_port->value();
    b.auth = static_cast<HostBookmark::Auth>(m_auth->currentData().toInt());
    b.pollMs = m_poll->value();
    b.tailStartBytes =
        m_tailOnly->isChecked() ? qint64(m_tailMb->value()) * 1024 * 1024 : 0;
    b.savePassword = m_remember->isChecked();
    const QString path = m_path->text().trimmed();
    if (!path.isEmpty())
        b.paths = QStringList{path};
    return b;
}

void OpenRemoteDialog::setPasswordAuth(bool password)
{
    m_remember->setEnabled(password);
    if (!password)
        m_remember->setChecked(false);

    // The plain-text warning is about a choice that only exists on this branch: with an
    // agent or a key there is no password for loftail to keep, so the warning would be
    // cautioning against something that cannot happen. Warnings that fire when nothing
    // is at stake are how people learn to read past the ones that matter.
    //
    // Tied to the auth SELECTION rather than to the checkbox being ticked, because it
    // has to inform that decision rather than confirm it after the fact.
    //
    // setRowVisible(), not hide(): addRow(QString(), …) still creates an empty label
    // beside the field, so hiding the label alone would leave its blank row behind.
    m_form->setRowVisible(m_warning, password);
}

void OpenRemoteDialog::absorbPastedUrl(QLineEdit *field)
{
    const QString text = field->text().trimmed();
    if (!RemoteLocation::isRemote(text))
        return;
    const auto location = RemoteLocation::parse(text);
    if (!location || location->host.isEmpty())
        return;

    // Every field is rewritten, which is also what removes the pasted URL from the one
    // it landed in. The path is the exception: a host-only URL (`ssh://prod-web`) must
    // not wipe a path already typed — unless it was pasted INTO the path field, where
    // leaving the URL text sitting there would be the odder answer.
    m_user->setText(location->user);
    m_host->setText(location->host);
    m_port->setValue(location->port);
    if (!location->path.isEmpty() || field == m_path)
        m_path->setText(location->path);
}

void OpenRemoteDialog::saveCurrentAsBookmark()
{
    if (!m_store)
        return;
    HostBookmark b = currentFields();
    if (b.host.isEmpty())
        return;

    // Keep every path already remembered for this host and add the current one, so
    // saving a second log on a known host does not forget the first.
    bool found = false;
    for (const HostBookmark &existing : m_bookmarks) {
        if (existing.user == b.user && existing.host == b.host && existing.port == b.port) {
            found = true;
            QStringList paths = existing.paths;
            for (const QString &p : b.paths) {
                if (!paths.contains(p))
                    paths.append(p);
            }
            b.paths = paths;
            // A password already stored for this host survives an edit that did not
            // set a new one.
            if (b.savePassword && b.password.isEmpty())
                b.password = existing.password;
            break;
        }
    }
    Q_UNUSED(found);
    m_store->save(b);
    reloadBookmarks();
}

void OpenRemoteDialog::removeCurrentBookmark()
{
    const int row = m_list->currentRow();
    if (!m_store || row < 0 || row >= m_bookmarks.size())
        return;
    const HostBookmark &b = m_bookmarks.at(row);
    m_store->remove(b.user, b.host, b.port);
    reloadBookmarks();
}

void OpenRemoteDialog::accept()
{
    const HostBookmark fields = currentFields();
    RemoteLocation location;
    location.user = fields.user;
    location.host = fields.host;
    location.port = fields.port;
    location.path = m_path->text().trimmed();
    if (!location.isValid())
        return; // nothing to open; leave the dialog up rather than failing silently

    // Carry the fetch tuning to the fetcher that is about to be built for this exact
    // location (SshFetcher.h), so the poll cadence and any tail-only choice apply to
    // this open rather than to some later one.
    setSshFetchOptions(location, fields.fetchOptions());

    m_chosenUrl = location.toString();
    QDialog::accept();
}

} // namespace loftail
