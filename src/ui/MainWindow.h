#pragma once

#include "DocumentContext.h"
#include "FormatSettings.h"
#include "LogView.h"
#include "SshPromptDialogs.h"

#include <QMainWindow>
#include <QString>
#include <QVector>

#include <memory>
#include <optional>
#include <vector>

QT_BEGIN_NAMESPACE
class QAction;
class QDockWidget;
class QLabel;
class QMenu;
class QProgressBar;
class QStackedWidget;
class QTabWidget;
QT_END_NAMESPACE

namespace loftail {

class Document;
class DocumentView;
class LogModel;
struct SessionDocument;
class FilterPane;
class HighlighterPane;
class PresetPane;
class RunPane;
class PaneTitleStyle;

// The application's top-level window. Per-file state lives in Document and the
// machinery around it in DocumentContext; the window holds the context vector and
// a pointer to the ACTIVE VIEW, never a "current file" global (invariant #7).
//
// The window is a DOCUMENT WELL plus docked panes: open files are tabs in the
// central QTabWidget and cannot be dragged out of it; the side panes are the only
// QDockWidgets, so a pane can never land in a document tab group, nor a log in the
// panes' (ARCHITECTURE.md §12.2).
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
    // `rawPath` is a local path or an ssh:// URL in any accepted spelling; it is
    // normalized before it becomes a Document path (RemoteLocation.h).
    void openFile(const QString &rawPath, const QString &pattern = QString());

    // Fill `menu` with what the record at `viewRow` of `view` offers (SPEC.md §5).
    // Public, and split from showRecordMenu(), so a test can inspect and trigger the
    // items without opening a modal menu — the same split LogView's pure geometry
    // helpers are public for. `column` ranks the items; it never changes which ones
    // are there.
    void buildRecordMenu(QMenu *menu, DocumentView *view, int viewRow, int column);

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
    // Open a log on another machine (SPEC.md §3, M11): the Open Remote dialog, and
    // the File ▸ Remote Hosts submenu rebuilt from the saved-host store.
    void chooseRemoteToOpen();
    void refreshRemoteHostsMenu();
    void showFormatDialog();
    // Application-wide settings (M18): today the default log format, previewed against
    // whichever log is open. Unlike every other action here it is always available —
    // it is not about the active document.
    void showPreferences();
    void showColumnMenu(const QPoint &pos);
    // Pop up the record menu where the click was (SPEC.md §5). Built per invocation
    // on the stack, exactly as the column menu is.
    void showRecordMenu(DocumentView *view, int viewRow, int column, const QPoint &globalPos);
    // Recompute the visible subset from the active document's filters and refresh
    // its views + status counts (M4). Wrapped in a model reset.
    void applyActiveFilters();
    // Walk the visible rows for the Find bar's query and move the selection
    // (SPEC.md §5); changes no filter state.
    void runFind(bool forward, bool fromStart);
    // Re-resolve the active document's highlight rules and repaint (M5). Highlighting
    // recolors rows in place, so no model reset — just a viewport update.
    void applyActiveHighlighters();

    // Run selection (SPEC.md §3a). The run-start pattern changed in the Run pane:
    // store it in the format (persist it), reconfigure the document, and re-apply.
    void onRunStartChanged(const QString &pattern, bool regex, bool caseSensitive);
    // A run was chosen in the Run pane (-1 == all runs): restrict the view and set
    // follow state (follow only when viewing the newest run or all runs).
    void onRunSelected(int runIndex);

private:
    // applyActiveFilters() for a NAMED file, so a background file finishing its scan
    // re-applies its own filters without disturbing the active one.
    void applyFiltersFor(DocumentContext *ctx);

    // The log a waiting document has been waiting for has turned up (M13, SPEC.md §3):
    // reopen it, index it, and settle its format from the bytes that have now arrived.
    // Wired to LiveController::resumeRequested — it lives here because the pattern
    // lives here and core must never hold one (invariant #3).
    void resumeWaitingDocument(DocumentContext *ctx);

    // A saved host's remembered password, handed to the transport the only way core will
    // take one: through the per-target cache it already consults (SshPrompter.h). This is
    // what gives HostBookmarkStore::find() a call site at all — it had none in src/ before
    // M14, so a bookmark's password was written and never read back at connect time.
    //
    // Here rather than in SshSession::authenticate() because that would bury an
    // AppConfigLocation read inside a network auth routine, in a class whose header says
    // it knows nothing but a RemoteLocation (ARCHITECTURE.md §6.3.2).
    void primeRemoteCredentials(const QString &path);

    void buildMenus();
    // The exclusive timestamp-display group offered on the Date column's header menu
    // (SPEC.md §4). Built once and owned by the window; showColumnMenu borrows it,
    // because that menu is stack-allocated per invocation.
    void buildTimeDisplayMenu();
    // Apply a mode to the ACTIVE file, routed through applySettings so it persists
    // exactly like a dialog change would.
    void setTimeDisplay(TimeDisplay mode);
    // Point the checkmark at the active file's mode. The mode is per file, so this
    // runs on every active-document change as well as on every menu popup.
    void updateTimeDisplayActions();
    void refreshRecentFilesMenu();
    void rememberRecentFile(const QString &path);
    void updateStatus();
    void updateModelTheme(); // push the light/dark cue into the model (highlighting)

    // Retitle a file's tabs, folding in its indexing progress.
    void updateTabTitles(DocumentContext *ctx);

    // Indexing progress/completion for ONE file. Taken per context rather than as a
    // plain slot because a background file keeps scanning while another is active.
    void onIndexProgress(DocumentContext *ctx, qint64 done, qint64 total);
    void onIndexFinished(DocumentContext *ctx, bool cancelled);

    // Full session persistence (SPEC.md §10, ARCHITECTURE.md §12.4): write the active
    // document's per-file state (format, filters, highlighters, columns) into the
    // `documents` array plus global geometry/pane layout on close; restore it on
    // launch. A missing last file yields an empty view with an inline notice.
    void saveSession();
    void restoreSession();

    // Open `path` under `settings`. When `promptIfNoMatch` and the pattern matches
    // no sample record, the Log Format dialog is offered first (SPEC.md §4). Builds
    // the model/view, starts indexing, and persists the format on a good result.
    // Returns false when the open did not happen — a source that cannot be opened,
    // or a format dialog the user cancelled. On false the previously open document
    // (if any) is left untouched.
    // `runRestore` carries a persisted run selection (session restore only); it is
    // re-resolved once indexing finishes. A normal open passes nullopt and defaults
    // to the newest run (§3a).
    bool openWithSettings(const QString &path, FormatSettings settings, bool promptIfNoMatch,
                          std::optional<RunRestore> runRestore = std::nullopt);
    // Build the model and the indexing controller for `ctx`. No views, no scan.
    void buildContext(DocumentContext *ctx);
    // An interactive open: buildContext plus one view, shown, with the scan started.
    void buildViewAndIndex(DocumentContext *ctx);
    // Session restore's half of an open: build a context from a saved document with
    // NO views (the caller creates one per saved view) and no scan. Returns nullptr
    // if the file cannot be opened.
    DocumentContext *prepareContext(const SessionDocument &d);
    // Build one view onto `ctx`, wire it up, and add its tab. Used both for a file's
    // first view and for further views onto the same file.
    DocumentView *createView(DocumentContext *ctx);
    // Raise `view`'s tab, make it active, and focus its table.
    void showView(DocumentView *view);
    // Window ▸ New View: a second, independently-scrolled view onto the active file.
    void newViewOfActiveDocument();
    // Apply a new format to the ALREADY-OPEN document, choosing the change-cost:
    // pattern/encoding change → full rescan; source-zone change → timestamp reparse;
    // display-zone change → repaint only (§5.1, §6.1).
    void applySettings(const FormatSettings &newSettings);
    void persistFormat(const QString &path, const FormatSettings &s);
    // Save `s` as the default for never-seen files (M18) and refresh m_defaultFormat
    // from what was actually stored. The counterpart to persistFormat(), one level up:
    // that one remembers a file, this one remembers a habit.
    void rememberDefaultFormat(const FormatSettings &s);

    // What came of asking whether a format fits (offerFormat).
    enum class FormatOutcome {
        Matched,  // it fits; nothing was shown
        Chosen,   // it did not, and the user picked one — `settings` was updated
        Declined, // it did not, and nobody chose: cancelled, or nobody to ask
    };
    // Whether `settings`'s pattern matches the bytes `doc` can now read. Asks nothing.
    static bool formatFits(Document *doc, const FormatSettings &settings);
    // formatFits(), and if it does not, the Log Format dialog seeded with M8's
    // autodetection. ONE COPY, shared by an ordinary open and by the first resume of a
    // log that opened waiting — which since M17 is every remote and archived log, so a
    // second copy would mean the prompt behaving differently for local and remote files.
    FormatOutcome offerFormat(Document *doc, const QString &path, FormatSettings *settings);

    // Keep session restore's bulk-prompt mode armed until the last restored remote log
    // has finished connecting, then end it. See the definition for why the restore loop
    // finishing is no longer the right moment.
    void armBulkRestore();

    // Close every open file (window close, and File ▸ Close All).
    void closeAllDocuments();
    // Close the active view's tab; the file itself closes with its last view.
    void closeActiveView();
    // Close the view in tab `index` (its close button, or Ctrl+W).
    void closeViewAt(int index);
    // A view is being destroyed (its tab was closed, or the window is going down):
    // drop it from the bookkeeping and reap its file if that was its last view.
    void onViewDestroyed(QObject *obj);
    // The tab bar moved to another page: that page IS the active view.
    void onCurrentTabChanged(int index);
    // The user dragged a tab: keep m_views in tab order, which is the order the
    // session stores and Ctrl+Tab walks.
    void onTabMoved(int from, int to);
    // The view showing `path`, or nullptr — reopening an open file raises it.
    DocumentView *viewOfPath(const QString &path) const;
    // Rebuild the Window menu's list of open views.
    void refreshWindowMenu();
    // Move the active view one tab forward (or back).
    void cycleView(int delta);

    // Wrap a side pane in its dock, with the areas and features every pane shares,
    // and add it to the right-hand area. Returns the dock, for tabifying.
    QDockWidget *addPaneDock(QWidget *pane, const QString &objectName, const QString &title);
    // Show the "no file open" notice when there are no documents and the tabs
    // otherwise; the two share the central widget through a stack.
    void updateEmptyState();

    // Make `view` the active one: repoint the status bar, title and per-file
    // actions at it, and re-emit activeDocumentChanged when the Document changes.
    void setActiveView(DocumentView *view);
    // Reflect the active context's state in the status bar, progress and actions.
    void updateActionStates();

    // Move the global panes' per-file widget state to/from a context as the active
    // document changes. See DocumentContext::filterState for why this is needed.
    void stashPaneState(DocumentContext *ctx);
    void hydratePanes(DocumentContext *ctx);

    Document        *activeDocument() const;
    DocumentContext *activeContext() const;
    LogView         *activeLogView() const;
    LogModel        *activeModel() const;

    std::vector<std::unique_ptr<DocumentContext>> m_contexts;
    // Every open view, in TAB order — which is also the order the session stores
    // them in, and the order Ctrl+Tab walks. Kept in step with the tab bar, which
    // the user can reorder by dragging.
    QVector<DocumentView *> m_views;
    DocumentView *m_activeView = nullptr;

    QMenu   *m_recentMenu = nullptr;
    QAction *m_openRemoteAction = nullptr;
    QMenu   *m_remoteHostsMenu = nullptr;
    // Answers the questions a remote open asks (host key, password). Owned here
    // because it puts up dialogs parented to this window.
    std::unique_ptr<GuiSshPrompter> m_sshPrompter;
    QMenu   *m_windowMenu = nullptr;  // the open-views list, rebuilt on aboutToShow
    QAction *m_closeTabAction = nullptr;
    QAction *m_closeAllAction = nullptr;
    QAction *m_newViewAction = nullptr; // Window ▸ New View (a second view on one file)
    QAction *m_cancelAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_copyColumnsAction = nullptr;
    QAction *m_formatAction = nullptr;
    QAction *m_reconnectAction = nullptr;
    QAction *m_followAction = nullptr; // View ▸ Follow Tail (return-to-bottom, M6)
    // The timestamp-column header submenu and its five exclusive actions, indexed by
    // TimeDisplay. Owned by the window (see buildTimeDisplayMenu).
    QMenu   *m_timeDisplayMenu = nullptr;
    QAction *m_timeDisplayActions[5] = {};
    QLabel       *m_statusLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;

    FilterPane      *m_filterPane = nullptr;      // M4 filters side pane
    HighlighterPane *m_highlighterPane = nullptr; // M5 highlighters side pane
    PresetPane      *m_presetPane = nullptr;      // M5 presets side pane
    RunPane         *m_runPane = nullptr;         // run selection side pane (§3a)
    QVector<QDockWidget *> m_paneDocks;                     // the four above, for View ▸ Panes
    QDockWidget *m_filtersDock = nullptr;                   // marked while filters are in force
    QDockWidget *m_highlightersDock = nullptr;              // marked while rules are present
    QAction     *m_clearFiltersAction = nullptr;
    // How those docks' title bars are painted (PaneTitleStyle.h). Shared by all four
    // and parented to the window, so it outlives every dock that points at it.
    PaneTitleStyle        *m_paneStyle = nullptr;

    // The centre of the window: the document well. The tabs and the "no file open"
    // notice take turns in the stack, so the centre is never an empty tab frame.
    QStackedWidget *m_centre = nullptr;
    QTabWidget     *m_tabs = nullptr;
    QLabel         *m_placeholder = nullptr;

    // True once a saved pane layout has been applied (or once first-run proportions
    // have been chosen), so the first-open sizing never overrides a restored layout.
    bool m_layoutRestored = false;

    // The format a file loftail has not seen before is tried with (M18, SPEC.md §4) —
    // a saved user setting, falling back to the built-in log4cplus layout. Application
    // scope, not per file: the per-file choice is the FormatCache, which outranks it.
    // Kept in step with DefaultFormatStore by showPreferences() and showFormatDialog().
    FormatSettings m_defaultFormat;
    // The wrap mode new views are created with — a window-wide View-menu choice
    // (SPEC.md §5), not per-file state; each LogView owns its own mode thereafter.
    LogView::WrapMode m_wrapMode = LogView::WrapMode::Off;
};

} // namespace loftail
