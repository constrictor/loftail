#include "OpenArchiveDialog.h"

#include "ArchiveLocation.h"
#include "RemoteLocation.h"

#include <QApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace loftail {

namespace {

// Pre-selected when the dialog opens: the largest member that looks like a log. A
// support bundle usually holds one big log and several small ones, and the big one is
// almost always what was wanted — but it is only a starting selection, never a filter,
// so nothing is hidden from someone who wanted a different file.
int likeliestMember(const QVector<ArchiveEntry> &members)
{
    int best = 0;
    qint64 bestSize = -1;
    for (int i = 0; i < members.size(); ++i) {
        const QString name = members.at(i).path;
        const bool logLike = name.endsWith(QLatin1String(".log"), Qt::CaseInsensitive)
            || name.endsWith(QLatin1String(".txt"), Qt::CaseInsensitive)
            || name.contains(QLatin1String(".log."), Qt::CaseInsensitive);
        if (!logLike)
            continue;
        if (members.at(i).size > bestSize) {
            bestSize = members.at(i).size;
            best = i;
        }
    }
    return best;
}

} // namespace

OpenArchiveDialog::OpenArchiveDialog(const QString &container,
                                     const QVector<ArchiveEntry> &members, QWidget *parent)
    : QDialog(parent), m_container(container), m_members(members)
{
    setWindowTitle(tr("Choose a Log"));
    setObjectName(QStringLiteral("openArchiveDialog"));

    auto *layout = new QVBoxLayout(this);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("archiveSummary"));
    m_summary->setText(tr("%1 holds %2 logs. Choose one or more to open.")
                           .arg(logSourceDisplayName(container))
                           .arg(members.size()));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_list = new QTreeWidget(this);
    m_list->setObjectName(QStringLiteral("archiveMembers"));
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setHeaderLabels({tr("Log"), tr("Size"),
                             tr("Modified")});

    const QLocale locale;
    for (const ArchiveEntry &entry : members) {
        auto *item = new QTreeWidgetItem(m_list);
        item->setText(0, entry.path);
        // A size of -1 means the archive does not record one, which a raw stream never
        // does. Show nothing rather than a zero that would read as an empty log.
        item->setText(1, entry.size >= 0 ? locale.formattedDataSize(entry.size) : QString());
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        if (entry.mtime > 0) {
            item->setText(2, locale.toString(QDateTime::fromSecsSinceEpoch(entry.mtime),
                                             QLocale::ShortFormat));
        }
    }
    m_list->resizeColumnToContents(0);
    layout->addWidget(m_list, 1);

    if (!members.isEmpty()) {
        const int preselect = likeliestMember(members);
        m_list->setCurrentItem(m_list->topLevelItem(preselect));
        m_list->topLevelItem(preselect)->setSelected(true);
    }

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    m_buttons->setObjectName(QStringLiteral("archiveButtons"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &OpenArchiveDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);

    // Double-clicking a row is the same as choosing it — the gesture people try first.
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, &OpenArchiveDialog::accept);

    resize(560, 380);
}

void OpenArchiveDialog::accept()
{
    m_chosen.clear();
    const QList<QTreeWidgetItem *> selected = m_list->selectedItems();
    for (QTreeWidgetItem *item : selected) {
        ArchiveLocation loc;
        loc.container = m_container;
        loc.member = item->text(0);
        m_chosen.append(loc.toString());
    }
    if (m_chosen.isEmpty())
        return; // nothing chosen; leave the dialog up rather than opening nothing
    QDialog::accept();
}

QStringList OpenArchiveDialog::chooseMembers(const QString &container, QWidget *parent,
                                             QString *error)
{
    QString listError;
    QVector<ArchiveEntry> members;
#if defined(LOFTAIL_HAVE_ARCHIVE)
    // Listing a compressed tar costs what expanding it costs — there is no index — so
    // the wait cursor is not decoration.
    QApplication::setOverrideCursor(Qt::WaitCursor);
    members = listArchiveMembers(container, &listError);
    QApplication::restoreOverrideCursor();
#else
    // The dialog itself is always compiled so both builds behave alike; only the
    // listing needs the dependency, and saying so beats an empty list.
    listError = tr(
        "Support for compressed and archived logs is not built into this copy of "
        "loftail. Rebuild with libarchive available to enable it.");
#endif

    if (members.isEmpty()) {
        if (error) {
            *error = listError.isEmpty()
                ? tr("%1 holds no logs.").arg(logSourceDisplayName(container))
                : listError;
        }
        return {};
    }

    // Nothing to choose: do not ask. A bare compressed stream has one member by
    // construction, and a container may simply hold one.
    if (members.size() == 1) {
        ArchiveLocation loc;
        loc.container = container;
        // A single-stream container collapses back to its plain path here, which is
        // what keeps `loftail app.log.gz` reading back as `app.log.gz`.
        if (!ArchiveLocation::isSingleStreamName(container))
            loc.member = members.first().path;
        return {loc.toString()};
    }

    OpenArchiveDialog dialog(container, members, parent);
    if (dialog.exec() != QDialog::Accepted)
        return {}; // cancelled: abandon the open silently, with no error to report
    return dialog.chosenPaths();
}

} // namespace loftail
