#pragma once

#include "LogSettings.h"

#include <QByteArray>
#include <QDialog>
#include <QSet>
#include <QString>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
QT_END_NAMESPACE

namespace loftail {

class LogProfileEditor;
class SectionBox;

// The Preferences dialog (SPEC.md §4): the whole settings tree on the left, the selected
// node's settings on the right.
//
//   Default settings          the root — what a log nothing else matches is tried with
//     *.log                   a file pattern, in precedence order; first match wins
//       /var/log/app.log      one concrete log, with settings of its own
//     Logs with no pattern    a VIRTUAL node with no settings and no editor
//
// The virtual node exists here and nowhere else. "No parent" is the absence of a match,
// not a thing to store, so a row for it in the model would be a row that is not a row —
// which is the objection the pathless default entry drew when it was proposed as a row
// in the old per-file cache. It survives the milestone; it just moves down a level.
//
// Like every other dialog here, this one APPLIES NOTHING. It mutates a working copy of
// the tree; the caller reads tree() after Accepted and does the single write. That is
// what makes Cancel exact — it discards a pattern added, a node deleted, a bulk forget,
// and the scratch node a mid-open invocation created, with no special case for any of
// them. "Apply to Current Log" is no exception: it only records the request, because
// applying reindexes and destroys the very Document this dialog is previewing.
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

    // Select the node for `address`, creating one seeded with `seed` when the log has
    // none yet. That scratch node is pruned again on OK if the user leaves it saying
    // nothing its parent does not already say.
    void selectLog(const QString &address, const LogProfile &seed);

    // Offer "Apply to <name>". Without this the button stays hidden — there is nothing
    // to apply to, which is also the mid-open case, where OK already means "open the
    // log like this".
    void setApplyTarget(const QString &name);

    // The edited tree. Read after exec() returns Accepted.
    const LogSettingsTree &tree() const { return m_settings; }

    // Whether the user asked for the selected node's settings to be put on the current
    // log, and what those settings are. Reported, never acted on.
    bool applyRequested() const { return m_applyRequested; }
    LogProfile applyProfile() const { return m_applyProfile; }

public slots:
    // Overridden to put every per-log node the user touched back through the tree's
    // prune rule before the dialog closes. Public, like QDialog's own.
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
    enum class NodeKind { Root, Pattern, File, Orphan };
    struct NodeRef
    {
        NodeKind kind = NodeKind::Root;
        QString  key; // pattern id, or file address; empty for Root and Orphan
    };
    static QString refToString(const NodeRef &r);
    static NodeRef refFromString(const QString &s);

    void buildUi(const QString &sampleName, const QByteArray &sample);
    void rebuildTree(const NodeRef &select);
    void loadNode();
    void commitCurrent(); // the editors' contents back into m_settings
    void refreshPatternValidity();
    void updateButtons();

    void addPattern();
    void deleteNode();
    void movePattern(int delta);
    void promoteToParent();
    void applyToCurrent();
    void forgetAllPerLogSettings();

    NodeRef currentRef() const;

    LogSettingsTree m_settings;
    QString         m_scratchAddress; // the node selectLog() created, if any
    QSet<QString>   m_touchedFiles;   // file nodes to re-test against their parent on OK

    QString    m_applyTarget;
    bool       m_applyRequested = false;
    LogProfile m_applyProfile;

    QTreeWidget      *m_treeWidget = nullptr;
    QToolButton      *m_addPattern = nullptr;
    QToolButton      *m_deleteNode = nullptr;
    QToolButton      *m_moveUp = nullptr;
    QToolButton      *m_moveDown = nullptr;
    QPushButton      *m_forgetFiles = nullptr;
    QLabel           *m_nodeTitle = nullptr;
    SectionBox       *m_patternGroup = nullptr;
    QComboBox        *m_patternKind = nullptr;
    QLineEdit        *m_patternMatch = nullptr;
    QCheckBox        *m_patternCase = nullptr;
    QCheckBox        *m_patternFullPath = nullptr;
    QLabel           *m_patternError = nullptr;
    QLabel           *m_fileAddress = nullptr;
    LogProfileEditor *m_editor = nullptr;
    QPushButton      *m_applyButton = nullptr;
    QPushButton      *m_promoteButton = nullptr;

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
