#pragma once

#include "DocumentContext.h"
#include "FormatSettings.h"
#include "LogSettingsStore.h"
#include "LogView.h"
#include "SshPromptDialogs.h"

#include <QElapsedTimer>
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
class QSystemTrayIcon;
class QTabWidget;
class QTimer;
QT_END_NAMESPACE

namespace loftail {

class Document;
class DocumentView;
class LogModel;
struct SessionDocument;
class FilterPane;
class HighlighterPane;
class PresetPane; // built only under LOFTAIL_HAVE_PRESETS; declaring it always is free
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

    // Open `path`. Its settings come from the most specific level of the settings tree
    // that names it — its own entry, the first file pattern that matches, or the
    // defaults (SPEC.md §4) — unless `pattern` overrides the conversion pattern.
    // Preferences is offered when what resolved cannot parse the file. Safe to call
    // repeatedly; it adds a tab.
    // `rawPath` is a local path or an ssh:// URL in any accepted spelling; it is
    // normalized before it becomes a Document path (RemoteLocation.h).
    // Returns false when the open was REFUSED and reported — a malformed address, a
    // dependency not built in, a format dialog the user cancelled. A log that is
    // merely not there yet opens a waiting tab and is a success (SPEC.md §3).
    bool openFile(const QString &rawPath, const QString &pattern = QString());

    // Open every one of them, in the order given, each in its own tab (SPEC.md §3).
    // The one place several addresses are opened at once: the command line, File ▸ Open,
    // a drop of several files, and several members picked out of one archive all come
    // through here, so they agree on what happens when one of the set is refused —
    // the rest still open, and the refusals are reported TOGETHER in one message
    // rather than each overwriting the last. The last one that opened is the tab left
    // in front, exactly as it is when they are opened one at a time.
    // Returns true when every one of them opened.
    bool openFiles(const QStringList &rawPaths, const QString &pattern = QString());

    // Fill `menu` with what the record at `viewRow` of `view` offers (SPEC.md §5).
    // Public, and split from showRecordMenu(), so a test can inspect and trigger the
    // items without opening a modal menu — the same split LogView's pure geometry
    // helpers are public for. `column` ranks the items; it never changes which ones
    // are there.
    void buildRecordMenu(QMenu *menu, DocumentView *view, int viewRow, int column);

    // Put one settings node onto the log that is open: its wrap mode into the tree and
    // the live views, then its format through applySettings(), which re-reads the log in
    // place when the pattern or the encoding moved (SPEC.md §4).
    //
    // Public for the reason buildRecordMenu() and aboutText() are: this is what
    // Preferences ▸ OK calls when "Apply to current file" was ticked, and a test can drive
    // it without a modal dialog on screen. It is the one entry point — the dialog itself
    // applies nothing, it only records the request.
    void applyProfileToActive(const LogProfile &p);

    // What Help ▸ About shows (SPEC.md §1 "Which build this is"): which release this
    // is, and which build of it. Public and separate from showAbout() for the same
    // reason buildRecordMenu() is separate from showRecordMenu() — a test can read the
    // text without a modal dialog on screen, which is the only part worth pinning.
    static QString aboutText();

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
    // The settings tree (M20): the defaults, the file patterns and the per-log entries,
    // previewed against whichever log is open. Unlike every other action here it is
    // always available — it is not about the active document, even though it opens on
    // the active document's entry when there is one.
    void showPreferences();
    // Help ▸ About: aboutText() in a message box. Always available, like Preferences
    // and for a sharper reason — "which build am I running" is a question asked of a
    // window that has failed to open anything.
    void showAbout();
    void showColumnMenu(const QPoint &pos);
    // Pop up the record menu where the click was (SPEC.md §5). Built per invocation
    // on the stack, exactly as the column menu is.
    void showRecordMenu(DocumentView *view, int viewRow, int column, const QPoint &globalPos);
    // What double-clicking a cell does (SPEC.md §5): the record menu's own *Show Only*
    // item for the column that was clicked — Subsystem and Thread, the two axes a cell
    // names a value of — and nothing at all for every other column. It goes through
    // buildRecordMenu() and triggers the item by object name rather than editing a
    // filter itself, so the gesture cannot acquire behaviour the menu lacks, and a
    // record or a format that cannot speak for the axis (which offers no item) makes it
    // do nothing with no second gate to keep in step.
    void activateRecordColumn(DocumentView *view, int viewRow, int column);
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
    // A run was chosen in the Run pane (RunPane::kAllRuns == all runs, kLastRun ==
    // follow the last one): restrict the view and set follow state (follow only when
    // viewing the newest run or all runs).
    void onRunSelected(int runIndex);

private:
    // A live append may have started a NEW run, and a document following the last one
    // (SPEC.md §3a) then has to move onto it. Nothing about that is an append: the
    // previous run's records leave the view wholesale, so this is the same re-apply an
    // explicit run switch does, just with nobody having clicked anything. A no-op —
    // and one integer compare — on every tick that starts no run, which is all of them
    // but the restart.
    void followLastRunIfMoved(DocumentContext *ctx);
    // Whether a filter re-apply keeps every view of the file where it was (SPEC.md §6)
    // or leaves the caller to position it. Yes is the default because a filter edit is
    // the case the feature exists for. No is for the two callers that position EVERY
    // view explicitly on the next line — opening at the file's end, and picking a run:
    // anchoring first would measure a wrapped selection and scroll to a place the very
    // next statement overwrites, which is one wasted pass and one visible jump.
    enum class KeepPosition { Yes, No };

    // applyActiveFilters() for a NAMED file, so a background file finishing its scan
    // re-applies its own filters without disturbing the active one.
    void applyFiltersFor(DocumentContext *ctx, KeepPosition keep = KeepPosition::Yes);

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
    // "Fit to Contents" and "Reset Widths" on the column header menu (SPEC.md §5).
    // Owned by the window for the same reason the timestamp submenu is — showColumnMenu
    // builds its QMenu on the stack — and they act on the ACTIVE view, which is the only
    // view whose header a right-click can land on.
    void buildColumnWidthActions();
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

    // Retitle a file's tabs, folding in its indexing progress and its unseen-match
    // marker.
    void updateTabTitles(DocumentContext *ctx);

    // Decide what every open log is CALLED (TabLabels.h) and retitle whatever moved.
    // Called when the set of open logs changes — an open, a restore, a close — and
    // never on the ingest path: a label is a statement about a log's neighbours, so it
    // cannot change while they stand still, and rewriting a QTabBar entry relays the
    // whole bar out.
    void relabelTabs();

    // --- Highlight actions beyond colour (M19, SPEC.md §7) -------------------------

    // Recompute one file's digest subset and republish it to its model. A wholesale
    // ordinal remap, so it is bracketed by a model reset exactly as applyFiltersFor()
    // is — the contrast with applyActiveHighlighters()'s bare repaint is the point.
    void rebuildDigestFor(DocumentContext *ctx);

    // What a finished ingest tick's matches mean for the tab marker and the
    // notification. Called from the `ingested` handler ABOVE its
    // `ctx != activeContext()` early return — the background tab is the only case the
    // marker exists for, and below that return it would never fire in real use.
    void handleAlerts(DocumentContext *ctx);

    // Clear `ctx`'s unseen-match marker if the log is now genuinely being looked at
    // (its tab is current AND the window is in front). Edge-triggered: retitling a tab
    // relays out the whole bar, so it must not run on every activation event.
    void clearUnseenMatch(DocumentContext *ctx);

    // Is this log being looked at right now? The gate for setting the marker and for
    // raising a notification at all.
    bool isBeingRead(const DocumentContext *ctx) const;

    // Create the tray icon when some open file has an enabled rule carrying Notify, and
    // destroy it when none does. QSystemTrayIcon::showMessage is a silent no-op unless
    // the icon is SHOWN, so "notify" necessarily means "loftail puts an icon in the
    // tray" — which is a visible thing to do to a user's desktop and is therefore
    // scoped to the feature rather than created at startup and left there.
    void updateTrayPresence();
    bool anyRuleWantsNotifications() const;
    // Whether this desktop offers notifications at all. False on a stock GNOME/Wayland
    // session — the reference desktop — so the degrade path is the COMMON path there,
    // not a corner: the pane says so and a Notify rule behaves as if it carried Tab.
    static bool notificationsAvailable();

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

    // --- Log text size (SPEC.md §5, ARCHITECTURE.md §7.1.5) ------------------------
    // One size for the application, not per view and not per log: it answers a question
    // about the reader's eyes, which does not change between two tabs, and a log's
    // settings node (M20) holds how a log is READ, which a font size is no part of. So
    // it lives in Fonts.h and is remembered in QSettings, not in the session — the
    // session describes THIS window's tabs, and a second window would disagree with it.
    //
    // setLogFontSize() is the single funnel: the menu items, Ctrl+wheel and the restore
    // all go through it, so nothing can change the size without every open view, digest
    // strips included, being re-fonted and the setting written.
    void setLogFontSize(int points);
    void stepLogFontSize(int steps);
    void applyLogFontToViews();
    // Say what the size is now, transiently. The bounds clamp silently, and a key held
    // down at 32 pt with no answer at all reads as a broken shortcut.
    void announceLogFontSize();

    // Open `path` under `settings`. When `promptIfNoMatch` and the pattern matches
    // no sample record, Preferences is offered first (SPEC.md §4). Builds
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
    // The scanning half of buildContext(): the IndexController and its two connections.
    // Separate because a RELOAD rebuilds exactly this — stopWorkers() destroys the
    // controller, while the model and the digest model are what the live views hold and
    // must therefore survive.
    void buildIndexController(DocumentContext *ctx);
    // Arm the live watch over `ctx`. Reached from the end of a successful scan, and again
    // from a reload, which destroyed the previous one along with the index worker.
    void startWatching(DocumentContext *ctx);
    // View ▸ Reload (F5): re-read the active log from the beginning, with the format it
    // already has (SPEC.md §3 "Reloading by hand"). Keeps the tab, its views, the filters,
    // the highlight rules and the run-start pattern; the scan runs on the worker thread,
    // so a large log shows progress instead of freezing the window. A WAITING document is
    // poked rather than reloaded — there is nothing to re-read, and tearing its workers
    // down would stop the only thing making progress.
    void reloadActiveDocument();

    // Whether a rebuild re-reads through the format the document already has, or settles
    // a new one from ctx->settings first.
    enum class KeepFormat { Yes, No };
    // THE one way an open document is rebuilt in place: F5, and a pattern or encoding
    // change. The document, the tab and the views survive; only the source, the index and
    // (for KeepFormat::No) the compiled format are replaced. One function because the two
    // callers differ in a single call and share everything that is easy to get wrong —
    // the worker teardown order, the model-reset bracket, re-arming the watch on BOTH
    // outcomes, and handing the scan to the worker thread.
    void rebuildDocument(DocumentContext *ctx, KeepFormat keep);
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
    // pattern/encoding change → re-read the log in place through the new format
    // (rebuildDocument, §6.6); source-zone change → timestamp reparse; display-zone
    // change → repaint only (§5.1, §6.1). The document, its tab and its views survive
    // all three.
    void applySettings(const FormatSettings &newSettings);
    // Write `s` into the settings tree as this log's own node — or DELETE that node,
    // when `s` is exactly what the log would inherit anyway (LogSettingsTree::
    // setFileProfile). The tree is saved only when it actually changed.
    void persistFormat(const QString &path, const FormatSettings &s);
    // Save `s` as the tree's root defaults: what a log nothing else matches is tried
    // with. The counterpart to persistFormat(), two levels up — that one remembers a
    // log, this one remembers a habit.
    void rememberDefaultFormat(const FormatSettings &s);

    // What came of asking whether a format fits (offerFormat).
    enum class FormatOutcome {
        Matched,  // it fits; nothing was shown
        Chosen,   // it did not, and the user picked one — `settings` was updated
        Declined, // it did not, and nobody chose: cancelled, or nobody to ask
    };
    // Whether `settings`'s pattern matches the bytes `doc` can now read. Asks nothing.
    static bool formatFits(Document *doc, const FormatSettings &settings);
    // formatFits(), and if it does not, Preferences seeded with M8's
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
    // Enable View ▸ Clear Filters exactly while there is something for it to clear.
    // Its own function rather than a line in updateActionStates(), because the answer
    // moves on two unrelated events — the active tab changing and the bound pane's
    // filters changing — and the second of those does not run the rest of that
    // function. The condition is asked of FilterPane::hasActiveFilters(), the very
    // question the Filters tab's own marker is drawn from, so the menu item and the
    // dot cannot drift apart.
    void updateClearFiltersState();

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
    // Owned by the WINDOW, not by the menu: triggering it rebuilds that menu, and
    // QMenu::clear() deletes the actions the menu owns — which would be this one,
    // mid-signal.
    QAction *m_clearRecentAction = nullptr;
    QAction *m_openRemoteAction = nullptr;
    QMenu   *m_remoteHostsMenu = nullptr;
    // Answers the questions a remote open asks (host key, password). Owned here
    // because it puts up dialogs parented to this window.
    std::unique_ptr<GuiSshPrompter> m_sshPrompter;
    QMenu   *m_windowMenu = nullptr;  // the open-views list, rebuilt on aboutToShow
    QAction *m_closeTabAction = nullptr;
    QAction *m_closeAllAction = nullptr;
    QAction *m_newViewAction = nullptr; // Window ▸ New View (a second view on one file)
    QAction *m_copyAction = nullptr;
    QAction *m_copyColumnsAction = nullptr;
    QAction *m_selectAllAction = nullptr; // Edit ▸ Select All (every record IN VIEW)
    QAction *m_reconnectAction = nullptr;
    QAction *m_reloadAction = nullptr;
    QAction *m_followAction = nullptr; // View ▸ Follow Tail (return-to-bottom, M6)
    // The timestamp-column header submenu and its five exclusive actions, indexed by
    // TimeDisplay. Owned by the window (see buildTimeDisplayMenu).
    QMenu   *m_timeDisplayMenu = nullptr;
    QAction *m_timeDisplayActions[5] = {};
    // Column widths, offered on the same header menu (see buildColumnWidthActions).
    QAction *m_fitColumnsAction = nullptr;
    QAction *m_resetColumnWidthsAction = nullptr;
    QLabel       *m_statusLabel = nullptr;
    // The scan indicator and its stop button, shown and hidden as one: cancelling is
    // only offered while there is a scan to cancel, so the two are never apart.
    QWidget      *m_progressBox = nullptr;
    QProgressBar *m_progressBar = nullptr;

    FilterPane      *m_filterPane = nullptr;      // M4 filters side pane
    HighlighterPane *m_highlighterPane = nullptr; // M5 highlighters side pane
#if defined(LOFTAIL_HAVE_PRESETS)
    PresetPane      *m_presetPane = nullptr;      // M5 presets side pane (a presets build only)
#endif
    RunPane         *m_runPane = nullptr;         // run selection side pane (§3a)
    QVector<QDockWidget *> m_paneDocks;                     // the panes above, for View ▸ Panes
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

    // THE SETTINGS TREE (M20, SPEC.md §4): the defaults, the ordered file patterns and
    // the per-log nodes, resolved on every open. Read once in the constructor — after
    // the one-time migration off the two QSettings stores it replaced and BEFORE
    // restoreSession(), which now resolves through it rather than carrying its own copy
    // of each document's format.
    LogSettingsStore m_settingsStore{LogSettingsStore::defaultDir()};
    LogSettingsTree  m_logSettings;
    // View ▸ Line Wrap. Held so the checked entry can be made to track the ACTIVE view,
    // which matters now that the mode a log opens in is its own (M20) rather than one
    // window-wide choice: each action carries its WrapMode in QAction::data().
    QActionGroup *m_wrapGroup = nullptr;
    // View ▸ Line Wrap ▸ Toggle. Off <-> Always On, and it TRIGGERS one of the three
    // mode actions above rather than setting the mode itself — there is one path that
    // sets wrap, and it is the one that also remembers the choice for the log.
    QAction *m_toggleWrapAction = nullptr;

    // --- Notification surface (M19) ------------------------------------------------
    // Created only while some rule asks for one; see updateTrayPresence().
    QSystemTrayIcon *m_tray = nullptr;
    // Releases a backlog AlertPolicy suppressed. Needed because `ingested` only fires
    // on a tick that produced records: a burst followed by silence would otherwise
    // leave its suppressed matches unreported forever. Runs only while m_tray exists.
    QTimer          *m_alertPump = nullptr;
    // Monotonic, and the only clock AlertPolicy ever sees — so the policy itself stays
    // testable with a plain counter.
    QElapsedTimer    m_alertClock;
    // The log whose notification is currently on screen, so clicking it raises the
    // right tab. Qt gives a message no identity, so only the most recent can be
    // honoured — which the rate limiter makes a distinction without a difference.
    DocumentContext *m_lastNotified = nullptr;
};

} // namespace loftail
