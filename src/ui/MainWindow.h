#pragma once

#include "FormatSettings.h"
#include "LogView.h"

#include <QMainWindow>
#include <QString>

#include <memory>
#include <vector>

QT_BEGIN_NAMESPACE
class QAction;
class QLabel;
class QMenu;
class QProgressBar;
class QVBoxLayout;
QT_END_NAMESPACE

namespace loftail {

class Document;
class LogModel;
class IndexController;
class LiveController;
class FilterPane;
class HighlighterPane;
class PresetPane;
class FindBar;

// The application's top-level window. M2b brings the open-file UI (dialog,
// drag-and-drop, recent files), the production LogView, and worker-thread indexing
// with a progress/cancel indicator. Per-file state lives in Document; the window
// holds the one-element document vector and an active pointer (invariant #7), never
// a "current file" global.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Open `path`. Its format is recalled from the per-file cache if seen before
    // (SPEC.md §4); otherwise `pattern` (or a common log4cplus default) is tried,
    // and the Log Format dialog is offered when that pattern does not match. Safe
    // to call repeatedly; it replaces the open document.
    void openFile(const QString &path, const QString &pattern = QString());

signals:
    // Panes bind to the active document by signal, not by construction (invariant
    // #7, ARCHITECTURE.md §12.3). Emitted with the newly-active Document (or
    // nullptr when the last one closes).
    void activeDocumentChanged(Document *document);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    // Re-resolve the highlight theme (light vs dark palette) when it changes so the
    // model resolves the right variant of each palette slot (SPEC.md §7).
    void changeEvent(QEvent *event) override;

private slots:
    void chooseFileToOpen();
    void showFormatDialog();
    void onIndexProgress(qint64 done, qint64 total);
    void onIndexFinished(bool cancelled);
    void showColumnMenu(const QPoint &pos);
    // Recompute the visible subset from the active document's filters and refresh
    // the view + status counts (M4). Wrapped in a model reset.
    void applyActiveFilters();
    // Walk the visible rows for the Find bar's query and move the selection
    // (SPEC.md §5); changes no filter state.
    void runFind(bool forward, bool fromStart);
    // Re-resolve the active document's highlight rules and repaint (M5). Highlighting
    // recolors rows in place, so no model reset — just a viewport update.
    void applyActiveHighlighters();

private:
    void buildMenus();
    void refreshRecentFilesMenu();
    void rememberRecentFile(const QString &path);
    void teardownDocument();
    void updateStatus();
    void updateModelTheme(); // push the light/dark cue into the model (highlighting)

    // Full session persistence (SPEC.md §10, ARCHITECTURE.md §12.4): write the active
    // document's per-file state (format, filters, highlighters, columns) into the
    // `documents` array plus global geometry/pane layout on close; restore it on
    // launch. A missing last file yields an empty view with an inline notice.
    void saveSession();
    void restoreSession();

    // Open `path` under `settings`. When `promptIfNoMatch` and the pattern matches
    // no sample record, the Log Format dialog is offered first (SPEC.md §4). Builds
    // the model/view, starts indexing, and persists the format on a good result.
    void openWithSettings(const QString &path, FormatSettings settings, bool promptIfNoMatch);
    // Build the model + view + controller for the active document and start the scan.
    void buildViewAndIndex(const QString &path);
    // Apply a new format to the ALREADY-OPEN document, choosing the change-cost:
    // pattern/encoding change → full rescan; source-zone change → timestamp reparse;
    // display-zone change → repaint only (§5.1, §6.1).
    void applySettings(const FormatSettings &newSettings);
    void persistFormat(const QString &path, const FormatSettings &s);

    Document *activeDocument() const;

    std::vector<std::unique_ptr<Document>> m_documents;
    int m_activeIndex = -1;

    LogModel        *m_model = nullptr;
    LogView         *m_view = nullptr;
    IndexController *m_controller = nullptr;
    LiveController  *m_live = nullptr; // M6 watch-and-append loop (starts post-scan)

    QMenu   *m_recentMenu = nullptr;
    QAction *m_cancelAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_copyColumnsAction = nullptr;
    QAction *m_formatAction = nullptr;
    QAction *m_followAction = nullptr; // View ▸ Follow Tail (return-to-bottom, M6)
    QLabel       *m_statusLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;

    FilterPane      *m_filterPane = nullptr;      // M4 filters side pane
    HighlighterPane *m_highlighterPane = nullptr; // M5 highlighters side pane
    PresetPane      *m_presetPane = nullptr;      // M5 presets side pane
    FindBar         *m_findBar = nullptr;         // M4 find bar
    QVBoxLayout     *m_centralLayout = nullptr;   // holds [LogView, placeholder, FindBar]
    QLabel          *m_placeholder = nullptr;     // shown when no file / last file missing

    QString m_defaultPattern;
    // The format choice for the active document (SPEC.md §4). Held here as UI
    // configuration for the single active document; the source of truth across
    // sessions is the per-file FormatCache. The pattern never reaches the view,
    // filters, or highlighters (invariant #3).
    FormatSettings m_currentSettings;
    LogView::WrapMode m_wrapMode = LogView::WrapMode::Off;
};

} // namespace loftail
