#pragma once

#include "SshPrompter.h"

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
public:
    explicit GuiSshPrompter(QWidget *parent = nullptr) : m_parent(parent) {}

    HostKeyChoice confirmHostKey(const HostKeyInfo &info) override;
    bool askPassword(const QString &target, const QString &promptText, QString *password,
                     bool *remember) override;
    void progress(const QString &message) override;

    // Session restore reopens every remote file at once. Rather than degrade that
    // into "skip anything that would ask" — which would silently drop tabs from a
    // password-authenticated host — restore prompts, and these two let the user get
    // out of it: skipping one host, or abandoning the rest.
    void beginBulkRestore();
    void endBulkRestore();
    bool restoreCancelled() const { return m_restoreCancelled; }

    // The file a remembered password would be written to, shown verbatim in the
    // warning. A secret whose location is unstated is worse than one you can find.
    void setPasswordStorePath(const QString &path) { m_passwordStorePath = path; }

    // The last progress note, for explaining how far a failed connect got.
    QString lastProgress() const { return m_lastProgress; }

private:
    QWidget *m_parent = nullptr;
    QString  m_passwordStorePath;
    QString  m_lastProgress;
    bool     m_bulkRestore = false;
    bool     m_restoreCancelled = false;
};

} // namespace loftail
