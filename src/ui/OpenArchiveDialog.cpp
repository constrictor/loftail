// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OpenArchiveDialog.h"

#include "ArchiveLocation.h"
#include "LogSettings.h"
#include "RemoteLocation.h"
#include "UiColors.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QThread>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <atomic>
#include <utility>

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

OpenArchiveDialog::OpenArchiveDialog(QString container,
                                     const QVector<ArchiveEntry> &members, QWidget *parent)
    : QDialog(parent), m_container(std::move(container)), m_members(members)
{
    setWindowTitle(tr("Choose a Log"));
    setObjectName(QStringLiteral("openArchiveDialog"));

    auto *layout = new QVBoxLayout(this);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("archiveSummary"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    // NARROWING THE LIST, because a support bundle holds hundreds of entries and
    // ctrl-clicking every `*.audit.log` out of them one at a time is not a gesture.
    // The line edit follows the Filters pane's, which is the only other place in
    // loftail where typing narrows a list (AxisEditor).
    auto *filterRow = new QHBoxLayout;
    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("archiveFilter"));
    m_filter->setPlaceholderText(tr("Narrow the list, e.g. *.audit.log"));
    m_filter->setToolTip(tr("Text matches anywhere in a log's name. Use * and ? to "
                            "match the whole name — *.log finds every log with that "
                            "extension. Not a regular expression."));
    ensureReadablePlaceholder(m_filter);
    m_filter->setClearButtonEnabled(true);
    filterRow->addWidget(m_filter, 1);

    m_selectAll = new QToolButton(this);
    m_selectAll->setObjectName(QStringLiteral("archiveSelectAll"));
    m_selectAll->setText(tr("Select All"));
    m_selectAll->setToolTip(tr("Select every log the list is currently showing."));
    filterRow->addWidget(m_selectAll);
    layout->addLayout(filterRow);

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

    // textChanged and NOT returnPressed: Return in this box would open whatever was
    // selected before the narrowing, which is the one thing narrowing must not do.
    connect(m_filter, &QLineEdit::textChanged, this, &OpenArchiveDialog::narrow);
    connect(m_selectAll, &QAbstractButton::clicked, this, &OpenArchiveDialog::selectShown);

    updateSummary();

    // The box takes the focus, so typing narrows straight away; Down walks into the
    // list from it. The pre-selection above is what keeps Open meaningful for somebody
    // who never types anything.
    m_filter->setFocus();

    resize(560, 380);
}

// THE MATCH RULE, and it is two rules chosen by what was typed rather than by a mode
// control the user has to find. Plain text is a substring, which is what a filter box
// does everywhere; the moment it carries a `*` or a `?` it becomes a wildcard over the
// WHOLE member path, so `*.log` means logs with that extension rather than "contains
// .log" — which would take `app.log.1` with it and defeat the one thing extensions are
// filtered for.
//
// Never a regular expression. wildcardToRegex() escapes everything else literally, so a
// member genuinely called `app(1).log` is found by typing its name.
bool OpenArchiveDialog::memberMatches(const QString &member, const QString &filter)
{
    if (filter.isEmpty())
        return true;

    if (!filter.contains(u'*') && !filter.contains(u'?'))
        return member.contains(filter, Qt::CaseInsensitive);

    // wildcardToRegex() is core's, and it is the one here for the reason its own header
    // gives: QRegularExpression::wildcardToRegularExpression() is path-aware (its `*`
    // stops at a separator, which is wrong for `*.log` over `var/log/app.log`) and the
    // option that turns that off is Qt 6.6, above this project's 6.4 floor.
    const QRegularExpression re(wildcardToRegex(filter),
                                QRegularExpression::CaseInsensitiveOption);
    if (!re.isValid())
        return false; // an unfinished pattern claims nothing rather than everything
    return re.match(member).hasMatch();
}

void OpenArchiveDialog::narrow(const QString &filter)
{
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_list->topLevelItem(i);
        item->setHidden(!memberMatches(item->text(0), filter));
    }
    updateSummary();
}

void OpenArchiveDialog::selectShown()
{
    // Not QTreeWidget::selectAll(), which goes through the selection model over the
    // whole model: this acts on the currently narrowed view only, the same rule
    // AxisEditor's All/None/Invert follow.
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_list->topLevelItem(i);
        if (!item->isHidden())
            item->setSelected(true);
    }
}

void OpenArchiveDialog::updateSummary()
{
    int shown = 0;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        if (!m_list->topLevelItem(i)->isHidden())
            ++shown;
    }
    // Off a filter this says exactly what it said before there was one, so the sentence
    // a reader sees on opening the picker has not moved.
    m_summary->setText(shown == m_members.size()
                           ? tr("%1 holds %2 logs. Choose one or more to open.")
                                 .arg(logSourceDisplayName(m_container))
                                 .arg(m_members.size())
                           : tr("%1 holds %2 logs; %3 shown. Choose one or more to open.")
                                 .arg(logSourceDisplayName(m_container))
                                 .arg(m_members.size())
                                 .arg(shown));
}

void OpenArchiveDialog::accept()
{
    m_chosen.clear();
    const QList<QTreeWidgetItem *> selected = m_list->selectedItems();
    for (QTreeWidgetItem *item : selected) {
        // A ROW THE FILTER HID IS NOT OPENED, whatever it was selected as. Pick
        // app.log, narrow to *.audit.log, press Open — and a tab for a row that is not
        // on screen is a tab nobody asked for. The empty-chosen guard below then covers
        // the case where the narrowing took every selected row away: the dialog stays
        // up rather than opening nothing.
        if (item->isHidden())
            continue;
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
