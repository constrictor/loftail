#pragma once

#include "LogSettings.h"

#include <QByteArray>
#include <QDialog>
#include <QString>

#include <optional>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSplitter;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
QT_END_NAMESPACE

namespace loftail {

class LogProfileEditor;
class MessageLabel;

// The Preferences dialog (SPEC.md §4): the whole settings tree on the left, the selected
// node's settings on the right.
//
//   Default settings          the root — what a log nothing else matches is tried with
//     *.log                   a file pattern, in precedence order; first match wins
//       Current file          the log that is open, under whichever level claims it
//
// ONE per-log row, and it is the log this dialog was opened on. Every other entry in the
// store is still resolved on an open and still swept — it is simply not listed, because
// the file level had become a list of logs nobody was looking at, and the errand here is
// nearly always "this log, the one I have in front of me, is not being read right". A log
// no pattern claims hangs directly under the root, which is the level it inherits from;
// the absence of a match is a row's POSITION and not a virtual parent standing in for it.
//
// A LOG ENTRY LASTS ONLY AS LONG AS IT SAYS SOMETHING ITS PATTERN DOES NOT, and since
// M21 that is ONE comparison in ONE place: fileProfile(), asked by the caller when OK is
// pressed. Delete and Promote to Parent Pattern both put the row back to what the log
// inherits, and the comparison turns that into "the entry is removed" without either
// gesture removing anything.
//
// THE ROW IS A FACT AND NOT A FIXTURE. It exists because a log is open, so nothing can
// take it away mid-visit and nothing has to keep putting it back. It used to be a node in
// the tree that rebuildTree() spared from a sweep and re-created whenever a gesture had
// removed it; the file level living in its own store (LogFileStore.h) retires both halves.
// The sweep of every OTHER log — which this dialog never lists, and which is needed
// because a pattern edit changes what those logs inherit while nothing writes them — is
// the caller's LogFileStore::pruneAgainst(), run once per visit and only when the tree
// actually moved.
//
// Like every other dialog here, this one APPLIES NOTHING. It mutates a working copy of
// the tree; the caller reads tree() after Accepted and does the single write. That is
// what makes Cancel exact — it discards a pattern added, a node deleted, and the node a
// mid-open invocation created for the log, with no special case for any of them. "Apply
// to current file" is no exception: it only records the request, because applying
// reindexes and destroys the very Document this dialog is previewing.
//
// AND SO IT DOES NOT CLOSE THE DIALOG EITHER. It used to call accept(), which is how the
// recorded request got carried out — but that made it the one button on a panel of
// in-place edits whose press ended the session, standing next to Promote, which does not.
// It is now a CHECKABLE button: pressing it arms the request, pressing it again disarms
// it, and OK is what performs it along with every other edit. Deferring costs nothing,
// because the caller only reads applyRequested() after Accepted — a request left armed
// when the user cancels is discarded with the working copy, exactly like the rest.
class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    // `sample` is the leading bytes of whichever log the preview should run over, or
    // EMPTY when there is none; `sampleName` names it in the preview caption.
    PreferencesDialog(const LogSettingsTree &tree,
                      const QString &sampleName,
                      const QByteArray &sample,
                      QWidget *parent = nullptr);

    // Show `address` as the one per-log row. `own` is what the log has stored, or
    // nullopt when it has nothing; `seed` is what the row shows in that case — which is
    // deliberately NOT what the log inherits, since offerFormat() seeds with the pattern
    // autodetection has just guessed and that guess is the whole of what the dialog is
    // opened to have confirmed.
    void selectLog(const QString &address, const std::optional<LogProfile> &own,
                   const LogProfile &seed);

    // Offer "Apply to current file", naming `name` in its tooltip. Without this the
    // button stays hidden — there is nothing to apply to, which is also the mid-open
    // case, where OK already means "open the log like this".
    void setApplyTarget(const QString &name);

    // The edited tree — the defaults and the ordered pattern list. Read after exec()
    // returns Accepted.
    const LogSettingsTree &tree() const { return m_settings; }

    // What the open log should have stored of its own afterwards, or nullopt when it
    // should have nothing — which is the redundancy rule, asked once, at the only moment
    // the answer is final. Read beside tree() after Accepted; the caller writes both, to
    // two stores (M21: the tree is one file, a log's own settings are one file each).
    //
    // This is also how Promote and Delete finish. Both put the row back to what the log
    // inherits, and this comparison is what turns that into "the log's entry is removed"
    // — so neither needs a removal of its own and neither can forget one.
    std::optional<LogProfile> fileProfile() const;

    // Whether the user asked for a node's settings to be put on the current log, and what
    // those settings are. Reported, never acted on. The profile is re-read from the node
    // the request named as accept() runs, so a request armed early and gone on editing
    // afterwards carries what the node finally says rather than a snapshot of it.
    bool applyRequested() const { return m_applyRequested; }
    LogProfile applyProfile() const { return m_applyProfile; }

public slots:
    // Overridden to fold the panel's last keystroke back into the node it belongs to, and
    // to re-read an armed apply request from the node it names. Public, like QDialog's own.
    void accept() override;

protected:
    // ENTER DOES NOT PRESS OK HERE. This dialog is mostly text fields — a conversion
    // pattern, a match expression, a run-start string — and Return is how one finishes
    // editing a field, which QDialog otherwise reads as "press the default button" and
    // closes on. Escape is deliberately untouched: cancelling still abandons the open
    // (SPEC.md §4), which tst_openflow drives with Esc.
    void keyPressEvent(QKeyEvent *event) override;
    // Clears the default-button ring QDialogButtonBox puts on OK when it is shown, so
    // nothing on screen promises an Enter behaviour that keyPressEvent has removed.
    void showEvent(QShowEvent *event) override;

private:
    // What a tree row points at. Held in Qt::UserRole as a string, so a rebuild can
    // reselect by identity rather than by row — a reorder re-parents file nodes, and a
    // row index means something different afterwards.
    enum class NodeKind { Root, Pattern, File };
    struct NodeRef
    {
        NodeKind kind = NodeKind::Root;
        QString  key; // pattern id, or file address; empty for Root
    };
    static QString refToString(const NodeRef &r);
    static NodeRef refFromString(const QString &s);

    void buildUi(const QString &sampleName, const QByteArray &sample);
    void rebuildTree(const NodeRef &select);
    void loadNode();
    // Fill the identity block: the node's name, the level it sits at, and the full
    // address a name that is only a name stands for (empty for the levels with none).
    void setIdentity(const QString &name, const QString &level, const QString &fullAddress);
    void commitCurrent(); // the editors' contents back into m_settings
    // The width the tree's own longest row asks for, indentation included — what the
    // initial split is derived from, so the pane opens wide enough to say what it has
    // to say rather than at a fraction somebody guessed.
    int treeContentWidth() const;
    // Give the tree that width, ONCE, and never again: the first show is the last moment
    // at which nobody has an opinion about where the handle goes. See m_splitSettled.
    void applyInitialSplit();
    void refreshPatternValidity();
    void updateButtons();
    // What is still to happen when OK is pressed, said out loud. A request that leaves no
    // mark on screen is worse than one that closes the dialog: the press then has no
    // observable effect at all until the log is re-read some seconds later.
    void updateApplyNotice();

    // A node's profile and its display name, read back from the tree — both false/empty
    // when the node is no longer there, which an armed request can outlive (its pattern
    // deleted, its entry pruned, a bulk forget).
    bool profileOfNode(const NodeRef &ref, LogProfile *out) const;
    QString nodeDisplayName(const NodeRef &ref) const;

    void addPattern();
    void deleteNode();
    void movePattern(int delta);
    void promoteToParent();
    void applyToCurrent();

    NodeRef currentRef() const;

    LogSettingsTree m_settings;
    // THE LOG THIS DIALOG WAS OPENED ON, in logSettingsKey() form, and the answer to two
    // questions that turn out to be one. WHETHER there is a per-log row at all, and which
    // log it stands for — every other log's entry lives in the pool, untouched and
    // unlisted. And whether a claim about the previewed bytes is a claim about this
    // entry's log.
    //
    // Set by selectLog(), the call both of MainWindow's entry points make, and never
    // cleared while the dialog lives — Delete and Promote included, which is what makes
    // the row a fact rather than something the two of them can take away.
    QString         m_currentAddress;

    // What that one row SHOWS: the open log's own stored settings, or what it inherits.
    // Always meaningful while m_currentAddress is set, which is why loadNode()'s File
    // branch no longer has a "the node is gone" case and updateButtons() no longer has to
    // ask whether one exists. Whether any of it is STORED is fileProfile()'s question,
    // asked once, at the end.
    LogProfile      m_fileProfile;

    QString    m_applyTarget;
    bool       m_applyRequested = false;
    LogProfile m_applyProfile;
    // WHICH node the armed request names, so accept() can read its settings again rather
    // than trust the copy taken when the button was pressed. Held as a NodeRef and not as
    // an index, for the reason the tree's selection is: a pattern edited or reordered
    // moves rows about, and an index would then name a different node.
    NodeRef    m_applyNode;
    // What that node was called when the request was armed, so the notice can still say
    // where the settings came from after the entry itself has gone.
    QString    m_applyNodeName;

    QSplitter        *m_splitter = nullptr;
    // Whether the split has been settled — by the first show, or by the user dragging the
    // handle. Either way nothing computes it again: a pane that re-derived its width on
    // every rebuild would take the handle back off the user the next time they added a
    // pattern, and one that re-derived it on a long pattern would jump as it was typed.
    bool              m_splitSettled = false;
    QTreeWidget      *m_treeWidget = nullptr;
    QToolButton      *m_addPattern = nullptr;
    QToolButton      *m_deleteNode = nullptr;
    QToolButton      *m_moveUp = nullptr;
    QToolButton      *m_moveDown = nullptr;
    QLabel           *m_nodeTitle = nullptr;
    // The identity block every node wears: its own NAME, and under that, muted, which of
    // the three levels it sits at. ONE block filled three ways rather than a block per
    // kind — that is what keeps the three panels one shape, and its absence is how the
    // model's word for a per-log entry ("Concrete file") came to sit over an address, in
    // the heading, with the thing it named underneath it.
    QWidget          *m_identityGroup = nullptr;
    QLabel           *m_nodeName = nullptr;
    QLabel           *m_nodeLevel = nullptr;
    QWidget          *m_patternGroup = nullptr;
    QComboBox        *m_patternKind = nullptr;
    QLineEdit        *m_patternMatch = nullptr;
    QCheckBox        *m_patternCase = nullptr;
    QCheckBox        *m_patternFullPath = nullptr;
    QLabel           *m_patternError = nullptr;
    // The rule between what the node IS (its identity block and, for a pattern, the
    // fields defining it) and what
    // it GIVES its logs. Shown only when there is something above it to cut off.
    QWidget          *m_nodeDivider = nullptr;
    LogProfileEditor *m_editor = nullptr;
    QPushButton      *m_applyButton = nullptr;
    QPushButton      *m_promoteButton = nullptr;
    MessageLabel     *m_applyNotice = nullptr;

    // Saved and RESTORED, never forced false. rebuildTree() drops the current item,
    // which re-enters loadNode() through currentItemChanged, and forcing the flag down
    // on the way out of that would unguard the rest of the rebuild — the shape of the
    // bug that once switched every highlight rule off from the second one on.
    bool m_updating = false;
    // The node whose contents the editors currently hold, so commitCurrent() knows
    // where to put them back. Not read from the tree widget: by the time a selection
    // has changed, the widget names the NEW node.
    NodeRef m_loaded;
    bool    m_loadedValid = false;
};

} // namespace loftail
