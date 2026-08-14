#include "PreferencesDialog.h"

#include "LogProfileEditor.h"
#include "RemoteLocation.h"
#include "UiColors.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

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

// What the node title keeps above and below itself. The top gap is the larger of the
// two on purpose: it separates the heading from the dialog's edge, while below it the
// first section's own title row already supplies air.
constexpr int kTitleTopGap = 10;
constexpr int kTitleBottomGap = 4;

// The air a node's identity/settings rule keeps on each side of itself.
constexpr int kDividerGap = 6;

// The three glyphs on the tree's toolbar. NOT translated, exactly as the Filters pane's
// are not: the words live on setToolTip and setAccessibleName beside them. Add is the
// fourth button and wears words instead — see where it is built.
constexpr auto kRemoveGlyph = "\xE2\x88\x92"; // U+2212 MINUS SIGN
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

// A panel caption: centred, bold, and standing off whatever is above it. Both captions
// in this dialog come from here, so "the same look" is one line of code rather than two
// that agree today.
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
    case NodeKind::Orphan:  return QStringLiteral("orphan");
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
    } else if (s == QLatin1String("orphan")) {
        r.kind = NodeKind::Orphan;
    }
    return r;
}

PreferencesDialog::PreferencesDialog(const LogSettingsTree &tree,
                                     const QString &sampleName,
                                     const QByteArray &sample,
                                     QWidget *parent)
    : QDialog(parent), m_settings(tree)
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
    splitter->setObjectName(QStringLiteral("settingsSplitter")); // findChild, for tests
    splitter->setChildrenCollapsible(false);

    // --- left: the tree and what can be done to it -----------------------------------
    auto *left = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, kPanelGap, 0);

    m_treeWidget = new QTreeWidget(left);
    m_treeWidget->setObjectName(QStringLiteral("settingsTree")); // findChild, for tests
    m_treeWidget->setHeaderHidden(true);
    m_treeWidget->setRootIsDecorated(true);
    m_treeWidget->setUniformRowHeights(true);
    connect(m_treeWidget, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *, QTreeWidgetItem *) { loadNode(); });
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
    // mnemonic: this dialog has already claimed A, C, D, E, F, P and S, and a duplicate
    // letter turns Alt into a focus cycle rather than a shortcut.
    m_addPattern = new QToolButton(left);
    m_addPattern->setObjectName(QStringLiteral("addPatternButton")); // findChild, for tests
    m_addPattern->setText(tr("Add Pattern"));
    m_addPattern->setToolTip(tr("Add a file pattern"));
    m_deleteNode = makeToolButton(left, kRemoveGlyph, QStringLiteral("deleteNodeButton"),
                                  tr("Delete the selected pattern or log"));
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

    m_forgetFiles = new QPushButton(tr("&Forget Individual Files"), left);
    m_forgetFiles->setObjectName(QStringLiteral("forgetFormatsButton")); // findChild, for tests
    m_forgetFiles->setToolTip(
        tr("Delete every per-log entry, so each log falls back to its pattern or the defaults"));
    connect(m_forgetFiles, &QPushButton::clicked, this,
            &PreferencesDialog::forgetAllPerLogSettings);
    leftLayout->addWidget(m_forgetFiles);

    // --- right: what the selected node says ------------------------------------------
    auto *right = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(kPanelGap, 0, 0, 0);

    // The line that says what the selected node IS, so it reads as this panel's heading
    // rather than as the first of its labels. Flush to the top edge and hard left it
    // looked like a stray sentence that had lost its control.
    m_nodeTitle = makeCaption(right, QStringLiteral("nodeTitleLabel"));

    // "Matches" is the OTHER caption, not another section. It says which logs a pattern
    // claims, which is not one of the settings those logs open with — and dressed as a
    // section (flat frame, title divider, exactly like File format and Display) that is
    // precisely what it read as. So: a plain container, captioned the same way the node
    // title is, from the same helper so the two cannot drift apart.
    //
    // SectionBox::setHeading() looked like the answer and is not: its `font-weight: bold`
    // lives in a QGroupBox::title style-sheet rule, and neither Breeze nor Fusion draws
    // the title bold from it — measured on a render, the caption came out at normal
    // weight in both while the same sheet's `subcontrol-position: top center` was obeyed.
    m_patternGroup = new QWidget(right);
    m_patternGroup->setObjectName(QStringLiteral("patternGroup")); // findChild, for tests
    auto *patternBox = new QVBoxLayout(m_patternGroup);
    patternBox->setContentsMargins(0, 0, 0, 0);
    patternBox->addWidget(makeCaption(m_patternGroup, QStringLiteral("patternCaption"),
                                      tr("Matches")));
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

    m_patternError = new QLabel(m_patternGroup);
    m_patternError->setObjectName(QStringLiteral("patternErrorLabel")); // findChild, for tests
    m_patternError->setWordWrap(true);
    m_patternError->setStyleSheet(
        QStringLiteral("color: %1;").arg(errorColor(palette()).name()));
    m_patternError->hide();
    patternForm->addRow(m_patternError);

    // Live validity only; the value is committed when focus leaves or the node changes,
    // because every commit rebuilds the tree (a pattern edit re-homes file nodes).
    connect(m_patternMatch, &QLineEdit::textChanged, this,
            &PreferencesDialog::refreshPatternValidity);
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

    // A log node's identity block, built exactly as the pattern's is: the caption says
    // what kind of node this is, and the one thing that identifies it — the address —
    // sits directly under it. Loose at the panel's top edge the address was a path with
    // nothing saying why it was there, and it had no gap above it either, which the
    // caption's own margins now supply.
    m_fileGroup = new QWidget(right);
    m_fileGroup->setObjectName(QStringLiteral("fileGroup")); // findChild, for tests
    auto *fileBox = new QVBoxLayout(m_fileGroup);
    fileBox->setContentsMargins(0, 0, 0, 0);
    fileBox->setSpacing(0); // "directly under it" — the caption's own bottom margin is the gap
    fileBox->addWidget(makeCaption(m_fileGroup, QStringLiteral("fileCaption"),
                                   tr("Concrete file")));

    m_fileAddress = new QLabel(m_fileGroup);
    m_fileAddress->setObjectName(QStringLiteral("fileAddressLabel")); // findChild, for tests
    m_fileAddress->setWordWrap(true);
    // Centred under the caption it belongs to, and still selectable: an address is the
    // one thing here somebody copies out, to paste into a terminal or another window.
    m_fileAddress->setAlignment(Qt::AlignHCenter);
    m_fileAddress->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_fileAddress->setStyleSheet(
        QStringLiteral("color: %1;").arg(mutedColor(palette()).name()));
    fileBox->addWidget(m_fileAddress);
    rightLayout->addWidget(m_fileGroup);

    // WHICH logs, then WHAT they get. The "Matches" box says which logs a pattern node
    // claims, and it is not a setting those logs open with — sitting under the heading
    // and above the format it read as the first of them, which is exactly what it is
    // not. So it goes first, the heading introduces what follows it rather than what is
    // above it, and a full-width rule marks where the identity of the node stops and its
    // settings begin. The line is drawn only when there is something above it to cut off
    // — the defaults have no identity block, and a rule against the panel's top edge
    // separates nothing.
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

    m_editor = new LogProfileEditor(right);
    m_editor->setObjectName(QStringLiteral("profileEditor")); // findChild, for tests
    m_editor->setPreviewCaption(
        sample.isEmpty() ? tr("Preview (open a log to see these settings applied to it):")
                         : tr("Preview (%1, split into fields):").arg(sampleName));
    m_editor->setSample(sample);
    rightLayout->addWidget(m_editor, 1);

    auto *actionRow = new QHBoxLayout;
    m_applyButton = new QPushButton(tr("&Apply to current file"), right);
    m_applyButton->setObjectName(QStringLiteral("applyToCurrentButton")); // findChild, for tests
    m_applyButton->hide();
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

    m_treeWidget->clear();

    auto makeItem = [](QTreeWidgetItem *parent, const QString &text, const NodeRef &ref) {
        auto *item = new QTreeWidgetItem(parent);
        item->setText(0, text);
        item->setData(0, kRefRole, refToString(ref));
        return item;
    };

    auto *root = new QTreeWidgetItem(m_treeWidget);
    root->setText(0, tr("Default settings"));
    root->setData(0, kRefRole, refToString(NodeRef{NodeKind::Root, QString()}));

    QVector<QTreeWidgetItem *> patternItems;
    for (const LogPatternNode &n : m_settings.patterns()) {
        // Quoted, so a row reads as the pattern it is rather than as a file that happens
        // to be spelt oddly — the rows under it are real names, and `*.log` beside
        // `app.log` at the same indentation is one glance away from looking like one. The
        // empty case stays unquoted: it is prose about a pattern that says nothing yet,
        // and a pair of quotes round nothing is a row wearing an empty name.
        QString label = n.match.isEmpty() ? tr("(empty pattern)")
                                          : QStringLiteral("\"%1\"").arg(n.match);
        if (n.kind == LogPatternNode::Kind::Regex)
            label += tr(" (regex)");
        if (n.matchFullPath)
            label += tr(" (whole path)");
        patternItems.append(makeItem(root, label, NodeRef{NodeKind::Pattern, n.id}));
    }

    // The virtual parent, created only when something actually needs one — otherwise it
    // is a row that says "none of these", about nothing.
    QTreeWidgetItem *orphan = nullptr;
    for (const LogFileNode &f : m_settings.files()) {
        const auto res = m_settings.resolve(f.path);
        QTreeWidgetItem *parent = nullptr;
        if (res.patternIndex >= 0 && res.patternIndex < patternItems.size()) {
            parent = patternItems.at(res.patternIndex);
        } else {
            if (!orphan) {
                orphan = new QTreeWidgetItem(root);
                orphan->setText(0, tr("Logs with no matching pattern"));
                orphan->setData(0, kRefRole,
                                refToString(NodeRef{NodeKind::Orphan, QString()}));
                // Nothing to edit and nothing to store: it is the ABSENCE of a match.
                orphan->setFlags(orphan->flags() & ~Qt::ItemIsSelectable);
            }
            parent = orphan;
        }
        auto *item = makeItem(parent, logSourceDisplayName(f.path),
                              NodeRef{NodeKind::File, f.path});
        item->setToolTip(0, logSourceDisplayPath(f.path));
    }

    m_treeWidget->expandAll();

    // Reselect by IDENTITY, never by row: a reorder or a pattern edit re-parents file
    // nodes, so the row that held a node before the rebuild holds a different one after.
    const QString wanted = refToString(select);
    QTreeWidgetItem *target = nullptr;
    for (QTreeWidgetItemIterator it(m_treeWidget); *it; ++it) {
        if ((*it)->data(0, kRefRole).toString() == wanted
            && ((*it)->flags() & Qt::ItemIsSelectable)) {
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

    switch (ref.kind) {
    case NodeKind::Root:
        m_nodeTitle->setText(
            tr("Used by default when file name doesn't match any particular pattern."));
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
        m_patternMatch->setText(n.match);
        m_patternKind->setCurrentIndex(m_patternKind->findData(int(n.kind)));
        m_patternCase->setChecked(n.caseSensitive);
        m_patternFullPath->setChecked(n.matchFullPath);
        profile = n.profile;
        break;
    }
    case NodeKind::File: {
        const int i = m_settings.indexOfFile(ref.key);
        if (i < 0) {
            haveProfile = false;
            break;
        }
        // No heading: "Concrete file" above the address already says which of the three
        // levels this is, and a second bold line under the rule saying the same thing at
        // greater length is the caption twice. The pattern and the defaults keep theirs,
        // because "Matches" and "Default settings" name the node without saying what it
        // then does for the logs it claims.
        m_nodeTitle->clear();
        m_fileAddress->setText(logSourceDisplayPath(m_settings.files().at(i).path));
        profile = m_settings.files().at(i).profile;
        break;
    }
    case NodeKind::Orphan:
        haveProfile = false;
        m_nodeTitle->setText(tr("Logs matched by no file pattern. They take the defaults, "
                                "unless they carry settings of their own."));
        break;
    }

    m_patternGroup->setVisible(isPattern);
    m_fileGroup->setVisible(ref.kind == NodeKind::File);
    // A heading with no words is a blank line the width of the panel — its margins are
    // still 14 px whatever the text says.
    m_nodeTitle->setVisible(!m_nodeTitle->text().isEmpty());
    // A PATTERN NODE ONLY. The rule earns its place there because "Matches" is a row of
    // editable controls sitting above another row of editable controls, and without it
    // the two blocks run together — which is the whole reason it exists. A log node's
    // identity is two lines of centred text that could not be mistaken for a settings
    // block, and with no heading under the rule either it was just a line cutting the
    // panel in half. The defaults and the virtual "no matching pattern" row have no
    // identity block at all, and a rule under nothing separates the panel from its edge.
    m_nodeDivider->setVisible(isPattern);
    m_editor->setVisible(haveProfile);
    // Whether what auto-detect made of the sample is a fact about THIS entry's log. Only
    // a concrete-file node naming the log the bytes came from can say so: a pattern node
    // and the defaults are about a class of logs, so a reading taken from whichever file
    // happens to be open is about a different file, printed under this entry's heading.
    m_editor->setSampleBelongsHere(ref.kind == NodeKind::File && !m_sampleAddress.isEmpty()
                                   && ref.key == m_sampleAddress);
    if (haveProfile)
        m_editor->setProfile(profile);

    m_loaded = ref;
    m_loadedValid = haveProfile;

    refreshPatternValidity();
    updateButtons();
    m_updating = wasUpdating;
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
    case NodeKind::File: {
        const int i = m_settings.indexOfFile(m_loaded.key);
        if (i < 0)
            return;
        m_settings.fileAt(i).profile = p;
        // Re-tested against its parent on OK. Not here: a node vanishing from under the
        // cursor mid-edit, because one field happens to match the pattern above, would
        // be a rebuild nobody asked for.
        m_touchedFiles.insert(m_loaded.key);
        break;
    }
    case NodeKind::Orphan:
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

    const int patternIndex = isPattern ? m_settings.indexOfPatternId(ref.key) : -1;
    m_moveUp->setEnabled(patternIndex > 0);
    m_moveDown->setEnabled(patternIndex >= 0
                           && patternIndex < m_settings.patterns().size() - 1);

    // Promotion goes up exactly one level, and the button says which level that is: a
    // pattern's parent is the defaults, a log's is the pattern that matched it. A log
    // under the VIRTUAL node is still not promotable — its only parent is the defaults,
    // and handing one log's settings to every log in the world is not a level up, it is
    // a level skipped. Its pattern is the thing that should be promoted, once it has one.
    if (isPattern) {
        m_promoteButton->setText(tr("&Promote to Default Settings"));
        m_promoteButton->setToolTip(
            tr("Make these the default settings, which every log with no pattern of its "
               "own opens with. This pattern stays where it is."));
    } else {
        m_promoteButton->setText(tr("&Promote to Parent Pattern"));
        m_promoteButton->setToolTip(
            tr("Give these settings to the pattern above, so every log it matches gets "
               "them. This log's own entry then has nothing left to say and is removed."));
    }
    m_promoteButton->setEnabled(
        isPattern || (isFile && m_settings.resolve(ref.key).patternIndex >= 0));

    m_applyButton->setVisible(!m_applyTarget.isEmpty());
    m_applyButton->setEnabled(!m_applyTarget.isEmpty() && ref.kind != NodeKind::Orphan);

    m_forgetFiles->setEnabled(!m_settings.files().isEmpty());
}

void PreferencesDialog::selectLog(const QString &address, const LogProfile &seed)
{
    commitCurrent();
    const QString key = logSettingsKey(address);
    if (m_settings.indexOfFile(key) < 0) {
        // insertFileProfile, not setFileProfile: the node must EXIST to be selected and
        // edited, even when the seed happens to equal what the log already inherits.
        // accept() puts it back through the prune rule, so nothing that says nothing
        // survives OK.
        m_settings.insertFileProfile(key, seed);
        m_scratchAddress = key;
    }
    m_touchedFiles.insert(key);
    m_sampleAddress = key;
    rebuildTree(NodeRef{NodeKind::File, key});
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
        tr("Put these settings on %1, the log that is open. If they match what it would "
           "inherit anyway, its own entry is removed.").arg(name));
    updateButtons();
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
    n.profile = ref.kind == NodeKind::File ? m_settings.resolve(ref.key).profile
                                           : m_settings.defaults();
    const int i = m_settings.addPattern(n);
    rebuildTree(NodeRef{NodeKind::Pattern, m_settings.patterns().at(i).id});
    m_patternMatch->setFocus();
}

void PreferencesDialog::deleteNode()
{
    const NodeRef ref = currentRef();
    // Nothing to commit: it is about to go.
    m_loadedValid = false;
    if (ref.kind == NodeKind::Pattern) {
        const int i = m_settings.indexOfPatternId(ref.key);
        if (i >= 0)
            m_settings.removePattern(i); // its logs re-home; nothing points at it
    } else if (ref.kind == NodeKind::File) {
        m_settings.removeFile(ref.key);
        m_touchedFiles.remove(ref.key);
        if (m_scratchAddress == ref.key)
            m_scratchAddress.clear();
    } else {
        return;
    }
    rebuildTree(NodeRef{});
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

    const int patternIndex = m_settings.resolve(ref.key).patternIndex;
    const int fileIndex = m_settings.indexOfFile(ref.key);
    if (patternIndex < 0 || fileIndex < 0)
        return;

    const LogProfile p = m_settings.files().at(fileIndex).profile;
    m_settings.patternAt(patternIndex).profile = p;
    // The log now says exactly what the pattern says, so its own entry has nothing left
    // to say. setFileProfile is what notices that; it is not a special case here.
    m_settings.setFileProfile(ref.key, p);
    m_touchedFiles.remove(ref.key);
    if (m_scratchAddress == ref.key)
        m_scratchAddress.clear();

    rebuildTree(NodeRef{NodeKind::Pattern, m_settings.patterns().at(patternIndex).id});
}

void PreferencesDialog::applyToCurrent()
{
    if (!m_loadedValid)
        return;
    commitCurrent();
    m_applyProfile = m_editor->profile();
    m_applyRequested = true;
    // Recorded, not performed: applying reindexes the log and destroys the Document this
    // dialog's preview is reading. MainWindow does it once exec() has returned.
    accept();
}

void PreferencesDialog::forgetAllPerLogSettings()
{
    // Destructive, so it asks — but it only edits the working copy, so Cancel undoes it
    // like every other edit here. It also does not reach into open tabs: they keep the
    // settings they are displaying, which is why the wording is about what opening a log
    // does from now on rather than about what is on screen.
    const auto answer = QMessageBox::question(
        this, tr("Forget Individual Files"),
        tr("Forget the settings remembered for every log?\n\n"
           "From now on, opening a log will use the file pattern that matches it, or the "
           "default settings. Logs already open are not affected."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    m_loadedValid = false;
    m_settings.clearFiles();
    m_touchedFiles.clear();
    m_scratchAddress.clear();
    rebuildTree(NodeRef{});
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
    // QDialogButtonBox makes its first accept button the dialog's default as it is
    // shown, which draws a ring saying "Enter presses this". It does not, so the ring is
    // cleared once the box has drawn it. autoDefault goes too, or any of these buttons
    // would become the default merely by taking focus.
    for (QPushButton *b : findChildren<QPushButton *>()) {
        b->setAutoDefault(false);
        b->setDefault(false);
    }
}

void PreferencesDialog::accept()
{
    commitCurrent();

    // Every per-log node the user touched — including the scratch one a mid-open
    // invocation created — goes back through the prune rule, so an entry that ended up
    // saying nothing its parent does not already say is not kept. Nodes nobody touched
    // are left exactly as they were loaded.
    if (!m_scratchAddress.isEmpty())
        m_touchedFiles.insert(m_scratchAddress);
    for (const QString &key : std::as_const(m_touchedFiles)) {
        const int i = m_settings.indexOfFile(key);
        if (i >= 0)
            m_settings.setFileProfile(key, m_settings.files().at(i).profile);
    }

    QDialog::accept();
}

} // namespace loftail
