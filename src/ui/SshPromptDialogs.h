#pragma once

#include "SshPrompter.h"

#include <QCoreApplication>

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace loftail {

// The widget-side answers to SshPrompter's questions (M11, SPEC.md §3).
//
// These run on the GUI thread, which they can rely on: a connect happens on the
// thread that opened the document, and only hands off to the fetcher thread once it
// has succeeded. So a prompt here is an ordinary modal dialog — no queued signals, no
// nested event loop of its own, and modality is what stops a second open from
// starting on top of a connect in progress.
class GuiSshPrompter final : public SshPrompter
{
    // Not a QObject — it implements a core interface and is owned by MainWindow — so
    // tr() has to be declared rather than inherited. Every string this class shows is
    // security-critical prose about where a password is going, so it is exactly the
    // last thing that should be left untranslatable.
    Q_DECLARE_TR_FUNCTIONS(loftail::GuiSshPrompter)

public:
    explicit GuiSshPrompter(QWidget *parent = nullptr) : m_parent(parent) {}

    HostKeyChoice confirmHostKey(const HostKeyInfo &info) override;
    bool askPassword(const QString &target, const QString &promptText, QString *password,
                     bool *remember) override;
    void passwordAccepted(const QString &target, const QString &password,
                          bool remember) override;
    void progress(const QString &message) override;

    // Session restore reopens every remote file at once. Rather than degrade that
    // into "skip anything that would ask" — which would silently drop tabs from a
    // password-authenticated host — restore prompts, and these two let the user get
    // out of it: skipping one host, or abandoning the rest.
    void beginBulkRestore();
    void endBulkRestore();
    bool restoreCancelled() const { return m_restoreCancelled; }

    // The directory saved hosts live in. Two things at once, which is why it replaced the
    // bare file path it used to be given: the file a remembered password would be written
    // to when there is no keychain (named verbatim in the warning — a secret whose
    // location is unstated is worse than one you can find), and the list consulted to
    // decide whether there is anywhere to keep one at all.
    void setBookmarkDir(const QString &dir) { m_bookmarkDir = dir; }

    // The last progress note, for explaining how far a failed connect got.
    QString lastProgress() const { return m_lastProgress; }

private:
    // A keychain that is present and refuses. Reported rather than worked around: the
    // fallback file is NOT substituted, because the user was shown the keychain's name.
    void reportRememberFailure(const QString &target, const QString &message);

    QWidget *m_parent = nullptr;
    QString  m_bookmarkDir;
    QString  m_lastProgress;
    // Session restore reopens every remote file at once, so a failure that is really one
    // failure must not become one message box per tab.
    QString  m_lastRememberFailure;
    bool     m_bulkRestore = false;
    bool     m_restoreCancelled = false;
};

} // namespace loftail
