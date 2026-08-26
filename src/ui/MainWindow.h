#pragma once

#include "DocumentContext.h"
#include "FormatSettings.h"
#include "LogFileStore.h"
#include "LogSettingsStore.h"
#include "LogView.h"
#include "SshPromptDialogs.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QPair>
#include <QString>
#include <QVector>

#include <memory>
#include <optional>
#include <vector>

QT_BEGIN_NAMESPACE
class QAction;
class QDockWidget;
class QFrame;
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
class ConfigView;
class DocumentView;
class LogModel;
struct SessionDocument;
class FilterPane;
class HighlighterPane;
class PresetPane; // built only under LOFTAIL_HAVE_PRESETS; declaring it always is free
class RunPane;
class PaneTitleStyle;
class PreferencesDialog;
// Why the log on screen was re-read (LiveController.h). Forward-declared rather than
// included: it is at namespace scope precisely so this header need not learn about the
// live controller, which it otherwise knows only by name.
enum class ReloadCause;

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
    // Preferences is offered when what resolved cannot parse the file, INCLUDING when
    // what resolved is `pattern`: an overriding pattern wins over every level but is
    // still judged against the log, and is remembered for it only if it fits or the
    // user corrects it in the dialog (SPEC.md §3). Safe to call repeatedly; it adds a
    // tab.
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

    // Open (or raise) an editor tab on an already-resolved config address (SPEC.md §4).
    // Returns null only for a refusal decided with NO I/O — a dependency that is not
    // built in, or a file that is there and cannot be read; a remote address returns a
    // tab that is still connecting.
    //
    // Public for the reason openFile() is: it is the seam a test drives, and the claims
    // that matter about it — that the tab is up before the far end answers, and that
    // closing it does not wait — cannot be made through a menu item without a live host.
    ConfigView *openConfigAt(const QString &address);

    // Fill `menu` with what the record at `viewRow` of `view` offers (SPEC.md §5).
    // Public, and split from showRecordMenu(), so a test can inspect and trigger the
    // items without opening a modal menu — the same split LogView's pure geometry
    // helpers are public for. `column` ranks the items; it never changes which ones
    // are there.
    void buildRecordMenu(QMenu *menu, DocumentView *view, int viewRow, int column);

    // File ▸ Restart App (Ctrl+R). Runs the active log's restart script and shows what it
    // did (SPEC.md §4); with none configured, explains and offers Preferences.
    //
    // Public for the reason openConfigAt() and buildRecordMenu() are: it is the seam a
    // test drives, and the claims that matter — that a clean run closes itself, that a
    // hanging one is abortable, that a log with no script is not a refusal — cannot be
    // made through a menu item.
    void restartActiveApp();

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

    // The other open LOGS, in tab order, one entry per file however many tabs it has —
    // what View ▸ Copy Highlighters from Another Log… offers (SPEC.md §7).
    //
    // Public and split from the picker for buildRecordMenu()'s reason: a test can read
    // what is on offer with no modal on screen, which is the only way to state that a
    // log open in two tabs is offered once.
    QVector<DocumentContext *> otherLogContexts() const;

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
    // Esc and Shift+Esc walk the active log's filter history (SPEC.md §6).
    //
    // HERE AND NOT ON A QAction, however much a menu shortcut would be the idiom on this
    // window. A window-scoped action's shortcut is dispatched BEFORE the focus widget's
    // keyPressEvent — the precedence LogView::keyPressEvent already records for Ctrl+A —
    // and the Find bar closes itself in FindBar::keyPressEvent, so registering Escape
    // would take that away, and take Escape off an open QMenu with it. Reaching Escape
    // only after everybody with a claim on it has declined is exactly what an override
    // here gives, with nothing to coordinate. It also means an Esc pressed with the caret
    // still in the Filters pane arrives, the docks being children of this window.
    void keyPressEvent(QKeyEvent *event) override;

private:
    // Which of the two filter chords (SPEC.md §5) a cell click was. A plain parameter
    // type and never a signal argument, so nothing needs Q_ENUM registration.
    enum class RecordFilterCommand { ShowOnly, Hide };

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
    // What the two filter chords do (SPEC.md §5): Ctrl+Alt+click is *Show Only* on a
    // Subsystem or Thread cell and the level floor on a Priority one; Alt+click is
    // *Hide* on the two value axes, and nothing on Priority, whose axis is a minimum
    // rather than a set. Same route as activateRecordColumn() above and for the same
    // reasons — the record menu's own item, triggered by object name, so the gating, the
    // persistence and the place on the log's filter history are the menu's rather than a
    // second copy of each.
    void applyRecordFilter(DocumentView *view, int viewRow, int column,
                           RecordFilterCommand command);
    // Build the record menu for this cell and trigger the item with this object name,
    // if it has one. The shared tail of both gestures above: an item the menu left out
    // because the record cannot speak for its axis simply is not found, which is what
    // makes every gesture inert exactly where the menu is with no gate of its own.
    void triggerRecordMenuItem(DocumentView *view, int viewRow, int column,
                               const char *objectName);
    // Recompute the visible subset from the active document's filters and refresh
    // its views + status counts (M4). Wrapped in a model reset.
    void applyActiveFilters();
    // Walk the visible rows for the Find bar's query and move the selection
    // (SPEC.md §5); changes no filter state.
    void runFind(bool forward, bool fromStart);
    // Turn the Find bar's standing query into a highlight rule on the message-text axis
    // and append it (SPEC.md §5, §7). The window's job rather than the view's because a
    // rule belongs to the DOCUMENT and is edited in the Highlighters pane (invariant #7).
    //
    // It reads the query, the regex flag and the case option off the same bar runFind()
    // does, so the rule and the search that prompted it cannot disagree about what they
    // match; and it reports into that bar's own label, which is the only surface a
    // gesture made there has (the window's status label is rewritten on every ingest
    // tick — ARCHITECTURE.md §7.1.3).
    void highlightFind();
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

    // There are bytes to read that there were not before: either the log a waiting
    // document has been waiting for has turned up (M13, SPEC.md §3), or a log that
    // opened EMPTY has just been written to for the first time. Reopen it, index it, and
    // settle its format and encoding from the bytes that have now arrived — and only
    // now decide whether that format fits, persist it, or ask about it, because until
    // this moment there was nothing to decide it against.
    //
    // Wired to LiveController::resumeRequested, which is emitted for both cases — it
    // lives here because the pattern lives here and core must never hold one
    // (invariant #3).
    void resumeOrSettleDocument(DocumentContext *ctx);

    // A saved host's remembered password, handed to the transport the only way core will
    // take one: through the per-target cache it already consults (SshPrompter.h). This is
    // what gives HostBookmarkStore::find() a call site at all — it had none in src/ before
    // M14, so a bookmark's password was written and never read back at connect time.
    //
    // Here rather than in SshSession::authenticate() because that would bury an
    // AppConfigLocation read inside a network auth routine, in a class whose header says
    // it knows nothing but a RemoteLocation (ARCHITECTURE.md §6.3.2).
    static void primeRemoteCredentials(const QString &path);

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

    // --- Reporting an open that did not happen (SPEC.md §3) -------------------------
    //
    // The no-I/O refusals (M17) are the only opens that leave no tab behind, so they
    // are the only ones with nowhere of their own to explain themselves. The status bar
    // cannot be that place: updateStatus() rewrites m_statusLabel on every ingest tick
    // and every tab switch, so beside one live log the reason is gone within a second
    // of being written and the reader is left with nothing having happened. They go to
    // a strip above the document well instead, which only the user takes away.
    //
    // Every refusal reports through here, naming the address AND the reason — the
    // reason being the part worth reading. Reports are collected while an open GESTURE
    // is in flight and shown TOGETHER when it ends, so several logs asked for at once
    // produce one message rather than N that overwrite each other.
    void reportOpenRefusal(const QString &displayName, const QString &reason);
    // Bracket one gesture. Nestable, because openFile() opens an archive's picked
    // members through openFiles(); only the outermost bracket shows anything.
    void beginOpenBatch();
    void endOpenBatch();
    // Render what was collected into the strip, REPLACING whatever it held: a gesture
    // is a deliberate act, and the message it displaces was on screen until the user
    // acted. This is also the whole of "does not accumulate" — nothing appends.
    void showOpenRefusals();
    void clearOpenNotice(); // the dismiss button, and nothing else

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
    static void rebuildDigestFor(DocumentContext *ctx);

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

    // Session persistence (SPEC.md §10, ARCHITECTURE.md §12.3): WHICH LOGS were open and
    // in what order, each view's column layout and wrap mode, and the global geometry and
    // pane layout — written on close, restored on launch. A missing last file comes back
    // as a waiting tab rather than being dropped.
    //
    // Nothing a log says about ITSELF is here. Its format, filters, highlight rules and
    // run are per-file state and live one record per log (§8.2); saveSession() flushes
    // each of them through persistFileSettings() on the way past, which is what makes a
    // quit store what the panes were holding.
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

    // Show or hide the density strip beside the scrollbar in EVERY view (SPEC.md §5).
    // Application-wide for the reason the font size directly above is: it says how
    // somebody reads, which does not differ between two tabs — so plain QSettings, not
    // the log's own settings node and not the session. The single funnel: the menu item
    // and the restore both go through it.
    void setDensityStripVisible(bool visible);
    void applyLogFontToViews();
    // Say what the size is now, transiently. The bounds clamp silently, and a key held
    // down at 32 pt with no answer at all reads as a broken shortcut.
    void announceLogFontSize();
    // Say that the log on screen was replaced or truncated behind us and re-read
    // (SPEC.md §3). The reload itself is unchanged; this is the sentence it lacked.
    void announceReload(ReloadCause cause, const QString &path);

    // Open `path` under `settings`. When the pattern matches no sample record,
    // Preferences is offered first (SPEC.md §4) — there is deliberately no way in for a
    // caller that would rather not ask, because the one that used to have it (a command
    // line carrying --pattern) is exactly how an unparseable pattern got saved over a
    // working one. Builds the model/view, starts indexing, and persists the format on a
    // good result.
    // Returns false when the open did not happen — a source that cannot be opened,
    // or a format dialog the user cancelled. On false the previously open document
    // (if any) is left untouched, and NOTHING is written to the settings tree.
    // `runRestore` carries a persisted run selection (session restore only); it is
    // re-resolved once indexing finishes. A normal open passes nullopt and defaults
    // to the newest run (§3a).
    bool openWithSettings(const QString &path, FormatSettings settings,
                          std::optional<RunSelection> runRestore = std::nullopt);
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
    // `error`, when given, takes the reason a refusal gives — restore reports the
    // logs it could not reopen with their reasons, exactly as an interactive open does.
    DocumentContext *prepareContext(const SessionDocument &d, QString *error = nullptr);
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
    // Store what a Preferences visit decided: the tree, the open log's own settings, and
    // — only when the tree actually moved — the sweep of every other log's record against
    // the patterns that have just changed under them. The single exit for both entry
    // points, so the two cannot come to write different things.
    void commitPreferences(const PreferencesDialog &dlg, const QString &address);

    // What `address` opens with: its own stored record where it has one, and what it
    // inherits from the tree where it has none. THE ONE RESOLVER — every site that used
    // to read `m_logSettings.resolve(path).profile` reads this instead.
    //
    // An OPEN log is answered from its context rather than from the disk, because the
    // context holds what the tab is actually using: a format the user has just changed
    // has reached ctx->fileSettings before it has reached the pool, and a second view
    // created in between must not open on the older answer.
    LogProfile resolvedProfile(const QString &address);

    // Which run this log is showing, or nullopt while there is nothing to read — during
    // the scan, and while a restore is still armed. Answering nullopt LEAVES THE STORED
    // SECTION ALONE, which is the whole reason it exists: mid-scan, runs() is empty and
    // selectedRun() is -1, and -1 with a run-start pattern set is the "all runs" answer —
    // so a format change or a resume arriving then would silently unpin a run the user
    // pinned, which SPEC.md §3a says only they move.
    //
    // saveSession()'s three-way branch, moved here so the quit path and every other
    // write cannot come to disagree about what "last run" saves.
    static std::optional<RunSelection> runSelectionOf(const DocumentContext *ctx);

    // THE ONE PLACE A PER-LOG RECORD IS WRITTEN. Everything a gesture may have changed is
    // folded in from the context, compared against what this context last stored, and
    // written only on a real difference. persistFormat() was this for the profile alone;
    // the CHANGE GATE is what lets it be called from paths that repeat, and it must stay,
    // because resumeOrSettleDocument() reaches it on every resume of a remote log.
    //
    // NOT REACHABLE FROM THE INGEST PATH. followLastRunIfMoved() retargets the run on
    // every tick of a restarting log, and HighlighterPane::refreshTimeBounds() runs there
    // too; neither may end in an atomic file write.
    void persistFileSettings(DocumentContext *ctx);

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
    // Whether to ask about unsaved config edits. The quit path has ALREADY asked by the
    // time it gets here — it must, because the question has to be answered before the
    // session is written — so it passes AlreadyAsked rather than asking twice.
    enum class Prompt { Ask, AlreadyAsked };
    void closeAllDocuments(Prompt prompt = Prompt::AlreadyAsked);
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
    // The context for `address`, matched through logSettingsKey() rather than by the
    // spelling the log was opened with, or nullptr when that log is not open.
    DocumentContext *contextOfPath(const QString &address) const;
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

    // Every log view, in the order its tab sits on the bar.
    //
    // THE TAB BAR IS THE ONLY THING THAT KNOWS TAB ORDER, and this is what asks it.
    // `m_views` used to answer by construction — its index WAS the tab index, kept in
    // step by onTabMoved() — which held only while every page in the well was a
    // DocumentView. One page of any other kind and that arithmetic is silently wrong:
    // a drag would reorder the wrong entry and the saved session would come back
    // scrambled, with every tab still showing the right text throughout.
    //
    // Deriving the order instead REMOVES that invariant rather than adding a second one
    // to maintain. `m_views` keeps ownership and reaping; it no longer claims an order.
    QVector<DocumentView *> viewsInTabOrder() const;

    // --- The config-file editor (SPEC.md §4) ---------------------------------

    // File ▸ Open Config File Editor. Resolves the active log's configured path; with
    // none configured, asks for one and makes the answer that LOG's own setting.
    void openConfigEditor();
    // The editor tab in front, or nullptr when the current page is a log.
    ConfigView *activeConfigView() const;
    // Whether the page in front is a log. NOT the same question as `hasFile`, which is
    // about the bound DOCUMENT and stays true while an editor tab is in front — see
    // onCurrentTabChanged(). Every per-log action asks this one instead, or it acts on a
    // log the reader is not looking at.
    bool activePageIsLog() const;
    void saveActiveConfig();
    // Ask about one editor's unsaved changes. False means the user cancelled, and every
    // caller must abandon what it was doing — including quitting.
    bool confirmDiscard(ConfigView *view);
    void updateConfigTabTitle(ConfigView *view);

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

    // Take the whole rule list from another open log (SPEC.md §7). Both entry points —
    // the Highlighters pane's Copy From… button and the View-menu item — reach this.
    void copyHighlightersFromAnotherLog();

    // Enable both of those exactly while there is another open log to copy from, and
    // tell the pane the same answer, so the button and the menu item cannot drift.
    void updateCopyHighlightersState();

    // Walk the active log's filter history one step. False when there was nothing to
    // walk to, which is what leaves the Escape key alone for everybody else.
    bool undoFilterChange();
    bool redoFilterChange();
    // Apply one state from that history and settle the stack against what the pane
    // actually ended up holding. The one place either direction goes through.
    bool applyFilterHistory(bool back);
    // Enablement for the two View items. A SEPARATE writer from updateActionStates(),
    // which setActiveView() runs BEFORE it rebinds the panes: this one reads the
    // CONTEXT's history, so it has to be called after the rebind or it answers about the
    // incoming tab using the outgoing tab's stack.
    void updateFilterUndoState();

    // Move the global panes' per-file widget state to/from a context as the active
    // document changes. See DocumentContext::filterState for why this is needed.
    void stashPaneState(DocumentContext *ctx);
    void hydratePanes(DocumentContext *ctx);

    Document        *activeDocument() const;
    DocumentContext *activeContext() const;
    LogView         *activeLogView() const;
    LogModel        *activeModel() const;

    std::vector<std::unique_ptr<DocumentContext>> m_contexts;
    // Every open view. OWNERSHIP AND REAPING ONLY — this list does NOT carry tab order,
    // and nothing may index it with a tab index. Ask viewsInTabOrder() for the order,
    // which reads it off the tab bar, the only place it actually lives.
    QVector<DocumentView *> m_views;
    // The config-editor pages, the second kind of page the well holds. A separate list
    // rather than a common base class with DocumentView: the two share no behaviour
    // worth abstracting, and qobject_cast on two concrete types is already this
    // window's idiom. Like m_views, it carries no ORDER — the tab bar does.
    QVector<ConfigView *> m_editors;
    QAction *m_openConfigAction = nullptr;
    QAction *m_restartAppAction = nullptr;
    QAction *m_saveConfigAction = nullptr;
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
    // Edit ▸ Find / Find Next / Find Previous. All three act on the active view's own
    // Find bar, so all three follow the active tab exactly as the copy actions above do.
    QAction *m_findAction = nullptr;
    QAction *m_findNextAction = nullptr;
    QAction *m_findPreviousAction = nullptr;
    QAction *m_reconnectAction = nullptr;
    QAction *m_reloadAction = nullptr;
    QAction *m_followAction = nullptr; // View ▸ Follow Tail (return-to-bottom, M6)
    // The timestamp-column header submenu and its five exclusive actions, indexed by
    // TimeDisplay. Owned by the window (see buildTimeDisplayMenu).
    QMenu   *m_timeDisplayMenu = nullptr;
    QAction *m_timeDisplayActions[6] = {};
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
    QAction     *m_copyHighlightersAction = nullptr;        // dead without another open log
    QAction     *m_undoFilterAction = nullptr;
    QAction     *m_redoFilterAction = nullptr;
    // Raised while the panes are being rebound to another log, and while an undo or redo
    // is being applied. FilterPane::restoreState() ends in a filtersChanged(), so without
    // this a tab switch would record an entry on the incoming log and an undo would
    // record its own result as a fresh edit.
    int          m_filterRebind = 0;
    // How those docks' title bars are painted (PaneTitleStyle.h). Shared by all four
    // and parented to the window, so it outlives every dock that points at it.
    PaneTitleStyle        *m_paneStyle = nullptr;

    // The centre of the window: the document well. The tabs and the "no file open"
    // notice take turns in the stack, so the centre is never an empty tab frame.
    QStackedWidget *m_centre = nullptr;
    QTabWidget     *m_tabs = nullptr;
    QLabel         *m_placeholder = nullptr;

    // The open-refusal strip, above the well and across its whole width, and what is
    // pending for it (see reportOpenRefusal). Hidden whenever there is nothing to say.
    QFrame *m_openNotice = nullptr;
    QLabel *m_openNoticeText = nullptr;
    QVector<QPair<QString, QString>> m_openRefusals; // display name, reason
    int m_openBatchDepth = 0;

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

    // THE PER-LOG POOL (M21, SPEC.md §4, §10): one JSON record per log under
    // fileSettings/, holding what that log alone says — its format profile, and from the
    // steps that follow its filters, its highlight rules and its run. The MAP is read
    // here in the constructor beside the tree; a record is read when its log is opened
    // and written through persistFileSettings().
    //
    // The pool is bounded, so eviction has to know what is open: relabelTabs() — the
    // function whose whole definition is "the set of open logs changed" — hands it the
    // pinned set, and nothing else does.
    LogFileStore     m_fileStore{LogFileStore::defaultDir()};
    // View ▸ Line Wrap. Held so the checked entry can be made to track the ACTIVE view,
    // which matters now that the mode a log opens in is its own (M20) rather than one
    // window-wide choice: each action carries its WrapMode in QAction::data().
    QActionGroup *m_wrapGroup = nullptr;
    // View ▸ Line Wrap ▸ Toggle. Off <-> Always On, and it TRIGGERS one of the three
    // mode actions above rather than setting the mode itself — there is one path that
    // sets wrap, and it is the one that also remembers the choice for the log.
    QAction *m_toggleWrapAction = nullptr;

    // The density strip's menu item and the preference behind it. The flag is read by
    // createView(), so a tab opened later comes up the way the reader last asked for
    // rather than at the built-in default.
    QAction *m_densityAction = nullptr;
    bool     m_densityStripOn = true;

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
