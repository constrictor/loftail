#include "FilterPane.h"

#include "Document.h"
#include "Filter.h"
#include "LogFormat.h"
#include "Priority.h"
#include "RecordIndex.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QJsonArray>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace loftail {

namespace {
// Combo index -> Priority, in severity order (§7.2). Index 0 is the lowest
// selectable minimum, TRACE (the default that shows everything, SPEC.md §6).
const Priority kPriorityByIndex[] = {
    Priority::Trace, Priority::Debug, Priority::Info,
    Priority::Warn,  Priority::Error, Priority::Fatal,
};
} // namespace

FilterPane::FilterPane(QWidget *parent) : QWidget(parent)
{
    buildUi();
    setDocument(nullptr);
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void FilterPane::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto *content = new QWidget(scroll);
    scroll->setWidget(content);
    auto *root = new QVBoxLayout(content);

    auto emitChange = [this] {
        if (m_populating)
            return;
        applyToDocument();
        emit filtersChanged();
    };

    // --- Priority -----------------------------------------------------------
    {
        auto *box = new QGroupBox(QStringLiteral("Priority"), content);
        auto *v = new QVBoxLayout(box);
        m_priorityEnable = new QCheckBox(QStringLiteral("Filter by minimum priority"), box);
        // On by default (SPEC.md §6). Off, the combo below it was inert — changing
        // the minimum level did nothing until the box was also ticked, which reads
        // as a broken control. At the default TRACE the axis hides nothing, and
        // applyToDocument() collapses that no-op state so it costs nothing either.
        m_priorityEnable->setChecked(true);
        v->addWidget(m_priorityEnable);
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("Minimum:"), box));
        m_priorityCombo = new QComboBox(box);
        for (Priority p : kPriorityByIndex)
            m_priorityCombo->addItem(priorityName(p));
        row->addWidget(m_priorityCombo, 1);
        v->addLayout(row);
        root->addWidget(box);
        connect(m_priorityEnable, &QCheckBox::toggled, this, emitChange);
        connect(m_priorityCombo, &QComboBox::currentIndexChanged, this, [emitChange](int) { emitChange(); });
    }

    // --- Subsystem ----------------------------------------------------------
    {
        auto *box = new QGroupBox(QStringLiteral("Subsystem"), content);
        auto *v = new QVBoxLayout(box);
        m_loggerEnable = new QCheckBox(QStringLiteral("Filter by subsystem"), box);
        m_loggerEnable->setChecked(true); // as for priority: unticking a subsystem acts at once
        v->addWidget(m_loggerEnable);
        m_loggerNarrow = new QLineEdit(box);
        m_loggerNarrow->setPlaceholderText(QStringLiteral("Narrow list..."));
        m_loggerNarrow->setClearButtonEnabled(true);
        v->addWidget(m_loggerNarrow);
        m_loggerList = new QListWidget(box);
        m_loggerList->setMinimumHeight(90);
        v->addWidget(m_loggerList);
        auto *btns = new QHBoxLayout;
        auto *all = new QPushButton(QStringLiteral("All"), box);
        auto *none = new QPushButton(QStringLiteral("None"), box);
        auto *invert = new QPushButton(QStringLiteral("Invert"), box);
        btns->addWidget(all);
        btns->addWidget(none);
        btns->addWidget(invert);
        v->addLayout(btns);
        auto *manualRow = new QHBoxLayout;
        m_loggerManual = new QLineEdit(box);
        m_loggerManual->setPlaceholderText(QStringLiteral("Add subsystem manually..."));
        auto *add = new QPushButton(QStringLiteral("Add"), box);
        manualRow->addWidget(m_loggerManual, 1);
        manualRow->addWidget(add);
        v->addLayout(manualRow);
        root->addWidget(box);

        connect(m_loggerEnable, &QCheckBox::toggled, this, emitChange);
        connect(m_loggerList, &QListWidget::itemChanged, this, [emitChange](QListWidgetItem *) { emitChange(); });
        connect(m_loggerNarrow, &QLineEdit::textChanged, this,
                [this](const QString &s) { narrowList(m_loggerList, s); });
        connect(all, &QPushButton::clicked, this, [this] { setAllChecked(m_loggerList, true); });
        connect(none, &QPushButton::clicked, this, [this] { setAllChecked(m_loggerList, false); });
        connect(invert, &QPushButton::clicked, this, [this] { invertChecked(m_loggerList); });
        auto addManual = [this, emitChange] {
            const QString name = m_loggerManual->text().trimmed();
            if (name.isEmpty())
                return;
            m_loggerManualNames.insert(name);
            m_loggerManual->clear();
            refreshDiscoveredLists(); // re-inserts the manual name, checked
            emitChange();
        };
        connect(add, &QPushButton::clicked, this, addManual);
        connect(m_loggerManual, &QLineEdit::returnPressed, this, addManual);
    }

    // --- Thread -------------------------------------------------------------
    {
        auto *box = new QGroupBox(QStringLiteral("Thread"), content);
        auto *v = new QVBoxLayout(box);
        m_threadEnable = new QCheckBox(QStringLiteral("Filter by thread"), box);
        v->addWidget(m_threadEnable);
        m_threadNarrow = new QLineEdit(box);
        m_threadNarrow->setPlaceholderText(QStringLiteral("Narrow list..."));
        m_threadNarrow->setClearButtonEnabled(true);
        v->addWidget(m_threadNarrow);
        m_threadList = new QListWidget(box);
        m_threadList->setMinimumHeight(70);
        v->addWidget(m_threadList);
        auto *btns = new QHBoxLayout;
        auto *all = new QPushButton(QStringLiteral("All"), box);
        auto *none = new QPushButton(QStringLiteral("None"), box);
        auto *invert = new QPushButton(QStringLiteral("Invert"), box);
        btns->addWidget(all);
        btns->addWidget(none);
        btns->addWidget(invert);
        v->addLayout(btns);
        auto *manualRow = new QHBoxLayout;
        m_threadManual = new QLineEdit(box);
        m_threadManual->setPlaceholderText(QStringLiteral("Add thread manually..."));
        auto *add = new QPushButton(QStringLiteral("Add"), box);
        manualRow->addWidget(m_threadManual, 1);
        manualRow->addWidget(add);
        v->addLayout(manualRow);
        root->addWidget(box);

        connect(m_threadEnable, &QCheckBox::toggled, this, emitChange);
        connect(m_threadList, &QListWidget::itemChanged, this, [emitChange](QListWidgetItem *) { emitChange(); });
        connect(m_threadNarrow, &QLineEdit::textChanged, this,
                [this](const QString &s) { narrowList(m_threadList, s); });
        connect(all, &QPushButton::clicked, this, [this] { setAllChecked(m_threadList, true); });
        connect(none, &QPushButton::clicked, this, [this] { setAllChecked(m_threadList, false); });
        connect(invert, &QPushButton::clicked, this, [this] { invertChecked(m_threadList); });
        auto addManual = [this, emitChange] {
            const QString name = m_threadManual->text().trimmed();
            if (name.isEmpty())
                return;
            m_threadManualNames.insert(name);
            m_threadManual->clear();
            refreshDiscoveredLists();
            emitChange();
        };
        connect(add, &QPushButton::clicked, this, addManual);
        connect(m_threadManual, &QLineEdit::returnPressed, this, addManual);
    }

    // --- Message text -------------------------------------------------------
    {
        auto *box = new QGroupBox(QStringLiteral("Message text"), content);
        auto *v = new QVBoxLayout(box);
        m_textEnable = new QCheckBox(QStringLiteral("Filter by message text"), box);
        v->addWidget(m_textEnable);
        m_textEdit = new QLineEdit(box);
        m_textEdit->setPlaceholderText(QStringLiteral("Substring or regex..."));
        m_textEdit->setClearButtonEnabled(true);
        v->addWidget(m_textEdit);
        m_textRegex = new QCheckBox(QStringLiteral("Regular expression"), box);
        m_textCase = new QCheckBox(QStringLiteral("Case sensitive"), box);
        m_textNegate = new QCheckBox(QStringLiteral("Hide matching (negate)"), box);
        v->addWidget(m_textRegex);
        v->addWidget(m_textCase);
        v->addWidget(m_textNegate);
        root->addWidget(box);

        connect(m_textEnable, &QCheckBox::toggled, this, emitChange);
        connect(m_textEdit, &QLineEdit::textChanged, this, [emitChange](const QString &) { emitChange(); });
        connect(m_textRegex, &QCheckBox::toggled, this, emitChange);
        connect(m_textCase, &QCheckBox::toggled, this, emitChange);
        connect(m_textNegate, &QCheckBox::toggled, this, emitChange);
    }

    // --- Time range ---------------------------------------------------------
    {
        auto *box = new QGroupBox(QStringLiteral("Time range"), content);
        auto *v = new QVBoxLayout(box);
        m_timeEnable = new QCheckBox(QStringLiteral("Filter by time range"), box);
        v->addWidget(m_timeEnable);
        const QString fmt = QStringLiteral("yyyy-MM-dd HH:mm:ss");
        auto *startRow = new QHBoxLayout;
        startRow->addWidget(new QLabel(QStringLiteral("Start:"), box));
        m_timeStart = new QDateTimeEdit(box);
        m_timeStart->setDisplayFormat(fmt);
        m_timeStart->setCalendarPopup(true);
        startRow->addWidget(m_timeStart, 1);
        v->addLayout(startRow);
        auto *endRow = new QHBoxLayout;
        endRow->addWidget(new QLabel(QStringLiteral("End:"), box));
        m_timeEnd = new QDateTimeEdit(box);
        m_timeEnd->setDisplayFormat(fmt);
        m_timeEnd->setCalendarPopup(true);
        endRow->addWidget(m_timeEnd, 1);
        v->addLayout(endRow);
        root->addWidget(box);

        connect(m_timeEnable, &QCheckBox::toggled, this, emitChange);
        connect(m_timeStart, &QDateTimeEdit::dateTimeChanged, this, [emitChange](const QDateTime &) { emitChange(); });
        connect(m_timeEnd, &QDateTimeEdit::dateTimeChanged, this, [emitChange](const QDateTime &) { emitChange(); });
    }

    root->addStretch(1);
}

// ---------------------------------------------------------------------------
// Document binding
// ---------------------------------------------------------------------------

void FilterPane::setDocument(Document *document)
{
    m_document = document;
    m_loggerManualNames.clear();
    m_threadManualNames.clear();
    // "Seen" is knowledge about THIS file's discovered values, so it dies with the
    // binding. Carrying it across a rebind would be actively harmful: the lists are
    // cleared here, so on the next population every remembered name would count as
    // already-seen-but-not-checked and the new file would open fully filtered out.
    m_loggerSeen.clear();
    m_threadSeen.clear();

    const bool hasDoc = document != nullptr;
    const bool hasThread = hasDoc && document->format().threadGroup > 0;
    const bool hasDate = hasDoc && document->format().dateGroup > 0;

    setEnabled(hasDoc);
    // Thread and time axes exist only when the format carries those fields
    // (SPEC.md §6). Disable rather than hide so the layout is stable.
    if (m_threadEnable)
        m_threadEnable->parentWidget()->setEnabled(hasThread);
    if (m_timeEnable)
        m_timeEnable->parentWidget()->setEnabled(hasDate);

    refreshDiscoveredLists();

    // Seed the time editors to the file's observed span so the pickers open near
    // useful values rather than the epoch.
    if (hasDate && document) {
        m_populating = true;
        const QTimeZone dz = document->displayZone();
        qint64 lo = FilterSet::kMaxTime, hi = FilterSet::kMinTime;
        for (const Record &r : document->index().records) {
            if (r.timestamp == Record::kNoTimestamp)
                continue;
            lo = qMin(lo, r.timestamp);
            hi = qMax(hi, r.timestamp);
        }
        if (lo <= hi) {
            m_timeStart->setDateTime(QDateTime::fromMSecsSinceEpoch(lo, dz));
            m_timeEnd->setDateTime(QDateTime::fromMSecsSinceEpoch(hi, dz));
        }
        m_populating = false;
    }
}

void FilterPane::refreshDiscoveredLists()
{
    if (!m_document) {
        m_populating = true;
        if (m_loggerList)
            m_loggerList->clear();
        if (m_threadList)
            m_threadList->clear();
        m_populating = false;
        return;
    }

    const QSet<QString> loggerChecked = checkedNames(m_loggerList);
    const QSet<QString> threadChecked = checkedNames(m_threadList);

    QStringList loggers = m_document->index().loggers.names();
    QStringList threads = m_document->index().threads.names();
    loggers.append(m_loggerManualNames.values());
    threads.append(m_threadManualNames.values());

    populateList(m_loggerList, loggers, loggerChecked, m_loggerManualNames, m_loggerSeen);
    populateList(m_threadList, threads, threadChecked, m_threadManualNames, m_threadSeen);
    narrowList(m_loggerList, m_loggerNarrow ? m_loggerNarrow->text() : QString());
    narrowList(m_threadList, m_threadNarrow ? m_threadNarrow->text() : QString());

    // Keep the Document's FilterSet in step with the (re)populated lists so a
    // subsequent apply resolves ids against what is now shown, not a stale set. This
    // does not emit filtersChanged() — MainWindow reapplies explicitly when the scan
    // finishes; a plain repopulation should not itself trigger a recompute storm.
    applyToDocument();
}

// ---------------------------------------------------------------------------
// Portable state snapshot (presets + session restore)
// ---------------------------------------------------------------------------

namespace {
QJsonArray namesToArray(const QSet<QString> &names)
{
    QStringList sorted = names.values();
    sorted.sort(Qt::CaseInsensitive);
    QJsonArray a;
    for (const QString &n : sorted)
        a.append(n);
    return a;
}

QSet<QString> arrayToNames(const QJsonArray &a)
{
    QSet<QString> out;
    for (const QJsonValue &v : a)
        out.insert(v.toString());
    return out;
}
} // namespace

QJsonObject FilterPane::saveState() const
{
    QJsonObject o;
    o.insert(QStringLiteral("priorityEnabled"), m_priorityEnable->isChecked());
    o.insert(QStringLiteral("minPriorityIndex"), m_priorityCombo->currentIndex());

    o.insert(QStringLiteral("loggerEnabled"), m_loggerEnable->isChecked());
    o.insert(QStringLiteral("loggerChecked"), namesToArray(checkedNames(m_loggerList)));

    o.insert(QStringLiteral("threadEnabled"), m_threadEnable->isChecked());
    o.insert(QStringLiteral("threadChecked"), namesToArray(checkedNames(m_threadList)));

    o.insert(QStringLiteral("textEnabled"), m_textEnable->isChecked());
    o.insert(QStringLiteral("text"), m_textEdit->text());
    o.insert(QStringLiteral("textRegex"), m_textRegex->isChecked());
    o.insert(QStringLiteral("textCase"), m_textCase->isChecked());
    o.insert(QStringLiteral("textNegate"), m_textNegate->isChecked());

    o.insert(QStringLiteral("timeEnabled"), m_timeEnable->isChecked());
    o.insert(QStringLiteral("timeStart"), m_timeStart->dateTime().toString(Qt::ISODate));
    o.insert(QStringLiteral("timeEnd"), m_timeEnd->dateTime().toString(Qt::ISODate));
    return o;
}

void FilterPane::restoreState(const QJsonObject &o)
{
    m_populating = true;

    // Absent key => the new default (on), matching a freshly-built pane; a state
    // written by this version always carries the key explicitly either way.
    m_priorityEnable->setChecked(o.value(QStringLiteral("priorityEnabled")).toBool(true));
    m_priorityCombo->setCurrentIndex(
        qMax(0, o.value(QStringLiteral("minPriorityIndex")).toInt(0)));

    // Restored subsystem/thread selections are carried as manual names so they
    // survive discovery timing: at restore the intern lists are usually still empty
    // (indexing just started), and manual names are always re-inserted and checked,
    // then merged with the discovered set when the scan completes (SPEC.md §6, §10).
    m_loggerEnable->setChecked(o.value(QStringLiteral("loggerEnabled")).toBool(true));
    m_loggerManualNames = arrayToNames(o.value(QStringLiteral("loggerChecked")).toArray());

    m_threadEnable->setChecked(o.value(QStringLiteral("threadEnabled")).toBool(false));
    m_threadManualNames = arrayToNames(o.value(QStringLiteral("threadChecked")).toArray());

    m_textEnable->setChecked(o.value(QStringLiteral("textEnabled")).toBool(false));
    m_textEdit->setText(o.value(QStringLiteral("text")).toString());
    m_textRegex->setChecked(o.value(QStringLiteral("textRegex")).toBool(false));
    m_textCase->setChecked(o.value(QStringLiteral("textCase")).toBool(false));
    m_textNegate->setChecked(o.value(QStringLiteral("textNegate")).toBool(false));

    m_timeEnable->setChecked(o.value(QStringLiteral("timeEnabled")).toBool(false));
    const QDateTime start =
        QDateTime::fromString(o.value(QStringLiteral("timeStart")).toString(), Qt::ISODate);
    const QDateTime end =
        QDateTime::fromString(o.value(QStringLiteral("timeEnd")).toString(), Qt::ISODate);
    if (start.isValid())
        m_timeStart->setDateTime(start);
    if (end.isValid())
        m_timeEnd->setDateTime(end);

    m_populating = false;

    // Repopulate the lists (checking the restored manual names) and push the state
    // into the document, then let the caller recompute the visible set.
    refreshDiscoveredLists();
    applyToDocument();
    emit filtersChanged();
}

// ---------------------------------------------------------------------------
// List helpers
// ---------------------------------------------------------------------------

void FilterPane::populateList(QListWidget *list, const QStringList &names,
                              const QSet<QString> &checked, const QSet<QString> &manual,
                              QSet<QString> &seen)
{
    if (!list)
        return;
    m_populating = true;
    list->clear();
    // De-duplicate and drop the empty-string id (id 0, the "field absent" sentinel).
    QStringList sorted;
    QSet<QString> dedup;
    for (const QString &n : names) {
        if (n.isEmpty() || dedup.contains(n))
            continue;
        dedup.insert(n);
        sorted.append(n);
    }
    sorted.sort(Qt::CaseInsensitive);
    // A name the pane has never shown before is checked; one it has shown keeps
    // whatever state it had. The distinction matters because the axis is enabled by
    // default (SPEC.md §6) and subsystems are discovered *as the file is scanned* —
    // without it, every subsystem that first appears after the initial population
    // would arrive unchecked and its records would vanish from an untouched view.
    // "Never shown" also covers the very first population, where everything is new.
    for (const QString &n : sorted) {
        auto *item = new QListWidgetItem(n, list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const bool on = !seen.contains(n) || checked.contains(n) || manual.contains(n);
        seen.insert(n);
        item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }
    m_populating = false;
}

bool FilterPane::allChecked(const QListWidget *list)
{
    if (!list)
        return true;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->checkState() != Qt::Checked)
            return false;
    return true; // an empty list excludes nothing
}

QSet<QString> FilterPane::checkedNames(const QListWidget *list) const
{
    QSet<QString> out;
    if (!list)
        return out;
    for (int i = 0; i < list->count(); ++i) {
        const QListWidgetItem *item = list->item(i);
        if (item->checkState() == Qt::Checked)
            out.insert(item->text());
    }
    return out;
}

void FilterPane::setAllChecked(QListWidget *list, bool checked)
{
    if (!list)
        return;
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem *item = list->item(i);
        if (item->isHidden())
            continue; // operate on the currently-narrowed view only
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
}

void FilterPane::invertChecked(QListWidget *list)
{
    if (!list)
        return;
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem *item = list->item(i);
        if (item->isHidden())
            continue;
        item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    }
}

void FilterPane::narrowList(QListWidget *list, const QString &needle)
{
    if (!list)
        return;
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem *item = list->item(i);
        item->setHidden(!needle.isEmpty()
                        && !item->text().contains(needle, Qt::CaseInsensitive));
    }
}

// ---------------------------------------------------------------------------
// Build the FilterSet
// ---------------------------------------------------------------------------

void FilterPane::applyToDocument()
{
    if (!m_document)
        return;

    FilterSet fs;

    // Priority: single minimum level (§7.2).
    //
    // Both metadata axes are enabled by default (SPEC.md §6), which would otherwise
    // put every file behind an active FilterSet and cost a materialized compact
    // index for no benefit. So an axis whose selection excludes NOTHING is written
    // as inactive: the checkbox stays ticked and responds instantly to the first
    // real narrowing, while FilteredIndex keeps its allocation-free identity path
    // (ARCHITECTURE.md §7.2). Both collapses are exact — TRACE is the lowest
    // selectable minimum, and an all-ticked list admits every id.
    constexpr int kPriorityCount = int(sizeof(kPriorityByIndex) / sizeof(kPriorityByIndex[0]));
    const int pi = qBound(0, m_priorityCombo->currentIndex(), kPriorityCount - 1);
    fs.minPriority = kPriorityByIndex[pi];
    fs.priorityEnabled = m_priorityEnable->isChecked() && fs.minPriority != kPriorityByIndex[0];

    // Subsystem / thread: resolve checked NAMES to interned ids (invariant #4). A
    // manually-entered name not yet in the file resolves to nothing and matches no
    // record until it appears.
    const RecordIndex &idx = m_document->index();
    fs.loggerEnabled = m_loggerEnable->isChecked() && !allChecked(m_loggerList);
    for (const QString &name : checkedNames(m_loggerList)) {
        bool found = false;
        const quint32 id = idx.loggers.idOf(name, &found);
        if (found)
            fs.loggerIds.insert(id);
    }
    fs.threadEnabled = m_threadEnable->isChecked() && !allChecked(m_threadList)
                       && m_document->format().threadGroup > 0;
    for (const QString &name : checkedNames(m_threadList)) {
        bool found = false;
        const quint32 id = idx.threads.idOf(name, &found);
        if (found)
            fs.threadIds.insert(id);
    }

    // Message text.
    fs.text.enabled = m_textEnable->isChecked();
    fs.text.negate = m_textNegate->isChecked();
    fs.text.matcher.set(m_textEdit->text(), m_textRegex->isChecked(),
                        m_textCase->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive);

    // Time range: the editors show display-zone wall-clock; convert to UTC ms once
    // here (§5.1). setTimeZone reinterprets the shown values in the display zone.
    fs.timeEnabled = m_timeEnable->isChecked() && m_document->format().dateGroup > 0;
    if (fs.timeEnabled) {
        const QTimeZone dz = m_document->displayZone();
        QDateTime s = m_timeStart->dateTime();
        QDateTime e = m_timeEnd->dateTime();
        s.setTimeZone(dz);
        e.setTimeZone(dz);
        fs.startMs = s.toMSecsSinceEpoch();
        fs.endMs = e.toMSecsSinceEpoch();
    }

    m_document->filters() = fs;
}

} // namespace loftail
