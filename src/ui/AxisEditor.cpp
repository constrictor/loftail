#include "AxisEditor.h"

#include "UiColors.h"


#include "Document.h"
#include "Filter.h"
#include "LogFormat.h"
#include "Priority.h"
#include "RecordIndex.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

namespace loftail {

namespace {

// A group box laid out as [enable checkbox] + [body]. The body is one widget rather
// than loose rows so setCollapsible() can hide the whole axis in a single call.
struct AxisBox
{
    QGroupBox   *box;
    QCheckBox   *enable;
    QWidget     *body;
    QVBoxLayout *bodyLayout;
};

AxisBox makeAxisBox(QWidget *parent, const QString &title, const QString &enableText,
                    bool enabledByDefault)
{
    AxisBox a;
    a.box = new QGroupBox(title, parent);
    auto *v = new QVBoxLayout(a.box);
    a.enable = new QCheckBox(enableText, a.box);
    a.enable->setChecked(enabledByDefault);
    v->addWidget(a.enable);
    a.body = new QWidget(a.box);
    a.bodyLayout = new QVBoxLayout(a.body);
    a.bodyLayout->setContentsMargins(0, 0, 0, 0);
    v->addWidget(a.body);
    return a;
}

// The All / None / Invert row shared by the two value lists.
QHBoxLayout *makeListButtons(QWidget *parent, QPushButton *&all, QPushButton *&none,
                             QPushButton *&invert)
{
    // Not a member, so there is no tr() in scope — the context is named explicitly, and
    // named for the class these buttons belong to.
    auto *btns = new QHBoxLayout;
    all = new QPushButton(QCoreApplication::translate("loftail::AxisEditor", "All"), parent);
    none = new QPushButton(QCoreApplication::translate("loftail::AxisEditor", "None"), parent);
    invert = new QPushButton(QCoreApplication::translate("loftail::AxisEditor", "Invert"), parent);
    btns->addWidget(all);
    btns->addWidget(none);
    btns->addWidget(invert);
    return btns;
}

QStringList toSortedList(const QSet<QString> &s)
{
    QStringList out = s.values();
    out.sort(Qt::CaseInsensitive);
    return out;
}

} // namespace

AxisEditor::AxisEditor(Defaults defaults, QWidget *parent) : QWidget(parent)
{
    buildUi(defaults);
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void AxisEditor::buildUi(Defaults defaults)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    auto emitChange = [this] { emitChanged(); };

    // --- Priority -----------------------------------------------------------
    {
        // Enabled by default for filtering (SPEC.md §6): off, the combo below was
        // inert — changing the minimum level did nothing until the box was also
        // ticked, which reads as a broken control. At the default TRACE the axis
        // narrows nothing, and the caller collapses that no-op state so it costs
        // nothing either. A highlight rule opts in instead.
        AxisBox a = makeAxisBox(this, tr("Priority"),
                                tr("Filter by minimum priority"),
                                defaults.priorityOn);
        m_priorityEnable = a.enable;
        m_priorityBody = a.body;
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(tr("Minimum:"), a.body));
        m_priorityCombo = new QComboBox(a.body);
        for (int i = 0; i < PriorityChoice::count(); ++i)
            m_priorityCombo->addItem(priorityName(PriorityChoice::at(i)));
        row->addWidget(m_priorityCombo, 1);
        a.bodyLayout->addLayout(row);
        root->addWidget(a.box);

        connect(m_priorityEnable, &QCheckBox::toggled, this, emitChange);
        connect(m_priorityCombo, &QComboBox::currentIndexChanged, this,
                [emitChange](int) { emitChange(); });
    }

    // --- Subsystem ----------------------------------------------------------
    {
        AxisBox a = makeAxisBox(this, tr("Subsystem"),
                                tr("Filter by subsystem"), defaults.loggerOn);
        m_loggerEnable = a.enable;
        m_loggerBody = a.body;
        m_loggerNarrow = new QLineEdit(a.body);
        m_loggerNarrow->setPlaceholderText(tr("Narrow list..."));
        ensureReadablePlaceholder(m_loggerNarrow);
        m_loggerNarrow->setClearButtonEnabled(true);
        a.bodyLayout->addWidget(m_loggerNarrow);
        m_loggerList = new QListWidget(a.body);
        m_loggerList->setMinimumHeight(90);
        a.bodyLayout->addWidget(m_loggerList);
        QPushButton *all = nullptr, *none = nullptr, *invert = nullptr;
        a.bodyLayout->addLayout(makeListButtons(a.body, all, none, invert));
        auto *manualRow = new QHBoxLayout;
        m_loggerManual = new QLineEdit(a.body);
        m_loggerManual->setPlaceholderText(tr("Add subsystem manually..."));
        ensureReadablePlaceholder(m_loggerManual);
        auto *add = new QPushButton(tr("Add"), a.body);
        manualRow->addWidget(m_loggerManual, 1);
        manualRow->addWidget(add);
        a.bodyLayout->addLayout(manualRow);
        root->addWidget(a.box);

        connect(m_loggerEnable, &QCheckBox::toggled, this, emitChange);
        // A hand edit to the list — one tick, or All / None / Invert, which reach here
        // the same way — returns the axis to the discovery default: whatever the user
        // is building now is a statement about the file, so a subsystem that appears
        // later belongs in it. Only showOnlyValue() says otherwise. The m_populating
        // guard keeps a repopulation from counting as an edit.
        connect(m_loggerList, &QListWidget::itemChanged, this,
                [this, emitChange](QListWidgetItem *) {
                    if (!m_populating)
                        m_loggerRestrictive = false;
                    emitChange();
                });
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
            m_loggerRestrictive = false; // typing a name is a hand edit like any other
            m_loggerManual->clear();
            refreshDiscoveredLists(); // re-inserts the manual name, checked
            emitChange();
        };
        connect(add, &QPushButton::clicked, this, addManual);
        connect(m_loggerManual, &QLineEdit::returnPressed, this, addManual);
    }

    // --- Thread -------------------------------------------------------------
    {
        AxisBox a = makeAxisBox(this, tr("Thread"),
                                tr("Filter by thread"), false);
        m_threadEnable = a.enable;
        m_threadBody = a.body;
        m_threadNarrow = new QLineEdit(a.body);
        m_threadNarrow->setPlaceholderText(tr("Narrow list..."));
        ensureReadablePlaceholder(m_threadNarrow);
        m_threadNarrow->setClearButtonEnabled(true);
        a.bodyLayout->addWidget(m_threadNarrow);
        m_threadList = new QListWidget(a.body);
        m_threadList->setMinimumHeight(70);
        a.bodyLayout->addWidget(m_threadList);
        QPushButton *all = nullptr, *none = nullptr, *invert = nullptr;
        a.bodyLayout->addLayout(makeListButtons(a.body, all, none, invert));
        auto *manualRow = new QHBoxLayout;
        m_threadManual = new QLineEdit(a.body);
        m_threadManual->setPlaceholderText(tr("Add thread manually..."));
        ensureReadablePlaceholder(m_threadManual);
        auto *add = new QPushButton(tr("Add"), a.body);
        manualRow->addWidget(m_threadManual, 1);
        manualRow->addWidget(add);
        a.bodyLayout->addLayout(manualRow);
        root->addWidget(a.box);

        connect(m_threadEnable, &QCheckBox::toggled, this, emitChange);
        connect(m_threadList, &QListWidget::itemChanged, this,
                [this, emitChange](QListWidgetItem *) {
                    if (!m_populating)
                        m_threadRestrictive = false;
                    emitChange();
                });
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
            m_threadRestrictive = false;
            m_threadManual->clear();
            refreshDiscoveredLists();
            emitChange();
        };
        connect(add, &QPushButton::clicked, this, addManual);
        connect(m_threadManual, &QLineEdit::returnPressed, this, addManual);
    }

    // --- Message text -------------------------------------------------------
    {
        AxisBox a = makeAxisBox(this, tr("Message text"),
                                tr("Filter by message text"), false);
        m_textEnable = a.enable;
        m_textBody = a.body;
        m_textEdit = new QLineEdit(a.body);
        m_textEdit->setPlaceholderText(tr("Substring or regex..."));
        ensureReadablePlaceholder(m_textEdit);
        m_textEdit->setClearButtonEnabled(true);
        a.bodyLayout->addWidget(m_textEdit);
        m_textRegex = new QCheckBox(tr("Regular expression"), a.body);
        m_textCase = new QCheckBox(tr("Case sensitive"), a.body);
        m_textNegate = new QCheckBox(tr("Hide matching (negate)"), a.body);
        a.bodyLayout->addWidget(m_textRegex);
        a.bodyLayout->addWidget(m_textCase);
        a.bodyLayout->addWidget(m_textNegate);
        root->addWidget(a.box);

        connect(m_textEnable, &QCheckBox::toggled, this, emitChange);
        connect(m_textEdit, &QLineEdit::textChanged, this,
                [emitChange](const QString &) { emitChange(); });
        connect(m_textRegex, &QCheckBox::toggled, this, emitChange);
        connect(m_textCase, &QCheckBox::toggled, this, emitChange);
        connect(m_textNegate, &QCheckBox::toggled, this, emitChange);
    }

    // --- Time range ---------------------------------------------------------
    {
        AxisBox a = makeAxisBox(this, tr("Time range"),
                                tr("Filter by time range"), false);
        m_timeEnable = a.enable;
        m_timeBody = a.body;
        const QString fmt = QStringLiteral("yyyy-MM-dd HH:mm:ss");
        auto *startRow = new QHBoxLayout;
        startRow->addWidget(new QLabel(tr("Start:"), a.body));
        m_timeStart = new QDateTimeEdit(a.body);
        m_timeStart->setDisplayFormat(fmt);
        m_timeStart->setCalendarPopup(true);
        startRow->addWidget(m_timeStart, 1);
        a.bodyLayout->addLayout(startRow);
        auto *endRow = new QHBoxLayout;
        endRow->addWidget(new QLabel(tr("End:"), a.body));
        m_timeEnd = new QDateTimeEdit(a.body);
        m_timeEnd->setDisplayFormat(fmt);
        m_timeEnd->setCalendarPopup(true);
        endRow->addWidget(m_timeEnd, 1);
        a.bodyLayout->addLayout(endRow);
        root->addWidget(a.box);

        connect(m_timeEnable, &QCheckBox::toggled, this, emitChange);
        connect(m_timeStart, &QDateTimeEdit::dateTimeChanged, this,
                [emitChange](const QDateTime &) { emitChange(); });
        connect(m_timeEnd, &QDateTimeEdit::dateTimeChanged, this,
                [emitChange](const QDateTime &) { emitChange(); });
    }

    root->addStretch(1);
    updateTextValidity();
}

void AxisEditor::emitChanged()
{
    if (m_populating)
        return;
    updateCollapse();
    updateTextValidity();
    emit changed();
}

void AxisEditor::setCollapsible(bool collapsible)
{
    m_collapsible = collapsible;
    updateCollapse();
}

void AxisEditor::updateCollapse()
{
    // Hide the body of a disabled axis so the whole editor shrinks to five title rows
    // when nothing is configured — what makes a five-axis rule editor fit a dock.
    struct Pair { QCheckBox *enable; QWidget *body; };
    const Pair pairs[] = {
        {m_priorityEnable, m_priorityBody}, {m_loggerEnable, m_loggerBody},
        {m_threadEnable, m_threadBody},     {m_textEnable, m_textBody},
        {m_timeEnable, m_timeBody},
    };
    for (const Pair &p : pairs) {
        if (p.enable && p.body)
            p.body->setVisible(!m_collapsible || p.enable->isChecked());
    }
}

void AxisEditor::updateTextValidity()
{
    if (!m_textEdit || !m_textRegex)
        return;
    // A regex that fails to compile matches NOTHING (TextMatcher::matches returns
    // false rather than throwing), which as a filter silently empties the view and as
    // a highlight rule silently colors nothing. Say so instead of leaving the user to
    // infer it — the Find bar already does.
    TextMatcher probe;
    probe.set(m_textEdit->text(), m_textRegex->isChecked(), Qt::CaseInsensitive);
    const bool bad = !probe.isValid();

    // Start from the field's OWN palette, not a default-constructed one: this runs on
    // every keystroke, and a fresh QPalette would drop the readable-placeholder repair
    // applied below at construction — the invalid-regex cue would silently undo it.
    QPalette pal = m_textEdit->palette();
    pal.setColor(QPalette::Text,
                 bad ? errorColor(pal) : qApp->palette(m_textEdit).color(QPalette::Text));
    m_textEdit->setPalette(pal);
    m_textEdit->setToolTip(bad ? tr("Invalid regular expression — matches nothing.")
                               : QString());
}

bool AxisEditor::textPatternValid() const
{
    if (!m_textEdit || !m_textRegex)
        return true;
    TextMatcher probe;
    probe.set(m_textEdit->text(), m_textRegex->isChecked(), Qt::CaseInsensitive);
    return probe.isValid();
}

// ---------------------------------------------------------------------------
// Document binding
// ---------------------------------------------------------------------------

void AxisEditor::setDocument(Document *document)
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
    // Likewise a statement about the previous file's values, and a wrong one about
    // this file's: carried across, a new file would open with every subsystem it
    // discovers arriving unticked.
    m_loggerRestrictive = false;
    m_threadRestrictive = false;

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

    // The zone the editors' digits are written in, tracked for every bound document
    // and not only for one carrying timestamps: setTimeBound() renders a record's UTC
    // ms through it, and refreshTimeBounds() reads it to recover the instant.
    if (hasDoc)
        m_renderZone = document->displayZone();

    // Seed the time editors to the file's observed span so the pickers open near
    // useful values rather than the epoch.
    if (hasDate) {
        m_populating = true;
        qint64 lo = 0, hi = 0;
        if (observedSpan(lo, hi)) {
            m_timeStart->setDateTime(wallClockOf(lo));
            m_timeEnd->setDateTime(wallClockOf(hi));
        }
        m_populating = false;
    }
    updateCollapse();
}

QDateTime AxisEditor::wallClockOf(qint64 utcMs) const
{
    // What the editors hold is display-zone WALL CLOCK with no zone attached, and
    // criteria() reinterprets those digits in the display zone (invariant #10). So an
    // instant has to be rendered in the display zone and the zone then dropped:
    // QDateTimeEdit converts whatever it is handed to its own spec, so passing a
    // zoned value would shift the digits by the machine's own offset and re-point the
    // bound at a different instant — on any machine whose local zone is not the
    // display zone, which is the normal case for a log from another host.
    QDateTime at = QDateTime::fromMSecsSinceEpoch(utcMs, m_renderZone);
    // Detaching the zone is spelled two ways across the supported Qt range and
    // neither spelling compiles on both: QTimeZone::LocalTime arrived in 6.5, and
    // setTimeSpec() is deprecated from 6.9. The floor is 6.4 — Ubuntu 24.04's Qt,
    // ARCHITECTURE.md §1 — so the older spelling cannot simply be kept.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    at.setTimeZone(QTimeZone::LocalTime);
#else
    at.setTimeSpec(Qt::LocalTime);
#endif
    return at;
}

bool AxisEditor::observedSpan(qint64 &lo, qint64 &hi) const
{
    if (!m_document)
        return false;
    lo = FilterSet::kMaxTime;
    hi = FilterSet::kMinTime;
    for (const Record &r : m_document->index().records) {
        if (r.timestamp == Record::kNoTimestamp)
            continue;
        lo = qMin(lo, r.timestamp);
        hi = qMax(hi, r.timestamp);
    }
    return lo <= hi;
}

bool AxisEditor::supportsThread() const
{
    return m_document && m_document->format().threadGroup > 0;
}

bool AxisEditor::supportsTime() const
{
    return m_document && m_document->format().dateGroup > 0;
}

void AxisEditor::refreshTimeBounds()
{
    if (!m_document || !m_timeStart || !m_timeEnd)
        return;
    const QTimeZone now = m_document->displayZone();
    if (now == m_renderZone)
        return;

    // The editors show display-zone wall clock, and criteria() reinterprets whatever
    // they hold in the CURRENT display zone. So when the zone moves under them the
    // shown text must be re-rendered, or the same digits would come to name a
    // different instant. Recover the instant using the zone the digits were written
    // in, then render it in the new one.
    m_populating = true;
    QDateTime s = m_timeStart->dateTime();
    QDateTime e = m_timeEnd->dateTime();
    s.setTimeZone(m_renderZone);
    e.setTimeZone(m_renderZone);
    m_timeStart->setDateTime(s.toTimeZone(now));
    m_timeEnd->setDateTime(e.toTimeZone(now));
    m_renderZone = now;
    m_populating = false;
}

void AxisEditor::refreshDiscoveredLists()
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
    repopulate(checkedNames(m_loggerList), checkedNames(m_threadList), /*exact=*/false);
}

void AxisEditor::repopulate(const QSet<QString> &loggerChecked,
                            const QSet<QString> &threadChecked, bool exact)
{
    if (!m_document)
        return;

    QStringList loggers = m_document->index().loggers.names();
    QStringList threads = m_document->index().threads.names();
    loggers.append(m_loggerManualNames.values());
    threads.append(m_threadManualNames.values());

    populateList(m_loggerList, loggers, loggerChecked, m_loggerManualNames, m_loggerSeen,
                 exact, m_loggerRestrictive);
    populateList(m_threadList, threads, threadChecked, m_threadManualNames, m_threadSeen,
                 exact, m_threadRestrictive);
    narrowList(m_loggerList, m_loggerNarrow ? m_loggerNarrow->text() : QString());
    narrowList(m_threadList, m_threadNarrow ? m_threadNarrow->text() : QString());
}

// ---------------------------------------------------------------------------
// Criteria in and out
// ---------------------------------------------------------------------------

MatchCriteria AxisEditor::criteria() const
{
    MatchCriteria c;

    c.priorityEnabled = m_priorityEnable->isChecked();
    c.minPriority = PriorityChoice::at(m_priorityCombo->currentIndex());

    c.loggerEnabled = m_loggerEnable->isChecked();
    c.loggerNames = toSortedList(checkedNames(m_loggerList));
    // Coverage is answered from the list the user was shown, never from the intern
    // table: the table grows mid-scan and the list lags it, so asking the table would
    // make a discovered-but-not-yet-listed subsystem look excluded.
    c.loggerCoversAll = allChecked(m_loggerList);
    c.loggerRestrictive = m_loggerRestrictive;

    c.threadEnabled = m_threadEnable->isChecked();
    c.threadNames = toSortedList(checkedNames(m_threadList));
    c.threadCoversAll = allChecked(m_threadList);
    c.threadRestrictive = m_threadRestrictive;

    c.text.enabled = m_textEnable->isChecked();
    c.text.negate = m_textNegate->isChecked();
    c.text.matcher.set(m_textEdit->text(), m_textRegex->isChecked(),
                       m_textCase->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive);

    c.timeEnabled = m_timeEnable->isChecked();
    c.start = m_timeStart->dateTime();
    c.end = m_timeEnd->dateTime();

    return c;
}

void AxisEditor::setCriteria(const MatchCriteria &c)
{
    m_populating = true;

    m_priorityEnable->setChecked(c.priorityEnabled);
    m_priorityCombo->setCurrentIndex(PriorityChoice::indexOf(c.minPriority));

    m_loggerEnable->setChecked(c.loggerEnabled);
    m_threadEnable->setChecked(c.threadEnabled);

    m_textEnable->setChecked(c.text.enabled);
    m_textEdit->setText(c.text.matcher.pattern());
    m_textRegex->setChecked(c.text.matcher.isRegex());
    m_textCase->setChecked(c.text.matcher.caseSensitivity() == Qt::CaseSensitive);
    m_textNegate->setChecked(c.text.negate);

    m_timeEnable->setChecked(c.timeEnabled);
    if (c.start.isValid())
        m_timeStart->setDateTime(c.start);
    if (c.end.isValid())
        m_timeEnd->setDateTime(c.end);

    // A selected name the scan has not produced yet is carried as a manual entry, so
    // it is listed (and stays listed) rather than silently dropping out of the rule
    // the moment the editor repopulates. This is also what makes a session restored
    // before indexing finishes keep its selection (SPEC.md §6, §10).
    const QSet<QString> loggerSel(c.loggerNames.begin(), c.loggerNames.end());
    const QSet<QString> threadSel(c.threadNames.begin(), c.threadNames.end());
    m_loggerManualNames = loggerSel;
    m_threadManualNames = threadSel;
    // Restore how the selection is meant to grow along with the selection itself: a
    // preset or session that restricted must not widen when this file turns up a
    // value the one it was made on never had.
    m_loggerRestrictive = c.loggerRestrictive;
    m_threadRestrictive = c.threadRestrictive;

    m_populating = false;

    // Exact: the stored selection is reproduced as-is rather than run through the
    // discovery rule, so loading one highlight rule after another shows each rule's
    // own subsystems instead of inheriting the previous rule's.
    repopulate(loggerSel, threadSel, /*exact=*/true);

    updateCollapse();
    updateTextValidity();
}

// ---------------------------------------------------------------------------
// Edits driven from a record (the record menu, SPEC.md §5)
// ---------------------------------------------------------------------------

QListWidget *AxisEditor::listFor(ValueAxis axis) const
{
    return axis == ValueAxis::Subsystem ? m_loggerList : m_threadList;
}

QCheckBox *AxisEditor::enableFor(ValueAxis axis) const
{
    return axis == ValueAxis::Subsystem ? m_loggerEnable : m_threadEnable;
}

QSet<QString> &AxisEditor::manualFor(ValueAxis axis)
{
    return axis == ValueAxis::Subsystem ? m_loggerManualNames : m_threadManualNames;
}

bool &AxisEditor::restrictiveFor(ValueAxis axis)
{
    return axis == ValueAxis::Subsystem ? m_loggerRestrictive : m_threadRestrictive;
}

void AxisEditor::ensureListed(ValueAxis axis, const QString &name)
{
    QListWidget *list = listFor(axis);
    if (!list)
        return;
    auto listed = [list, &name] {
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text() == name)
                return true;
        return false;
    };
    if (listed())
        return;
    // The value came off a record, so the intern table has it even when the list this
    // pane last drew does not — indexing runs ahead of the repopulations that follow
    // it. Refresh from the table first, and only fall back to carrying the name as a
    // manual entry if it is somehow still absent.
    refreshDiscoveredLists();
    if (listed())
        return;
    manualFor(axis).insert(name);
    refreshDiscoveredLists();
}

void AxisEditor::showOnlyValue(ValueAxis axis, const QString &name)
{
    QListWidget *list = listFor(axis);
    if (!list || name.isEmpty() || (axis == ValueAxis::Thread && !supportsThread()))
        return;
    ensureListed(axis, name);

    m_populating = true;
    // Every item, including the ones the narrow box is currently hiding. All / None
    // deliberately act on the narrowed view only, because there the user can see what
    // they are acting on; a menu item that read "show only net.http" and quietly left
    // hidden values ticked would restrict to more than it says.
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem *item = list->item(i);
        item->setCheckState(item->text() == name ? Qt::Checked : Qt::Unchecked);
    }
    if (QCheckBox *enable = enableFor(axis))
        enable->setChecked(true);
    restrictiveFor(axis) = true;
    m_populating = false;

    emitChanged();
}

void AxisEditor::hideValue(ValueAxis axis, const QString &name)
{
    QListWidget *list = listFor(axis);
    if (!list || name.isEmpty() || (axis == ValueAxis::Thread && !supportsThread()))
        return;
    ensureListed(axis, name);

    m_populating = true;
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem *item = list->item(i);
        if (item->text() == name)
            item->setCheckState(Qt::Unchecked);
    }
    // Unticking one value out of everything says nothing until the axis is on — and
    // the thread axis ships off (SPEC.md §6).
    if (QCheckBox *enable = enableFor(axis))
        enable->setChecked(true);
    m_populating = false;

    emitChanged();
}

void AxisEditor::setMinimumPriority(Priority p)
{
    if (!m_priorityEnable || !m_priorityCombo || p == Priority::Unknown)
        return;
    m_populating = true;
    m_priorityCombo->setCurrentIndex(PriorityChoice::indexOf(p));
    m_priorityEnable->setChecked(true);
    m_populating = false;
    emitChanged();
}

void AxisEditor::setTimeBound(TimeBound which, qint64 utcMs)
{
    if (!m_timeEnable || !m_timeStart || !m_timeEnd || !supportsTime())
        return;
    const QDateTime at = wallClockOf(utcMs);

    // What the opposite bound should be when it has to move. The file's observed span
    // is the honest "open end": the editors cannot hold "no bound", and leaving an
    // unseeded end at the year 2000 — which is what a file that had no timestamps
    // when the pane bound to it leaves behind — would hide everything.
    qint64 lo = 0, hi = 0;
    const bool span = observedSpan(lo, hi);
    const QDateTime openEnd = wallClockOf(span ? qMax(hi, utcMs) : utcMs);
    const QDateTime openStart = wallClockOf(span ? qMin(lo, utcMs) : utcMs);

    const bool wasEnabled = m_timeEnable->isChecked();
    m_populating = true;
    if (which == TimeBound::Start) {
        m_timeStart->setDateTime(at);
        // Widen the far end only when it would otherwise exclude the record just
        // pointed at — an end the user set deliberately is left where it is.
        if (!wasEnabled || m_timeEnd->dateTime() < at)
            m_timeEnd->setDateTime(openEnd);
    } else {
        m_timeEnd->setDateTime(at);
        if (!wasEnabled || m_timeStart->dateTime() > at)
            m_timeStart->setDateTime(openStart);
    }
    m_timeEnable->setChecked(true);
    m_populating = false;

    emitChanged();
}

void AxisEditor::setTimeRange(qint64 fromUtcMs, qint64 toUtcMs)
{
    if (!m_timeEnable || !m_timeStart || !m_timeEnd || !supportsTime())
        return;
    if (fromUtcMs > toUtcMs)
        std::swap(fromUtcMs, toUtcMs);
    m_populating = true;
    m_timeStart->setDateTime(wallClockOf(fromUtcMs));
    m_timeEnd->setDateTime(wallClockOf(toUtcMs));
    m_timeEnable->setChecked(true);
    m_populating = false;
    emitChanged();
}

// ---------------------------------------------------------------------------
// List helpers
// ---------------------------------------------------------------------------

void AxisEditor::populateList(QListWidget *list, const QStringList &names,
                              const QSet<QString> &checked, const QSet<QString> &manual,
                              QSet<QString> &seen, bool exact, bool restrictive)
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
    // Discovery rule (exact == false): a name the editor has never shown before is
    // checked; one it has shown keeps whatever state it had. The distinction matters
    // because the subsystem axis is enabled by default (SPEC.md §6) and subsystems are
    // discovered *as the file is scanned* — without it, every subsystem that first
    // appears after the initial population would arrive unchecked and its records
    // would vanish from an untouched view. "Never shown" also covers the very first
    // population, where everything is new.
    //
    // Load rule (exact == true): checked means exactly `checked`, because the caller is
    // reproducing a stored selection, not discovering values.
    //
    // Restriction rule (restrictive, exact == false): the discovery rule is exactly
    // wrong — the selection names what the user asked to see, so a value nobody has
    // seen yet is not part of it and arrives UNCHECKED. A name the user typed by hand
    // still arrives checked: adding it is the request to see it.
    //
    // A name already listed keeps its own state under every rule but the load, which
    // is what makes an unticked value stay unticked across the repopulations indexing
    // drives — including one the user typed in and then unticked.
    for (const QString &n : sorted) {
        auto *item = new QListWidgetItem(n, list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const bool fresh = !seen.contains(n);
        const bool on = exact ? checked.contains(n)
                              : (checked.contains(n)
                                 || (fresh && (!restrictive || manual.contains(n))));
        seen.insert(n);
        item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }
    m_populating = false;
}

bool AxisEditor::allChecked(const QListWidget *list)
{
    if (!list)
        return true;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->checkState() != Qt::Checked)
            return false;
    return true; // an empty list excludes nothing
}

QSet<QString> AxisEditor::checkedNames(const QListWidget *list) const
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

void AxisEditor::setAllChecked(QListWidget *list, bool checked)
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

void AxisEditor::invertChecked(QListWidget *list)
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

void AxisEditor::narrowList(QListWidget *list, const QString &needle)
{
    if (!list)
        return;
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem *item = list->item(i);
        item->setHidden(!needle.isEmpty()
                        && !item->text().contains(needle, Qt::CaseInsensitive));
    }
}

} // namespace loftail
