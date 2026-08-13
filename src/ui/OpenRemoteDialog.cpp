#include "OpenRemoteDialog.h"

#include "CollapsibleSection.h"
#include "RemoteLocation.h"
#include "SecretStore.h"
#include "SshFetcher.h"
#include "UiColors.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace loftail {

OpenRemoteDialog::OpenRemoteDialog(HostBookmarkStore *store, QWidget *parent)
    : QDialog(parent), m_store(store)
{
    setWindowTitle(tr("Open Remote Log"));
    setObjectName(QStringLiteral("openRemoteDialog"));
    setModal(true);

    auto *outer = new QVBoxLayout(this);
    auto *columns = new QHBoxLayout;
    outer->addLayout(columns);

    // --- Saved hosts -------------------------------------------------------
    auto *savedBox = new QGroupBox(tr("Saved hosts"), this);
    auto *savedLayout = new QVBoxLayout(savedBox);
    m_list = new QListWidget(savedBox);
    m_list->setObjectName(QStringLiteral("remoteBookmarkList"));
    m_list->setMinimumWidth(180);
    savedLayout->addWidget(m_list);

    // An empty list is the largest thing in this dialog on a first run, and a blank
    // rectangle the size of the form says nothing about how it stops being blank.
    // QListWidget has no placeholder of its own, so this is a label laid out over the
    // viewport and hidden the moment there is a row.
    m_listEmptyHint = new QLabel(tr("No saved hosts yet.\n\nFill in the form and press "
                                    "Save to keep one."),
                                 m_list->viewport());
    m_listEmptyHint->setObjectName(QStringLiteral("remoteBookmarkListEmptyHint"));
    m_listEmptyHint->setAlignment(Qt::AlignCenter);
    m_listEmptyHint->setWordWrap(true);
    {
        QPalette hintPalette = m_listEmptyHint->palette();
        hintPalette.setColor(QPalette::WindowText, mutedColor(palette()));
        m_listEmptyHint->setPalette(hintPalette);
        auto *viewportLayout = new QVBoxLayout(m_list->viewport());
        viewportLayout->addWidget(m_listEmptyHint, 0, Qt::AlignCenter);
    }

    auto *savedButtons = new QHBoxLayout;
    m_saveButton = new QPushButton(tr("&Save"), savedBox);
    m_saveButton->setObjectName(QStringLiteral("remoteSaveButton"));
    m_removeButton = new QPushButton(tr("&Remove"), savedBox);
    m_removeButton->setObjectName(QStringLiteral("remoteRemoveButton"));
    m_saveButton->setToolTip(
        tr("Remember the host on the right, replacing any saved under the same name"));
    savedButtons->addWidget(m_saveButton);
    savedButtons->addWidget(m_removeButton);
    savedLayout->addLayout(savedButtons);
    columns->addWidget(savedBox);

    auto *rightColumn = new QVBoxLayout;
    columns->addLayout(rightColumn, 1);

    // --- Where the log is --------------------------------------------------
    auto *connectionBox = new QGroupBox(tr("Connection"), this);
    auto *connectionLayout = new QVBoxLayout(connectionBox);

    auto *pasteHint = new QLabel(tr("Paste a whole ssh:// address into any field to fill "
                                    "in the rest."),
                                 connectionBox);
    pasteHint->setWordWrap(true);
    {
        QPalette hintPalette = pasteHint->palette();
        hintPalette.setColor(QPalette::WindowText, mutedColor(palette()));
        pasteHint->setPalette(hintPalette);
    }
    connectionLayout->addWidget(pasteHint);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    connectionLayout->addLayout(form);

    m_user = new QLineEdit(connectionBox);
    m_user->setObjectName(QStringLiteral("remoteUserField"));
    m_user->setPlaceholderText(tr("defaults to your SSH configuration"));
    form->addRow(tr("&User:"), m_user);

    // Port belongs beside the host it qualifies, not on a row of its own: it is part of
    // one address, and a whole form row for a number that is 22 gave it the weight of a
    // decision.
    m_host = new QLineEdit(connectionBox);
    m_host->setObjectName(QStringLiteral("remoteHostField"));
    m_host->setPlaceholderText(tr("host name or ssh:// address"));
    // The host is the most important field in the dialog and shared its row with a
    // five-digit number, which left it too narrow to show its own placeholder.
    m_host->setMinimumWidth(200);
    m_port = new QSpinBox(connectionBox);
    m_port->setObjectName(QStringLiteral("remotePortField"));
    m_port->setRange(1, 65535);
    m_port->setValue(RemoteLocation::kDefaultPort);
    m_port->setMaximumWidth(80);
    auto *hostRow = new QHBoxLayout;
    hostRow->addWidget(m_host, 1);
    auto *portLabel = new QLabel(tr("&Port:"), connectionBox);
    portLabel->setBuddy(m_port);
    hostRow->addWidget(portLabel);
    hostRow->addWidget(m_port);
    // Built by hand rather than by addRow(QString, QLayout *): that overload makes a
    // label with no buddy, and a QLabel only interprets '&' as a mnemonic when it has
    // one — so the accelerator would render as a literal ampersand in the form.
    auto *hostLabel = new QLabel(tr("&Host:"), connectionBox);
    hostLabel->setBuddy(m_host);
    form->addRow(hostLabel, hostRow);

    // Editable, and seeded with every path already remembered for the selected host.
    // Saving a second log under one name appends to that list (saveCurrentAsBookmark),
    // and until now the list was write-only: nothing in this dialog could show what had
    // accumulated, and nothing anywhere could remove an entry from it.
    m_path = new QComboBox(connectionBox);
    m_path->setObjectName(QStringLiteral("remotePathCombo"));
    m_path->setEditable(true);
    m_path->setInsertPolicy(QComboBox::NoInsert);
    m_path->lineEdit()->setObjectName(QStringLiteral("remotePathField"));
    m_path->lineEdit()->setPlaceholderText(QStringLiteral("/var/log/app.log"));
    m_path->lineEdit()->setContextMenuPolicy(Qt::CustomContextMenu);
    form->addRow(tr("Pa&th:"), m_path);

    m_label = new QLineEdit(connectionBox);
    m_label->setObjectName(QStringLiteral("remoteNameField"));
    m_label->setPlaceholderText(tr("prod-web-1"));
    m_label->setToolTip(tr("What to call this host in the saved list. The host name is "
                           "used if this is left blank."));
    form->addRow(tr("&Name:"), m_label);

    rightColumn->addWidget(connectionBox);

    // Qt's PlaceholderText role is unset by many themes, which leaves these hints black
    // on a dark field — present, occupying space, and unreadable.
    for (QLineEdit *field : {m_label, m_user, m_host, m_path->lineEdit()})
        ensureReadablePlaceholder(field);

    // --- How to sign in ----------------------------------------------------
    auto *signInBox = new QGroupBox(tr("Sign in"), this);
    auto *signInForm = new QFormLayout(signInBox);
    signInForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_auth = new QComboBox(signInBox);
    m_auth->setObjectName(QStringLiteral("remoteAuthCombo"));
    m_auth->addItem(tr("SSH agent or key (recommended)"), int(HostBookmark::Auth::Agent));
    m_auth->addItem(tr("Password"), int(HostBookmark::Auth::Password));
    signInForm->addRow(tr("&With:"), m_auth);

    m_remember = new QCheckBox(tr("Re&member the password after I enter it"), signInBox);
    m_remember->setObjectName(QStringLiteral("remoteRememberPassword"));
    signInForm->addRow(QString(), m_remember);

    // WHERE A REMEMBERED PASSWORD WOULD GO, in the same words the ad-hoc prompt uses
    // (SshPromptDialogs) — a keychain holds it and there is no ⚠, or a file does and
    // there is.
    //
    // Spanning BOTH columns, via the single-argument addRow() overload. In the field
    // column it was allotted a fraction of the dialog's width and wrapped the
    // hosts.json path across three lines, breaking it mid-token: the one sentence whose
    // whole job is to name a file, naming it illegibly.
    //
    // ALWAYS PRESENT, in every auth mode. It used to be hidden for agent auth, on the
    // reasoning that a warning firing when nothing is at stake is how people learn to
    // read past the ones that matter — which is right about WARNINGS, and this is not
    // one in that mode: it is a plain statement that nothing is stored, in muted text
    // with no ⚠. Keeping the row costs nothing and buys the thing hiding it cost, which
    // is that the dialog resized by some eighty pixels, under the pointer, every time
    // the sign-in method changed.
    m_consent = new QLabel(signInBox);
    m_consent->setObjectName(QStringLiteral("remoteConsentNote"));
    m_consent->setTextFormat(Qt::RichText);
    m_consent->setWordWrap(true);
    // Three lines' worth reserved, which is what the longest of the three wordings — the
    // plain-text one, whose length is a file path and therefore not fixed — takes at this
    // dialog's width. Reserving it is what keeps the section the same height in every
    // sign-in mode; without it the shorter wordings shrink the dialog and changing the
    // combo box moves the buttons under the pointer.
    m_consent->setMinimumHeight(3 * m_consent->fontMetrics().lineSpacing());
    m_consent->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    // A path the user is being warned about is a path they may need to go and look at.
    m_consent->setTextInteractionFlags(Qt::TextSelectableByMouse);
    signInForm->addRow(m_consent);

    rightColumn->addWidget(signInBox);

    // --- Everything with a good default ------------------------------------
    m_advanced = new CollapsibleSection(tr("Ad&vanced"), this);
    m_advanced->setObjectName(QStringLiteral("remoteAdvancedSection"));
    auto *advancedContent = new QWidget(m_advanced);
    auto *advancedForm = new QFormLayout(advancedContent);
    advancedForm->setContentsMargins(16, 0, 0, 0);

    m_poll = new QSpinBox(advancedContent);
    m_poll->setObjectName(QStringLiteral("remotePollField"));
    m_poll->setRange(200, 60000);
    m_poll->setSingleStep(250);
    m_poll->setValue(1000);
    m_poll->setSuffix(tr(" ms"));
    advancedForm->addRow(tr("Check for new lines every:"), m_poll);

    // A remote log is fetched WHOLE by default, so it behaves exactly like a local
    // one. Starting mid-file is opt-in, because it silently hides the beginning.
    m_tailOnly = new QCheckBox(tr("Start from the &end of the file only:"), advancedContent);
    m_tailOnly->setObjectName(QStringLiteral("remoteTailOnly"));
    m_tailMb = new QSpinBox(advancedContent);
    m_tailMb->setObjectName(QStringLiteral("remoteTailMb"));
    m_tailMb->setRange(1, 100000);
    m_tailMb->setValue(64);
    m_tailMb->setSuffix(tr(" MB"));
    m_tailMb->setEnabled(false);
    auto *tailRow = new QHBoxLayout;
    tailRow->addWidget(m_tailOnly);
    tailRow->addWidget(m_tailMb);
    tailRow->addStretch();
    advancedForm->addRow(tailRow);

    m_advanced->setContentWidget(advancedContent);
    rightColumn->addWidget(m_advanced);
    rightColumn->addStretch();

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    m_openButton = m_buttons->button(QDialogButtonBox::Open);
    outer->addWidget(m_buttons);

    // --- Wiring ------------------------------------------------------------
    connect(m_buttons, &QDialogButtonBox::accepted, this, &OpenRemoteDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &OpenRemoteDialog::reject);
    connect(m_saveButton, &QPushButton::clicked, this, &OpenRemoteDialog::saveCurrentAsBookmark);
    connect(m_removeButton, &QPushButton::clicked, this, &OpenRemoteDialog::removeCurrentBookmark);
    connect(m_list, &QListWidget::currentRowChanged, this, &OpenRemoteDialog::showBookmark);
    connect(m_tailOnly, &QCheckBox::toggled, m_tailMb, &QSpinBox::setEnabled);
    connect(m_path->lineEdit(), &QLineEdit::customContextMenuRequested, this,
            &OpenRemoteDialog::showPathMenu);

    // A whole ssh:// address pasted into any of the three text fields splits itself
    // across them, so the form takes a URL from a colleague or a wiki page as readily
    // as it takes typing. Hung off textEdited, never textChanged: setText() below must
    // not re-enter this, and only a person can paste.
    for (QLineEdit *edit : {m_user, m_host, m_path->lineEdit()})
        connect(edit, &QLineEdit::textEdited, this, [this, edit] { absorbPastedUrl(edit); });

    // Open, Save and Remove all used to be permanently enabled and to do nothing at all
    // when pressed with the form in the wrong state — a dialog that appears to accept a
    // click and then sits there is indistinguishable from one that has hung.
    for (QLineEdit *edit : {m_user, m_host}) {
        connect(edit, &QLineEdit::textChanged, this,
                &OpenRemoteDialog::dropPathChoicesIfHostChanged);
    }
    connect(m_port, &QSpinBox::valueChanged, this,
            &OpenRemoteDialog::dropPathChoicesIfHostChanged);

    connect(m_host, &QLineEdit::textChanged, this, &OpenRemoteDialog::updateActions);
    connect(m_label, &QLineEdit::textChanged, this, &OpenRemoteDialog::updateActions);
    connect(m_path, &QComboBox::editTextChanged, this, &OpenRemoteDialog::updateActions);
    connect(m_list, &QListWidget::currentRowChanged, this, &OpenRemoteDialog::updateActions);

    connect(m_auth, &QComboBox::currentIndexChanged, this, [this] {
        setPasswordAuth(m_auth->currentData().toInt() == int(HostBookmark::Auth::Password));
    });
    setPasswordAuth(false); // the combo starts on "SSH agent or key"

    reloadBookmarks();
    updateActions();
    m_host->setFocus();
}

void OpenRemoteDialog::reloadBookmarks()
{
    m_bookmarks = m_store ? m_store->all() : QVector<HostBookmark>();
    const QSignalBlocker block(m_list);
    m_list->clear();
    for (const HostBookmark &b : m_bookmarks)
        m_list->addItem(b.displayName());
    m_listEmptyHint->setVisible(m_bookmarks.isEmpty());
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
    setPathChoices(bookmark.paths, path);
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

    // A host whose defaults have been changed should say so without being unfolded.
    if (bookmark.tailStartBytes > 0 || (bookmark.pollMs > 0 && bookmark.pollMs != 1000))
        m_advanced->setExpanded(true);
}

QString OpenRemoteDialog::currentTarget() const
{
    return QStringLiteral("%1@%2:%3")
        .arg(m_user->text().trimmed(), m_host->text().trimmed())
        .arg(m_port->value());
}

void OpenRemoteDialog::setPathChoices(const QStringList &paths, const QString &current)
{
    const QSignalBlocker block(m_path);
    m_path->clear();
    m_path->addItems(paths);
    m_path->setEditText(current);
    m_pathsTarget = currentTarget();
    updateActions(); // blocked above, so the enabled state needs asking for by hand
}

void OpenRemoteDialog::dropPathChoicesIfHostChanged()
{
    const QString target = currentTarget();
    if (target == m_pathsTarget)
        return;

    // The list belongs to the machine it was loaded for. Editing the host reuses this
    // form for a different one, and offering that machine's log paths as choices for
    // this one would be wrong in the drop-down and — since the combo's contents are
    // what gets saved — wrong in hosts.json a moment later. The typed path stays; only
    // the remembered alternatives go.
    const QString typed = m_path->currentText();
    const QSignalBlocker block(m_path);
    m_path->clear();
    m_path->setEditText(typed);
    m_pathsTarget = target;
}

void OpenRemoteDialog::showPathMenu(const QPoint &where)
{
    QLineEdit *edit = m_path->lineEdit();
    QMenu *menu = edit->createStandardContextMenu();

    // The one operation on the remembered-path list that the dialog has never offered.
    // Saving appends, so a host accumulates every path ever opened on it; without this
    // the only way to drop one was to remove the whole saved host and retype it.
    const int row = m_path->findText(m_path->currentText().trimmed());
    if (row >= 0) {
        menu->addSeparator();
        QAction *forget = menu->addAction(tr("Forget This Path"));
        connect(forget, &QAction::triggered, this, [this, row] {
            const QString text = m_path->currentText();
            m_path->removeItem(row);
            m_path->setEditText(text); // removing a row also clears the edit
        });
    }

    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(edit->mapToGlobal(where));
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

    // The paths on the combo ARE the remembered list, so what it holds is what should be
    // saved — that is what makes "Forget This Path" mean anything. The one being opened
    // goes first and is added if it is new.
    QStringList paths;
    for (int i = 0; i < m_path->count(); ++i)
        paths.append(m_path->itemText(i));
    const QString path = m_path->currentText().trimmed();
    if (!path.isEmpty()) {
        paths.removeAll(path);
        paths.prepend(path);
    }
    b.paths = paths;
    return b;
}

void OpenRemoteDialog::setPasswordAuth(bool password)
{
    m_remember->setEnabled(password);
    if (!password)
        m_remember->setChecked(false);
    updateConsent();
}

void OpenRemoteDialog::updateConsent()
{
    const bool password = m_auth->currentData().toInt() == int(HostBookmark::Auth::Password);
    if (!password) {
        m_consent->setText(QStringLiteral("<span style='color:%1'>%2</span>")
                               .arg(mutedColor(palette()).name(),
                                    tr("Nothing is stored here — your agent or key answers "
                                       "for you.")
                                        .toHtmlEscaped()));
        return;
    }

    // Asked each time rather than cached at construction: a keychain seam can be swapped
    // (SecretStore.h), and this must describe the one that will actually be used.
    const QString backend = secretStore()->backendName();
    if (backend.isEmpty()) {
        m_consent->setText(
            QStringLiteral("<span style='color:%1'>%2</span>")
                .arg(warningColor(palette()).name(),
                     tr("⚠ A remembered password is stored as <b>plain text</b> in %1 — "
                        "not encrypted.")
                         .arg((m_store ? m_store->filePath() : QString()).toHtmlEscaped())));
    } else {
        m_consent->setText(tr("A remembered password goes to %1 — nothing is written to a "
                              "file here.")
                               .arg(backend.toHtmlEscaped()));
    }
}

void OpenRemoteDialog::updateActions()
{
    RemoteLocation location;
    location.user = m_user->text().trimmed();
    location.host = m_host->text().trimmed();
    location.port = m_port->value();
    location.path = m_path->currentText().trimmed();
    m_openButton->setEnabled(location.isValid());

    // A host is all a bookmark needs; the path is optional, since a saved host with no
    // remembered log is a legitimate entry (MainWindow's Remote Hosts submenu lists one).
    const HostBookmark fields = currentFields();
    m_saveButton->setEnabled(m_store && !fields.host.isEmpty());
    m_removeButton->setEnabled(m_store && m_list->currentRow() >= 0);

    // Save silently replaced the row of that name, and the only sign it had done so
    // rather than appended was the selection moving afterwards. Say which it will be.
    const bool replacing =
        m_store && HostBookmarkStore::indexOfName(m_bookmarks, fields.displayName()) >= 0;
    // "Up&date", not "&Update": U is already the User field's accelerator, and two
    // claims on one letter turn Alt+U into a focus cycle rather than a shortcut.
    m_saveButton->setText(replacing ? tr("Up&date") : tr("&Save"));
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
    if (!location->path.isEmpty() || field == m_path->lineEdit())
        m_path->setEditText(location->path);
}

void OpenRemoteDialog::saveCurrentAsBookmark()
{
    if (!m_store)
        return;
    HostBookmark b = currentFields();
    if (b.host.isEmpty())
        return;

    // A saved host is identified by its name, so this overwrites the entry of that name
    // outright and asks nothing: the name in the form is the user saying which entry
    // they mean. Confirming it would be asking whether they meant what they typed — and
    // the button already reads "Update" whenever that is what pressing it will do.
    const int existingRow = HostBookmarkStore::indexOfName(m_bookmarks, b.displayName());
    if (existingRow >= 0) {
        const HostBookmark &existing = m_bookmarks.at(existingRow);
        // Everything the form does not carry is inherited only when the entry still
        // points at the same machine. Keep every path already remembered there and add
        // the current one, so saving a second log under one name does not forget the
        // first; and let a password already stored survive an edit that set no new one.
        // Repointed at another host, the name is being reused for something else, and
        // neither the old paths nor the old secret belong to it.
        //
        // The form's own path list is authoritative for what it still holds — a path
        // dropped through "Forget This Path" must not come back from the stored entry —
        // so this adds what the store knows and the form has not removed, in the store's
        // order, rather than unioning blindly.
        if (existing.user == b.user && existing.host == b.host && existing.port == b.port) {
            QStringList paths = b.paths;
            for (const QString &p : existing.paths) {
                if (!paths.contains(p) && m_path->findText(p) >= 0)
                    paths.append(p);
            }
            b.paths = paths;
            if (b.savePassword && b.password.isEmpty())
                b.password = existing.password;
        }
    }
    m_store->save(b);
    reloadBookmarks();

    // Show which entry the save landed on — the only visible sign that an overwrite
    // happened rather than an append. Blocked, because selecting a row otherwise
    // reloads the form from the bookmark and would shuffle the fields underneath the
    // user (the path shown becomes the host's first remembered one, not theirs).
    const int savedRow = HostBookmarkStore::indexOfName(m_bookmarks, b.displayName());
    if (savedRow >= 0) {
        const QSignalBlocker block(m_list);
        m_list->setCurrentRow(savedRow);
    }

    // The path list just grew, possibly by the entry being saved. Show it, so the
    // accumulation is something the user can see rather than something they discover
    // several months later in the Remote Hosts menu.
    setPathChoices(b.paths, m_path->currentText());
    updateActions();
}

void OpenRemoteDialog::removeCurrentBookmark()
{
    const int row = m_list->currentRow();
    if (!m_store || row < 0 || row >= m_bookmarks.size())
        return;

    // Asked, unlike Save, because the two are not symmetrical: saving replaces something
    // the user has in front of them and can retype, while removing may be discarding a
    // remembered password and every path ever opened on that host, with nothing on
    // screen afterwards to reconstruct it from and no undo anywhere in the dialog.
    const QString name = m_bookmarks.at(row).displayName();
    const auto answer = QMessageBox::question(
        this, tr("Remove saved host"),
        tr("Remove the saved host “%1”?").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    m_store->remove(name);
    reloadBookmarks();
    updateActions();
}

void OpenRemoteDialog::accept()
{
    const HostBookmark fields = currentFields();
    RemoteLocation location;
    location.user = fields.user;
    location.host = fields.host;
    location.port = fields.port;
    location.path = m_path->currentText().trimmed();
    if (!location.isValid())
        return; // a guard only: the Open button is disabled while this can happen

    // Carry the fetch tuning to the fetcher that is about to be built for this exact
    // location (SshFetcher.h), so the poll cadence and any tail-only choice apply to
    // this open rather than to some later one.
    setSshFetchOptions(location, fields.fetchOptions());

    m_chosenUrl = location.toString();
    QDialog::accept();
}

} // namespace loftail
