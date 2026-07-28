#pragma once

#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QLineEdit;
class QListWidget;
class QPushButton;
QT_END_NAMESPACE

namespace loftail {

class Document;

// M4 — the Filters side pane (SPEC.md §6, §8). A dockable widget, NOT a modal
// dialog: every axis is visible and toggles with a single click. It binds to the
// ACTIVE document by signal (invariant #7): setDocument() rebinds it, never a
// global "current file". It owns no per-file state beyond its own widget selections
// — the authoritative FilterSet lives in the Document; the pane writes into it and
// emits filtersChanged(), which MainWindow turns into a Document::applyFilters()
// wrapped in a model reset.
//
// Subsystem/thread selections are kept as NAMES here (so a manually-entered or
// deselected value survives a rebuild) and resolved to interned quint32 ids in
// buildFilterSet() via InternTable::idOf — the predicate compares ids, not strings
// (invariant #4).
class FilterPane : public QWidget
{
    Q_OBJECT

public:
    explicit FilterPane(QWidget *parent = nullptr);

    // Rebind to a document (or nullptr to clear). Repopulates the auto-discovered
    // subsystem/thread lists from its intern tables and enables the time/thread
    // axes according to whether the format carries those fields.
    void setDocument(Document *document);

    // Refresh the auto-discovered lists from the document's intern tables — called
    // when indexing progresses/finishes so newly-seen subsystems/threads appear
    // (SPEC.md §6: the lists are discovered as the file is scanned).
    void refreshDiscoveredLists();

    // Re-render the time-range editors from the document's stored UTC ms bounds in
    // the CURRENT display zone. Call after the display zone moves (a timestamp
    // display-mode change, SPEC.md §4): the editors hold wall clock, so leaving the
    // digits alone would silently re-point the bounds at a different instant.
    void refreshTimeBounds();

    // Portable, name-based snapshot of the pane's filter state (SPEC.md §9, §10).
    // Used for both filter presets (create-from-current-state / apply) and per-file
    // session restore. Carries subsystem/thread NAMES, never interned ids, so it is
    // portable across files and a re-index. restoreState() applies a snapshot and
    // emits filtersChanged() so the caller recomputes the visible set.
    QJsonObject saveState() const;
    void restoreState(const QJsonObject &state);

signals:
    // Emitted whenever any control changes. MainWindow rebuilds the Document's
    // FilterSet from buildFilterSet() and reapplies inside a model reset.
    void filtersChanged();

private:
    void buildUi();
    // Write the current control state into the bound Document's FilterSet, resolving
    // names to ids and display-zone times to UTC ms. No-op without a document.
    void applyToDocument();

    // Repopulate one checkable list from `names`, preserving prior checked state for
    // names still present and any manually-added names.
    // `seen` accumulates every name this list has ever shown: a name missing from
    // it is newly discovered and starts checked, so an enabled-by-default axis does
    // not hide subsystems that appear later in the scan.
    void populateList(QListWidget *list, const QStringList &names,
                      const QSet<QString> &checked, const QSet<QString> &manual,
                      QSet<QString> &seen);
    // True when every entry is ticked, i.e. the axis selects everything and so
    // narrows nothing. An empty list counts as all-checked.
    static bool allChecked(const QListWidget *list);
    QSet<QString> checkedNames(const QListWidget *list) const;
    void setAllChecked(QListWidget *list, bool checked);
    void invertChecked(QListWidget *list);
    void narrowList(QListWidget *list, const QString &needle);

    Document *m_document = nullptr;
    bool      m_populating = false; // guards itemChanged storms during repopulation

    // Priority
    QCheckBox *m_priorityEnable = nullptr;
    QComboBox *m_priorityCombo = nullptr;

    // Subsystem
    QCheckBox   *m_loggerEnable = nullptr;
    QLineEdit   *m_loggerNarrow = nullptr;
    QListWidget *m_loggerList = nullptr;
    QLineEdit   *m_loggerManual = nullptr;
    QSet<QString> m_loggerManualNames; // manually-added subsystems (may be absent)
    QSet<QString> m_loggerSeen;        // every subsystem name ever listed

    // Thread
    QCheckBox   *m_threadEnable = nullptr;
    QLineEdit   *m_threadNarrow = nullptr;
    QListWidget *m_threadList = nullptr;
    QLineEdit   *m_threadManual = nullptr;
    QSet<QString> m_threadManualNames;
    QSet<QString> m_threadSeen;

    // Message text
    QCheckBox *m_textEnable = nullptr;
    QLineEdit *m_textEdit = nullptr;
    QCheckBox *m_textRegex = nullptr;
    QCheckBox *m_textCase = nullptr;
    QCheckBox *m_textNegate = nullptr;

    // Time range
    QCheckBox     *m_timeEnable = nullptr;
    QDateTimeEdit *m_timeStart = nullptr;
    QDateTimeEdit *m_timeEnd = nullptr;
};

} // namespace loftail
