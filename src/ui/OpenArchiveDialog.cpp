#include "OpenArchiveDialog.h"

#include "ArchiveLocation.h"
#include "RemoteLocation.h"

#include <QApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QProgressDialog>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <atomic>

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

#if defined(LOFTAIL_HAVE_ARCHIVE)
namespace {

// Enumerate `container` on a worker thread, with a cancellable progress dialog in front.
//
// Listing a compressed tar costs what expanding it costs — there is no index — and for a
// remote one it also waits for the container to arrive. A wait cursor used to be the
// whole of the answer, which meant a large `.tar.gz` locked the window up for minutes
// with no way out.
//
// It still pumps events from this frame rather than returning and continuing later, and
// deliberately: the user asked to open an archive and must pick a member before anything
// can happen, so there is nothing for them to do in this window meanwhile. What it buys
// over the wait cursor is the three things that matter — the window repaints, the listing
// can be cancelled, and every OTHER tab goes on tailing, because their fetchers are on
// their own threads.
//
// The worker owns its own LogSource, which is what the per-instance form of the
// refreshSize() rule requires (SpooledLogSource.h): nothing else refreshes it.
class MemberLister : public QThread
{
public:
    MemberLister(QString container, std::atomic_bool *cancelled)
        : m_container(std::move(container)), m_cancelled(cancelled)
    {
    }

    void run() override
    {
        auto *flag = m_cancelled;
        members = listArchiveMembers(m_container, &error, [flag]() { return flag->load(); });
    }

    QVector<ArchiveEntry> members;
    QString               error;

private:
    QString           m_container;
    std::atomic_bool *m_cancelled;
};

// How long each wait slice is: the granularity at which the progress dialog repaints
// and its Cancel button is noticed.
constexpr int kListenSliceMs = 50;

QVector<ArchiveEntry> listOffThread(const QString &container, QWidget *parent,
                                    QString *error)
{
    std::atomic_bool cancelled{false};
    MemberLister lister(container, &cancelled);

    QProgressDialog progress(
        OpenArchiveDialog::tr("Looking inside %1…").arg(logSourceDisplayName(container)),
        OpenArchiveDialog::tr("Cancel"), 0, 0, parent);
    progress.setObjectName(QStringLiteral("archiveListProgress")); // findChild, for tests
    progress.setWindowTitle(OpenArchiveDialog::tr("Open Archive"));
    progress.setWindowModality(Qt::WindowModal);
    // Not immediately: a zip is read through its central directory and is quick at any
    // size, and a dialog that flashes for 30 ms is worse than no dialog.
    progress.setMinimumDuration(400);
    progress.setValue(0); // indeterminate; there is no total to count towards

    QObject::connect(&progress, &QProgressDialog::canceled, &progress,
                     [&cancelled]() { cancelled = true; });

    // Waited for by POLLING rather than by quitting a nested QEventLoop on the thread's
    // finished() signal, and the difference is a hang. QProgressDialog::setValue() calls
    // processEvents() for a modal dialog, so a listing that finishes quickly — a zip,
    // which is most of them — can have its queued quit() delivered before exec() has
    // begun. QEventLoop::quit() on a loop that is not running does nothing, and the wait
    // never ends. wait(timeout) has no such edge: it reports a thread that has ALREADY
    // finished just as readily as one that finishes while it waits.
    lister.start();
    while (!lister.wait(QDeadlineTimer(kListenSliceMs)))
        QCoreApplication::processEvents(QEventLoop::AllEvents, kListenSliceMs);

    progress.reset();
    if (error)
        *error = lister.error;
    if (cancelled) {
        // Cancelled listing and cancelled picker mean the same thing to the caller:
        // abandon the open silently, with nothing to report.
        if (error)
            error->clear();
        return {};
    }
    return lister.members;
}

} // namespace
#endif

QStringList OpenArchiveDialog::chooseMembers(const QString &container, QWidget *parent,
                                             QString *error)
{
    QString listError;
    QVector<ArchiveEntry> members;
#if defined(LOFTAIL_HAVE_ARCHIVE)
    members = listOffThread(container, parent, &listError);
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
