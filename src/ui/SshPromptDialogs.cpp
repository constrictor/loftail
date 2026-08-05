#include "SshPromptDialogs.h"

#include "UiColors.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace loftail {

SshPrompter::HostKeyChoice GuiSshPrompter::confirmHostKey(const HostKeyInfo &info)
{
    const QString where = info.port == 22
        ? info.host
        : QStringLiteral("%1:%2").arg(info.host).arg(info.port);

    if (info.mismatch) {
        // A different key is already on record. This is the interception signature,
        // and there is deliberately NO way to proceed from this dialog — the session
        // refuses regardless of what is answered here, so offering a button that
        // looked like it might help would be a lie.
        QMessageBox box(m_parent);
        box.setIcon(QMessageBox::Critical);
        box.setWindowTitle(QStringLiteral("Host key changed"));
        box.setText(QStringLiteral("The host key for %1 has CHANGED.").arg(where));
        box.setInformativeText(QStringLiteral(
            "This can happen when a server is rebuilt — but it is also exactly what "
            "an intercepted connection looks like.\n\n"
            "%1 key now offered:\n%2\n\n"
            "loftail will not connect. If you are certain the server was rebuilt, "
            "verify this fingerprint out of band, then remove the old entry from "
            "~/.ssh/known_hosts.")
                                   .arg(info.keyType, info.fingerprintSha256));
        box.setStandardButtons(QMessageBox::Close);
        box.exec();
        return HostKeyChoice::Reject;
    }

    QMessageBox box(m_parent);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Unknown host"));
    box.setText(QStringLiteral("%1 is not in your known_hosts file.").arg(where));
    box.setInformativeText(QStringLiteral(
        "Its %1 key fingerprint is:\n\n%2\n\n"
        "Check that against the server (ssh-keygen -lf on its host key) before "
        "accepting. Accepting means loftail will send your credentials to whatever "
        "answered at this address.")
                               .arg(info.keyType, info.fingerprintSha256));

    QPushButton *remember =
        box.addButton(QStringLiteral("Accept and Remember"), QMessageBox::AcceptRole);
    QPushButton *once = box.addButton(QStringLiteral("Accept Once"), QMessageBox::AcceptRole);
    QPushButton *reject = box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(reject); // the safe answer is the one you get by pressing Enter
    box.exec();

    if (box.clickedButton() == remember)
        return HostKeyChoice::AcceptAndRemember;
    if (box.clickedButton() == once)
        return HostKeyChoice::AcceptOnce;
    return HostKeyChoice::Reject;
}

bool GuiSshPrompter::askPassword(const QString &target, const QString &promptText,
                                 QString *password, bool *remember)
{
    if (m_restoreCancelled)
        return false; // the user already asked to stop reopening remote files

    QDialog dialog(m_parent);
    dialog.setWindowTitle(QStringLiteral("Password for %1").arg(target));
    dialog.setModal(true);

    auto *layout = new QVBoxLayout(&dialog);

    auto *heading = new QLabel(
        QStringLiteral("<b>%1</b><br>%2").arg(target.toHtmlEscaped(), promptText.toHtmlEscaped()),
        &dialog);
    heading->setTextFormat(Qt::RichText);
    layout->addWidget(heading);

    auto *field = new QLineEdit(&dialog);
    field->setEchoMode(QLineEdit::Password);
    field->setObjectName(QStringLiteral("sshPasswordField"));
    layout->addWidget(field);

    auto *save = new QCheckBox(QStringLiteral("Remember this password"), &dialog);
    save->setObjectName(QStringLiteral("sshRememberPassword"));
    layout->addWidget(save);

    // The warning is always visible, not revealed on tick: someone deciding whether
    // to tick the box needs it BEFORE they decide, and it names the actual file.
    const QString where = m_passwordStorePath.isEmpty()
        ? QStringLiteral("loftail's configuration directory")
        : m_passwordStorePath;
    auto *warning = new QLabel(
        QStringLiteral("<span style='color:%1'>⚠ Stored as <b>plain text</b> in "
                       "%2 — not encrypted. Anyone who can read your home directory can "
                       "read it. An SSH key or agent is safer.</span>")
            .arg(warningColor(dialog.palette()).name(), where.toHtmlEscaped()),
        &dialog);
    warning->setTextFormat(Qt::RichText);
    warning->setWordWrap(true);
    layout->addWidget(warning);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    if (m_bulkRestore) {
        // Restoring a session reopens everything at once. Without these, a host that
        // needs a password and is not available turns into an unskippable queue of
        // dialogs at launch.
        buttons->addButton(QStringLiteral("Skip This Host"), QDialogButtonBox::DestructiveRole);
        buttons->addButton(QStringLiteral("Skip All Remaining"), QDialogButtonBox::RejectRole);
    }
    layout->addWidget(buttons);

    bool accepted = false;
    bool cancelRemaining = false;
    QObject::connect(buttons, &QDialogButtonBox::clicked, &dialog,
                     [&](QAbstractButton *button) {
                         const auto role = buttons->buttonRole(button);
                         if (role == QDialogButtonBox::AcceptRole) {
                             accepted = true;
                         } else if (role == QDialogButtonBox::RejectRole
                                    && button->text().contains(QStringLiteral("Remaining"))) {
                             cancelRemaining = true;
                         }
                         dialog.close();
                     });

    field->setFocus();
    dialog.exec();

    if (cancelRemaining)
        m_restoreCancelled = true;
    if (!accepted) {
        field->clear();
        return false;
    }

    *password = field->text();
    *remember = save->isChecked();
    field->clear();
    return true;
}

void GuiSshPrompter::progress(const QString &message)
{
    // Recorded, not drawn. The connect blocks this thread, so there is no event loop
    // to paint a progress dialog with — the caller shows a wait cursor around the
    // whole open, and this text explains afterwards how far it got if it failed. A
    // live "Connecting…" dialog with a Cancel button needs the connect moved off this
    // thread, which is a follow-up (ARCHITECTURE.md §6.3).
    m_lastProgress = message;
}

void GuiSshPrompter::beginBulkRestore()
{
    m_bulkRestore = true;
    m_restoreCancelled = false;
}

void GuiSshPrompter::endBulkRestore()
{
    m_bulkRestore = false;
    m_restoreCancelled = false;
}

} // namespace loftail
