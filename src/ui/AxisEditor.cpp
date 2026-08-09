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
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

namespace loftail {

namespace {

// A group box whose TITLE ROW IS THE ENABLE CONTROL, plus a body. Spending a title
// on the axis name and then a checkbox on "Filter by <the same name>" cost two lines
// per axis for one fact; a checkable group box states it once, and Qt greys the body
// out while it is off instead of leaving dead controls live. The body is one widget
// rather than loose rows so setCollapsible() can hide the whole axis in a single call.
struct AxisBox
{
    QGroupBox   *box;
    QWidget     *body;
    QVBoxLayout *bodyLayout;
};

AxisBox makeAxisBox(QWidget *parent, const QString &title, const QString &objectName,
                    bool enabledByDefault)
{
    AxisBox a;
    a.box = new QGroupBox(title, parent);
    a.box->setObjectName(objectName);
    a.box->setCheckable(true);
    a.box->setChecked(enabledByDefault);
    auto *v = new QVBoxLayout(a.box);
    // Tighter than the style default all round: five stacked group boxes pay these
    // margins five times, and the top one twice over — the title row already sits
    // above the frame. It also keeps a COLLAPSED axis (the Highlighters pane) from
    // leaving a tall empty frame under its title.
    v->setContentsMargins(8, 4, 8, 6);
    a.body = new QWidget(a.box);
    a.bodyLayout = new QVBoxLayout(a.body);
    a.bodyLayout->setContentsMargins(0, 0, 0, 0);
    v->addWidget(a.body);
    return a;
}

// The All / None / Invert buttons shared by the two value lists, as a COLUMN beside
// the list rather than a row under it: the list is the tall thing in the pane, and a
// button row under each of two lists is two lines that the column reclaims for free.
//
// Tool buttons rather than push buttons. A QPushButton carries a style's full button
// margins and takes ~90 px of a dock that is only a third of the window wide — width
// the LIST wants, since a subsystem name is a dotted path and the ones that matter
// differ at the END.
//
// NOT auto-raised, though, unlike the message-text toggles: a flat frameless button
// with a one-word label and no icon reads as a caption, and "All / None / Invert" in
// a column beside a list reads as a caption particularly well. The frame is what says
// these are things to press. Auto-raise earns its keep on a TOGGLE, where the checked
// state supplies the missing affordance; it does not on a plain command.
//
// The column ends in the "New" checkbox, which is the same question asked about the
// values the list does not hold YET: All, None and Invert say what happens to what is
// listed, and it says what happens to what turns up next. It goes here rather than in
// a row of its own because it is one word wide and this column is exactly one word
// wide — and because the three buttons set it, which is far more obvious when it is
// the next thing under them.
QVBoxLayout *makeListButtons(QWidget *parent, const QString &namePrefix,
                             QAbstractButton *&all, QAbstractButton *&none,
                             QAbstractButton *&invert, QCheckBox *&newValues,
                             const QString &newValuesHint)
{
    // Not a member, so there is no tr() in scope — the context is named explicitly, and
    // named for the class these buttons belong to.
    auto make = [parent](const char *label, const QString &name) {
        auto *b = new QToolButton(parent);
        b->setText(QCoreApplication::translate("loftail::AxisEditor", label));
        b->setObjectName(name);
        // Without this a QToolButton hugs its text and the three end up ragged.
        b->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        return b;
    };
    auto *btns = new QVBoxLayout;
    all = make("All", namePrefix + QStringLiteral("All"));
    none = make("None", namePrefix + QStringLiteral("None"));
    invert = make("Invert", namePrefix + QStringLiteral("Invert"));
    btns->addWidget(all);
    btns->addWidget(none);
    btns->addWidget(invert);

    newValues = new QCheckBox(QCoreApplication::translate("loftail::AxisEditor", "New"),
                              parent);
    newValues->setObjectName(namePrefix + QStringLiteral("NewValues"));
    newValues->setChecked(true); // discovery is the default (populateList)
    newValues->setToolTip(newValuesHint);
    newValues->setAccessibleName(newValuesHint);
    btns->addWidget(newValues);

    btns->addStretch(1);
    return btns;
}

QStringList toSortedList(const QSet<QString> &s)
{
    QStringList out = s.values();
    out.sort(Qt::CaseInsensitive);
    return out;
}

} // namespace

AxisEditor::AxisEditor(Defaults defaults, QWidget *parent)
    : QWidget(parent), m_defaults(defaults)
{
    buildUi(defaults);
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void AxisEditor::buildUi(Defaults defaults)
{
    auto *root = new QVBoxLayout(this);
    // Not flush. A group box is a rounded, framed panel, and a rounded frame whose left
    // and right edges are 0 px from the pane's own edge reads as a rendering fault
    // rather than as a group — the corners curve away from a border that is not there.
    // Both users put this widget in a QScrollArea with no frame of its own, so these
    // few pixels are the only thing standing between the frames and the dock edge.
    root->setContentsMargins(6, 4, 6, 4);

    auto emitChange = [this] { emitChanged(); };

    // The order is how often an axis is reached for, not how the FilterSet declares
    // them: a level floor and a message search are the everyday narrowing, and the
    // message axis carries the context row the Filters pane injects (addTextExtra()),
    // which belongs next to the search it widens rather than at the foot of the pane.

    // --- Priority -----------------------------------------------------------
    {
        // One row, and no group box: an enable control plus a single combo does not
        // need a frame and a title to say so.
        //
        // Enabled by default for filtering (SPEC.md §6): off, the combo beside it was
        // inert — changing the minimum level did nothing until the box was also
        // ticked, which reads as a broken control. At the default TRACE the axis
        // narrows nothing, and the caller collapses that no-op state so it costs
        // nothing either. A highlight rule opts in instead.
        auto *row = new QHBoxLayout;
        m_priorityEnable = new QCheckBox(tr("Minimum priority:"), this);
        m_priorityEnable->setObjectName(QStringLiteral("priorityEnable"));
        m_priorityEnable->setChecked(defaults.priorityOn);
        row->addWidget(m_priorityEnable);
        m_priorityCombo = new QComboBox(this);
        m_priorityCombo->setObjectName(QStringLiteral("priorityCombo"));
        for (int i = 0; i < PriorityChoice::count(); ++i)
            m_priorityCombo->addItem(priorityName(PriorityChoice::at(i)));
        row->addWidget(m_priorityCombo, 1);
        // The combo is this axis's whole body, so setCollapsible() hides it and the
        // row shrinks to the checkbox — the same one-line stub the group boxes leave.
        m_priorityBody = m_priorityCombo;
        root->addLayout(row);

        connect(m_priorityEnable, &QCheckBox::toggled, this, emitChange);
        connect(m_priorityCombo, &QComboBox::currentIndexChanged, this,
                [emitChange](int) { emitChange(); });
    }

    // --- Message text -------------------------------------------------------
    {
        AxisBox a = makeAxisBox(this, tr("Message text"), QStringLiteral("messageGroup"),
                                false);
        m_textGroup = a.box;
        m_textBody = a.body;
        m_textEdit = new QLineEdit(a.body);
        m_textEdit->setObjectName(QStringLiteral("messageText"));
        m_textEdit->setPlaceholderText(tr("Substring or regex..."));
        ensureReadablePlaceholder(m_textEdit);
        m_textEdit->setClearButtonEnabled(true);
        a.bodyLayout->addWidget(m_textEdit);

        // An invalid regex matches NOTHING, so as a filter it empties the view and as
        // a highlight rule it colors nothing — in both cases indistinguishable from a
        // pattern that is merely too narrow, and the status bar's "0 of 4000 records
        // shown" reads like a legitimate answer. Red text in the field said so only to
        // someone already looking at the field, and only if they knew what the colour
        // meant; textPatternValid() existed to be asked and nothing asked it.
        m_textError = new QLabel(a.body);
        m_textError->setObjectName(QStringLiteral("messageError"));
        m_textError->setText(tr("Not a valid regular expression — this matches nothing."));
        m_textError->setWordWrap(true);
        m_textError->setVisible(false);
        a.bodyLayout->addWidget(m_textError);

        // The three options as toggles on ONE row rather than three stacked
        // checkboxes. This is the search field's own row of modifiers — the shape Qt
        // Creator, VS Code and the browser inspectors all use — and the three rows it
        // replaces were, with the context row below them, four fifths of this axis's
        // height for two bits and a sign. The glyphs are NOT translated (they are not
        // prose, ARCHITECTURE.md §9.1); the words move to the tooltip, and to the
        // accessible name so a screen reader still hears them.
        auto makeToggle = [&a](const QString &glyph, const QString &name,
                               const QString &prose) {
            auto *b = new QToolButton(a.body);
            b->setText(glyph);
            b->setObjectName(name);
            b->setCheckable(true);
            b->setAutoRaise(true);
            b->setToolTip(prose);
            b->setAccessibleName(prose);
            return b;
        };
        m_textRegex = makeToggle(QStringLiteral(".*"), QStringLiteral("messageRegex"),
                                 tr("Regular expression"));
        m_textCase = makeToggle(QStringLiteral("Aa"), QStringLiteral("messageCase"),
                                tr("Case sensitive"));
        m_textNegate = makeToggle(QString::fromUtf8("≠"),
                                  QStringLiteral("messageNegate"),
                                  tr("Hide matching records instead of showing them"));
        m_textOptionsRow = new QHBoxLayout;
        m_textOptionsRow->addWidget(m_textRegex);
        m_textOptionsRow->addWidget(m_textCase);
        m_textOptionsRow->addWidget(m_textNegate);
        // Whatever addTextExtra() injects lands after this, so it sits at the right
        // end of the row rather than crowding the toggles.
        m_textOptionsRow->addStretch(1);
        a.bodyLayout->addLayout(m_textOptionsRow);
        root->addWidget(a.box);

        connect(m_textGroup, &QGroupBox::toggled, this, emitChange);
        connect(m_textEdit, &QLineEdit::textChanged, this,
                [emitChange](const QString &) { emitChange(); });
        connect(m_textRegex, &QAbstractButton::toggled, this, emitChange);
        connect(m_textCase, &QAbstractButton::toggled, this, emitChange);
        connect(m_textNegate, &QAbstractButton::toggled, this, emitChange);
    }

    // --- Subsystem, then Thread ---------------------------------------------
    //
    // One function, twice. These two axes differ in a title, an object-name prefix, a
    // list height and whether they ship on; everything else — the discovery rule, the
    // narrowing, the manual add, the three list buttons — was written out twice and
    // had to be kept in step by hand.
    buildValueAxis(root, ValueAxis::Subsystem, tr("Subsystem"), QStringLiteral("subsystem"),
                   /*listMinHeight=*/90, defaults.loggerOn);
    buildValueAxis(root, ValueAxis::Thread, tr("Thread"), QStringLiteral("thread"),
                   /*listMinHeight=*/70, false);

    // --- Time range ---------------------------------------------------------
    {
        AxisBox a = makeAxisBox(this, tr("Time range"), "timeGroup", false);
        m_timeGroup = a.box;
        m_timeBody = a.body;
        const QString fmt = QStringLiteral("yyyy-MM-dd HH:mm:ss");
        // Both bounds on one row. Two second-resolution editors with calendar popups
        // want ~150 px each, which would make this row alone the widest thing in the
        // pane and put a horizontal scrollbar under a dock of ordinary width — with
        // the value lists' buttons the first thing off the edge. So each is given an
        // explicit floor and an Ignored horizontal policy. The policy is the part
        // that matters: a plain setMinimumWidth() BELOW the size hint does nothing,
        // because qSmartMinSize() takes minimumSizeHint() and only ever expands it to
        // minimumSize. Ignored drops the hint out of the sum; the floor then puts a
        // usable width back. Each editor still takes half of whatever the dock has.
        constexpr int kTimeEditMinWidth = 96;
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(tr("Start:"), a.body));
        m_timeStart = new QDateTimeEdit(a.body);
        m_timeStart->setObjectName(QStringLiteral("timeStart"));
        m_timeStart->setDisplayFormat(fmt);
        m_timeStart->setCalendarPopup(true);
        m_timeStart->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_timeStart->setMinimumWidth(kTimeEditMinWidth);
        row->addWidget(m_timeStart, 1);
        row->addWidget(new QLabel(tr("End:"), a.body));
        m_timeEnd = new QDateTimeEdit(a.body);
        m_timeEnd->setObjectName(QStringLiteral("timeEnd"));
        m_timeEnd->setDisplayFormat(fmt);
        m_timeEnd->setCalendarPopup(true);
        m_timeEnd->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_timeEnd->setMinimumWidth(kTimeEditMinWidth);
        row->addWidget(m_timeEnd, 1);
        a.bodyLayout->addLayout(row);
        root->addWidget(a.box);

        // Seed on the way ON as well as from refreshDiscoveredLists(): a log that is
        // still scanning when the axis is switched on has a span now that it did not
        // have at open time, and the alternative is the year-2000 default that hides
        // every record. Seeding first means the emitChange() below resolves the bounds
        // the user is about to see rather than the ones being replaced.
        connect(m_timeGroup, &QGroupBox::toggled, this, [this, emitChange](bool on) {
            if (on)
                refreshObservedSpan();
            emitChange();
        });
        // A bound the user moved is theirs; the seed must not take it back on the next
        // repopulation. m_populating covers every programmatic write, including
        // refreshObservedSpan()'s own.
        auto timeEdited = [this, emitChange](const QDateTime &) {
            if (!m_populating)
                m_timeUserEdited = true;
            emitChange();
        };
        connect(m_timeStart, &QDateTimeEdit::dateTimeChanged, this, timeEdited);
        connect(m_timeEnd, &QDateTimeEdit::dateTimeChanged, this, timeEdited);
    }

    root->addStretch(1);
    updateTextValidity();
}

void AxisEditor::buildValueAxis(QVBoxLayout *root, ValueAxis axis, const QString &title,
                                const QString &prefix, int listMinHeight,
                                bool enabledByDefault)
{
    const bool subsystem = axis == ValueAxis::Subsystem;
    AxisBox a = makeAxisBox(this, title, prefix + QStringLiteral("Group"), enabledByDefault);
    (subsystem ? m_loggerGroup : m_threadGroup) = a.box;
    (subsystem ? m_loggerBody : m_threadBody) = a.body;

    // The narrow field is ALSO the add field. It used to be one of two look-alike line
    // edits — "Narrow list..." above the list and "Add subsystem manually..." below it
    // with its own button — which spent a row and a button per axis on telling them
    // apart, and put the rarer of the two in the more prominent place.
    //
    // The "+" appears while the typed text is not ALREADY a name in the list, which is
    // most of the time during a narrowing and all of the time once the narrowing has
    // come up empty. Not "only when the list is empty", tempting as that is: a parent
    // logger is a strict prefix of its children, so "com.acme" would be unaddable for
    // exactly as long as "com.acme.db.Pool" was listed.
    auto *narrowRow = new QHBoxLayout;
    auto *narrow = new QLineEdit(a.body);
    narrow->setObjectName(prefix + QStringLiteral("Narrow"));
    narrow->setPlaceholderText(tr("Narrow the list, or type a name to add..."));
    ensureReadablePlaceholder(narrow);
    narrow->setClearButtonEnabled(true);
    auto *add = new QToolButton(a.body);
    add->setObjectName(prefix + QStringLiteral("Add"));
    add->setText(QStringLiteral("+")); // a glyph, not prose — never translated
    // Framed, like the list buttons and for the same reason: it is a command, not a
    // toggle, so there is no checked state to supply the affordance. It appears only
    // when the typed text names nothing, which makes it easy to miss if it also looks
    // like punctuation.
    add->setVisible(false);
    add->setToolTip(subsystem
                        ? tr("Add this subsystem to the list, ticked. Use it for a "
                             "subsystem the scan has not reached yet.")
                        : tr("Add this thread to the list, ticked. Use it for a thread "
                             "the scan has not reached yet."));
    narrowRow->addWidget(narrow, 1);
    narrowRow->addWidget(add);
    a.bodyLayout->addLayout(narrowRow);
    (subsystem ? m_loggerNarrow : m_threadNarrow) = narrow;

    auto *list = new QListWidget(a.body);
    list->setObjectName(prefix + QStringLiteral("List"));
    list->setMinimumHeight(listMinHeight);
    (subsystem ? m_loggerList : m_threadList) = list;

    QAbstractButton *all = nullptr, *none = nullptr, *invert = nullptr;
    QCheckBox *newValues = nullptr;
    auto *listRow = new QHBoxLayout;
    listRow->addWidget(list, 1);
    listRow->addLayout(makeListButtons(
        a.body, prefix, all, none, invert, newValues,
        subsystem ? tr("Tick a subsystem the scan turns up later. Untick and the list "
                       "is a restriction: only what is ticked here is ever shown.")
                  : tr("Tick a thread the scan turns up later. Untick and the list is a "
                       "restriction: only what is ticked here is ever shown.")));
    a.bodyLayout->addLayout(listRow);
    QAbstractButton **slot = subsystem ? m_loggerListButtons : m_threadListButtons;
    slot[0] = all;
    slot[1] = none;
    slot[2] = invert;
    (subsystem ? m_loggerNewValues : m_threadNewValues) = newValues;

    root->addWidget(a.box);

    connect(a.box, &QGroupBox::toggled, this, [this] { emitChanged(); });
    // A tick is a statement about ONE value and nothing else. It used to also return
    // the axis to the discovery default, on the reasoning that whatever the user is
    // building now is a statement about the file — which was defensible while the
    // discovery rule was invisible and showOnlyValue() was the only way into it, and
    // is not now that "New" is a control sitting right there. A checkbox that unticks
    // itself when the user ticks something else is worse than no checkbox.
    connect(list, &QListWidget::itemChanged, this,
            [this](QListWidgetItem *) { emitChanged(); });
    // All / None / Invert carry the same answer to the values that have not turned up
    // yet: "everything" and "nothing" are claims about the axis, not about the six
    // names that happen to be listed a third of the way through a scan.
    connect(all, &QAbstractButton::clicked, this, [this, axis] {
        setAllChecked(listFor(axis), true);
        setRestrictiveFor(axis, false);
    });
    connect(none, &QAbstractButton::clicked, this, [this, axis] {
        setAllChecked(listFor(axis), false);
        setRestrictiveFor(axis, true);
    });
    connect(invert, &QAbstractButton::clicked, this, [this, axis] {
        invertChecked(listFor(axis));
        setRestrictiveFor(axis, !restrictiveFor(axis));
    });
    connect(newValues, &QCheckBox::toggled, this, [this] { emitChanged(); });

    // Adding is only offered for a name the list does not already hold — otherwise the
    // "+" would promise a second copy of a value that is right there under it.
    auto addTyped = [this, axis, narrow, add] {
        const QString name = narrow->text().trimmed();
        if (!canAddTyped(axis, name))
            return;
        // The name goes in ticked whatever "New" says — populateList treats a manual
        // entry as the user asking to see it, which is exactly what typing it in is —
        // so the discovery rule is left alone here. Adding one name says nothing about
        // the ones the scan has not reached.
        manualFor(axis).insert(name);
        {
            // Blocked so the clear does not re-narrow a list that is about to be
            // rebuilt; refreshDiscoveredLists() re-narrows with the now-empty text.
            const QSignalBlocker block(narrow);
            narrow->clear();
        }
        add->setVisible(false);
        refreshDiscoveredLists(); // re-inserts the manual name, checked, and un-narrows
        emitChanged();
    };
    connect(add, &QAbstractButton::clicked, this, addTyped);
    connect(narrow, &QLineEdit::returnPressed, this, addTyped);
    connect(narrow, &QLineEdit::textChanged, this, [this, axis, add](const QString &s) {
        narrowList(listFor(axis), s);
        add->setVisible(canAddTyped(axis, s.trimmed()));
        updateListButtonHints(axis);
    });
}

bool AxisEditor::canAddTyped(ValueAxis axis, const QString &name) const
{
    if (name.isEmpty())
        return false;
    const QListWidget *list = listFor(axis);
    if (!list)
        return false;
    // Exact, case-sensitive: a logger name is an identifier, and "Net.Http" alongside
    // "net.http" is two subsystems, not a typo to be swallowed.
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text() == name)
            return false;
    return true;
}

void AxisEditor::updateListButtonHints(ValueAxis axis)
{
    const QListWidget *list = listFor(axis);
    QAbstractButton **btns =
        axis == ValueAxis::Subsystem ? m_loggerListButtons : m_threadListButtons;
    if (!list || !btns[0])
        return;

    // All / None / Invert act on the NARROWED view, deliberately — there the user can
    // see what they are acting on, which is the same reasoning showOnlyValue() states
    // for going the other way. The trap is that a hidden entry keeps its tick, so
    // "None" over a narrowed list can leave the axis still letting records through
    // while the list on screen reads as fully cleared. Nothing said so; now the
    // buttons do, and only when there is something to say.
    int hidden = 0;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->isHidden())
            ++hidden;
    QString note;
    if (hidden == 1)
        note = tr(" One entry hidden by the text above keeps its current state.");
    else if (hidden > 1)
        note = tr(" %1 entries hidden by the text above keep their current state.")
                   .arg(hidden);

    btns[0]->setToolTip(tr("Tick every entry shown.") + note);
    btns[1]->setToolTip(tr("Untick every entry shown.") + note);
    btns[2]->setToolTip(tr("Tick what is unticked and untick what is ticked.") + note);
}

void AxisEditor::clearAll()
{
    // Back to the state a freshly-bound editor is in — the DEFAULTS it was built with,
    // not "everything off": for the Filters pane that means priority and subsystem
    // still ticked but excluding nothing, which is what the pane has always meant by
    // unfiltered and what applyToDocument()'s NoOpAxes::Collapse then writes as
    // inactive. Turning them off instead would make "clear" leave the pane in a state
    // no fresh document ever starts in.
    m_populating = true;
    m_priorityEnable->setChecked(m_defaults.priorityOn);
    m_priorityCombo->setCurrentIndex(0); // the widest level; PriorityChoice is ordered

    m_loggerGroup->setChecked(m_defaults.loggerOn);
    m_threadGroup->setChecked(false);
    m_timeGroup->setChecked(false);

    m_textGroup->setChecked(false);
    m_textEdit->clear();
    m_textRegex->setChecked(false);
    m_textCase->setChecked(false);
    m_textNegate->setChecked(false);

    if (m_loggerNarrow)
        m_loggerNarrow->clear();
    if (m_threadNarrow)
        m_threadNarrow->clear();
    m_loggerManualNames.clear();
    m_threadManualNames.clear();
    setRestrictiveFor(ValueAxis::Subsystem, false);
    setRestrictiveFor(ValueAxis::Thread, false);
    m_timeUserEdited = false;
    // Every value ticked again. "Seen" is what makes an already-listed name keep its
    // state across a repopulation (populateList's discovery rule), so clearing it is
    // exactly how a value the user unticked comes back — and the only way, short of
    // walking the lists by hand.
    m_loggerSeen.clear();
    m_threadSeen.clear();
    m_populating = false;

    repopulate({}, {}, /*exact=*/false); // nothing seen, nothing restrictive: all ticked
    refreshObservedSpan();
    updateCollapse();
    updateTextValidity();
    // One notification for the whole reset, as restoreState() does for its own.
    emitChanged();
}

void AxisEditor::emitChanged()
{
    if (m_populating)
        return;
    updateCollapse();
    updateTextValidity();
    emit changed();
}

void AxisEditor::addTextExtra(QWidget *w)
{
    if (!w || !m_textOptionsRow)
        return;
    m_textOptionsRow->addWidget(w);
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
    // Priority's "title row" is its checkbox and its body is the combo beside it; the
    // other four are checkable group boxes, whose title row is the control itself.
    struct Pair { bool on; QWidget *body; };
    const Pair pairs[] = {
        {m_priorityEnable && m_priorityEnable->isChecked(), m_priorityBody},
        {m_loggerGroup && m_loggerGroup->isChecked(), m_loggerBody},
        {m_threadGroup && m_threadGroup->isChecked(), m_threadBody},
        {m_textGroup && m_textGroup->isChecked(), m_textBody},
        {m_timeGroup && m_timeGroup->isChecked(), m_timeBody},
    };
    for (const Pair &p : pairs) {
        if (p.body)
            p.body->setVisible(!m_collapsible || p.on);
    }

    // The four group-box axes get this from Qt, which greys a checkable box's contents
    // while it is unchecked. Priority has no group box — it is a checkbox and a combo
    // on one row — so nothing was doing it, and the combo stayed bright and spinnable
    // with the axis off: exactly the "changing the minimum level did nothing" failure
    // the enabled-by-default choice above was made to avoid, merely moved one click away.
    if (m_priorityCombo && m_priorityEnable)
        m_priorityCombo->setEnabled(m_priorityEnable->isChecked());
}

void AxisEditor::updateTextValidity()
{
    if (!m_textEdit || !m_textRegex)
        return;
    // A regex that fails to compile matches NOTHING (TextMatcher::matches returns
    // false rather than throwing), which as a filter silently empties the view and as
    // a highlight rule silently colors nothing. Say so instead of leaving the user to
    // infer it — the Find bar already does.
    const bool bad = !textPatternValid();

    // Start from the field's OWN palette, not a default-constructed one: this runs on
    // every keystroke, and a fresh QPalette would drop the readable-placeholder repair
    // applied below at construction — the invalid-regex cue would silently undo it.
    QPalette pal = m_textEdit->palette();
    pal.setColor(QPalette::Text,
                 bad ? errorColor(pal) : qApp->palette(m_textEdit).color(QPalette::Text));
    m_textEdit->setPalette(pal);
    m_textEdit->setToolTip(bad ? tr("Invalid regular expression — matches nothing.")
                               : QString());

    // And say it in words under the field, not only as a colour. The colour is the
    // cue for someone already typing; the line is for someone looking at an empty
    // view and wondering.
    if (m_textError) {
        if (bad) {
            QPalette errPal = m_textError->palette();
            errPal.setColor(QPalette::WindowText, errorColor(errPal));
            m_textError->setPalette(errPal);
        }
        m_textError->setVisible(bad);
    }
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
    // discovers arriving unticked. Guarded, because the discovery rule is a CHECKBOX
    // now and setting it emits toggled(): a rebind is not a user edit, and letting one
    // reach changed() would have the Highlighters pane write the incoming document's
    // defaults into whichever rule happened to be selected.
    m_populating = true;
    setRestrictiveFor(ValueAxis::Subsystem, false);
    setRestrictiveFor(ValueAxis::Thread, false);
    m_populating = false;
    // Also a statement about the previous file: a bound the user set on that log is
    // not a bound they set on this one, so this file's span is free to seed.
    m_timeUserEdited = false;

    const bool hasDoc = document != nullptr;
    const bool hasThread = hasDoc && document->format().threadGroup > 0;
    const bool hasDate = hasDoc && document->format().dateGroup > 0;

    setEnabled(hasDoc);
    // Thread and time axes exist only when the format carries those fields
    // (SPEC.md §6). Disable rather than hide so the layout is stable — and say WHY in
    // the title, because a greyed axis otherwise explains nothing and a preset or a
    // restored session can leave one greyed AND TICKED, at which point resolve()
    // drops it and the pane shows a selection that is not in force.
    //
    // In the title and not in a tooltip: Qt delivers no mouse events to a disabled
    // widget, so a tooltip on one is never seen. Whatever is said here has to be said
    // where it is already visible.
    if (m_threadGroup) {
        m_threadGroup->setEnabled(hasThread);
        m_threadGroup->setTitle(hasThread ? tr("Thread")
                                          : tr("Thread — not in this log's format"));
    }
    if (m_timeGroup) {
        m_timeGroup->setEnabled(hasDate);
        m_timeGroup->setTitle(hasDate ? tr("Time range")
                                      : tr("Time range — no timestamps in this log"));
    }

    refreshDiscoveredLists();

    // The zone the editors' digits are written in, tracked for every bound document
    // and not only for one carrying timestamps: setTimeBound() renders a record's UTC
    // ms through it, and refreshTimeBounds() reads it to recover the instant.
    if (hasDoc)
        m_renderZone = document->displayZone();

    // Seed the time editors to the file's observed span so the pickers open near
    // useful values rather than the epoch. Almost always a no-op HERE: this runs from
    // activeDocumentChanged at open time, before the scan has produced a record. The
    // seed that does the work is the one refreshDiscoveredLists() makes as the index
    // grows; this one only matters when rebinding to a document already scanned.
    refreshObservedSpan();
    updateCollapse();
}

void AxisEditor::refreshObservedSpan()
{
    if (!m_document || !m_timeStart || !m_timeEnd || !supportsTime())
        return;
    if (m_timeUserEdited)
        return;
    // A programmatic write is in progress and owns the bounds. This matters because
    // the group's toggled() handler calls us: setTimeBound() sets its bound, then
    // ticks the axis, and without this the seed would overwrite the bound between
    // those two lines — which is a record-menu time filter that silently does nothing.
    if (m_populating)
        return;
    qint64 lo = 0, hi = 0;
    if (!observedSpan(lo, hi))
        return; // nothing parsed yet — leave the editors alone and try again next time

    // Re-read the zone before rendering, rather than trusting the one captured when
    // the pane bound. m_renderZone means "the zone the digits currently in the editors
    // are written in", and we are about to write both of them — so taking the
    // document's zone now IS keeping that invariant, not bending it.
    //
    // It also has to be re-read: setDocument() runs from activeDocumentChanged, which
    // for a session-restored or not-yet-arrived log (SPEC.md §3, M13/M17) fires before
    // the document has a format or a zone at all. A default-constructed QTimeZone is
    // INVALID, fromMSecsSinceEpoch() through it yields an invalid QDateTime, and
    // QDateTimeEdit::setDateTime() ignores that and stays at its year-2000 minimum —
    // silently reinstating the very bug this function exists to fix.
    m_renderZone = m_document->displayZone();

    // m_populating, so the editors' own dateTimeChanged does not read this back as a
    // hand edit and latch m_timeUserEdited against every later seed.
    m_populating = true;
    m_timeStart->setDateTime(wallClockOf(lo));
    m_timeEnd->setDateTime(wallClockOf(hi));
    m_populating = false;
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
    // The other thing the index tells the editor. Both callers of this function mean
    // "the scan has moved on", and a time axis seeded once at open time is seeded from
    // an empty file.
    refreshObservedSpan();
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
                 exact, restrictiveFor(ValueAxis::Subsystem));
    populateList(m_threadList, threads, threadChecked, m_threadManualNames, m_threadSeen,
                 exact, restrictiveFor(ValueAxis::Thread));
    narrowList(m_loggerList, m_loggerNarrow ? m_loggerNarrow->text() : QString());
    narrowList(m_threadList, m_threadNarrow ? m_threadNarrow->text() : QString());
    // The list just changed under the narrowing, so the hidden count the buttons
    // report has too.
    updateListButtonHints(ValueAxis::Subsystem);
    updateListButtonHints(ValueAxis::Thread);
}

// ---------------------------------------------------------------------------
// Criteria in and out
// ---------------------------------------------------------------------------

MatchCriteria AxisEditor::criteria() const
{
    MatchCriteria c;

    c.priorityEnabled = m_priorityEnable->isChecked();
    c.minPriority = PriorityChoice::at(m_priorityCombo->currentIndex());

    c.loggerEnabled = m_loggerGroup->isChecked();
    c.loggerNames = toSortedList(checkedNames(m_loggerList));
    // Coverage is answered from the list the user was shown, never from the intern
    // table: the table grows mid-scan and the list lags it, so asking the table would
    // make a discovered-but-not-yet-listed subsystem look excluded.
    c.loggerCoversAll = allChecked(m_loggerList);
    c.loggerRestrictive = restrictiveFor(ValueAxis::Subsystem);

    c.threadEnabled = m_threadGroup->isChecked();
    c.threadNames = toSortedList(checkedNames(m_threadList));
    c.threadCoversAll = allChecked(m_threadList);
    c.threadRestrictive = restrictiveFor(ValueAxis::Thread);

    c.text.enabled = m_textGroup->isChecked();
    c.text.negate = m_textNegate->isChecked();
    c.text.matcher.set(m_textEdit->text(), m_textRegex->isChecked(),
                       m_textCase->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive);

    c.timeEnabled = m_timeGroup->isChecked();
    c.start = m_timeStart->dateTime();
    c.end = m_timeEnd->dateTime();

    return c;
}

void AxisEditor::setCriteria(const MatchCriteria &c)
{
    m_populating = true;

    m_priorityEnable->setChecked(c.priorityEnabled);
    m_priorityCombo->setCurrentIndex(PriorityChoice::indexOf(c.minPriority));

    m_loggerGroup->setChecked(c.loggerEnabled);
    m_threadGroup->setChecked(c.threadEnabled);

    m_textGroup->setChecked(c.text.enabled);
    m_textEdit->setText(c.text.matcher.pattern());
    m_textRegex->setChecked(c.text.matcher.isRegex());
    m_textCase->setChecked(c.text.matcher.caseSensitivity() == Qt::CaseSensitive);
    m_textNegate->setChecked(c.text.negate);

    m_timeGroup->setChecked(c.timeEnabled);
    if (c.start.isValid())
        m_timeStart->setDateTime(c.start);
    if (c.end.isValid())
        m_timeEnd->setDateTime(c.end);
    // A stored range with the axis ON is a bound somebody chose, and the seed must not
    // take it back the moment the scan reports a wider span. With the axis off the
    // bounds are whatever the editors happened to hold, so leave them seedable.
    m_timeUserEdited = c.timeEnabled;

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
    setRestrictiveFor(ValueAxis::Subsystem, c.loggerRestrictive);
    setRestrictiveFor(ValueAxis::Thread, c.threadRestrictive);

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

QGroupBox *AxisEditor::enableFor(ValueAxis axis) const
{
    return axis == ValueAxis::Subsystem ? m_loggerGroup : m_threadGroup;
}

QSet<QString> &AxisEditor::manualFor(ValueAxis axis)
{
    return axis == ValueAxis::Subsystem ? m_loggerManualNames : m_threadManualNames;
}

QCheckBox *AxisEditor::newValuesBoxFor(ValueAxis axis) const
{
    return axis == ValueAxis::Subsystem ? m_loggerNewValues : m_threadNewValues;
}

bool AxisEditor::restrictiveFor(ValueAxis axis) const
{
    const QCheckBox *box = newValuesBoxFor(axis);
    return box && !box->isChecked();
}

void AxisEditor::setRestrictiveFor(ValueAxis axis, bool restrictive)
{
    if (QCheckBox *box = newValuesBoxFor(axis))
        box->setChecked(!restrictive); // toggled() -> emitChanged(), unless populating
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
    if (QGroupBox *enable = enableFor(axis))
        enable->setChecked(true);
    setRestrictiveFor(axis, true);
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
    if (QGroupBox *enable = enableFor(axis))
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
    if (!m_timeGroup || !m_timeStart || !m_timeEnd || !supportsTime())
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

    const bool wasEnabled = m_timeGroup->isChecked();
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
    // Before setChecked(), not after: ticking the group emits toggled(), which seeds
    // the span unless a bound is already spoken for.
    m_timeUserEdited = true; // pointed at a record, so as deliberate as typing it
    m_timeGroup->setChecked(true);
    m_populating = false;

    emitChanged();
}

void AxisEditor::setTimeRange(qint64 fromUtcMs, qint64 toUtcMs)
{
    if (!m_timeGroup || !m_timeStart || !m_timeEnd || !supportsTime())
        return;
    if (fromUtcMs > toUtcMs)
        std::swap(fromUtcMs, toUtcMs);
    m_populating = true;
    m_timeStart->setDateTime(wallClockOf(fromUtcMs));
    m_timeEnd->setDateTime(wallClockOf(toUtcMs));
    m_timeUserEdited = true; // before setChecked(), as in setTimeBound()
    m_timeGroup->setChecked(true);
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
