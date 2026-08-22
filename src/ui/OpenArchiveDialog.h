#pragma once

#include "ArchiveReader.h"

#include <QDialog>
#include <QStringList>
#include <QVector>

QT_BEGIN_NAMESPACE
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QToolButton;
class QTreeWidget;
QT_END_NAMESPACE

namespace loftail {

// Choose which log to read out of an archive holding several (SPEC.md §3, M12).
//
// SHOWN ONLY WHEN THERE IS SOMETHING TO CHOOSE. A bare compressed stream holds one
// member by construction and a container may hold exactly one, and in neither case is
// the user asked — the same principle as Preferences, which appears only
// when loftail cannot work the format out for itself.
//
// Several members can be picked at once, each opening in its own tab. That is not a
// new idea in the product: SPEC.md §3 already says dropping several files opens all of
// them, and a logs.zip holding app.log and app.log.1 is exactly that case.
//
// A support bundle can hold hundreds of entries, so the list NARROWS. What is typed is
// a substring until it carries a `*` or a `?`, at which point it is a wildcard over the
// whole member path — never a regular expression, which is a language nobody should
// have to know to find a file by its extension. Narrowing is only ever a view: a row it
// hides is a row that cannot be opened, however it came to be selected.
//
// Like OpenRemoteDialog, this returns ADDRESSES rather than opening anything itself,
// so the one open path in MainWindow stays the one open path.
class OpenArchiveDialog : public QDialog
{
    Q_OBJECT

public:
    // `container` is the archive's own path — local, or an ssh:// URL. Listing it may
    // take real time for a compressed tar, which has no index and must be decompressed
    // to be enumerated; the caller does that first and passes the result in, so this
    // dialog never blocks on I/O of its own.
    OpenArchiveDialog(const QString &container, const QVector<ArchiveEntry> &members,
                      QWidget *parent = nullptr);

    // The chosen members as full nested addresses, valid once exec() returned Accepted.
    QStringList chosenPaths() const { return m_chosen; }

    // Every member of `container` as a nested address, with the picker shown only when
    // there is a genuine choice. Returns empty when the user cancelled OR when the
    // archive could not be read, with `error` distinguishing the two — cancelling must
    // abandon the open silently, exactly as cancelling Preferences does.
    static QStringList chooseMembers(const QString &container, QWidget *parent,
                                     QString *error);

    // Public because QDialog's is: a test drives the modal picker from a timer, and
    // there is nothing to protect here that QDialog does not already expose.
    void accept() override;

    // Whether `member` is shown under `filter`. Static and public because it is the
    // whole of the match rule and is worth testing without a dialog on screen.
    static bool memberMatches(const QString &member, const QString &filter);

private:
    void narrow(const QString &filter);
    void selectShown();
    void updateSummary();

    QString                m_container;
    QVector<ArchiveEntry>  m_members;
    QLineEdit             *m_filter = nullptr;
    QToolButton           *m_selectAll = nullptr;
    QTreeWidget           *m_list = nullptr;
    QLabel                *m_summary = nullptr;
    QDialogButtonBox      *m_buttons = nullptr;
    QStringList            m_chosen;
};

} // namespace loftail
