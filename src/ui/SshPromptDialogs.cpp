#include "SshPromptDialogs.h"

#include "HostBookmarkStore.h"
#include "SecretStore.h"
#include "UiColors.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QIcon>
#include <QIconEngine>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace loftail {

namespace {

// A "show password" eye, drawn rather than loaded.
//
// There is no icon to load. Qt has no QStyle::SP_ for this — the standard pixmaps are
// dialog and file-manager furniture — and QIcon::fromTheme() answers only where a
// freedesktop icon theme is installed, so it is empty on Windows and macOS and
// theme-dependent on Linux. A control that silently becomes an invisible zero-size
// button on two of the three platforms is not a control. It is also the dependency
// AppStyle just took OFF the dialog buttons, so reintroducing it here would be
// contradictory.
//
// Drawn at whatever size it is asked for, the same way PaneTitleStyle draws the dock
// title glyphs and for the same reason: this lands in a ~16 px box, and every fixed
// pixmap size would therefore be a downscale of a thin two-curve mark.
constexpr qreal kStrokeRatio = 0.085;
constexpr qreal kMinStroke = 1.1;
constexpr qreal kInsetRatio = 0.08;

void paintEye(QPainter *painter, const QRectF &bounds, bool struck, const QColor &color)
{
    const qreal side = qMin(bounds.width(), bounds.height());
    if (side <= 0)
        return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    QPen pen(color);
    pen.setWidthF(qMax(kMinStroke, side * kStrokeRatio));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    const QPointF center = bounds.center();
    const qreal half = side / 2 - side * kInsetRatio - pen.widthF() / 2;
    if (half <= 0) {
        painter->restore();
        return;
    }

    // Two quadratic curves meeting at the corners. A quadratic's peak is halfway to its
    // control point, so the lid offset is twice the eye's half-height. 1.2 is measured,
    // not chosen: at 16 px the stroke itself eats about 1.4 px of lid top and bottom, and
    // the first cut's flatter 0.78 left an interior the pupil filled completely — an eye
    // that rendered as a solid lens.
    const qreal lid = half * 1.2;
    QPainterPath eye;
    eye.moveTo(center.x() - half, center.y());
    eye.quadTo(center.x(), center.y() - lid, center.x() + half, center.y());
    eye.quadTo(center.x(), center.y() + lid, center.x() - half, center.y());

    // Filled, not stroked: at this size an outlined pupil is a ring one pixel wide that
    // antialiasing turns into a grey smudge.
    const qreal pupil = qMax(pen.widthF() * 0.75, half * 0.2);

    const QLineF slash(center.x() - half * 0.68, center.y() - half * 0.68,
                       center.x() + half * 0.68, center.y() + half * 0.68);

    if (struck) {
        // Clear a gap around the slash before drawing the eye, so the slash reads as
        // lying ON the eye rather than as a third curve crossing it — the difference
        // between a recognisable mark and a knot at this size. Done by clipping rather
        // than by CompositionMode_Clear, which needs an alpha-backed device and would
        // therefore work only when the engine is asked for a pixmap.
        QPainterPath slashPath;
        slashPath.moveTo(slash.p1());
        slashPath.lineTo(slash.p2());
        QPainterPathStroker stroker;
        stroker.setWidth(pen.widthF() * 2.2);
        stroker.setCapStyle(Qt::RoundCap);
        QPainterPath keep;
        keep.addRect(bounds.adjusted(-1, -1, 1, 1));
        painter->setClipPath(keep.subtracted(stroker.createStroke(slashPath)));
    }

    painter->drawPath(eye);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawEllipse(center, pupil, pupil);

    if (struck) {
        painter->setClipping(false);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawLine(slash);
    }

    painter->restore();
}

class EyeIconEngine final : public QIconEngine
{
public:
    EyeIconEngine(bool struck, QColor color) : m_struck(struck), m_color(std::move(color)) {}

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode, QIcon::State) override
    {
        paintEye(painter, rect, m_struck, m_color);
    }

    QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override { return size; }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap pixmap(size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        return pixmap;
    }

    QIconEngine *clone() const override { return new EyeIconEngine(m_struck, m_color); }

private:
    bool   m_struck;
    QColor m_color;
};

// Hang a reveal toggle inside `field`'s trailing edge.
//
// The colour is taken from the field's palette at build time rather than tracked, which
// is sound only because this is a modal dialog that lives for one prompt; a long-lived
// field would need the icon rebuilt on PaletteChange.
void addRevealToggle(QLineEdit *field)
{
    const QColor color = mutedColor(field->palette());

    QAction *reveal = field->addAction(QIcon(new EyeIconEngine(false, color)),
                                       QLineEdit::TrailingPosition);
    reveal->setObjectName(QStringLiteral("sshRevealPassword"));
    reveal->setCheckable(true);
    // Not a member, so the context is named for the class this control is built for.
    reveal->setToolTip(
        QCoreApplication::translate("loftail::GuiSshPrompter", "Show password (Ctrl+Shift+H)"));

    // QLineEdit's own action buttons take no focus, so without this the control would be
    // mouse-only — in the one dialog where the keyboard is the whole interaction. The
    // context confines it to the field, so it cannot collide with anything in the window,
    // and Ctrl+Shift+H rather than Ctrl+H because Ctrl+H is Backspace in a QLineEdit
    // under macOS's standard key bindings.
    reveal->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H));
    reveal->setShortcutContext(Qt::WidgetShortcut);

    QObject::connect(reveal, &QAction::toggled, field, [field, reveal, color](bool shown) {
        field->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
        // Struck while the password is visible: the glyph shows what clicking does next,
        // which is the convention every browser's password box uses.
        reveal->setIcon(QIcon(new EyeIconEngine(shown, color)));
        reveal->setToolTip(
            shown ? QCoreApplication::translate("loftail::GuiSshPrompter",
                                                "Hide password (Ctrl+Shift+H)")
                  : QCoreApplication::translate("loftail::GuiSshPrompter",
                                                "Show password (Ctrl+Shift+H)"));
    });
}

} // namespace

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
        box.setWindowTitle(tr("Host key changed"));
        box.setText(tr("The host key for %1 has CHANGED.").arg(where));
        box.setInformativeText(tr(
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
    box.setWindowTitle(tr("Unknown host"));
    box.setText(tr("%1 is not in your known_hosts file.").arg(where));
    box.setInformativeText(tr(
        "Its %1 key fingerprint is:\n\n%2\n\n"
        "Check that against the server (ssh-keygen -lf on its host key) before "
        "accepting. Accepting means loftail will send your credentials to whatever "
        "answered at this address.")
                               .arg(info.keyType, info.fingerprintSha256));

    QPushButton *remember =
        box.addButton(tr("Accept and Remember"), QMessageBox::AcceptRole);
    QPushButton *once = box.addButton(tr("Accept Once"), QMessageBox::AcceptRole);
    QPushButton *reject = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
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
    if (m_skippedTargets.contains(target))
        return false; // "Skip This Host" meant the host, not just the file it was on

    QDialog dialog(m_parent);
    dialog.setWindowTitle(tr("Password for %1").arg(target));
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
    addRevealToggle(field);
    layout->addWidget(field);

    // Where a remembered password would go, decided BEFORE the box can be ticked.
    //
    // available() is evaluated here rather than once at startup for two reasons: it keeps
    // a D-Bus round trip out of launch, and it is more correct, because a wallet may have
    // been unlocked since. We are about to block on the network anyway.
    const QString backend = secretStore()->backendName();
    const bool haveKeychain = !backend.isEmpty();
    const HostBookmarkStore bookmarks(m_bookmarkDir);
    const bool haveBookmark =
        HostBookmarkStore::indexOfTarget(bookmarks.all(), target) >= 0;

    auto *save = new QCheckBox(
        haveKeychain ? tr("Remember this password in %1").arg(backend)
                     : tr("Remember this password"),
        &dialog);
    save->setObjectName(QStringLiteral("sshRememberPassword"));
    save->setEnabled(haveKeychain || haveBookmark);
    layout->addWidget(save);

    // The note is always visible, not revealed on tick: someone deciding whether to tick
    // the box needs it BEFORE they decide, and it names the actual destination. Same rule
    // in all three states — only the destination differs.
    QString note;
    if (haveKeychain) {
        // No ⚠ here, and that is the point of the whole feature: this is the case where
        // loftail is NOT writing a secret to a file it owns.
        note = tr("%1 holds it, not loftail — nothing is written to a file "
                              "here. Remove it with your system's keychain manager.")
                   .arg(backend.toHtmlEscaped());
    } else if (haveBookmark) {
        const QString where = bookmarks.filePath().isEmpty()
            ? tr("loftail's configuration directory")
            : bookmarks.filePath();
        note = tr(
                   "<span style='color:%1'>⚠ Stored as <b>plain text</b> in %2 — not "
                   "encrypted. Anyone who can read your home directory can read it. An "
                   "SSH key or agent is safer.</span>")
                   .arg(warningColor(dialog.palette()).name(), where.toHtmlEscaped());
    } else {
        // The honest rendering of what already happened silently before M14: with no
        // keychain and no saved host there is nowhere to put it, so the box did nothing.
        // Saying so beats letting someone tick it and believe it worked.
        note = tr("There is no keychain on this machine and no saved host for "
                              "%1 to keep a password in. Save the host under "
                              "File ▸ Open Remote… first.")
                   .arg(target.toHtmlEscaped());
    }

    auto *warning = new QLabel(note, &dialog);
    warning->setObjectName(QStringLiteral("sshRememberNote"));
    warning->setTextFormat(Qt::RichText);
    warning->setWordWrap(true);
    layout->addWidget(warning);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QAbstractButton *skipAll = nullptr;
    QAbstractButton *skipHost = nullptr;
    if (m_bulkRestore) {
        // Restoring a session reopens everything at once. Without these, a host that
        // needs a password and is not available turns into an unskippable queue of
        // dialogs at launch.
        skipHost = buttons->addButton(tr("Skip This Host"), QDialogButtonBox::DestructiveRole);
        skipAll = buttons->addButton(tr("Skip All Remaining"), QDialogButtonBox::RejectRole);
    }
    layout->addWidget(buttons);

    bool accepted = false;
    bool cancelRemaining = false;
    bool skipThisHost = false;
    QObject::connect(buttons, &QDialogButtonBox::clicked, &dialog,
                     [&](QAbstractButton *button) {
                         const auto role = buttons->buttonRole(button);
                         if (role == QDialogButtonBox::AcceptRole) {
                             accepted = true;
                         } else if (button == skipHost && skipHost) {
                             // By identity, like skipAll below, and for the same reason.
                             skipThisHost = true;
                         } else if (role == QDialogButtonBox::RejectRole && button == skipAll) {
                             // By identity, not by label. This matched
                             // text().contains("Remaining") until the tr() sweep, at which
                             // point the first translated build would have quietly turned
                             // "Skip All Remaining" back into a plain Cancel — the two
                             // share a role, and the label was the only thing telling
                             // them apart.
                             cancelRemaining = true;
                         }
                         dialog.close();
                     });

    field->setFocus();
    dialog.exec();

    if (cancelRemaining)
        m_restoreCancelled = true;
    // Remembered, because the button says "host" and a host commonly has several files
    // open on it. Without this it skipped the FILE and the next one on the same host
    // asked again — which, at session restore, is exactly the queue of dialogs the
    // button exists to escape.
    if (skipThisHost)
        m_skippedTargets.insert(target);
    if (!accepted) {
        field->clear();
        return false;
    }

    *password = field->text();
    *remember = save->isChecked();
    field->clear();
    return true;
}

void GuiSshPrompter::passwordAccepted(const QString &target, const QString &password,
                                      bool remember)
{
    if (!remember)
        return;

    QString message;
    switch (rememberSshPassword(target, password, &message)) {
    case RememberOutcome::StoredInKeychain:
        return;
    case RememberOutcome::Failed:
        // There IS a keychain, the user was shown its name on the checkbox, and it
        // refused. Reported, never substituted: writing plain text here would put a secret
        // in a file the user was never told about, from the one dialog whose whole job is
        // to say where the secret goes.
        reportRememberFailure(target, message);
        return;
    case RememberOutcome::UseFileFallback:
        break;
    }

    // No keychain here — which is what the checkbox's own label and note said when it was
    // ticked, so this is the destination the user consented to.
    HostBookmarkStore store(m_bookmarkDir);
    QVector<HostBookmark> all = store.all();
    const int at = HostBookmarkStore::indexOfTarget(all, target);
    if (at < 0) {
        // Cannot normally happen: with neither a keychain nor a bookmark the box is
        // disabled. Reachable only if the host was deleted between the prompt and the
        // server's answer, and inventing a bookmark from a connect would make an entry
        // appear under File ▸ Remote Hosts that nobody saved.
        return;
    }
    all[at].savePassword = true;
    all[at].password = password;
    store.replaceAll(all); // writePrivate, now that anySecret is true
}

void GuiSshPrompter::reportRememberFailure(const QString &target, const QString &message)
{
    // Restoring a session reopens every remote file at once, and one wallet that will not
    // unlock is one problem however many tabs hit it. Same reasoning as the bulk-restore
    // skip buttons above.
    if (m_bulkRestore && m_lastRememberFailure == message)
        return;
    m_lastRememberFailure = message;

    QMessageBox::warning(
        m_parent, tr("Could not remember the password"),
        tr("%1\n\nThe password works and will be used for %2 until loftail "
                       "closes, but it has not been saved anywhere.")
            .arg(message, target));
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
