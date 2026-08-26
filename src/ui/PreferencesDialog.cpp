#include "PreferencesDialog.h"

#include "LogProfileEditor.h"
#include "MessageLabel.h"
#include "RemoteLocation.h"
#include "UiColors.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QRegularExpression>
#include <QShowEvent>
#include <QSplitter>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <utility>

namespace loftail {

namespace {
constexpr int kRefRole = Qt::UserRole;

// The gap each splitter panel keeps on the side facing the other one. Without it the
// only thing between the tree's frame and the right panel's prose is the splitter
// handle — measured at 4 px — and the text reads as if it were spilling into the tree.
// It also keeps the section frames on the right off the panel edge, which a rounded
// group-box border flush against a boundary otherwise makes look like a rendering
// fault: the same reason AxisEditor carries kSideMargin.
constexpr int kPanelGap = 8;

// What the tree pane opens at. The width it ASKS for is its own longest row (see
// treeContentWidth()) rather than a fraction of the dialog, because what it has to show
// is labels and a fraction knows nothing about them — at the old 1:2 stretch the virtual
// longest row was elided on a dialog with room to spare on the other side.
//
// Bounded at both ends, and the cap is the half that matters: a pattern is a line the
// user types, so one long enough would otherwise hand the tree most of the dialog and
// squeeze the settings it exists to reach. The floor keeps a tree of short rows from
// opening narrower than its own toolbar reads well at.
constexpr double kTreeMaxShare = 0.4;
constexpr int kTreeMinWidth = 220;

// What the node title keeps above and below itself. The top gap is the larger of the
// two on purpose: it separates the heading from the dialog's edge, while below it the
// first section's own title row already supplies air.
constexpr int kTitleTopGap = 10;
constexpr int kTitleBottomGap = 4;

// The air a node's identity/settings rule keeps on each side of itself.
constexpr int kDividerGap = 6;

// The two glyphs left on the tree's toolbar. NOT translated, exactly as the Filters
// pane's are not: the words live on setToolTip and setAccessibleName beside them. An
// arrow is a direction and draws better than it reads; Add and Delete wear words instead
// — see where they are built.
constexpr auto kUpGlyph = "\xE2\x86\x91";     // U+2191
constexpr auto kDownGlyph = "\xE2\x86\x93";   // U+2193

// The rule between what a node IS and what it gives its logs. PAINTED, not a
// QFrame::HLine, for exactly the reason SectionBox paints its title hairline: a frame's
// line colour belongs to the style, and a style sheet does not take it back. Measured on
// this dialog with `color:` set in one — Fusion drew #acacac, the colour asked for, and
// Breeze drew #e2e2e2, its own, because a style sheet hands the whole frame to
// QStyleSheetStyle and it draws the border its way. The two kinds of line here are meant
// to be one thing at two lengths, and that made them differ per desktop, in a direction
// nobody chose.
//
// Painting also resolves the colour per paint, so it follows a theme switched mid-session
// and dims with a disabled panel — the same two properties SectionBox's line has.
class DividerLine : public QWidget
{
public:
    explicit DividerLine(QWidget *parent) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(
            dividerColor(palette(), isEnabled() ? QPalette::Active : QPalette::Disabled));
        const int y = height() / 2;
        painter.drawLine(0, y, width(), y);
    }
};

// A panel caption: centred, bold, and standing off whatever is above it. Every caption
// in this dialog comes from here, so "the same look" is one line of code rather than
// several that agree today.
//
// Bold by setFont() and NOT by a style sheet, which is the opposite of what SectionBox
// does for a group box's title — there a sheet is the only way in, since a QGroupBox's
// font reaches every widget inside it and setFont() would bold the whole body. A QLabel
// has no children to bold by accident. What makes the assignment stick is that setBold()
// marks weight in the font's RESOLVE MASK: Qt propagates the parent's font over
// everything a font does not claim, so a font handed back unmodified by font() is
// ignored (the trap the preview's empty-state notice records).
QLabel *makeCaption(QWidget *parent, const QString &name, const QString &text = QString())
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(name); // findChild, for tests
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    label->setContentsMargins(0, kTitleTopGap, 0, kTitleBottomGap);
    return label;
}

// The name a pattern node wears — in its tree row and as the heading of its panel, from
// one place so the two cannot come to disagree. Quoted, so it reads as the pattern it is
// rather than as a file that happens to be spelt oddly: the rows under it are real names,
// and `*.log` beside `app.log` at the same indentation is one glance away from looking
// like one. The empty case stays unquoted — it is prose about a pattern that says nothing
// yet, and a pair of quotes round nothing is a row wearing an empty name.
QString patternDisplayName(const QString &match)
{
    return match.isEmpty() ? PreferencesDialog::tr("(empty pattern)")
                           : QStringLiteral("\"%1\"").arg(match);
}

QToolButton *makeToolButton(QWidget *parent, const char *glyph, const QString &name,
                            const QString &words)
{
    auto *b = new QToolButton(parent);
    b->setObjectName(name); // findChild, for tests
    b->setText(QString::fromUtf8(glyph));
    b->setToolTip(words);
    b->setAccessibleName(words);
    return b;
}
} // namespace

QString PreferencesDialog::refToString(const NodeRef &r)
{
    switch (r.kind) {
    case NodeKind::Pattern: return QStringLiteral("pattern:") + r.key;
    case NodeKind::File:    return QStringLiteral("file:") + r.key;
    case NodeKind::Root:    break;
    }
    return QStringLiteral("root");
}

PreferencesDialog::NodeRef PreferencesDialog::refFromString(const QString &s)
{
    NodeRef r;
    if (s.startsWith(QLatin1String("pattern:"))) {
        r.kind = NodeKind::Pattern;
        r.key = s.mid(8);
    } else if (s.startsWith(QLatin1String("file:"))) {
        r.kind = NodeKind::File;
        r.key = s.mid(5);
    }
    return r;
}

PreferencesDialog::PreferencesDialog(LogSettingsTree tree,
                                     const QString &sampleName,
                                     const QByteArray &sample,
                                     QWidget *parent)
    : QDialog(parent), m_settings(std::move(tree))
{
    setObjectName(QStringLiteral("preferencesDialog")); // findChild, for tests
    setWindowTitle(tr("Preferences"));
    resize(980, 700);
    buildUi(sampleName, sample);
    rebuildTree(NodeRef{});
}

void PreferencesDialog::buildUi(const QString &sampleName, const QByteArray &sample)
{
    auto *outer = new QVBoxLayout(this);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter = splitter;
    splitter->setObjectName(QStringLiteral("settingsSplitter")); // findChild, for tests
    splitter->setChildrenCollapsible(false);
    // A handle the user has dragged is an answer, and it outranks the computed one for
    // good — which costs nothing here, since the computed one is applied at the first
    // show and a drag cannot precede it.
    connect(splitter, &QSplitter::splitterMoved, this,
            [this](int, int) { m_splitSettled = true; });

    // --- left: the tree and what can be done to it -----------------------------------
    auto *left = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, kPanelGap, 0);

    m_treeWidget = new QTreeWidget(left);
    m_treeWidget->setObjectName(QStringLiteral("settingsTree")); // findChild, for tests
    m_treeWidget->setHeaderHidden(true);
    m_treeWidget->setRootIsDecorated(true);
    m_treeWidget->setUniformRowHeights(true);
    // MOVING TO ANOTHER ROW FOLDS THE ONE BEING LEFT BACK IN FIRST. Every gesture with a
    // button behind it commits before it acts — Add, Move, Promote, Apply, OK — and a
    // plain click on the tree was the one route that did not, so whatever had been typed
    // into the settings panel was discarded by merely looking at another node: loadNode()
    // overwrites the editors and then takes m_loaded with it, and nothing on screen says
    // anything happened. The pattern's own match fields survived only because they ride
    // editingFinished, which the same click fires by taking the focus away.
    //
    // Guarded on m_updating, which is what tells a user's click from rebuildTree()'s own
    // clear(): during a rebuild the editors hold whatever the last load put there while
    // m_loaded may name a node that has just been deleted or re-homed, and committing
    // that would write one node's settings into another. Every gesture that rebuilds has
    // already committed, or has set m_loadedValid false because the node is going.
    connect(m_treeWidget, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *, QTreeWidgetItem *) {
                if (!m_updating)
                    commitCurrent();
                loadNode();
            });
    leftLayout->addWidget(m_treeWidget, 1);

    auto *toolRow = new QHBoxLayout;
    // The one button on this row that SAYS what it does. A glyph works for the other
    // three because each acts on the row already selected — delete this, move this — so
    // the tree supplies the noun. "+" has no such subject: it makes a thing that is not
    // there yet, and nothing on screen says which. The words are the button's own rather
    // than a tooltip's, since a tooltip is only read by someone who already suspects.
    //
    // Still a QToolButton, so it keeps the row's height and metrics (and the tests' handle
    // on it) rather than sitting a few pixels taller than the glyphs beside it. No
    // mnemonic: this dialog has already claimed A, C, D, E, P and S, and a duplicate
    // letter turns Alt into a focus cycle rather than a shortcut.
    m_addPattern = new QToolButton(left);
    m_addPattern->setObjectName(QStringLiteral("addPatternButton")); // findChild, for tests
    m_addPattern->setText(tr("Add Pattern"));
    m_addPattern->setToolTip(tr("Add a file pattern"));
    // Worded for the same reason Add is, and it is the button that most needs it: a "−"
    // beside a "+" reads as the pair's other half — remove what was just added — while
    // this one deletes whichever row is selected, including one with settings behind it.
    // The tooltip still says WHICH row; the button says what happens to it — and it says
    // it per node, in updateButtons(), because the two rows it can be pressed on no
    // longer have one answer between them: a pattern is deleted, while the current file's
    // row keeps its place and gives up only what the log said of its own.
    m_deleteNode = new QToolButton(left);
    m_deleteNode->setObjectName(QStringLiteral("deleteNodeButton")); // findChild, for tests
    m_deleteNode->setText(tr("Delete"));
    m_moveUp = makeToolButton(
        left, kUpGlyph, QStringLiteral("moveUpButton"),
        tr("Move this pattern up — a log matching two patterns takes the higher one"));
    m_moveDown = makeToolButton(
        left, kDownGlyph, QStringLiteral("moveDownButton"),
        tr("Move this pattern down — a log matching two patterns takes the higher one"));
    connect(m_addPattern, &QToolButton::clicked, this, &PreferencesDialog::addPattern);
    connect(m_deleteNode, &QToolButton::clicked, this, &PreferencesDialog::deleteNode);
    connect(m_moveUp, &QToolButton::clicked, this, [this]() { movePattern(-1); });
    connect(m_moveDown, &QToolButton::clicked, this, [this]() { movePattern(1); });
    for (QToolButton *b : {m_addPattern, m_deleteNode, m_moveUp, m_moveDown})
        toolRow->addWidget(b);
    toolRow->addStretch();
    leftLayout->addLayout(toolRow);

    // --- right: what the selected node says ------------------------------------------
    auto *right = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(kPanelGap, 0, 0, 0);

    // The line that says what the selected node IS, so it reads as this panel's heading
    // rather than as the first of its labels. Flush to the top edge and hard left it
    // looked like a stray sentence that had lost its control.
    m_nodeTitle = makeCaption(right, QStringLiteral("nodeTitleLabel"));

    // WHAT THIS NODE IS, IN THE READER'S WORDS. The node's own name in the heading, and
    // under it, muted, which of the three levels it sits at: `app.log` over "This log
    // only", `"*.audit.log"` over "File pattern", "Default settings" over what it covers.
    // A name is what identifies a panel, and the level is what qualifies it — the other
    // way round the panel is headed by the same three words whatever it is showing.
    //
    // ONE block filled three ways, not a block per kind, which is what keeps the three
    // panels the same shape. They were three shapes before: a caption over an address for
    // a log, a caption over a form for a pattern, and nothing at all for the defaults —
    // which is how the model's word for a per-log entry ("Concrete file", the name of a
    // level in a comment in LogSettings.h) stayed on screen as a heading.
    m_identityGroup = new QWidget(right);
    m_identityGroup->setObjectName(QStringLiteral("nodeIdentityGroup")); // findChild, for tests
    auto *identityBox = new QVBoxLayout(m_identityGroup);
    identityBox->setContentsMargins(0, 0, 0, 0);
    identityBox->setSpacing(0); // "under it" — the caption's own bottom margin is the gap

    m_nodeName = makeCaption(m_identityGroup, QStringLiteral("nodeNameLabel"));
    // Selectable, as the address it replaced was: a log's name is the one thing here
    // somebody copies out, to paste into a terminal or another window.
    m_nodeName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    identityBox->addWidget(m_nodeName);

    m_nodeLevel = new QLabel(m_identityGroup);
    m_nodeLevel->setObjectName(QStringLiteral("nodeLevelLabel")); // findChild, for tests
    m_nodeLevel->setWordWrap(true);
    m_nodeLevel->setAlignment(Qt::AlignHCenter);
    // Muted FROM THE PALETTE, so it lands on either theme rather than on the one it was
    // picked against — the same mix every other aside in this dialog is drawn in.
    m_nodeLevel->setStyleSheet(
        QStringLiteral("color: %1;").arg(mutedColor(palette()).name()));
    identityBox->addWidget(m_nodeLevel);
    rightLayout->addWidget(m_identityGroup);

    // The fields that DEFINE a pattern, under the identity block naming it. They used to
    // carry a caption of their own — "Matches", which said which logs a pattern claims —
    // and it is what the identity block now says at greater length and in the same place:
    // a bold "Matches" between "File pattern" and "Logs named:" is a third heading for a
    // form of three rows. It is NOT a section (flat frame, title divider, like File format
    // and Display), because what a pattern matches on is not one of the settings its logs
    // open with, and dressed as one that is precisely what it read as.
    m_patternGroup = new QWidget(right);
    m_patternGroup->setObjectName(QStringLiteral("patternGroup")); // findChild, for tests
    auto *patternBox = new QVBoxLayout(m_patternGroup);
    patternBox->setContentsMargins(0, 0, 0, 0);
    auto *patternForm = new QFormLayout;
    patternBox->addLayout(patternForm);

    m_patternMatch = new QLineEdit(m_patternGroup);
    m_patternMatch->setObjectName(QStringLiteral("patternMatchEdit")); // findChild, for tests
    // A placeholder rather than a starting value: it says what the field wants without
    // the field claiming anything, which is the whole reason a new pattern starts empty.
    m_patternMatch->setPlaceholderText(tr("e.g. *.audit.log"));
    patternForm->addRow(tr("Logs named:"), m_patternMatch);

    m_patternKind = new QComboBox(m_patternGroup);
    m_patternKind->setObjectName(QStringLiteral("patternKindCombo")); // findChild, for tests
    m_patternKind->addItem(tr("Wildcard (* and ?)"), int(LogPatternNode::Kind::Wildcard));
    m_patternKind->addItem(tr("Regular expression"), int(LogPatternNode::Kind::Regex));
    // The two anchoring conventions differ, and each is the conventional one for its
    // kind — a wildcard describes the whole name, a regular expression is a search.
    m_patternKind->setToolTip(tr("A wildcard must match the whole name; a regular "
                                 "expression matches anywhere in it."));
    patternForm->addRow(tr("Written as:"), m_patternKind);

    auto *flags = new QHBoxLayout;
    m_patternCase = new QCheckBox(tr("Case sensitive"), m_patternGroup);
    m_patternCase->setObjectName(QStringLiteral("patternCaseSensitive")); // findChild, for tests
    m_patternFullPath = new QCheckBox(tr("Match the whole path"), m_patternGroup);
    m_patternFullPath->setObjectName(QStringLiteral("patternFullPath")); // findChild, for tests
    m_patternFullPath->setToolTip(
        tr("Off: only the log's own file name is matched. On: the whole address, "
           "including the ssh:// host for a remote log."));
    flags->addWidget(m_patternCase);
    flags->addWidget(m_patternFullPath);
    flags->addStretch();
    patternForm->addRow(QString(), flags);

    // Same kind of thing as the format editor's two messages, and the same MessageLabel
    // for the same reason: a regular expression's own complaint about itself is as long
    // as the expression, so this one wraps sooner than either of those.
    m_patternError = new MessageLabel(m_patternGroup);
    m_patternError->setObjectName(QStringLiteral("patternErrorLabel")); // findChild, for tests
    m_patternError->setStyleSheet(
        QStringLiteral("color: %1;").arg(errorColor(palette()).name()));
    m_patternError->hide();
    patternForm->addRow(m_patternError);

    // Live validity only; the value is committed when focus leaves or the node changes,
    // because every commit rebuilds the tree (a pattern edit re-homes file nodes). The
    // HEADING follows the field as it is typed, though — it names the node the reader is
    // looking at, and a heading naming the pattern as it was three keystrokes ago names
    // something that no longer exists. The tree row still waits for the commit, because
    // that is what re-homes the logs under it.
    connect(m_patternMatch, &QLineEdit::textChanged, this, [this] {
        refreshPatternValidity();
        if (currentRef().kind == NodeKind::Pattern)
            m_nodeName->setText(patternDisplayName(m_patternMatch->text()));
    });
    auto recommit = [this]() {
        if (m_updating)
            return;
        const NodeRef ref = currentRef();
        commitCurrent();
        rebuildTree(ref);
    };
    connect(m_patternMatch, &QLineEdit::editingFinished, this, recommit);
    connect(m_patternKind, &QComboBox::currentIndexChanged, this, recommit);
    connect(m_patternCase, &QCheckBox::toggled, this, recommit);
    connect(m_patternFullPath, &QCheckBox::toggled, this, recommit);
    rightLayout->addWidget(m_patternGroup);

    // WHICH logs, then WHAT they get. A pattern's match fields say which logs it claims,
    // and that is not a setting those logs open with — sitting under the heading and
    // above the format they read as the first of them, which is exactly what they are
    // not. So they go first, the heading introduces what follows it rather than what is
    // above it, and a full-width rule marks where the identity of the node stops and its
    // settings begin. The line is drawn only when there is a form above it to cut off —
    // two lines of centred text could not be mistaken for a settings block, and a rule
    // against the panel's top edge separates nothing.
    //
    // The same hairline colour SectionBox paints its title divider in, so the two kinds
    // of line in this panel are one thing at two lengths rather than two decisions.
    m_nodeDivider = new DividerLine(right);
    m_nodeDivider->setObjectName(QStringLiteral("nodeDividerLine")); // findChild, for tests
    // The air around the rule is the WIDGET'S OWN HEIGHT, not a spacer item beside it: a
    // QVBoxLayout spacer cannot be hidden, so it would hold the gap open above the
    // heading on the one node that has no divider. The line is drawn down the middle of
    // whatever height it is given, so the gap goes away exactly when the line does.
    m_nodeDivider->setFixedHeight(kDividerGap * 2 + 1);
    rightLayout->addWidget(m_nodeDivider);

    rightLayout->addWidget(m_nodeTitle);

    // THE EDITOR SCROLLS, and that is what keeps this dialog usable on a laptop.
    //
    // The settings panel grows a section per non-format field — run splitting, display,
    // the config path, the restart script — and each one adds its full height to the
    // dialog's MINIMUM, which a minimum cannot be shrunk below. The restart script alone
    // took that minimum from 675 px to 819, which is taller than the usable height of a
    // 1366x768 screen: the row holding OK and Cancel goes off the bottom and there is no
    // way to reach it. Scrolling the editor puts the floor back where the tree and the
    // button rows put it, and the next field added here costs nothing at all.
    //
    // Frameless, like the panes' own scroll areas (FilterPane, HighlighterPane): the
    // sections inside already draw their own rules, and a box around them would read as a
    // second frame inside the dialog.
    m_editorScroll = new QScrollArea(right);
    QScrollArea *editorScroll = m_editorScroll;
    editorScroll->setObjectName(QStringLiteral("profileEditorScroll")); // findChild, for tests
    editorScroll->setWidgetResizable(true);
    editorScroll->setFrameShape(QFrame::NoFrame);

    m_editor = new LogProfileEditor(editorScroll);
    m_editor->setObjectName(QStringLiteral("profileEditor")); // findChild, for tests
    m_editor->setPreviewCaption(
        sample.isEmpty() ? tr("Preview (open a log to see these settings applied to it):")
                         : tr("Preview (%1, split into fields):").arg(sampleName));
    m_editor->setSample(sample);
    editorScroll->setWidget(m_editor);

    // WIDTH IS NOT SCROLLED, only height. A QScrollArea reports a tiny minimum in both
    // directions, so without this the dialog could be dragged narrower than the settings
    // it holds and answer with a horizontal bar — which is the one thing the splitter's
    // 40% tree cap already exists to prevent. The editor's own minimum is what it was
    // before this wrapper.
    editorScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editorScroll->setMinimumWidth(m_editor->minimumSizeHint().width());

    rightLayout->addWidget(editorScroll, 1);

    // AND IT STILL OPENS AT ITS FULL HEIGHT where there is room for one. A QScrollArea
    // reports a small size hint as well as a small minimum, so wrapping the editor would
    // otherwise make the dialog OPEN scrolled on a screen with plenty of space — which is
    // the opposite failure from the one above and just as unwelcome. The initial height is
    // therefore taken from what the editor actually wants, and clamped to the screen; only
    // the FLOOR was given up, which is the half that made the dialog unusable.
    m_preferredEditorHeight = m_editor->sizeHint().height();

    auto *actionRow = new QHBoxLayout;
    m_applyButton = new QPushButton(tr("&Apply to current file"), right);
    m_applyButton->setObjectName(QStringLiteral("applyToCurrentButton")); // findChild, for tests
    m_applyButton->hide();
    // CHECKABLE, because what the press does is arm a request that OK carries out — and
    // a state that lasts until OK is what a button that stays pressed says. It used to
    // accept() the dialog instead, which made it the one control on a panel of in-place
    // edits whose press ended the session, an inch from Promote, whose press does not.
    // Pressing it again disarms the request, so a misfire costs a click rather than a
    // Cancel — the only other way back, and one that would discard every other edit too.
    m_applyButton->setCheckable(true);
    connect(m_applyButton, &QPushButton::clicked, this, &PreferencesDialog::applyToCurrent);

    // Text and tooltip are set by updateButtons(), which is the only thing that knows
    // which level the selected node's parent IS.
    m_promoteButton = new QPushButton(right);
    m_promoteButton->setObjectName(QStringLiteral("promoteToParentButton")); // findChild, for tests
    connect(m_promoteButton, &QPushButton::clicked, this, &PreferencesDialog::promoteToParent);

    actionRow->addWidget(m_applyButton);
    actionRow->addWidget(m_promoteButton);
    actionRow->addStretch();
    rightLayout->addLayout(actionRow);

    // What an armed request is waiting for. The button going down is the acknowledgement
    // that it was heard; this is the one that says WHEN it happens, which is the whole of
    // what changed when the press stopped closing the dialog. Muted from the palette, as
    // every other aside here is, and a MessageLabel because it names a log and a node and
    // will wrap on a narrow panel.
    m_applyNotice = new MessageLabel(right);
    m_applyNotice->setObjectName(QStringLiteral("applyNoticeLabel")); // findChild, for tests
    m_applyNotice->setStyleSheet(
        QStringLiteral("color: %1;").arg(mutedColor(palette()).name()));
    m_applyNotice->hide();
    rightLayout->addWidget(m_applyNotice);

    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    outer->addWidget(splitter, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("preferencesButtons")); // findChild, for tests
    // Explicit text for the same reason the other dialogs give it: the platform theme
    // labels standard buttons in the desktop's language, and loftail ships none.
    buttons->button(QDialogButtonBox::Ok)->setText(tr("OK"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    connect(buttons, &QDialogButtonBox::accepted, this, &PreferencesDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);
}

void PreferencesDialog::rebuildTree(const NodeRef &select)
{
    const bool wasUpdating = m_updating; // saved and RESTORED, never forced false
    m_updating = true;

    // THE ROW IS A FACT NOW, not a fixture this function has to keep putting back. It
    // used to take two blocks here: a sweep of the per-log nodes against what they would
    // inherit, and a re-creation of the open log's node whenever Delete or Promote had
    // removed it. Both are gone with the file level (M21).
    //
    // The row exists because the ADDRESS does — one log is open, so one row is drawn, and
    // nothing can take it away mid-visit. What it shows is m_fileProfile, which Delete
    // and Promote to Parent Pattern set back to what the log inherits rather than
    // removing; both therefore still read as "put this log back on the level above" and
    // answer themselves on screen, with one comparison instead of a removal and a
    // re-creation. Whether anything is STORED is asked once, at the end, by
    // fileProfile() — and the sweep of every OTHER log, which this dialog never lists,
    // is the caller's LogFileStore::pruneAgainst(), run only when the tree actually moved
    // (reading five hundred records on every keystroke here is not affordable).

    m_treeWidget->clear();

    auto makeItem = [](QTreeWidgetItem *parent, const QString &text, const NodeRef &ref) {
        auto *item = new QTreeWidgetItem(parent);
        item->setText(0, text);
        item->setData(0, kRefRole, refToString(ref));
        return item;
    };

    // EVERY ROW CARRIES A TOOLTIP, because every row is elided by a pane narrow enough:
    // the widget cutting a label short is fine only while the rest is one hover away.
    // What each says is the row's own full text, except a log's, which says the full
    // ADDRESS its name is short for — the same thing the panel's heading says on hover,
    // for the same reason. The defaults are the one row whose text is already whole at
    // any width, so theirs says instead what the level covers, as the panel does: a
    // tooltip repeating the label it sits on says nothing.
    auto *root = new QTreeWidgetItem(m_treeWidget);
    root->setText(0, tr("Default settings"));
    root->setToolTip(0, tr("Every log with nothing more specific"));
    root->setData(0, kRefRole, refToString(NodeRef{NodeKind::Root, QString()}));

    QVector<QTreeWidgetItem *> patternItems;
    for (const LogPatternNode &n : m_settings.patterns()) {
        // The same name the panel on the right heads itself with — see
        // patternDisplayName(), which is why the quoting rule is stated once.
        QString label = patternDisplayName(n.match);
        if (n.kind == LogPatternNode::Kind::Regex)
            label += tr(" (regex)");
        if (n.matchFullPath)
            label += tr(" (whole path)");
        QTreeWidgetItem *item = makeItem(root, label, NodeRef{NodeKind::Pattern, n.id});
        item->setToolTip(0, label);
        patternItems.append(item);
    }

    // ONE FILE ROW, AND IT IS THE LOG THIS DIALOG WAS OPENED ON. The file level used to
    // list every entry in the store, which grows with every log ever given settings of
    // its own and is not what anyone came here for: the errand is nearly always "this
    // log, the one I have open, is not being read right".
    //
    // The cut is in the WIDGET and nowhere else. Every other entry stays in m_settings,
    // is still resolved on every open, and is still swept by the prune above and by
    // accept()'s — so a log tuned last week still opens the way it was tuned, and an
    // entry that stops saying anything still goes. What is lost is only the ability to
    // reach another log's entry without opening that log.
    //
    // A log NO PATTERN CLAIMS hangs under the root, because that is the level it
    // inherits from. There is no virtual "no matching pattern" parent any more: it was a
    // grouping row, and one row does not need grouping. What it used to explain — that
    // such a log has no level to be promoted to — moved to the panel that log heads (see
    // loadNode()), with the Promote button's tooltip saying it a second time where the
    // question is actually asked.
    if (!m_currentAddress.isEmpty()) {
        const int patternIndex = m_settings.matchingPattern(m_currentAddress);
        QTreeWidgetItem *parent =
            patternIndex >= 0 && patternIndex < patternItems.size()
            ? patternItems.at(patternIndex)
            : root;
        // Named for its ROLE and not for the log, which is the whole point of showing one:
        // there is nothing to tell it apart from, so a file name would only be a shorter
        // way of saying "the file". The log it stands for is named in the panel beside the
        // tree, and its full address is on the tooltip below, as every other row's is.
        auto *item = makeItem(parent, tr("Current file"),
                              NodeRef{NodeKind::File, m_currentAddress});
        item->setToolTip(0, logSourceDisplayPath(m_currentAddress));
    }

    m_treeWidget->expandAll();

    // Reselect by IDENTITY, never by row: a reorder or a pattern edit re-parents file
    // nodes, so the row that held a node before the rebuild holds a different one after.
    const QString wanted = refToString(select);
    QTreeWidgetItem *target = nullptr;
    for (QTreeWidgetItemIterator it(m_treeWidget); *it; ++it) {
        if ((*it)->data(0, kRefRole).toString() == wanted) {
            target = *it;
            break;
        }
    }
    m_treeWidget->setCurrentItem(target ? target : root);

    m_updating = wasUpdating;
    loadNode();
}

PreferencesDialog::NodeRef PreferencesDialog::currentRef() const
{
    if (QTreeWidgetItem *item = m_treeWidget->currentItem())
        return refFromString(item->data(0, kRefRole).toString());
    return NodeRef{};
}

void PreferencesDialog::loadNode()
{
    const NodeRef ref = currentRef();
    const bool wasUpdating = m_updating;
    m_updating = true;

    LogProfile profile;
    bool isPattern = false;
    bool haveProfile = true;

    // Cleared first and filled by whichever branch has something to say, so a node that
    // is not there any more (a pattern id that no longer resolves) heads its panel with
    // nothing rather than with the last node's name.
    setIdentity(QString(), QString(), QString());

    switch (ref.kind) {
    case NodeKind::Root:
        // The defaults ARE the level, so the name says so and the muted line says who
        // that covers — the level naming itself twice would be the caption twice.
        setIdentity(tr("Default settings"), tr("Every log with nothing more specific"),
                    QString());
        m_nodeTitle->clear();
        profile = m_settings.defaults();
        break;
    case NodeKind::Pattern: {
        const int i = m_settings.indexOfPatternId(ref.key);
        if (i < 0) {
            haveProfile = false;
            break;
        }
        isPattern = true;
        const LogPatternNode &n = m_settings.patterns().at(i);
        // Nothing here about precedence between patterns. It is a rule about a case
        // most trees never contain — two patterns matching one log — and it was being
        // stated on every pattern node, where the reader has come to set a format. The
        // ↑ and ↓ tooltips are where it belongs: they are the only thing it governs,
        // and they are read exactly when the question arises.
        m_nodeTitle->setText(tr("What every log matching this pattern opens with."));
        setIdentity(patternDisplayName(n.match), tr("File pattern"), QString());
        m_patternMatch->setText(n.match);
        m_patternKind->setCurrentIndex(m_patternKind->findData(int(n.kind)));
        m_patternCase->setChecked(n.caseSensitive);
        m_patternFullPath->setChecked(n.matchFullPath);
        profile = n.profile;
        break;
    }
    case NodeKind::File: {
        // No "is there a node?" branch any more (M21). The row is drawn because a log is
        // open, and what it shows is m_fileProfile — which always holds something, being
        // either the log's own stored settings or what it inherits. A file row whose node
        // had gone was a state this dialog could reach and can no longer.
        //
        // The log's OWN NAME heads the panel — logSourceDisplayName(), which is what
        // names it everywhere else in the application, so it is recognisable as the tab
        // it belongs to rather than as an address to be read left to right. Its full
        // address stays a hover away, which is where the identical basenames that make a
        // name ambiguous are told apart; tabLabelsFor() is the other answer to that and
        // is not this one's, because a tab label is a statement about the log's
        // NEIGHBOURS in the tab bar and there are none here.
        //
        // No heading under the rule either — there is no rule on a log node, and the
        // identity block above already says which of the three levels this is. With ONE
        // exception, and it is the sentence the virtual "no matching pattern" parent used
        // to carry: with that row gone there is nowhere else to say that this log takes
        // the defaults and has no level to be promoted to, and here is where a reader who
        // selected the row is already looking. The Promote button's tooltip says it too,
        // but a tooltip has to be hunted for and a disabled control is not where anyone
        // hunts.
        m_nodeTitle->clear();
        if (m_settings.matchingPattern(ref.key) < 0) {
            m_nodeTitle->setText(
                tr("No file pattern matches this log, so it takes the default settings "
                   "unless its own say otherwise. Promoting needs a pattern to promote "
                   "to — add one that matches it first."));
        }
        setIdentity(logSourceDisplayName(ref.key), tr("This log only"),
                    logSourceDisplayPath(ref.key));
        profile = m_fileProfile;
        break;
    }
    }

    m_patternGroup->setVisible(isPattern);
    // Empty when the selected node is not there any more — a pattern id that no longer
    // resolves — which setIdentity() cleared at the top and no branch then filled. A
    // heading naming the last node the panel showed would be worse than none.
    m_identityGroup->setVisible(!m_nodeName->text().isEmpty());
    // A heading with no words is a blank line the width of the panel — its margins are
    // still 14 px whatever the text says.
    m_nodeTitle->setVisible(!m_nodeTitle->text().isEmpty());
    // A PATTERN NODE ONLY. The rule earns its place there because the match fields are a
    // row of editable controls sitting above another row of editable controls, and
    // without it the two blocks run together — which is the whole reason it exists. The
    // other levels' identity is two lines of centred text that could not be mistaken for
    // a settings block, and with no heading under the rule either it was just a line
    // cutting the panel in half.
    m_nodeDivider->setVisible(isPattern);
    m_editor->setVisible(haveProfile);
    // Whether what auto-detect made of the sample is a fact about THIS entry's log. Only
    // a concrete-file node naming the log the bytes came from can say so: a pattern node
    // and the defaults are about a class of logs, so a reading taken from whichever file
    // happens to be open is about a different file, printed under this entry's heading.
    m_editor->setSampleBelongsHere(ref.kind == NodeKind::File && !m_currentAddress.isEmpty()
                                   && ref.key == m_currentAddress);
    if (haveProfile)
        m_editor->setProfile(profile);

    m_loaded = ref;
    m_loadedValid = haveProfile;

    refreshPatternValidity();
    updateButtons();
    // A node deleted, renamed or re-homed can be the one an armed request names, so the
    // notice is re-derived wherever the buttons are.
    updateApplyNotice();
    m_updating = wasUpdating;
}

void PreferencesDialog::setIdentity(const QString &name, const QString &level,
                                    const QString &fullAddress)
{
    m_nodeName->setText(name);
    m_nodeLevel->setText(level);
    // A muted line with no words is still a blank line the width of the panel.
    m_nodeLevel->setVisible(!level.isEmpty());
    // On BOTH labels, because either is what a pointer aimed at the heading lands on,
    // and empty on the two levels that name no single log — a tooltip repeating the
    // heading is a tooltip that says nothing.
    m_nodeName->setToolTip(fullAddress);
    m_nodeLevel->setToolTip(fullAddress);
}

int PreferencesDialog::treeContentWidth() const
{
    // A row's text starts one indentation per LEVEL from the left, plus one more for the
    // branch decoration the root items also get (rootIsDecorated is on) — which is the
    // offset the delegate then elides the text within. So a label fits exactly when the
    // column is that offset plus the label's own width, and that is what this returns
    // for the widest row.
    const QFontMetrics fm = m_treeWidget->fontMetrics();
    const int indent = m_treeWidget->indentation();
    int widest = 0;
    for (QTreeWidgetItemIterator it(m_treeWidget); *it; ++it) {
        int level = 1;
        for (QTreeWidgetItem *p = (*it)->parent(); p; p = p->parent())
            ++level;
        widest = qMax(widest, level * indent + fm.horizontalAdvance((*it)->text(0)));
    }
    return widest;
}

void PreferencesDialog::applyInitialSplit()
{
    if (m_splitSettled)
        return;
    m_splitSettled = true;

    // The splitter's own width, which needs the layout to have run — showEvent arrives
    // before the posted layout request is processed, and a split computed against a
    // zero-width splitter is redistributed from nothing by the stretch factors.
    if (QLayout *l = layout())
        l->activate();
    const int total = m_splitter->width() > 0 ? m_splitter->width() : width();

    // What the pane spends on things that are not the label: its frame both sides, the
    // gap it keeps from the right panel, and room for the vertical scrollbar — which is
    // there whenever the tree is longer than the pane, and takes its width off the
    // column when it is.
    const int chrome = 2 * m_treeWidget->frameWidth() + kPanelGap
        + style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, m_treeWidget);
    const int cap = qMax(kTreeMinWidth, int(total * kTreeMaxShare));
    const int wanted = qBound(kTreeMinWidth, treeContentWidth() + chrome, cap);
    m_splitter->setSizes({wanted, qMax(1, total - wanted)});
}

void PreferencesDialog::applyInitialHeight()
{
    // Once, like the split beside it: recomputing on every show would take a height the
    // user had dragged back off them.
    if (m_heightSettled)
        return;
    m_heightSettled = true;

    // What the dialog wants if the editor is not scrolled at all, less what the wrapper
    // now reports for it.
    const int wanted = sizeHint().height() + qMax(0, m_preferredEditorHeight)
        - (m_editorScroll ? m_editorScroll->sizeHint().height() : 0);

    // Clamped to the screen, leaving room for a title bar and a panel: this is precisely
    // the case the scrolling exists for, and asking for more than the screen has would
    // put the button row back off the bottom.
    int available = wanted;
    if (const QScreen *screen = this->screen())
        available = int(screen->availableGeometry().height() * 0.9);
    resize(width(), qBound(minimumSizeHint().height(), wanted, qMax(1, available)));
}

void PreferencesDialog::commitCurrent()
{
    if (!m_loadedValid)
        return;

    const LogProfile p = m_editor->profile();
    switch (m_loaded.kind) {
    case NodeKind::Root:
        m_settings.setDefaults(p);
        break;
    case NodeKind::Pattern: {
        const int i = m_settings.indexOfPatternId(m_loaded.key);
        if (i < 0)
            return;
        LogPatternNode &n = m_settings.patternAt(i);
        n.match = m_patternMatch->text();
        n.kind = static_cast<LogPatternNode::Kind>(m_patternKind->currentData().toInt());
        n.caseSensitive = m_patternCase->isChecked();
        n.matchFullPath = m_patternFullPath->isChecked();
        n.profile = p;
        break;
    }
    case NodeKind::File:
        // Re-tested against its parent once, by fileProfile(), when OK is pressed. Not
        // here: a row vanishing from under the cursor mid-edit, because one field happens
        // to match the pattern above, would be a rebuild nobody asked for.
        m_fileProfile = p;
        break;
    }
}

void PreferencesDialog::refreshPatternValidity()
{
    if (!m_patternGroup->isVisible()) {
        m_patternError->hide();
        return;
    }
    const auto kind = static_cast<LogPatternNode::Kind>(m_patternKind->currentData().toInt());
    if (kind != LogPatternNode::Kind::Regex) {
        m_patternError->hide();
        return;
    }
    const QRegularExpression re(m_patternMatch->text());
    if (re.isValid()) {
        m_patternError->hide();
        return;
    }
    // An invalid expression matches nothing rather than everything, so the cost is a
    // pattern that quietly stops claiming its logs — which is what this says out loud.
    m_patternError->setText(tr("Not a valid regular expression: %1. It matches no log "
                               "until it is fixed.").arg(re.errorString()));
    m_patternError->show();
}

void PreferencesDialog::updateButtons()
{
    const NodeRef ref = currentRef();
    const bool isPattern = ref.kind == NodeKind::Pattern;
    const bool isFile = ref.kind == NodeKind::File;

    m_deleteNode->setEnabled(isPattern || isFile);
    // Two rows, two different things, so the tooltip is chosen here rather than written
    // once at construction. Deleting a pattern removes a row; "deleting" the current
    // file's entry does not, because that row is the whole file level of this tree and
    // the log is still the one that is open — what goes is only what the log said of its
    // own, leaving it following whatever it inherits.
    m_deleteNode->setToolTip(
        isFile ? tr("Clear this log's own settings, so it goes back to following the "
                    "pattern above it, or the default settings. The row stays.")
               : tr("Delete this file pattern. The logs it matched fall back to whichever "
                    "pattern matches them next, or to the default settings."));

    const int patternIndex = isPattern ? m_settings.indexOfPatternId(ref.key) : -1;
    m_moveUp->setEnabled(patternIndex > 0);
    m_moveDown->setEnabled(patternIndex >= 0
                           && patternIndex < m_settings.patterns().size() - 1);

    // Promotion goes up exactly one level, and the button says which level that is: a
    // pattern's parent is the defaults, a log's is the pattern that matched it. A log NO
    // PATTERN CLAIMS is not promotable — its only parent is the defaults, and handing one
    // log's settings to every log in the world is not a level up, it is a level skipped.
    // Its pattern is the thing that should be promoted, once it has one.
    //
    // THAT LAST SENTENCE IS THIS TOOLTIP'S JOB NOW. It used to be the panel prose of the
    // virtual "no matching pattern" row, which is gone with the row; a disabled button
    // still shows its tooltip, so the explanation arrives exactly when the question is
    // asked — which is better than where it was, since nobody selected that heading in
    // order to read it.
    const bool hasParentPattern = isFile && m_settings.matchingPattern(ref.key) >= 0;
    if (isPattern) {
        m_promoteButton->setText(tr("&Promote to Default Settings"));
        m_promoteButton->setToolTip(
            tr("Make these the default settings, which every log with no pattern of its "
               "own opens with. This pattern stays where it is."));
    } else if (isFile && !hasParentPattern) {
        m_promoteButton->setText(tr("&Promote to Parent Pattern"));
        m_promoteButton->setToolTip(
            tr("No file pattern matches this log, so there is no level to promote it to: "
               "its only parent is the default settings, and giving one log's settings to "
               "every log is a level skipped rather than a level up. Add a pattern that "
               "matches it first."));
    } else {
        m_promoteButton->setText(tr("&Promote to Parent Pattern"));
        m_promoteButton->setToolTip(
            tr("Give these settings to the pattern above, so every log it matches gets "
               "them. This log's own entry then has nothing left to say and is removed."));
    }
    m_promoteButton->setEnabled(isPattern || hasParentPattern);

    m_applyButton->setVisible(!m_applyTarget.isEmpty());
    m_applyButton->setEnabled(!m_applyTarget.isEmpty());
    // Down on the node the armed request names, and only there: the button says "these
    // settings are the ones asked for", which is a claim about the node on screen. Moving
    // to another node leaves it up, so pressing it there arms that one instead of reading
    // as a second press on the first. setChecked emits toggled, never clicked, so this
    // cannot re-enter applyToCurrent().
    m_applyButton->setChecked(m_applyRequested
                              && refToString(m_applyNode) == refToString(ref));
}

void PreferencesDialog::selectLog(const QString &address, const std::optional<LogProfile> &own,
                                  const LogProfile &seed)
{
    commitCurrent();
    const QString key = logSettingsKey(address);
    // What the row shows: the log's own stored settings where it has some, and the SEED
    // where it has none. The seed is not the same thing as what the log inherits and
    // cannot be replaced by it — offerFormat() seeds with the pattern autodetection has
    // just guessed, which is deliberately not what the log inherits, that guess being the
    // whole of what the dialog is opened to have confirmed.
    //
    // Whether any of it is STORED is a separate question, asked once by fileProfile() at
    // the end. So a seed equal to what the log inherits shows on screen, can be edited,
    // and leaves nothing behind if it is not.
    m_fileProfile = own.value_or(seed);
    // Set whether or not the log had settings, and never cleared afterwards. This is what
    // the tree lists its one file row for and what the sample's bytes are judged a fact
    // about — two questions with one answer.
    m_currentAddress = key;
    rebuildTree(NodeRef{NodeKind::File, key});
}

std::optional<LogProfile> PreferencesDialog::fileProfile() const
{
    // THE REDUNDANCY RULE, asked once, at the only moment the answer is final. A log gets
    // settings of its own only while they say something the level above does not — so a
    // row left showing exactly what it inherits stores nothing, whether it started that
    // way, was edited back into line, or had its pattern taught what it said.
    //
    // It also answers Promote and Delete without either needing to know: both put
    // m_fileProfile back to inherited(), and this comparison turns that into "the log's
    // entry is removed".
    if (m_currentAddress.isEmpty() || m_fileProfile == m_settings.inherited(m_currentAddress))
        return std::nullopt;
    return m_fileProfile;
}

void PreferencesDialog::setApplyTarget(const QString &name)
{
    m_applyTarget = name;
    // The name goes in the TOOLTIP, not in the label. It used to be the label — "Apply to
    // app.log" — which put a value of unbounded length in a button and took its mnemonic
    // from data, so which letter Alt claimed depended on what happened to be open. The
    // tooltip also says what applying does to the TREE: settings equal to what the log
    // inherits leave it with nothing of its own to say, so its entry goes.
    m_applyButton->setToolTip(
        tr("Ask for %1, the log that is open, to be re-read with these settings when you "
           "press OK. If they match what it would inherit anyway, its own entry is "
           "removed.").arg(name));
    updateButtons();
    updateApplyNotice(); // the notice names the target, which is what this just supplied
}

void PreferencesDialog::addPattern()
{
    const NodeRef ref = currentRef();
    commitCurrent();

    LogPatternNode n;
    // The match text starts EMPTY, and a suggestion is not worth what it costs here.
    // "*.log" — whether typed in as a constant or derived from the selected log's
    // extension — claims every log on the machine the instant the row appears, which is
    // the one thing a new pattern must not do: the settings under it are the defaults
    // or one log's, and they would silently become everything's while the user is still
    // deciding what the pattern is for. An empty pattern matches nothing (LogSettings.h),
    // so the row sits there claiming no logs until it says what it is about.
    //
    // The PROFILE is still seeded from the selected log, which is the useful half of the
    // gesture: "make a pattern out of how this one is read".
    n.profile = ref.kind == NodeKind::File ? m_fileProfile : m_settings.defaults();
    const int i = m_settings.addPattern(n);
    rebuildTree(NodeRef{NodeKind::Pattern, m_settings.patterns().at(i).id});
    m_patternMatch->setFocus();
}

void PreferencesDialog::deleteNode()
{
    const NodeRef ref = currentRef();
    // Nothing to commit: it is about to go.
    m_loadedValid = false;
    switch (ref.kind) {
    case NodeKind::Pattern: {
        const int i = m_settings.indexOfPatternId(ref.key);
        if (i >= 0)
            m_settings.removePattern(i); // its logs re-home; nothing points at it
        // The row is gone, so the selection has nowhere to go but the root.
        rebuildTree(NodeRef{});
        break;
    }
    case NodeKind::File:
        // GIVING UP what was remembered is showing what the log inherits instead — the
        // row itself cannot go, because the log is still the one that is open and taking
        // the only route to its settings away mid-visit is not an answer to the press.
        // fileProfile() then reports nothing stored, so the entry really is removed.
        m_fileProfile = m_settings.inherited(ref.key);
        rebuildTree(ref);
        break;
    case NodeKind::Root:
        break; // the defaults are not a thing that can be deleted
    }
}

void PreferencesDialog::movePattern(int delta)
{
    const NodeRef ref = currentRef();
    if (ref.kind != NodeKind::Pattern)
        return;
    commitCurrent();
    const int i = m_settings.indexOfPatternId(ref.key);
    if (i < 0)
        return;
    m_settings.movePattern(i, delta);
    rebuildTree(ref);
}

void PreferencesDialog::promoteToParent()
{
    const NodeRef ref = currentRef();
    if (ref.kind == NodeKind::Pattern) {
        commitCurrent();
        const int i = m_settings.indexOfPatternId(ref.key);
        if (i < 0)
            return;
        m_settings.setDefaults(m_settings.patterns().at(i).profile);
        // The PATTERN STAYS, which is where this parts company with the file case below.
        // A file node is nothing but settings, so once it says what its parent says it
        // has nothing left to say and goes. A pattern node is also a MATCHER, and its
        // place in the order is what stops a later pattern claiming its logs — deleting
        // it because its settings are now the defaults would silently re-home every log
        // under it. Nothing needs re-pruning either: the file nodes under it inherit
        // from the pattern, which has not changed.
        rebuildTree(ref);
        return;
    }
    if (ref.kind != NodeKind::File)
        return;
    commitCurrent();

    const int patternIndex = m_settings.matchingPattern(ref.key);
    if (patternIndex < 0)
        return;

    m_settings.patternAt(patternIndex).profile = m_fileProfile;
    // The log now says exactly what the pattern says, so its own entry has nothing left
    // to say — which fileProfile()'s comparison notices on its own. Nothing is special-
    // cased here and nothing is removed here; the row goes on showing the same settings,
    // now inherited rather than its own, which is exactly what promoting means.

    rebuildTree(NodeRef{NodeKind::Pattern, m_settings.patterns().at(patternIndex).id});
}

void PreferencesDialog::applyToCurrent()
{
    if (!m_loadedValid) {
        // Nothing to arm it from, so the button must not stay down saying otherwise.
        updateButtons();
        return;
    }
    // Pressed while it was already down, on the node it names: the request is withdrawn.
    // Pressed anywhere else it moves here, which is why this is not a plain toggle of the
    // flag — one node's settings can be asked for while another's are on screen.
    const bool sameNode = m_applyRequested && refToString(m_applyNode) == refToString(m_loaded);
    if (sameNode && !m_applyButton->isChecked()) {
        m_applyRequested = false;
        updateButtons();
        updateApplyNotice();
        return;
    }

    commitCurrent();
    m_applyNode = m_loaded;
    m_applyNodeName = nodeDisplayName(m_loaded);
    // Taken now as well as at accept(), so the getter answers for the request the moment
    // it is armed rather than only after OK.
    m_applyProfile = m_editor->profile();
    m_applyRequested = true;
    // Recorded, not performed, and NOT closing: applying reindexes the log and destroys
    // the Document this dialog's preview is reading, so it waits for OK along with every
    // other edit. MainWindow does it once exec() has returned Accepted.
    updateButtons();
    updateApplyNotice();
}

bool PreferencesDialog::profileOfNode(const NodeRef &ref, LogProfile *out) const
{
    switch (ref.kind) {
    case NodeKind::Root:
        *out = m_settings.defaults();
        return true;
    case NodeKind::Pattern: {
        const int i = m_settings.indexOfPatternId(ref.key);
        if (i < 0)
            return false;
        *out = m_settings.patterns().at(i).profile;
        return true;
    }
    case NodeKind::File:
        if (ref.key != m_currentAddress)
            return false;
        *out = m_fileProfile;
        return true;
    }
    return false;
}

QString PreferencesDialog::nodeDisplayName(const NodeRef &ref) const
{
    switch (ref.kind) {
    case NodeKind::Root:
        return tr("Default settings");
    case NodeKind::Pattern: {
        const int i = m_settings.indexOfPatternId(ref.key);
        // The same name the tree row wears, so the notice cannot come to call a pattern
        // something no other part of the dialog calls it. It follows the COMMIT, as the
        // row does, and not the keystroke the way the heading over the fields does: the
        // heading names the node being edited, while this names one that may well be
        // somewhere else in the tree.
        return i < 0 ? QString() : patternDisplayName(m_settings.patterns().at(i).match);
    }
    case NodeKind::File:
        // THE LOG'S OWN NAME, deliberately, and not the "Current file" its row wears.
        // This feeds the apply notice, which says "the settings for app.log" about a node
        // that may well not be the one on screen — a role word there would name whichever
        // row the reader is looking at instead of the one the request points at.
        return ref.key != m_currentAddress ? QString() : logSourceDisplayName(ref.key);
    }
    return {};
}

void PreferencesDialog::updateApplyNotice()
{
    if (!m_applyRequested || m_applyTarget.isEmpty()) {
        m_applyNotice->clear();
        m_applyNotice->hide();
        return;
    }

    // WHICH settings, and the answer depends on what is on screen. "These settings" is
    // true only while the entry the request names is the one being SHOWN — which is also
    // exactly when the button beside this is down. Navigate away and the same words
    // promise whatever panel is now in front of the reader, so the entry has to be named
    // instead: its own, when the request is on the open log's entry, and otherwise the
    // pattern or the defaults it came from. The name it wore when the request was armed
    // is the fallback for an entry deleted or swept away since — it is still where the
    // settings came from, and there is nothing left in the tree to read a name off.
    const QString shown = nodeDisplayName(m_applyNode);
    const QString from = shown.isEmpty() ? m_applyNodeName : shown;
    const bool displayed = refToString(m_applyNode) == refToString(currentRef());
    const bool ownEntry = m_applyNode.kind == NodeKind::File && m_applyNode.key == m_currentAddress;
    QString text;
    if (displayed || from.isEmpty())
        text = tr("%1 will be re-read with these settings when you press OK.").arg(m_applyTarget);
    else if (ownEntry)
        // Not "the settings for app.log" for the log named app.log two words earlier.
        text = tr("%1 will be re-read with its own settings when you press OK.").arg(m_applyTarget);
    else
        text = tr("%1 will be re-read with the settings for %2 when you press OK.")
                   .arg(m_applyTarget, from);
    m_applyNotice->setText(text);
    m_applyNotice->show();
}

void PreferencesDialog::keyPressEvent(QKeyEvent *event)
{
    // Swallowed rather than forwarded: QDialog reads a bare Return as "click the default
    // button", and a QLineEdit passes Return up after emitting editingFinished — so
    // finishing a field would close the dialog. The commit still happens, because it
    // rides on editingFinished before this is ever reached. A modifier combination is
    // left alone, as is every other key, Escape included.
    const bool bareReturn = (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && !(event->modifiers() & ~Qt::KeypadModifier);
    if (bareReturn) {
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void PreferencesDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    applyInitialHeight();
    // QDialogButtonBox makes its first accept button the dialog's default as it is
    // shown, which draws a ring saying "Enter presses this". It does not, so the ring is
    // cleared once the box has drawn it. autoDefault goes too, or any of these buttons
    // would become the default merely by taking focus.
    for (QPushButton *b : findChildren<QPushButton *>()) {
        b->setAutoDefault(false);
        b->setDefault(false);
    }
    // Once. Every later rebuild leaves the handle where it is, and so does a drag — see
    // applyInitialSplit().
    applyInitialSplit();
}

void PreferencesDialog::accept()
{
    commitCurrent();

    // The armed request names a NODE, so its settings are read again here rather than
    // taken from the copy made when the button went down: the press no longer ends the
    // session, so the node it named may have been edited half a dozen times since. Read
    // BEFORE the prune, and left holding that copy when the node has gone — a pattern
    // deleted or a bulk forget does not withdraw a request, it only removes the entry
    // that would have been consulted for it.
    if (m_applyRequested)
        profileOfNode(m_applyNode, &m_applyProfile);

    // No prune here any more (M21). The open log's entry is decided by fileProfile(),
    // which the caller asks once and which compares against what the log inherits — so a
    // row left saying nothing is still not stored, and commitCurrent() above is still
    // what makes that comparison see the last keystroke. Every OTHER log's entry lives
    // in the pool and is swept by the caller's LogFileStore::pruneAgainst(), which is
    // affordable exactly once per visit and only when the tree actually moved.
    QDialog::accept();
}

} // namespace loftail
