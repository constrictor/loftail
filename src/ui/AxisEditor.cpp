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

#include "AxisEditor.h"

#include "UiColors.h"


#include "Document.h"
#include "Filter.h"
#include "LogFormat.h"
#include "Priority.h"
#include "RecordIndex.h"
#include "SectionBox.h"
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDoubleSpinBox>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
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
// rather than loose rows so that greying, and the axis's own geometry, are one thing
// each — it is what setCollapsible() used to hide in a single call, back when an axis
// could collapse to its title row at all.
struct AxisBox
{
    SectionBox  *box;
    QWidget     *body;
    QVBoxLayout *bodyLayout;
};

AxisBox makeAxisBox(QWidget *parent, const QString &title, const QString &objectName,
                    bool enabledByDefault)
{
    AxisBox a;
    // A SectionBox: a QGroupBox that draws a hairline along its own title row instead of a
    // frame round its body. Nothing else about the box changes, which is why every axis
    // enable control is still a QGroupBox * to its callers.
    //
    // Unconditional, in both panes. It began as a per-pane answer — the Highlighters
    // pane's axes sit inside a framed "Condition" box, and a framed axis in there is a
    // border inside a border, three deep by the subsystem list — but the Filters pane
    // wanted the same look for a reason that turns out to be the same one: an axis is a
    // section of a pane, and a pane is a frame already. Five framed panels stacked inside
    // one dock is five borders saying "this belongs together" about things nothing else
    // groups. So there is no flag; a divider per axis is what an axis looks like.
    a.box = new SectionBox(title, parent);
    a.box->setObjectName(objectName);
    a.box->setCheckable(true);
    a.box->setChecked(enabledByDefault);
    a.box->setFlat(true);
    a.box->setTitleDivider(true);
    auto *v = new QVBoxLayout(a.box);
    // Tighter than the style default all round: five stacked group boxes pay these
    // margins five times, and the top one twice over — the title row already sits
    // above the frame.
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
// NOT auto-raised: a flat frameless button with a one-word label and no icon reads as
// a caption, and "All / None / Invert" in a column beside a list reads as a caption
// particularly well. The frame is what says these are things to press.
//
// That used to be stated as a distinction from the message-text toggles, where the
// checked state was held to supply the affordance a frame otherwise gives. It does not
// — it says which of them are ON to someone who has already worked out that they are
// buttons — so nothing in this editor is auto-raised now.
//
// The column used to end in a "New" checkbox carrying the discovery rule — what
// happens to a value the list does not hold YET. It is the first ROW of the list now
// (see buildValueAxis): the rule is a tick against "everything else", which is what
// the rest of the list is a set of ticks against, and asking it in the same column as
// All / None / Invert made it read as a fourth button rather than as an entry the
// three of them set.
QVBoxLayout *makeListButtons(QWidget *parent, const QString &namePrefix,
                             QAbstractButton *&all, QAbstractButton *&none,
                             QAbstractButton *&invert)
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
    // Not flush. An axis now draws a hairline rather than a frame, and a line that runs
    // into the dock's own edge reads as a rendering seam rather than as a division inside
    // the pane; before that it was a rounded frame 0 px from the edge, whose corners
    // curved away from a border that was not there. Both users put this widget in a
    // QScrollArea with no frame of its own, so these few pixels are the only gap there is
    // — and the Highlighters pane insets its action rows by the same kSideMargin, which is
    // what puts all nine title rows in one column.
    root->setContentsMargins(kSideMargin, 4, kSideMargin, 4);

    auto emitChange = [this] { emitChanged(); };

    // The order is how often an axis is reached for, not how the FilterSet declares
    // them. Priority, Subsystem, Message text, Thread, Time range: the level floor and
    // the subsystem list are what a log is narrowed with before anything is searched
    // for in it — you pick the stream, then you search it — so the two that answer
    // "which records am I reading" come first and the free-text search sits under them.
    // Message text keeps the context row the Filters pane injects (addTextExtra()),
    // which belongs next to the search it widens rather than at the foot of the pane.
    //
    // Subsystem and Thread are the two that GROW (see buildValueAxis's stretch): they
    // are lists of unknown length, and everything else here is a fixed number of rows.

    // --- Priority -----------------------------------------------------------
    {
        // A checkable group box like the other four, although this axis is one combo
        // and needs no frame to hold it together. It was a bare checkbox-plus-combo
        // row, and that was the mistake: an axis is a section, and the one axis laid
        // out unlike the rest read as a stray setting above the matchers rather than
        // as the first of them — while its title row was the only enable control in
        // the pane that did not look like the others.
        //
        // Two things stop being special cases with it. Every enable control is now a
        // QGroupBox *, so there is no exception for a caller to remember; and Qt greys
        // a checkable box's body, which is what kept the combo from staying live with
        // the axis off — a hand-written setEnabled() used to do that, and it had to,
        // because "changing the minimum level did nothing" is exactly what the
        // enabled-by-default choice below exists to avoid.
        //
        // Opt-in in both panes now — see Defaults for why it stopped shipping on here.
        AxisBox a = makeAxisBox(this, tr("Minimum priority"),
                                QStringLiteral("priorityGroup"), defaults.priorityOn);
        m_priorityEnable = a.box;
        m_priorityCombo = new QComboBox(a.body);
        m_priorityCombo->setObjectName(QStringLiteral("priorityCombo"));
        // TRACE is left out. It is the lowest level the enum has, so "minimum priority
        // is TRACE" excludes nothing — the one row in this combo that could not answer
        // the question the axis asks, sitting where a list is read from first. The
        // no-op it named is still reachable, and by the control that means it: unticking
        // the axis. Every OTHER level is offered, so nothing a user could express before
        // has become inexpressible.
        //
        // Each item carries its Priority as data, and the item ORDER is not the
        // PriorityChoice order any more (see comboPriority() in the header — that table
        // is the on-disk format and cannot lose a row). Nothing may index this combo by
        // a PriorityChoice index, and nothing does.
        for (int i = 0; i < PriorityChoice::count(); ++i) {
            const Priority p = PriorityChoice::at(i);
            if (p == Priority::Trace)
                continue;
            m_priorityCombo->addItem(priorityName(p), int(p));
        }
        setComboPriority(kDefaultFloor);
        a.bodyLayout->addWidget(m_priorityCombo);
        root->addWidget(a.box);

        connect(m_priorityEnable, &QGroupBox::toggled, this, emitChange);
        connect(m_priorityCombo, &QComboBox::currentIndexChanged, this,
                [emitChange](int) { emitChange(); });
    }

    // --- Subsystem ----------------------------------------------------------
    //
    // buildValueAxis(), which the Thread axis below calls too. The two differ in a
    // title, an object-name prefix, a list height and whether they ship on; everything
    // else — the discovery rule, the narrowing, the manual add, the three list buttons
    // — was written out twice and had to be kept in step by hand.
    //
    // The two are no longer adjacent: the message search sits between them (see the
    // ordering note above). They still expand together, which is a property of the
    // stretch each is given in `root` and not of their being neighbours.
    buildValueAxis(root, ValueAxis::Subsystem, tr("Subsystem"), QStringLiteral("subsystem"),
                   /*listMinHeight=*/90, defaults.loggerOn);

    // --- Message text -------------------------------------------------------
    {
        AxisBox a = makeAxisBox(this, tr("Message text"), QStringLiteral("messageGroup"),
                                false);
        m_textGroup = a.box;
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
        //
        // FRAMED, like every other button in this editor. They were auto-raised, on the
        // reasoning that a toggle's checked state supplies the affordance a frame
        // otherwise gives — which is true only once you know there is something there to
        // press. Three glyphs floating under a text field read as a legend for the field,
        // and the two bits and a sign they carry are the difference between a filter that
        // works and one that silently matches nothing. So the rule the list buttons state
        // now holds for the whole editor: the frame is what says "this is a thing to
        // press", and the sunken checked state is what says which ones are pressed.
        auto makeToggle = [&a](const QString &glyph, const QString &name,
                               const QString &prose) {
            auto *b = new QToolButton(a.body);
            b->setText(glyph);
            b->setObjectName(name);
            b->setCheckable(true);
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

    // --- Thread -------------------------------------------------------------
    buildValueAxis(root, ValueAxis::Thread, tr("Thread"), QStringLiteral("thread"),
                   /*listMinHeight=*/70, false);

    // --- Time range ---------------------------------------------------------
    {
        AxisBox a = makeAxisBox(this, tr("Time range"), "timeGroup", false);
        m_timeGroup = a.box;
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
        // A bound is entered in the SAME terms the timestamp column is showing
        // (SPEC.md §6): a date for the three wall-clock display modes, a number of
        // seconds for the two that render the column as seconds. Reading "12.480" off
        // a row and then being asked for a calendar date is the pane asking a question
        // about a quantity the log never showed — and answering it means converting by
        // hand, from a baseline the pane knows and the user has to guess.
        //
        // Both spellings are built here and one pair hidden, rather than a widget
        // being replaced when the mode changes: the display mode is a menu item away,
        // and rebuilding a row under the pointer loses focus, tab order and whatever
        // was half-typed. syncTimeEditorKind() decides which pair is up.
        const auto addBound = [&](TimeBound which) {
            const bool start = which == TimeBound::Start;
            row->addWidget(new QLabel(start ? tr("Start:") : tr("End:"), a.body));
            auto *edit = new QDateTimeEdit(a.body);
            edit->setObjectName(start ? QStringLiteral("timeStart") : QStringLiteral("timeEnd"));
            edit->setDisplayFormat(fmt);
            edit->setCalendarPopup(true);
            edit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            edit->setMinimumWidth(kTimeEditMinWidth);
            row->addWidget(edit, 1);

            auto *spin = new QDoubleSpinBox(a.body);
            spin->setObjectName(start ? QStringLiteral("timeStartSeconds")
                                      : QStringLiteral("timeEndSeconds"));
            // Wide enough for epoch seconds (~1.8e9 now, 2.5e11 at year 9999) and
            // signed, because seconds-from-run-start goes negative for a record
            // back-dated before its own run's first line — exactly as the column's own
            // formatSeconds() allows. Keyboard tracking off: a partly-typed number is
            // not a bound, and re-filtering the whole file on every digit turns typing
            // "1500" into four applies, three of them over a range nobody asked for.
            spin->setRange(-1e12, 1e12);
            spin->setDecimals(0);
            spin->setKeyboardTracking(false);
            // The C locale, so the editor writes the number the COLUMN writes: the
            // column formats seconds by hand (LogModel::formatSeconds) and is not
            // localized, so a spin box showing "1784635200,000" beside a row reading
            // "1784635200.000" would be the same value spelled two ways in one window.
            spin->setLocale(QLocale::c());
            // No up/down buttons. Epoch seconds run to ten digits before the point and
            // the row holds two of these plus their labels, so ~16 px of arrows per
            // editor is what decides whether the last digit is on screen — and stepping
            // a ten-digit second one at a time is not how anyone uses this. The keyboard
            // arrows still step; only the painted buttons are gone.
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            spin->setMinimumWidth(kTimeEditMinWidth);
            row->addWidget(spin, 1);

            if (start) {
                m_timeStart = edit;
                m_secStart = spin;
            } else {
                m_timeEnd = edit;
                m_secEnd = spin;
            }
        };
        addBound(TimeBound::Start);
        addBound(TimeBound::End);
        syncTimeEditorKind(); // no document yet: starts on the wall-clock pair
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
        // A number typed into the seconds pair is the same edit to the same bound, and
        // it has to reach the DATE pair: those are what criteria() reads, because a
        // count of seconds is not portable enough to store (see criteria()). The two
        // editors are two spellings of one bound and only the visible one is ever
        // edited by hand, so the hand edit writes the other spelling itself — under
        // m_populating, or the mirrored write would come back as a second edit.
        const auto secondsEdited = [this, emitChange](TimeBound which) {
            if (m_populating)
                return; // setBoundInstant() wrote both spellings; nothing to mirror
            const QDoubleSpinBox *spin = which == TimeBound::Start ? m_secStart : m_secEnd;
            QDateTimeEdit *edit = which == TimeBound::Start ? m_timeStart : m_timeEnd;
            m_populating = true;
            edit->setDateTime(wallClockOf(instantOfSeconds(spin->value())));
            m_populating = false;
            m_timeUserEdited = true;
            emitChange();
        };
        connect(m_secStart, &QDoubleSpinBox::valueChanged, this,
                [secondsEdited](double) { secondsEdited(TimeBound::Start); });
        connect(m_secEnd, &QDoubleSpinBox::valueChanged, this,
                [secondsEdited](double) { secondsEdited(TimeBound::End); });
    }

    // No trailing addStretch(). There used to be one, and it is exactly what kept the
    // value lists at their minimum height: a stretch at the foot of the layout claims
    // every spare pixel the pane has, so the lists could only ever be as tall as their
    // floors while an empty gap grew under Time range. The spare height belongs to the
    // two lists now (the stretch given to each value axis above), and a stretch left
    // here would compete with them for it.
    updateTextValidity();
}

void AxisEditor::buildValueAxis(QVBoxLayout *root, ValueAxis axis, const QString &title,
                                const QString &prefix, int listMinHeight,
                                bool enabledByDefault)
{
    const bool subsystem = axis == ValueAxis::Subsystem;
    AxisBox a = makeAxisBox(this, title, prefix + QStringLiteral("Group"), enabledByDefault);
    (subsystem ? m_loggerGroup : m_threadGroup) = a.box;

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
    // "Only this one" as a chord, because it is what a list of ticks is most often
    // wanted for and the alternatives are None-then-tick or a trip to the record menu.
    // On the VIEWPORT, which is where a view's mouse events arrive; on the list itself
    // the filter would see nothing but the scroll bars. The hint is the list's own
    // tooltip: the value rows carry none of their own, so it is what shows over them.
    list->setToolTip(subsystem
                         ? tr("Ctrl+click a subsystem to show only that one.")
                         : tr("Ctrl+click a thread to show only that one."));
    list->viewport()->installEventFilter(this);
    (subsystem ? m_loggerList : m_threadList) = list;

    // Row 0 is the discovery rule, and it is a row rather than a checkbox beside the
    // list because that is the shape of the question. The list is a set of ticks
    // against the values the scan has found; "what happens to the ones it has not
    // found yet" is one more tick against the rest of them, and a list of five
    // subsystems and *the others* is a complete answer where five subsystems on their
    // own is not. On top rather than at the bottom because the bottom of this list is
    // wherever the scan has got to — a row appended after the names would move on
    // every repopulation and be scrolled past on a log with forty loggers.
    //
    // Created before the connects below, so its arrival cannot reach changed().
    auto *others = new QListWidgetItem(list);
    others->setText(tr("Others"));
    others->setFlags(others->flags() | Qt::ItemIsUserCheckable);
    others->setCheckState(Qt::Checked); // discovery is the default (populateList)
    others->setData(kOthersRole, true); // what every value loop skips it by
    // Italic, because a value list is a list of names and this is not one of them —
    // and because a log may genuinely have a subsystem called "Others".
    QFont othersFont = list->font();
    othersFont.setItalic(true);
    others->setFont(othersFont);
    others->setToolTip(
        subsystem ? tr("Subsystems the scan has not turned up yet. Ticked, a new "
                       "subsystem arrives ticked; unticked, the list is a restriction "
                       "and only what is ticked here is ever shown.")
                  : tr("Threads the scan has not turned up yet. Ticked, a new thread "
                       "arrives ticked; unticked, the list is a restriction and only "
                       "what is ticked here is ever shown."));
    (subsystem ? m_loggerOthers : m_threadOthers) = others;

    QAbstractButton *all = nullptr;
    QAbstractButton *none = nullptr;
    QAbstractButton *invert = nullptr;
    auto *listRow = new QHBoxLayout;
    listRow->addWidget(list, 1);
    listRow->addLayout(makeListButtons(a.body, prefix, all, none, invert));
    // The list row is the one part of a value axis that GROWS. setMinimumHeight above is
    // a floor and not a size: a value list is as long as the log has subsystems, which is
    // unknown when the pane is built and changes while it scans, so the height that fits
    // it cannot be a constant. Everything else in this axis — the title row, the
    // narrowing field — is a fixed number of rows, so the stretch here is what carries
    // the pane's spare height all the way down to the list. It is inert on its own; it
    // only does anything because the box itself is given a stretch in `root` below.
    a.bodyLayout->addLayout(listRow, 1);
    QAbstractButton **slot = subsystem ? m_loggerListButtons : m_threadListButtons;
    slot[0] = all;
    slot[1] = none;
    slot[2] = invert;

    // Stretch 1, which the other three axes do not get: this is what makes the two
    // value lists take the pane's spare height instead of leaving it at the bottom.
    // Both value axes get the SAME factor, so spare height splits evenly between them
    // rather than by how many values each happens to have found — a split that would
    // otherwise shift under the reader mid-scan, every time a new subsystem turned up.
    // Their differing setMinimumHeight floors (90 and 70) still hold underneath, so a
    // pane too short to satisfy both scrolls rather than starving either.
    //
    // A switched-off axis keeps its share. That follows from bodies staying visible
    // while an axis is off, which is deliberate and documented above; an axis that gave
    // up its height when unticked would move every other axis on the pane each time one
    // was toggled, which is a worse thing to do to a reader than showing a greyed list.
    root->addWidget(a.box, 1);

    connect(a.box, &QGroupBox::toggled, this, [this] { emitChanged(); });
    // One handler for the whole list, "Others" included: a tick is a tick whichever
    // row it lands on, and the two kinds of row differ only in what criteria() reads
    // them as. A tick is also a statement about ONE row and nothing else — it used to
    // return the axis to the discovery default as well, on the reasoning that whatever
    // the user is building now is a statement about the file, which was defensible
    // while the rule was invisible and showOnlyValue() was the only way into it. It is
    // not now that the rule is the row directly above: a row that unticks itself when
    // the user ticks another one is worse than no row at all.
    connect(list, &QListWidget::itemChanged, this, [this, axis](QListWidgetItem *item) {
        // The memo behind the discovery rule follows the ROW, and it has to be written
        // here rather than at the next repopulation: what a name is remembered as is
        // read only once the row is gone, and the only thing that takes a row away is a
        // rotation replacing the index — by which time there is nothing left to read it
        // off. Every route that moves a tick passes through here (a click, All/None/
        // Invert, the record menu, setCriteria), which is why it is one line and not a
        // rule repeated per caller. "Others" is a rule and not a name, so it is skipped
        // exactly as checkedNames() skips it.
        // Not while POPULATING: populateList() maintains the memo itself and its rules
        // are not all "record what the row says" — ListRule::Unstated deliberately
        // UN-sees a name it leaves unticked, and it builds those rows through
        // setCheckState(), so writing here would put every one of them straight back
        // and leave the axis that ships switched off empty for good.
        if (!m_populating && item && !isOthersRow(item))
            seenFor(axis).insert(item->text(), item->checkState() == Qt::Checked);
        emitChanged();
    });
    // All / None / Invert carry the same answer to the values that have not turned up
    // yet: "everything" and "nothing" are claims about the axis, not about the six
    // names that happen to be listed a third of the way through a scan. They set the
    // "Others" row EXPLICITLY rather than by sweeping the whole list, because Invert
    // reads the rule back after flipping the names — a sweep would flip it twice.
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

    // Adding is only offered for a name the list does not already hold — otherwise the
    // "+" would promise a second copy of a value that is right there under it.
    auto addTyped = [this, axis, narrow, add] {
        const QString name = narrow->text().trimmed();
        if (!canAddTyped(axis, name))
            return;
        // The name goes in ticked whatever "Others" says — populateList treats a manual
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
    // "net.http" is two subsystems, not a typo to be swallowed. From kFirstValueRow,
    // so a log whose loggers really are called "Others" can still add one.
    for (int i = kFirstValueRow; i < list->count(); ++i)
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
    for (int i = kFirstValueRow; i < list->count(); ++i)
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
    // not "everything off": for the Filters pane that means subsystem still ticked but
    // excluding nothing, which is what the pane has always meant by unfiltered and what
    // applyToDocument()'s NoOpAxes::Collapse then writes as inactive. Turning it off
    // instead would make "clear" leave the pane in a state no fresh document ever
    // starts in. The combo goes back to kDefaultFloor under an axis that is off, so
    // clear still returns the view to every record — that outcome now comes from the
    // tick rather than from the level, which is the whole of the change (see Defaults).
    m_populating = true;
    m_priorityEnable->setChecked(m_defaults.priorityOn);
    setComboPriority(kDefaultFloor); // never index 0 — the combo's order is its own now

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

    // Nothing seen, nothing restrictive: everything ticked.
    repopulate({}, {}, ListRule::Discover, ListRule::Discover);
    refreshObservedSpan();
    updateAxisState();
    updateTextValidity();
    // One notification for the whole reset, as restoreState() does for its own.
    emitChanged();
}

void AxisEditor::emitChanged()
{
    if (m_populating)
        return;
    updateAxisState();
    updateTextValidity();
    emit changed();
}

void AxisEditor::addTextExtra(QWidget *w)
{
    if (!w || !m_textOptionsRow)
        return;
    m_textOptionsRow->addWidget(w);
}

void AxisEditor::updateAxisState()
{
    // An axis this log's format cannot fill — thread with no %t, time range with no %d —
    // is left out entirely, in BOTH panes. There used to be a per-pane flag
    // (setHidesUnsupportedAxes) so the Filters pane could instead show it greyed with the
    // reason in its title, on the argument that the Filters pane describes the whole log
    // and a missing axis is worth saying. It is not worth a section: the sentence is read
    // once, the space is spent for the session, and five axes already compete for the
    // height of one dock. The flag is gone rather than defaulted, so the two panes cannot
    // drift on a question about which they were never really of two minds.
    //
    // The whole group box goes, not its body: what is being removed IS the title row
    // offering an axis that can never match. And a supported axis is always on screen,
    // ticked or not — an axis that reveals its controls only once it is ticked cannot be
    // read, only explored (the setCollapsible() both panes reversed).
    //
    // Nothing else changes: setDocument() has already disabled the axis whether or not it
    // is visible, so a rule or preset that arrives with it ticked is still dropped by
    // MatchCriteria::resolve() and still cannot be edited into force from here.
    if (m_threadGroup)
        m_threadGroup->setVisible(supportsThread());
    if (m_timeGroup)
        m_timeGroup->setVisible(supportsTime());

    // Greying a switched-off axis's body is Qt's, not ours: a checkable QGroupBox
    // disables its contents while it is unchecked. Priority used to be the exception
    // — a bare checkbox and combo with no box to do it — and carried a hand-written
    // setEnabled() here. It is a group box now, so the exception is gone; do not
    // reintroduce one, because a hand-written enable RE-ENABLES a body Qt has just
    // greyed if the two ever disagree about which way the axis is set.
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

bool AxisEditor::focusIsInAContinuousEditor() const
{
    const QWidget *focus = QApplication::focusWidget();
    if (!focus)
        return false;
    // "Is, or is inside": a QDateTimeEdit and a QDoubleSpinBox both give the focus to an
    // internal QLineEdit, so comparing pointers alone answers false about the very
    // widgets this exists for.
    const auto holds = [focus](const QWidget *w) {
        return w && (w == focus || w->isAncestorOf(focus));
    };
    return holds(m_textEdit) || holds(m_timeStart) || holds(m_timeEnd) || holds(m_secStart)
        || holds(m_secEnd);
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
    // Thread and time axes exist only when the format carries those fields (SPEC.md §6).
    // Disabled here and hidden by updateAxisState() below — two separate things, and the
    // disabling is the one that matters: it stands whether or not the axis is on screen,
    // so a preset or a restored session that arrives with a ticked axis this format
    // cannot fill has it dropped by resolve() and cannot edit it back into force.
    //
    // The titles used to carry the reason ("Thread — not in this log's format") for the
    // pane that showed the axis greyed instead of hiding it. Neither pane does now, so
    // the axis is either usable or absent and there is no greyed row left to explain.
    if (m_threadGroup)
        m_threadGroup->setEnabled(hasThread);
    if (m_timeGroup)
        m_timeGroup->setEnabled(hasDate);

    refreshDiscoveredLists();

    // What the editors' digits are written in, tracked for every bound document and not
    // only for one carrying timestamps: setTimeBound() renders a record's UTC ms through
    // it, and refreshTimeBounds() reads it back to recover the instant. Rebinding is a
    // fresh start, so the terms come from the new document rather than being carried
    // over from the old one — otherwise a seconds log rebound onto a wall-clock one
    // reads its own date digits as a count of seconds.
    if (hasDoc) {
        m_renderZone = document->displayZone();
        m_renderMode = document->timeDisplay();
        m_renderBase = secondsBaseMs();
    }
    syncTimeEditorKind();

    // Seed the time editors to the file's observed span so the pickers open near
    // useful values rather than the epoch. Almost always a no-op HERE: this runs from
    // activeDocumentChanged at open time, before the scan has produced a record. The
    // seed that does the work is the one refreshDiscoveredLists() makes as the index
    // grows; this one only matters when rebinding to a document already scanned.
    refreshObservedSpan();
    updateAxisState();
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
    qint64 lo = 0;
    qint64 hi = 0;
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
    // Same for the mode and its baseline, and for the same reason: the seed writes both
    // spellings of both bounds, so it is the point at which "what the digits are
    // written in" becomes whatever the document currently says.
    m_renderZone = m_document->displayZone();
    m_renderMode = m_document->timeDisplay();
    m_renderBase = secondsBaseMs();

    // m_populating, so the editors' own dateTimeChanged does not read this back as a
    // hand edit and latch m_timeUserEdited against every later seed.
    m_populating = true;
    syncTimeEditorKind();
    setBoundInstant(TimeBound::Start, lo);
    setBoundInstant(TimeBound::End, hi);
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

namespace {
// Which spelling the bound editors take (SPEC.md §6). TimeDisplay::SincePrevious is
// DELIBERATELY not one of them although its column reads as seconds: those seconds are
// an interval between two rows, and a bound is one instant — there is no baseline that
// would turn a typed "2.5" into one. So a gap column keeps the wall-clock editors, and
// the axis goes on naming the instants the records are actually at.
bool rendersSeconds(TimeDisplay mode)
{
    return mode == TimeDisplay::EpochSeconds || mode == TimeDisplay::RunSeconds;
}
} // namespace

bool AxisEditor::secondsMode() const
{
    return m_document && rendersSeconds(m_document->timeDisplay());
}

qint64 AxisEditor::secondsBaseMs() const
{
    if (!m_document || m_document->timeDisplay() != TimeDisplay::RunSeconds)
        return 0; // epoch seconds, and the wall-clock modes, which never ask
    // "Seconds from run start" is per-RECORD in the column — each record counts from
    // its own run — and a filter bound cannot be: FilterSet compares UTC ms, so a
    // bound has to name one instant. The selected run's baseline is the one that makes
    // the pane agree with what is on screen, because a run selection is already what
    // restricts the view (SPEC.md §3a). With "All runs" showing, that is the first
    // run's baseline, which is what the topmost row counts from.
    const QVector<Document::Run> &runs = m_document->runs();
    const int sel = m_document->selectedRun();
    const int firstRecord = (sel >= 0 && sel < runs.size()) ? runs.at(sel).startRecord : 0;
    const qint64 base = m_document->runBaseTimestamp(firstRecord);
    return base == Record::kNoTimestamp ? 0 : base;
}

double AxisEditor::secondsOf(qint64 utcMs) const
{
    return double(utcMs - m_renderBase) / 1000.0;
}

qint64 AxisEditor::instantOfSeconds(double seconds) const
{
    return m_renderBase + qRound64(seconds * 1000.0);
}

qint64 AxisEditor::boundInstant(TimeBound which) const
{
    // m_renderMode, NOT the document's mode: this asks what the digits on screen mean,
    // and refreshTimeBounds() runs precisely when the two disagree. Reading the new
    // mode here would interpret seconds as a date the moment the column changed, which
    // is the one instant a bound must survive intact.
    if (rendersSeconds(m_renderMode)) {
        const QDoubleSpinBox *spin = which == TimeBound::Start ? m_secStart : m_secEnd;
        return instantOfSeconds(spin->value());
    }
    return instantOfWallClock((which == TimeBound::Start ? m_timeStart : m_timeEnd)
                                  ->dateTime());
}

qint64 AxisEditor::instantOfWallClock(const QDateTime &wallClock) const
{
    // The inverse of wallClockOf(): the digits carry no zone, and criteria() hands them
    // to resolve() to be read in the display zone, so that is the zone that says which
    // instant they name.
    QDateTime at = wallClock;
    at.setTimeZone(m_renderZone);
    return at.toMSecsSinceEpoch();
}

void AxisEditor::setBoundInstant(TimeBound which, qint64 utcMs)
{
    // BOTH spellings, always — the hidden pair is what the next display-mode change
    // will show, and a stale value there is a bound that silently changes when the
    // user switches how the column reads.
    if (which == TimeBound::Start) {
        m_timeStart->setDateTime(wallClockOf(utcMs));
        m_secStart->setValue(secondsOf(utcMs));
    } else {
        m_timeEnd->setDateTime(wallClockOf(utcMs));
        m_secEnd->setValue(secondsOf(utcMs));
    }
}

void AxisEditor::syncTimeEditorKind()
{
    if (!m_timeStart || !m_secStart)
        return;
    // Nothing this function writes is a user edit, in any caller — and one of the
    // writes has a signal behind it: setDecimals() below re-rounds the value the spin
    // box is holding and emits valueChanged when the rounding moves it. The
    // secondsEdited handler is guarded on m_populating alone, so an unguarded call
    // reaches it, latches m_timeUserEdited and emits changed() — a claim that the user
    // edited an axis, which is false wherever it is heard.
    //
    // It WAS heard: setDocument() called this without the guard, so rebinding the panes
    // from a log whose format carries milliseconds (whose observed span had seeded a
    // fractional value into the hidden seconds pair) onto one whose format does not took
    // the decimals 3 → 0 mid-rebind, and HighlighterPane's changed handler — by then
    // holding the INCOMING document but still the outgoing log's m_rules — wrote the
    // outgoing log's whole rule list onto the incoming document, persisted it, and read
    // it straight back so nothing on screen was left to notice (bugs.md 26).
    //
    // Guarded here rather than at the call sites, so a fifth caller cannot forget it,
    // and SAVED AND RESTORED rather than cleared on the way out: three of the four
    // callers are already inside an m_populating block of their own and clearing it
    // would unguard the rest of their own writes.
    const bool wasPopulating = m_populating;
    m_populating = true;
    const bool seconds = secondsMode();
    // Milliseconds only where the file's own %d carries them, which is the rule the
    // column renders by (LogModel::formatSeconds, DateFormat::hasMillis): ".000" under
    // a format with no ms invents precision the log does not have, and a bound the user
    // cannot see the effect of.
    const bool millis = m_document && m_document->format().impliedDateFormat.hasMillis;
    // Which zero the number is counted from, said where the number is typed. The column
    // itself never has to say it — a reader compares one row against another — but a
    // bound is a single figure with nothing beside it to be relative to.
    const QString hint = m_document && m_document->timeDisplay() == TimeDisplay::RunSeconds
        ? tr("Seconds from the start of the selected run, as the timestamp column shows.")
        : tr("Seconds since the epoch, as the timestamp column shows.");
    for (QDoubleSpinBox *spin : {m_secStart, m_secEnd}) {
        // setDecimals() re-rounds the held value, so it must not run on every sync or
        // an integer-seconds column would quietly truncate a bound set from a record.
        if (spin->decimals() != (millis ? 3 : 0))
            spin->setDecimals(millis ? 3 : 0);
        spin->setSingleStep(1.0);
        spin->setToolTip(hint);
        spin->setVisible(seconds);
    }
    m_timeStart->setVisible(!seconds);
    m_timeEnd->setVisible(!seconds);
    m_populating = wasPopulating;
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
    const QTimeZone nowZone = m_document->displayZone();
    const TimeDisplay nowMode = m_document->timeDisplay();
    const qint64 nowBase = secondsBaseMs();
    if (nowZone == m_renderZone && nowMode == m_renderMode && nowBase == m_renderBase)
        return;

    // The editors hold what the timestamp column holds, and criteria() reads them back
    // through whatever those terms CURRENTLY are. So when any of the three move under
    // them the digits have to be re-rendered, or they would come to name a different
    // instant: a zone shift moves the wall clock, a mode change swaps seconds for a
    // date, and a run selection moves what "0 seconds" is counted from.
    //
    // Recover the instant in the OLD terms first, then write it in the new ones — the
    // instant is the bound, the digits are only how it is being asked for.
    const qint64 startAt = boundInstant(TimeBound::Start);
    const qint64 endAt = boundInstant(TimeBound::End);

    m_populating = true;
    m_renderZone = nowZone;
    m_renderMode = nowMode;
    m_renderBase = nowBase;
    syncTimeEditorKind();
    setBoundInstant(TimeBound::Start, startAt);
    setBoundInstant(TimeBound::End, endAt);
    m_populating = false;
}

void AxisEditor::refreshDiscoveredLists()
{
    if (!m_document) {
        m_populating = true;
        // The values go; the "Others" row stays, as it does through every other
        // repopulation — unbinding is not the user retracting the discovery rule.
        clearValueRows(m_loggerList);
        clearValueRows(m_threadList);
        m_populating = false;
        return;
    }
    repopulate(checkedNames(m_loggerList), checkedNames(m_threadList), ListRule::Discover,
               ListRule::Discover);
    // The other thing the index tells the editor. Both callers of this function mean
    // "the scan has moved on", and a time axis seeded once at open time is seeded from
    // an empty file.
    refreshObservedSpan();
}

void AxisEditor::repopulate(const QSet<QString> &loggerChecked,
                            const QSet<QString> &threadChecked, ListRule loggerRule,
                            ListRule threadRule)
{
    if (!m_document)
        return;

    QStringList loggers = m_document->index().loggers.names();
    QStringList threads = m_document->index().threads.names();
    loggers.append(m_loggerManualNames.values());
    threads.append(m_threadManualNames.values());

    // Each half re-narrows and re-counts only if its own rows actually moved: the
    // narrowing writes a hidden flag per row and the hints write button text, and both
    // are repaints of state that did not change on an append that discovered nothing.
    if (populateList(m_loggerList, loggers, loggerChecked, m_loggerManualNames,
                     m_loggerSeen, loggerRule, restrictiveFor(ValueAxis::Subsystem))) {
        narrowList(m_loggerList, m_loggerNarrow ? m_loggerNarrow->text() : QString());
        // The list just changed under the narrowing, so the hidden count the buttons
        // report has too.
        updateListButtonHints(ValueAxis::Subsystem);
    }
    if (populateList(m_threadList, threads, threadChecked, m_threadManualNames,
                     m_threadSeen, threadRule, restrictiveFor(ValueAxis::Thread))) {
        narrowList(m_threadList, m_threadNarrow ? m_threadNarrow->text() : QString());
        updateListButtonHints(ValueAxis::Thread);
    }
}

// ---------------------------------------------------------------------------
// Criteria in and out
// ---------------------------------------------------------------------------

Priority AxisEditor::comboPriority() const
{
    if (!m_priorityCombo)
        return kDefaultFloor;
    const QVariant data = m_priorityCombo->currentData();
    return data.isValid() ? Priority(data.toInt()) : kDefaultFloor;
}

bool AxisEditor::setComboPriority(Priority p)
{
    if (!m_priorityCombo)
        return false;
    const int row = m_priorityCombo->findData(int(p));
    m_priorityCombo->setCurrentIndex(row >= 0 ? row : m_priorityCombo->findData(int(kDefaultFloor)));
    return row >= 0;
}

MatchCriteria AxisEditor::criteria() const
{
    MatchCriteria c;

    c.priorityEnabled = m_priorityEnable->isChecked();
    c.minPriority = comboPriority();

    c.loggerEnabled = m_loggerGroup->isChecked();
    c.loggerNames = toSortedList(checkedNames(m_loggerList));
    // Coverage is answered from the list the user was shown, never from the intern
    // table: the table grows mid-scan and the list lags it, so asking the table would
    // make a discovered-but-not-yet-listed subsystem look excluded.
    c.loggerCoversAll = coversAllFor(ValueAxis::Subsystem);
    c.loggerRestrictive = restrictiveFor(ValueAxis::Subsystem);

    c.threadEnabled = m_threadGroup->isChecked();
    c.threadNames = toSortedList(checkedNames(m_threadList));
    c.threadCoversAll = coversAllFor(ValueAxis::Thread);
    c.threadRestrictive = restrictiveFor(ValueAxis::Thread);

    c.text.enabled = m_textGroup->isChecked();
    c.text.negate = m_textNegate->isChecked();
    c.text.matcher.set(m_textEdit->text(), m_textRegex->isChecked(),
                       m_textCase->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive);

    c.timeEnabled = m_timeGroup->isChecked();
    // Wall clock either way, whichever pair the user typed into: MatchCriteria is the
    // PORTABLE form (invariant #10) and a count of seconds is not portable — it is
    // relative to a baseline that belongs to this file, this run partition and this
    // display mode, none of which a preset carries or a session can promise to
    // restore. The date editors are therefore the bound; every write to the seconds
    // pair — programmatic (setBoundInstant) or typed (the valueChanged handler) —
    // mirrors into them, so reading them here reads what the seconds pair is showing.
    c.start = m_timeStart->dateTime();
    c.end = m_timeEnd->dateTime();

    return c;
}

void AxisEditor::setCriteria(const MatchCriteria &c)
{
    m_populating = true;

    // A TRACE floor can still arrive — from a preset or session written before TRACE
    // stopped being offered, or from a highlight rule whose criteria were never edited
    // here. It is not promoted to the lowest level still on offer: that would turn a
    // filter that showed everything into one that hides every TRACE record, silently,
    // on restore. It is taken for what it always meant instead — no narrowing by level
    // — and expressed the way the pane expresses that now, by leaving the axis off. The
    // view is identical either way, which is the test of a migration that costs nothing.
    const bool offerable = setComboPriority(c.minPriority);
    m_priorityEnable->setChecked(c.priorityEnabled && offerable);

    m_loggerGroup->setChecked(c.loggerEnabled);
    m_threadGroup->setChecked(c.threadEnabled);

    m_textGroup->setChecked(c.text.enabled);
    m_textEdit->setText(c.text.matcher.pattern());
    m_textRegex->setChecked(c.text.matcher.isRegex());
    m_textCase->setChecked(c.text.matcher.caseSensitivity() == Qt::CaseSensitive);
    m_textNegate->setChecked(c.text.negate);

    m_timeGroup->setChecked(c.timeEnabled);
    // The stored wall clock goes into the date editors verbatim — it is what criteria()
    // reads back, and pushing it through an instant and out again would move a bound
    // that lands in a DST gap. The seconds pair is derived from it, because a rule
    // loaded while the column shows seconds must show ITS bounds rather than keep
    // whatever the previously selected rule left in a hidden editor.
    if (c.start.isValid()) {
        m_timeStart->setDateTime(c.start);
        m_secStart->setValue(secondsOf(instantOfWallClock(c.start)));
    }
    if (c.end.isValid()) {
        m_timeEnd->setDateTime(c.end);
        m_secEnd->setValue(secondsOf(instantOfWallClock(c.end)));
    }
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

    // Load: the stored selection is reproduced as-is rather than run through the
    // discovery rule, so loading one highlight rule after another shows each rule's
    // own subsystems instead of inheriting the previous rule's.
    //
    // Unless it narrowed nothing, which is a statement about the file and not a list of
    // names: its list is only as long as the scan had got when it was written, so a
    // state stored before its log was indexed lists nothing at all. Loading THAT as a
    // literal selection ticks nothing, files every name into `seen` as shown-and-
    // unticked — which puts the axis beyond the discovery rule's reach for the rest of
    // this binding — and leaves an axis that is on with an empty id set, i.e. a log
    // showing none of its records. Keyed on coversAll and never on which pane hosts the
    // editor: an axis that named some of the values still loads exactly. A RESTRICTIVE
    // selection loads exactly whatever its coverage says, because "these names,
    // whatever turns up later" is the one statement that must not widen.
    //
    // An axis that is OFF has no selection to show at all, so it loads nothing ticked
    // and records nothing as seen instead — see ListRule::Unstated. That is what fixes
    // the same trap on the Thread axis, which ships off: ticking its values here would
    // write them into any highlight rule that merely has this one selected.
    const auto ruleFor = [](bool enabled, bool coversAll, bool restrictive) {
        if (!coversAll || restrictive)
            return ListRule::Load;
        return enabled ? ListRule::CoverAll : ListRule::Unstated;
    };
    repopulate(loggerSel, threadSel,
               ruleFor(c.loggerEnabled, c.loggerCoversAll, c.loggerRestrictive),
               ruleFor(c.threadEnabled, c.threadCoversAll, c.threadRestrictive));

    updateAxisState();
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

QHash<QString, bool> &AxisEditor::seenFor(ValueAxis axis)
{
    return axis == ValueAxis::Subsystem ? m_loggerSeen : m_threadSeen;
}

bool AxisEditor::axisOfViewport(const QObject *viewport, ValueAxis &axis) const
{
    if (m_loggerList && viewport == m_loggerList->viewport()) {
        axis = ValueAxis::Subsystem;
        return true;
    }
    if (m_threadList && viewport == m_threadList->viewport()) {
        axis = ValueAxis::Thread;
        return true;
    }
    return false;
}

bool AxisEditor::eventFilter(QObject *watched, QEvent *event)
{
    const bool press = event->type() == QEvent::MouseButtonPress
                       || event->type() == QEvent::MouseButtonDblClick;
    ValueAxis axis = ValueAxis::Subsystem;
    if (press && axisOfViewport(watched, axis)) {
        auto *me = static_cast<QMouseEvent *>(event);
        // Exactly Ctrl, so Ctrl+Shift and friends are left to the view: this claims one
        // chord, not every chord that happens to include Ctrl. The double-click type is
        // taken as well because a second Ctrl+click arrives as one, and the alternative
        // is that holding Ctrl and clicking twice reverts half the edit.
        if (me->button() == Qt::LeftButton && me->modifiers() == Qt::ControlModifier) {
            if (QListWidgetItem *item = listFor(axis)->itemAt(me->position().toPoint())) {
                // The row still becomes current, because the click was on it and the
                // keyboard has to carry on from where the mouse left off.
                listFor(axis)->setCurrentItem(item);
                checkOnly(axis, item);
                return true; // taken: no tick toggled underneath it, no selection swept
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

QListWidgetItem *AxisEditor::othersItemFor(ValueAxis axis) const
{
    return axis == ValueAxis::Subsystem ? m_loggerOthers : m_threadOthers;
}

bool AxisEditor::isOthersRow(const QListWidgetItem *item)
{
    return item && item->data(kOthersRole).toBool();
}

bool AxisEditor::restrictiveFor(ValueAxis axis) const
{
    const QListWidgetItem *item = othersItemFor(axis);
    return item && item->checkState() != Qt::Checked;
}

void AxisEditor::setRestrictiveFor(ValueAxis axis, bool restrictive)
{
    // setCheckState() emits itemChanged() only when the state actually moves, and the
    // list's handler turns that into emitChanged() — so this behaves exactly as the
    // checkbox's setChecked() did, m_populating guard included.
    if (QListWidgetItem *item = othersItemFor(axis))
        item->setCheckState(restrictive ? Qt::Unchecked : Qt::Checked);
}

void AxisEditor::ensureListed(ValueAxis axis, const QString &name)
{
    QListWidget *list = listFor(axis);
    if (!list)
        return;
    auto listed = [list, &name] {
        for (int i = kFirstValueRow; i < list->count(); ++i)
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

void AxisEditor::checkOnly(ValueAxis axis, QListWidgetItem *target)
{
    QListWidget *list = listFor(axis);
    if (!list || !target)
        return;

    m_populating = true;
    // Every item, including the ones the narrow box is currently hiding. All / None
    // deliberately act on the narrowed view only, because there the user can see what
    // they are acting on; "only db.pool" that quietly left hidden values ticked would
    // restrict to more than it says, whether it was reached from the record menu or by
    // Ctrl+clicking the row. Not the "Others" row, which setRestrictiveFor() is the
    // statement about — and which is the one row that can be the target here.
    for (int i = kFirstValueRow; i < list->count(); ++i) {
        QListWidgetItem *item = list->item(i);
        item->setCheckState(item == target ? Qt::Checked : Qt::Unchecked);
    }
    if (QGroupBox *enable = enableFor(axis))
        enable->setChecked(true);
    setRestrictiveFor(axis, !isOthersRow(target));
    m_populating = false;

    emitChanged();
}

void AxisEditor::showOnlyValue(ValueAxis axis, const QString &name)
{
    QListWidget *list = listFor(axis);
    if (!list || name.isEmpty() || (axis == ValueAxis::Thread && !supportsThread()))
        return;
    ensureListed(axis, name);

    // By name rather than by row, because ensureListed() may have rebuilt the list.
    for (int i = kFirstValueRow; i < list->count(); ++i) {
        if (list->item(i)->text() == name) {
            checkOnly(axis, list->item(i));
            return;
        }
    }
}

void AxisEditor::hideValue(ValueAxis axis, const QString &name)
{
    QListWidget *list = listFor(axis);
    if (!list || name.isEmpty() || (axis == ValueAxis::Thread && !supportsThread()))
        return;
    ensureListed(axis, name);

    m_populating = true;
    for (int i = kFirstValueRow; i < list->count(); ++i) {
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
    // "Show this level and above" on a TRACE record asks for every record there is, and
    // the combo no longer has a row for it. Ticking the axis at the lowest level it DOES
    // offer would hide the very record the menu was opened on, so the axis goes off —
    // which is that request, exactly, and leaves nothing narrowed by level.
    const bool offerable = setComboPriority(p);
    m_priorityEnable->setChecked(offerable);
    m_populating = false;
    emitChanged();
}

void AxisEditor::setTimeBound(TimeBound which, qint64 utcMs)
{
    if (!m_timeGroup || !m_timeStart || !m_timeEnd || !supportsTime())
        return;
    // What the opposite bound should be when it has to move. The file's observed span
    // is the honest "open end": the editors cannot hold "no bound", and leaving an
    // unseeded end at the year 2000 — which is what a file that had no timestamps
    // when the pane bound to it leaves behind — would hide everything.
    qint64 lo = 0;
    qint64 hi = 0;
    const bool span = observedSpan(lo, hi);
    const qint64 openEnd = span ? qMax(hi, utcMs) : utcMs;
    const qint64 openStart = span ? qMin(lo, utcMs) : utcMs;

    // In instants, not in digits: the comparison against the far bound has to mean the
    // same thing whichever pair of editors is on screen, and a seconds value compared
    // as a QDateTime would compare the year 2000 against itself.
    const bool wasEnabled = m_timeGroup->isChecked();
    m_populating = true;
    if (which == TimeBound::Start) {
        setBoundInstant(TimeBound::Start, utcMs);
        // Widen the far end only when it would otherwise exclude the record just
        // pointed at — an end the user set deliberately is left where it is.
        if (!wasEnabled || boundInstant(TimeBound::End) < utcMs)
            setBoundInstant(TimeBound::End, openEnd);
    } else {
        setBoundInstant(TimeBound::End, utcMs);
        if (!wasEnabled || boundInstant(TimeBound::Start) > utcMs)
            setBoundInstant(TimeBound::Start, openStart);
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
    setBoundInstant(TimeBound::Start, fromUtcMs);
    setBoundInstant(TimeBound::End, toUtcMs);
    m_timeUserEdited = true; // before setChecked(), as in setTimeBound()
    m_timeGroup->setChecked(true);
    m_populating = false;
    emitChanged();
}

// ---------------------------------------------------------------------------
// List helpers
// ---------------------------------------------------------------------------

bool AxisEditor::populateList(QListWidget *list, const QStringList &names,
                              const QSet<QString> &checked, const QSet<QString> &manual,
                              QHash<QString, bool> &seen, ListRule rule, bool restrictive)
{
    if (!list)
        return false;
    m_populating = true;
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
    // Discovery rule (ListRule::Discover): a name the editor has never shown before is
    // checked; one it has shown keeps whatever state it had. The distinction matters
    // because the subsystem axis is enabled by default (SPEC.md §6) and subsystems are
    // discovered *as the file is scanned* — without it, every subsystem that first
    // appears after the initial population would arrive unchecked and its records
    // would vanish from an untouched view. "Never shown" also covers the very first
    // population, where everything is new.
    //
    // Load rule (ListRule::Load): checked means exactly `checked`, because the caller
    // is reproducing a stored selection, not discovering values.
    //
    // Coverage rule (ListRule::CoverAll): everything is checked, because the caller is
    // reproducing a selection that excluded NOTHING — which is a statement about the
    // file rather than a list of names, and its list is only as long as the scan had
    // got when it was stored. `seen` is filled as under every other rule, which costs
    // nothing here: every name is ticked, so the next Discover pass carries it through
    // `checked` rather than through freshness.
    //
    // Unstated rule (ListRule::Unstated): checked means exactly `checked`, as under the
    // load — but nothing is recorded in `seen`, so the discovery rule is still armed
    // for every name here. It is the same "excluded nothing" as CoverAll with the axis
    // switched OFF, where there is no selection to show and ticking one would put names
    // into a rule that does not use this axis the next time criteria() is read.
    //
    // Restriction rule (restrictive, under Discover): the discovery rule is exactly
    // wrong — the selection names what the user asked to see, so a value nobody has
    // seen yet is not part of it and arrives UNCHECKED. A name the user typed by hand
    // still arrives checked: adding it is the request to see it.
    //
    // A name already listed keeps its own state under Discover, which is what makes an
    // unticked value stay unticked across the repopulations indexing drives —
    // including one the user typed in and then unticked.
    //
    // RETURNING NAMES (under Discover): a name that has been shown but is NOT on screen
    // now comes back with the state it was last shown in, because `seen` records that
    // state and not mere membership. The only thing that takes a name off the list is
    // the index being replaced wholesale — a rotation or a truncation — and for a
    // REMOTE log that leaves a long gap, since the spool is re-fetched from the top and
    // the rescan lands on almost nothing. Recording membership alone made every one of
    // those names read as "shown, and the user unticked it": the whole subsystem list
    // came back unticked an ingest tick later, the axis narrowed to nothing, and the
    // log the reader was watching emptied itself. It is the same misreading setDocument()
    // clears `seen` to avoid across a rebind, arriving from inside one binding instead.
    //
    // The answer is worked out in full BEFORE the widget is touched, because this
    // function runs on every ingest tick of a growing log and the answer is almost
    // always the one already on screen. Rebuilding it anyway — clearValueRows() and a
    // fresh QListWidgetItem per name — emptied and refilled the list ~1.3 times a
    // second, which on a loaded machine is a visible flash and a lost scroll position
    // in a pane nothing about the append changed. `seen` is still updated for every
    // name whatever the comparison decides, since that bookkeeping is what the
    // discovery rule reads and it must not depend on whether the rows moved.
    // Which names are ON SCREEN right now, ticked or not. Only a name that is NOT is
    // answered from `seen`'s remembered state: for one that is, the widget is the truth
    // and the memo is a pass behind it, so consulting it would re-tick a row the user
    // had just unticked.
    QSet<QString> listedNow;
    for (int i = kFirstValueRow; i < list->count(); ++i)
        listedNow.insert(list->item(i)->text());

    QList<QPair<QString, bool>> wanted;
    wanted.reserve(sorted.size());
    for (const QString &n : sorted) {
        const bool fresh = !seen.contains(n);
        bool       on = false;
        switch (rule) {
        case ListRule::Discover:
            on = checked.contains(n) || (fresh && (!restrictive || manual.contains(n)))
                 || (!fresh && !listedNow.contains(n) && seen.value(n));
            break;
        case ListRule::Load:
        case ListRule::Unstated:
            on = checked.contains(n);
            break;
        case ListRule::CoverAll:
            on = true;
            break;
        }
        // `seen` is what the discovery rule tests a name's freshness against. Under
        // Unstated a name that arrives unticked is UN-seen rather than recorded: the
        // axis excluded nothing, so nothing here may be carried as excluded — and the
        // set has usually just been filled by setDocument()'s own discovery pass, which
        // runs immediately before the pane hydrates. Without the removal the pass that
        // follows this one finds nothing fresh and the axis stays empty for good, which
        // is the whole defect on the axis that ships switched off.
        if (on || rule != ListRule::Unstated)
            seen.insert(n, on);
        else
            seen.remove(n);
        wanted.append({n, on});
    }

    // Nothing about the rows differs: leave every item, its check state, its hidden
    // flag and the view's scroll position exactly where they are.
    if (list->count() - kFirstValueRow == wanted.size()) {
        bool same = true;
        for (int i = 0; i < wanted.size() && same; ++i) {
            const QListWidgetItem *item = list->item(kFirstValueRow + i);
            same = item->text() == wanted.at(i).first
                   && (item->checkState() == Qt::Checked) == wanted.at(i).second;
        }
        if (same) {
            m_populating = false;
            return false;
        }
    }

    // NOT clear(): row 0 is the "Others" row and carries the discovery rule this
    // function is being handed as `restrictive`. Clearing it would delete the state
    // and leave restrictiveFor() reading a dangling row.
    clearValueRows(list);
    for (const auto &w : std::as_const(wanted)) {
        auto *item = new QListWidgetItem(w.first, list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(w.second ? Qt::Checked : Qt::Unchecked);
    }
    m_populating = false;
    return true;
}

void AxisEditor::clearValueRows(QListWidget *list)
{
    if (!list)
        return;
    while (list->count() > kFirstValueRow)
        delete list->takeItem(kFirstValueRow);
}

bool AxisEditor::coversAllFor(ValueAxis axis) const
{
    const QListWidget *list = listFor(axis);
    if (allChecked(list))
        return true;
    // The lossy half of criteria(): a switched-off axis loads under ListRule::Unstated,
    // which ticks nothing at all, so allChecked() answers false about a state that
    // narrows nothing and never did. Reading that back would turn every edit to some
    // OTHER axis of the same rule into a stored `loggerCoversAll: false` over an empty
    // name list — which reloads under ListRule::Load, files every value as seen-and-
    // excluded, and puts the axis beyond the discovery rule for good. So an axis with
    // nothing ticked AND nothing switched on states nothing, and says so.
    const QGroupBox *enable = enableFor(axis);
    return enable && !enable->isChecked() && checkedNames(list).isEmpty();
}

bool AxisEditor::allChecked(const QListWidget *list)
{
    if (!list)
        return true;
    // Values only. This answers MatchCriteria::loggerCoversAll — "does the selection
    // cover everything the user was OFFERED" — which is read only to collapse a
    // narrows-nothing axis, and is a different question from what "Others" answers:
    // an unticked "Others" over a fully ticked list excludes nothing YET, and stops
    // being all-checked of its own accord the moment a new value arrives unticked.
    for (int i = kFirstValueRow; i < list->count(); ++i)
        if (list->item(i)->checkState() != Qt::Checked)
            return false;
    return true; // an empty list excludes nothing
}

QSet<QString> AxisEditor::checkedNames(const QListWidget *list)
{
    QSet<QString> out;
    if (!list)
        return out;
    // From kFirstValueRow: "Others" is a rule, not a name, and letting its label
    // through here would put a subsystem nothing is logged under into criteria(),
    // into every saved preset, and into the session file.
    for (int i = kFirstValueRow; i < list->count(); ++i) {
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
    for (int i = kFirstValueRow; i < list->count(); ++i) {
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
    for (int i = kFirstValueRow; i < list->count(); ++i) {
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
    // "Others" is never narrowed away. It is not one of the names being searched, and
    // hiding it would take the discovery rule off screen exactly when the user is
    // building a selection — which is when it matters most.
    for (int i = kFirstValueRow; i < list->count(); ++i) {
        QListWidgetItem *item = list->item(i);
        item->setHidden(!needle.isEmpty()
                        && !item->text().contains(needle, Qt::CaseInsensitive));
    }
}

} // namespace loftail
