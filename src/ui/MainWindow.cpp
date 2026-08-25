#include "MainWindow.h"

#include "Decoder.h"
#include "DetectingFormatProvider.h"
#include "DiagnosticLog.h"
#include "Document.h"
#include "DocumentContext.h"
#include "ConfigFileIO.h"
#include "ConfigLocation.h"
#include "RestartDialog.h"
#include "RestartTarget.h"
#include "ConfigView.h"
#include "DocumentView.h"
#include "Fonts.h"
#include "Filter.h"
#include "FilterPane.h"
#include "FindBar.h"
#include "FormatPreview.h"
#include "HighlighterPane.h"
#include "IndexController.h"
#include "LiveController.h"
#include "LogFormat.h"
#include "LogModel.h"
#include "LogSource.h"
#include "ManualFormatProvider.h"
#include "MessageLabel.h"
#include "UiColors.h"
#include "HostBookmarkStore.h"
#include "ArchiveLocation.h"
#include "OpenArchiveDialog.h"
#include "PreferencesDialog.h"
#include "SourceSpool.h"
#include "SpooledLogSource.h"
#include "Version.h"

#include <QElapsedTimer>
#include <QTimer>
#include "OpenRemoteDialog.h"
#if defined(LOFTAIL_HAVE_PRESETS)
#include "PresetPane.h"
#endif
#include "PaneTitleStyle.h"
#include "RemoteLocation.h"
#include "RunPane.h"
#include "SshFetcher.h"
#include "SshPromptDialogs.h"
#include "SessionStore.h"
#include "TabLabels.h"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>
#include <QMimeData>
#include <QProgressBar>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace loftail {

namespace {
// What a never-seen log is tried with lives in the settings tree (M20): the defaults,
// the file patterns, and the per-log nodes, with the built-in log4cplus layout only as
// what the defaults fall back to. A log that matches nothing still opens with unparsed
// lines as plain text (SPEC.md §4).
constexpr int  kMaxRecentFiles = 10;

// How often the restore checks whether its remote logs have finished connecting, and the
// ceiling on how long it will keep the bulk-prompt mode armed waiting for them. The cap
// exists so that one host that neither answers nor fails cannot leave "Skip All
// Remaining" on every password dialog for the rest of the session.
constexpr int  kBulkRestoreWatchMs = 500;
constexpr int  kBulkRestoreCapMs = 60000;
constexpr auto kRecentFilesKey = "recentFiles";
// The log text size (SPEC.md §5). An application preference, deliberately NOT in the
// session group: the session describes one window's tabs, and two windows would
// otherwise disagree about how big the text is. Absent means "the platform's own size".
constexpr auto kLogFontSizeKey = "logFontPointSize";

// How long a rotation notice stays up (SPEC.md §3). Longer than announceLogFontSize()'s
// 2000, because a zoom is a setting the reader just pressed and this is something that
// happened to their data while they were reading it. Not longer still: a temporary
// message COVERS m_statusLabel for its whole life, so every second of it is a second the
// record count and the filtered/total pair are off screen — at the one moment they have
// just changed the most.
constexpr int kReloadNoticeMs = 5000;

// How far Find will go to count the matches behind its "3 of 47" (SPEC.md §5).
// Finding a match stops at the first one; counting them asks the text of EVERY visible
// record, which on a four-million-record log is several seconds — per keystroke, since
// typing in the bar re-searches. So the count is bounded twice over: by rows, which
// keeps it predictable, and by wall clock, which is what actually holds when a record
// is a hundred wrapped lines or the query is an expensive regex. Whichever bound bites
// first, the total is reported as a floor rather than as a fact (ARCHITECTURE.md
// §7.1.3).
constexpr int kFindTallyRows = 200000;
constexpr int kFindTallyMs = 30;

// May a pane be torn off into a window of its own?
//
// Not under Wayland. Dragging a dock out is a two-part trick: keep receiving pointer
// motion after the pointer has left the widget, and place the resulting window under
// the cursor. Wayland grants neither — a client cannot grab the pointer (the plugin
// says so out loud: "This plugin supports grabbing the mouse only for popup
// windows"), and it cannot position its own top-level windows. What Wayland does
// give is an IMPLICIT grab for as long as a button is held, delivered to the surface
// that received the press — which is why a drag that stays inside the main window
// works, and a tear-off, which moves the dock to a NEW surface mid-drag, loses the
// rest of the gesture and leaves the pane wedged mid-drag.
//
// So on Wayland panes move and close but do not float. The test is the QPA platform,
// not the OS: the same machine under XWayland (`QT_QPA_PLATFORM=xcb`) can do both.
bool panesMayFloat()
{
    return QGuiApplication::platformName() != QLatin1String("wayland");
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    {
        // THE SETTINGS TREE (M20), read once here so an open never touches the disk for
        // it and kept in step by showPreferences().
        //
        // The migration runs FIRST, and that ordering is load-bearing: restoreSession()
        // later in this constructor resolves each restored tab's settings through the
        // tree instead of carrying its own copy, so migrating after it would open every
        // restored tab on the built-in defaults, once, on the first launch after upgrade.
        QSettings store;
        m_settingsStore.migrateLegacy(store);
        m_logSettings = m_settingsStore.load();

        // THE PER-LOG POOL (M21). Only the map is read here — a record is read when its
        // log is opened — so this costs one small file however many logs are remembered.
        m_fileStore.load();

        // The one-time drain of the per-log profiles that used to live in the tree's
        // `files[]` and, before that, in M18's `formatCache`. It runs HERE for the reason
        // the migration above does: restoreSession() resolves each restored tab through
        // the pool, so draining after it would open every restored tab on the built-in
        // defaults, once, on the first launch after upgrade.
        //
        // Rewriting the tree is what CLOSES it: save() no longer emits `files[]`, so the
        // next launch has nothing to take and there is no "migrated" flag to keep.
        if (const auto legacy = m_settingsStore.takeLegacyFiles(); !legacy.isEmpty()) {
            m_fileStore.adoptLegacy(legacy, m_logSettings);
            m_fileStore.flush();
            m_settingsStore.save(m_logSettings);
        }

        // The remembered log text size (SPEC.md §5). Read HERE, with the settings tree
        // and before restoreSession(), because every LogView is constructed with
        // logTextFont() — so a restored tab has to find the size already set rather than
        // be re-fonted afterwards. An absent key leaves the platform's own size alone.
        const int savedFontSize = store.value(QLatin1String(kLogFontSizeKey), 0).toInt();
        if (savedFontSize > 0)
            setLogFontPointSize(savedFontSize);
    }

    setWindowTitle(QStringLiteral("loftail"));
    resize(1100, 720);
    setAcceptDrops(true);

    // Docking applies to the SIDE PANES ONLY (ARCHITECTURE.md §12.2): panes tab and
    // split among themselves along the edges, while open files are tabs in the
    // central document well below and take no part in it. A real central widget is
    // what keeps the two apart — Qt's dock areas cannot encroach on it, so no pane
    // can be dropped into the document area and no log can be dragged out into the
    // panes'.
    //
    // Deliberately WITHOUT GroupedDragging: it makes a drag on any dock's title bar
    // move that dock's whole tab group, so pulling the Filters pane out took the
    // other three with it. Its purpose is dragging a whole group on purpose, which
    // is not worth making the ordinary single-pane drag do something else.
    //
    // Dock options must be set BEFORE any dock widget is added.
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks
                   | QMainWindow::AllowTabbedDocks);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);

    // Remote logs ask questions — an unknown host key, a password — and this is what
    // answers them. Installed unconditionally: in a build without SSH nothing ever
    // calls it, and having it here means the two builds differ in one place only.
    m_sshPrompter = std::make_unique<GuiSshPrompter>(this);
    m_sshPrompter->setBookmarkDir(HostBookmarkStore::defaultDir());
    setSshPrompter(m_sshPrompter.get());

    // Scan progress, and beside it the one way to stop the scan (SPEC.md §3). The stop
    // button lives here rather than in a menu because the progress bar is the only thing
    // that says a scan is running at all, and on any ordinary log the whole opportunity
    // to act lasts a fraction of a second — long enough to click what is already under
    // the eye, not long enough to go looking through the menu bar for it.
    m_progressBox = new QWidget(this);
    m_progressBox->setObjectName(QStringLiteral("scanProgress")); // findChild, for tests
    auto *progressLayout = new QHBoxLayout(m_progressBox);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(4);
    m_progressBar = new QProgressBar(m_progressBox);
    m_progressBar->setObjectName(QStringLiteral("scanProgressBar")); // findChild, for tests
    m_progressBar->setMaximumWidth(200);
    auto *cancelIndexButton = new QToolButton(m_progressBox);
    cancelIndexButton->setObjectName(QStringLiteral("cancelIndexButton")); // findChild, for tests
    cancelIndexButton->setAutoRaise(true);
    // Icon, never a letter: the glyph sits in a 16 px box beside a progress bar, and a
    // "✕" typed as text is a blank on any platform whose font database is empty (the
    // Windows offscreen case). The close mark is one of QCommonStyle's own built-in
    // pixmaps, so it resolves without an icon theme; the text is the fallback of last
    // resort for a style that answers with nothing.
    const QIcon stopIcon = style()->standardIcon(QStyle::SP_TitleBarCloseButton);
    if (stopIcon.isNull())
        cancelIndexButton->setText(QString::fromUtf8("✕"));
    else
        cancelIndexButton->setIcon(stopIcon);
    cancelIndexButton->setToolTip(tr("Stop scanning this log. What has been scanned so "
                                     "far stays usable."));
    cancelIndexButton->setAccessibleName(tr("Cancel indexing"));
    connect(cancelIndexButton, &QToolButton::clicked, this, [this]() {
        if (DocumentContext *ctx = activeContext(); ctx && ctx->controller)
            ctx->controller->cancel();
    });
    progressLayout->addWidget(m_progressBar);
    progressLayout->addWidget(cancelIndexButton);

    m_statusLabel = new QLabel(tr("No file open"), this);
    m_statusLabel->setObjectName(QStringLiteral("statusLabel")); // findChild, for tests
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_progressBox);
    // AFTER the add, never before: QStatusBar shows what it is handed once it is itself
    // visible, so hiding first is a race with when the window is shown.
    m_progressBox->setVisible(false);

    // The document well: every open file is a page here (SPEC.md §5a). Movable so
    // tabs can be reordered, closable so a tab carries its own close button; NOT a
    // dock, so it can be neither a drag source nor a drop target for the panes.
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("documentTabs")); // findChild, for tests
    m_tabs->setDocumentMode(true);
    m_tabs->setMovable(true);
    m_tabs->setTabsClosable(true);
    m_tabs->tabBar()->setUsesScrollButtons(true);
    m_tabs->tabBar()->setElideMode(Qt::ElideMiddle);
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeViewAt);
    connect(m_tabs->tabBar(), &QTabBar::tabMoved, this, &MainWindow::onTabMoved);

    // The empty state shares the centre with the tabs, rather than sitting behind an
    // empty tab frame; updateEmptyState() swaps between them.
    m_placeholder = new QLabel(tr("No file open. Open a log file to begin."), this);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);

    m_centre = new QStackedWidget(this);
    m_centre->addWidget(m_placeholder);
    m_centre->addWidget(m_tabs);

    // ABOVE the well, spanning it: what an open that did not happen says for itself
    // (SPEC.md §3). It is the one channel a refusal has — the tab it would have
    // explained itself in is exactly what a no-I/O refusal does not get — so it must
    // outlast the tick that follows it, which is what the status bar cannot do. Not
    // modal either: restoreSession() runs in this constructor, BEFORE show(), so a box
    // raised on a refusal there would be a dialog on top of no window, and a session of
    // several unreachable logs a stack of them.
    m_openNotice = new QFrame(this);
    m_openNotice->setObjectName(QStringLiteral("openNotice")); // findChild, for tests
    m_openNotice->setFrameShape(QFrame::StyledPanel);
    auto *noticeLayout = new QHBoxLayout(m_openNotice);
    noticeLayout->setContentsMargins(8, 4, 4, 4);
    noticeLayout->setSpacing(6);
    // A MessageLabel, because the message is a list of addresses with their reasons and
    // wraps at any ordinary window width — a plain wrapped QLabel is sized from a hint
    // its own text does not fit in (MessageLabel.h).
    m_openNoticeText = new MessageLabel(m_openNotice);
    m_openNoticeText->setObjectName(QStringLiteral("openNoticeText")); // findChild, for tests
    // Selectable: the address and the transport's wording are what a reader takes to a
    // colleague or a search box, and they are otherwise only retypable.
    m_openNoticeText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *dismiss = new QToolButton(m_openNotice);
    dismiss->setObjectName(QStringLiteral("openNoticeDismiss")); // findChild, for tests
    dismiss->setAutoRaise(true);
    // The same close pixmap the scan's stop button uses, and the same reason: a "✕"
    // typed as text is a blank wherever the font database is empty.
    if (const QIcon closeIcon = style()->standardIcon(QStyle::SP_TitleBarCloseButton);
        closeIcon.isNull())
        dismiss->setText(QString::fromUtf8("✕"));
    else
        dismiss->setIcon(closeIcon);
    dismiss->setToolTip(tr("Dismiss this message"));
    dismiss->setAccessibleName(tr("Dismiss this message"));
    connect(dismiss, &QToolButton::clicked, this, &MainWindow::clearOpenNotice);
    noticeLayout->addWidget(m_openNoticeText, 1);
    noticeLayout->addWidget(dismiss, 0, Qt::AlignTop);
    m_openNotice->setVisible(false);

    auto *centreBox = new QWidget(this);
    auto *centreLayout = new QVBoxLayout(centreBox);
    centreLayout->setContentsMargins(0, 0, 0, 0);
    centreLayout->setSpacing(0);
    centreLayout->addWidget(m_openNotice);
    centreLayout->addWidget(m_centre, 1);
    setCentralWidget(centreBox);

    // The side panes (SPEC.md §8): filters, highlighters, runs — and presets, which is a
    // build option and off by default, so a stock build has three. Each binds to the
    // active document by signal (invariant #7 / §12.3), never a fixed Document; the
    // presets pane is the exception that binds to nothing, being file-independent.
    m_filterPane = new FilterPane(this);
    QDockWidget *filterDock = addPaneDock(m_filterPane, QStringLiteral("filtersDock"),
                                          tr("Filters"));
    connect(this, &MainWindow::activeDocumentChanged, m_filterPane, &FilterPane::setDocument);
    connect(m_filterPane, &FilterPane::filtersChanged, this, &MainWindow::applyActiveFilters);
    // Mark the dock while anything is being hidden. The four panes ship TABBED, so
    // three times out of four the Filters pane is behind another tab with every axis
    // still in force — and a tab label is the only part of it still on screen. The
    // marker rides the window TITLE, which is what a QDockWidget's tab shows, and the
    // object name (what restoreState() keys off) is untouched.
    m_filtersDock = filterDock;
    connect(m_filterPane, &FilterPane::activityChanged, this, [this](bool active) {
        if (m_filtersDock)
            m_filtersDock->setWindowTitle(active ? tr("Filters •") : tr("Filters"));
        // The same answer drives View ▸ Clear Filters, from the same signal: this is
        // the only notification that a filter edit has landed, and it arrives AFTER
        // the pane's debounce rather than per keystroke. The pane's own change guard
        // is what keeps it off the per-tick path (FilterPane::updateActivity).
        updateClearFiltersState();
    });

    m_highlighterPane = new HighlighterPane(this);
    QDockWidget *highlightDock = addPaneDock(m_highlighterPane,
                                             QStringLiteral("highlightersDock"),
                                             tr("Highlighters"));
    connect(this, &MainWindow::activeDocumentChanged, m_highlighterPane, &HighlighterPane::setDocument);
    connect(m_highlighterPane, &HighlighterPane::highlightersChanged,
            this, &MainWindow::applyActiveHighlighters);
    // The same marker the Filters dock carries, for the same reason: with the panes
    // tabbed, rules colour the log while the pane that holds them is out of sight.
    // "Not the seeded rules", not "present": every log opens with the three default
    // rules (HighlighterSet::defaults), so a marker meaning "there are rules" would be
    // on for every file from the moment it opened and would say nothing. The pane
    // compares its whole list, in order, against that seed (hasCustomRules).
    m_highlightersDock = highlightDock;
    connect(m_highlighterPane, &HighlighterPane::activityChanged, this, [this](bool active) {
        if (m_highlightersDock)
            m_highlightersDock->setWindowTitle(active ? tr("Highlighters •") : tr("Highlighters"));
    });

#if defined(LOFTAIL_HAVE_PRESETS)
    m_presetPane = new PresetPane(m_filterPane, m_highlighterPane, this);
    QDockWidget *presetDock = addPaneDock(m_presetPane, QStringLiteral("presetsDock"),
                                          tr("Presets"));
#endif

    // Run selection pane (§3a): a run-start regexp splits the file into runs and the
    // user views one at a time. Binds to the active document by signal like the rest.
    m_runPane = new RunPane(this);
    QDockWidget *runDock = addPaneDock(m_runPane, QStringLiteral("runsDock"),
                                       tr("Runs"));
    connect(this, &MainWindow::activeDocumentChanged, m_runPane, &RunPane::setDocument);
    connect(m_runPane, &RunPane::runStartChanged, this, &MainWindow::onRunStartChanged);
    connect(m_runPane, &RunPane::runSelected, this, &MainWindow::onRunSelected);

    // Tab the panes together by default so they share the right edge; the user can
    // pull any out, and the arrangement is part of the saved session.
    // The chain runs through a cursor rather than naming each link, because the presets
    // pane is optional and dropping the middle link of a written-out chain would leave
    // the Runs pane docked on its own instead of tabbed with the rest.
    tabifyDockWidget(filterDock, highlightDock);
    QDockWidget *lastTabbed = highlightDock;
#if defined(LOFTAIL_HAVE_PRESETS)
    tabifyDockWidget(highlightDock, presetDock);
    lastTabbed = presetDock;
#endif
    tabifyDockWidget(lastTabbed, runDock);
    filterDock->raise();

    buildMenus();

    // Restore the previous working state last, once every pane dock exists with its
    // object name (restoreState keys off those) — SPEC.md §10.
    restoreSession();
}

MainWindow::~MainWindow()
{
    closeAllDocuments();
    // Every config transfer this window started has just been abandoned along with the
    // tab that owned it; this is what WAITS for their threads to notice. Abandoning is
    // enough while the process is alive, and not enough on the way out: Qt's own globals
    // go with the application object, and a worker still inside QTcpSocket then writes
    // through a pointer that has just become null (ConfigFileIO.h). Bounded, so a host
    // that is not answering cannot hang the quit.
    drainConfigTransfers();
    // The prompter is about to be destroyed; a stale pointer would outlive it and be
    // reachable from any later open (a second window, in the multi-instance case).
    if (sshPrompter() == m_sshPrompter.get())
        setSshPrompter(nullptr);
}

void MainWindow::buildMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    QAction *openAction = fileMenu->addAction(tr("&Open..."));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::chooseFileToOpen);

    // Remote logs (M11, SPEC.md §3). Both entries are present whether or not SSH was
    // compiled in — a disabled item with a tooltip explains the situation, where a
    // missing one would just look like the feature does not exist.
    m_openRemoteAction = fileMenu->addAction(tr("Open &Remote..."));
    m_openRemoteAction->setObjectName(QStringLiteral("openRemoteAction")); // findChild, for tests
    m_openRemoteAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    connect(m_openRemoteAction, &QAction::triggered, this, &MainWindow::chooseRemoteToOpen);

    m_recentMenu = fileMenu->addMenu(tr("Open &Recent"));
    m_recentMenu->setObjectName(QStringLiteral("recentMenu")); // findChild, for tests
    // A menu shows an action's tooltip only when asked to, and here the tooltip is the
    // whole reason the entry itself can be short: it carries the full address.
    m_recentMenu->setToolTipsVisible(true);
    refreshRecentFilesMenu();

    m_remoteHostsMenu = fileMenu->addMenu(tr("Remote &Hosts"));
    m_remoteHostsMenu->setObjectName(QStringLiteral("remoteHostsMenu"));
    refreshRemoteHostsMenu();

#if !defined(LOFTAIL_HAVE_SSH)
    const QString noSsh = tr(
        "This copy of loftail was built without SSH support, so remote logs cannot "
        "be opened. Rebuild with libssh2 available to enable it.");
    m_openRemoteAction->setEnabled(false);
    m_openRemoteAction->setToolTip(noSsh);
    m_remoteHostsMenu->setEnabled(false);
    m_remoteHostsMenu->setToolTip(noSsh);
#endif

    fileMenu->addSeparator();

    // The config-file editor (SPEC.md §4). Reading a log and tuning what goes into it
    // are the same errand, so the way in sits with the other opens rather than off in a
    // menu of its own.
    m_openConfigAction = fileMenu->addAction(tr("Open Config File &Editor"));
    m_openConfigAction->setObjectName(QStringLiteral("openConfigAction")); // findChild
    m_openConfigAction->setEnabled(false);
    connect(m_openConfigAction, &QAction::triggered, this, &MainWindow::openConfigEditor);

    // VISIBLE only on an editor tab, which is what was asked for — and paired with
    // setEnabled() rather than relying on Qt to take the shortcut away with the
    // visibility. This is the one place the window's usual argument inverts: elsewhere a
    // disabled QAction "swallows its shortcut with no feedback", and that is a cost
    // because there is an answer to give; on a log tab there is genuinely nothing for
    // Ctrl+S to do.
    m_saveConfigAction = fileMenu->addAction(tr("&Save"));
    m_saveConfigAction->setObjectName(QStringLiteral("saveConfigAction")); // findChild
    m_saveConfigAction->setShortcut(QKeySequence::Save); // Ctrl+S
    m_saveConfigAction->setVisible(false);
    m_saveConfigAction->setEnabled(false);
    connect(m_saveConfigAction, &QAction::triggered, this, &MainWindow::saveActiveConfig);

    fileMenu->addSeparator();

    // Restart the application that WRITES this log (SPEC.md §4). The third verb in the
    // same errand as the two above — read the log, tune what goes into it, bounce the
    // thing producing it — so it sits with them and not among the closes.
    //
    // Ctrl+R, which is free precisely because Reload deliberately took F5 instead (see
    // the comment on the reload action). Mnemonic T: this menu already spends R twice,
    // on Open Recent and Reconnect, and a third would make the cycle useless.
    //
    // NOT hidden without SSH, exactly as Open Remote is not: a remote target in such a
    // build refuses in words through restartTargetIsRunnable(), which is a sentence
    // somebody can act on where a missing menu item is a mystery.
    m_restartAppAction = fileMenu->addAction(tr("Res&tart App..."));
    m_restartAppAction->setObjectName(QStringLiteral("restartAppAction")); // findChild
    m_restartAppAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    m_restartAppAction->setEnabled(false);
    connect(m_restartAppAction, &QAction::triggered, this, &MainWindow::restartActiveApp);

    fileMenu->addSeparator();
    m_closeTabAction = fileMenu->addAction(tr("&Close Tab"));
    m_closeTabAction->setObjectName(QStringLiteral("closeTabAction")); // findChild, for tests
    m_closeTabAction->setShortcut(QKeySequence::Close); // Ctrl+W
    m_closeTabAction->setEnabled(false);
    connect(m_closeTabAction, &QAction::triggered, this, &MainWindow::closeActiveView);

    m_closeAllAction = fileMenu->addAction(tr("Close &All"));
    m_closeAllAction->setObjectName(QStringLiteral("closeAllAction")); // findChild, for tests
    m_closeAllAction->setEnabled(false);
    connect(m_closeAllAction, &QAction::triggered, this,
            [this]() { closeAllDocuments(Prompt::Ask); });

    // Ask a spooled log's fetcher to try again NOW rather than at its next backoff.
    // The one case it is required for, rather than merely convenient: a reconnect that
    // needs a password cannot prompt from the fetcher thread, so it stops trying and
    // says so — and this is how the user says "ask me again" (§6.5).
    m_reconnectAction = fileMenu->addAction(tr("&Reconnect"));
    m_reconnectAction->setObjectName(QStringLiteral("reconnectAction")); // findChild, for tests
    m_reconnectAction->setEnabled(false);
    connect(m_reconnectAction, &QAction::triggered, this, [this]() {
        DocumentContext *ctx = activeContext();
        if (!ctx || !ctx->doc)
            return;
        if (auto *spooled = dynamic_cast<SpooledLogSource *>(ctx->doc->source())) {
            if (const auto &spool = spooled->spool())
                spool->poke();
        }
    });


    // M18 — application-wide settings. Deliberately NOT disabled when no log is open:
    // the default format is what the NEXT open uses, so the moment before there is a
    // document is exactly when someone wants to set it. PreferencesRole is what moves it
    // into the application menu on macOS, where a File menu entry is the wrong place for it.
    fileMenu->addSeparator();
    QAction *preferencesAction = fileMenu->addAction(tr("&Preferences..."));
    preferencesAction->setObjectName(QStringLiteral("preferencesAction")); // findChild, for tests
    // Ctrl+P, ADDED to whatever the platform binds rather than instead of it, and not
    // added at all on macOS, where Cmd+, is the convention and Cmd+P is Print.
    //
    // QKeySequence::Preferences alone is not an accelerator anybody can press: it is
    // empty on Windows, and on X11/Wayland it resolves to Qt::Key_Settings — a system
    // key that virtually no keyboard carries — so the entry read "Settings" in the menu
    // and answered nothing. It is kept in the list because a desktop that DOES deliver
    // that key should still reach this. Ctrl+P collides with nothing: loftail does not
    // print. It is FIRST because a QMenu shows the first sequence beside the entry.
    QList<QKeySequence> preferencesKeys = QKeySequence::keyBindings(QKeySequence::Preferences);
#ifndef Q_OS_MACOS
    const QKeySequence ctrlP(Qt::CTRL | Qt::Key_P);
    if (!preferencesKeys.contains(ctrlP))
        preferencesKeys.prepend(ctrlP);
#endif
    preferencesAction->setShortcuts(preferencesKeys);
    preferencesAction->setMenuRole(QAction::PreferencesRole);
    connect(preferencesAction, &QAction::triggered, this, &MainWindow::showPreferences);

    fileMenu->addSeparator();
    QAction *quitAction = fileMenu->addAction(tr("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    m_copyAction = editMenu->addAction(tr("&Copy"));
    m_copyAction->setObjectName(QStringLiteral("copyAction")); // findChild, for tests
    m_copyAction->setShortcut(QKeySequence::Copy);
    m_copyAction->setEnabled(false);
    connect(m_copyAction, &QAction::triggered, this, [this]() {
        if (LogView *v = activeLogView())
            v->copySelectionRaw();
    });
    m_copyColumnsAction = editMenu->addAction(tr("Copy as &Columns"));
    m_copyColumnsAction->setObjectName(QStringLiteral("copyColumnsAction")); // findChild, for tests
    m_copyColumnsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    m_copyColumnsAction->setEnabled(false);
    connect(m_copyColumnsAction, &QAction::triggered, this, [this]() {
        if (LogView *v = activeLogView())
            v->copySelectionAsColumns();
    });

    // Select All (SPEC.md §5). It acts on the ACTIVE log view — the same activeLogView()
    // the copy actions above resolve, so what is copied afterwards is what was selected,
    // whatever happens to hold the keyboard focus (a window-scoped shortcut is dispatched
    // before the key reaches a widget, so the digest strip cannot intercept it). "All" is
    // every record IN VIEW, which is the filtered subset while a filter is active.
    editMenu->addSeparator();
    m_selectAllAction = editMenu->addAction(tr("Select &All"));
    m_selectAllAction->setObjectName(QStringLiteral("selectAllAction")); // findChild, for tests
    m_selectAllAction->setShortcut(QKeySequence::SelectAll);
    m_selectAllAction->setEnabled(false);
    connect(m_selectAllAction, &QAction::triggered, this, [this]() {
        if (LogView *v = activeLogView())
            v->selectAllRecords();
    });

    // Find / Find Next / Find Previous (SPEC.md §5). Find opens the bar; F3 /
    // Shift+F3 navigate the current query over the visible rows. All three need a view
    // to act on — the bar is a DocumentView child — so all three start disabled and are
    // tracked in updateActionStates() beside Copy and Select All.
    editMenu->addSeparator();
    m_findAction = editMenu->addAction(tr("&Find..."));
    m_findAction->setObjectName(QStringLiteral("findAction")); // findChild, for tests
    m_findAction->setShortcut(QKeySequence::Find);
    m_findAction->setEnabled(false);
    connect(m_findAction, &QAction::triggered, this, [this]() {
        if (ConfigView *editor = activeConfigView())
            editor->activateFind();
        else if (m_activeView)
            m_activeView->activateFind();
    });
    m_findNextAction = editMenu->addAction(tr("Find &Next"));
    m_findNextAction->setObjectName(QStringLiteral("findNextAction")); // findChild, for tests
    m_findNextAction->setShortcut(QKeySequence::FindNext); // F3
    m_findNextAction->setEnabled(false);
    connect(m_findNextAction, &QAction::triggered, this, [this]() { runFind(true, false); });
    m_findPreviousAction = editMenu->addAction(tr("Find Pre&vious"));
    m_findPreviousAction->setObjectName(QStringLiteral("findPreviousAction"));
    m_findPreviousAction->setShortcut(QKeySequence::FindPrevious); // Shift+F3
    m_findPreviousAction->setEnabled(false);
    connect(m_findPreviousAction, &QAction::triggered, this, [this]() { runFind(false, false); });

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    QMenu *wrapMenu = viewMenu->addMenu(tr("Line &Wrap"));
    m_wrapGroup = new QActionGroup(this);
    QAction *wrapOff = wrapMenu->addAction(tr("&Off"));
    QAction *wrapSel = wrapMenu->addAction(tr("&Selected Record Only"));
    QAction *wrapAll = wrapMenu->addAction(tr("&Always On"));
    // Object names, because a test that finds a menu entry by its visible text stops
    // working the day the entry is translated or reworded (ARCHITECTURE.md §9.1).
    wrapOff->setObjectName(QStringLiteral("wrapOffAction"));
    wrapSel->setObjectName(QStringLiteral("wrapSelectedAction"));
    wrapAll->setObjectName(QStringLiteral("wrapAlwaysOnAction"));
    wrapOff->setData(int(LogView::WrapMode::Off));
    wrapSel->setData(int(LogView::WrapMode::SelectedRecordOnly));
    wrapAll->setData(int(LogView::WrapMode::AlwaysOn));
    for (QAction *a : {wrapOff, wrapSel, wrapAll}) {
        a->setCheckable(true);
        m_wrapGroup->addAction(a);
    }
    wrapOff->setChecked(true);
    auto setWrap = [this](LogView::WrapMode mode) {
        LogView *v = activeLogView();
        if (!v)
            return;
        v->setWrapMode(mode);
        // Remembered for this log (M20), so it opens the same way next time. It is
        // still the VIEW that owns the live mode — a second view of the same log keeps
        // its own, and the session restores each view's — but the node is what a NEW
        // view starts from, and a gesture the user made is the best answer it can hold.
        if (DocumentContext *ctx = activeContext(); ctx && ctx->doc) {
            if (!ctx->fileSettings.profile)
                ctx->fileSettings.profile = resolvedProfile(ctx->doc->path());
            ctx->fileSettings.profile->wrapMode = mode;
            persistFileSettings(ctx);
        }
    };
    connect(wrapOff, &QAction::triggered, this, [setWrap]() { setWrap(LogView::WrapMode::Off); });
    connect(wrapSel, &QAction::triggered, this,
            [setWrap]() { setWrap(LogView::WrapMode::SelectedRecordOnly); });
    connect(wrapAll, &QAction::triggered, this,
            [setWrap]() { setWrap(LogView::WrapMode::AlwaysOn); });

    // The keyboard gesture (SPEC.md §5). It TOGGLES Off <-> Always On rather than cycling
    // all three, and the third mode is deliberately not on the way: Selected Record Only
    // is a reading aid picked for one record, not a state anybody wants to land in while
    // reaching for the other one — a three-way cycle would make the key's effect depend
    // on invisible state and would re-lay out the whole view on the way past. From
    // Selected Record Only the key means "wrap it all", which is the useful reading of a
    // toggle pressed from a third state.
    //
    // It sets nothing itself: it TRIGGERS one of the three actions above, so the mode,
    // the checkmark and the write into this log's settings node all happen exactly once
    // and in one place.
    //
    // Alt+Z is the editors' own binding for this and collides with nothing here: Ctrl+W
    // is Close Tab, and no menu on the bar answers to Z.
    wrapMenu->addSeparator();
    m_toggleWrapAction = wrapMenu->addAction(tr("&Toggle Wrap"));
    m_toggleWrapAction->setObjectName(QStringLiteral("toggleWrapAction")); // findChild, for tests
    m_toggleWrapAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Z));
    m_toggleWrapAction->setEnabled(false);
    connect(m_toggleWrapAction, &QAction::triggered, this, [this, wrapOff, wrapAll]() {
        LogView *v = activeLogView();
        if (!v)
            return;
        (v->wrapMode() == LogView::WrapMode::AlwaysOn ? wrapOff : wrapAll)->trigger();
    });

    // Log text size (SPEC.md §5). One size for the whole application — see
    // MainWindow::setLogFontSize — so these are enabled with no file open too: the size
    // is a preference, and the next log opens at it.
    QMenu *zoomMenu = viewMenu->addMenu(tr("Text &Size"));
    QAction *zoomInAction = zoomMenu->addAction(tr("Zoom &In"));
    zoomInAction->setObjectName(QStringLiteral("zoomInAction")); // findChild, for tests
    // Ctrl+= as well as whatever the platform calls ZoomIn, because on most keyboards
    // Ctrl++ is really Ctrl+Shift+= and the unshifted key is what people press.
    QList<QKeySequence> zoomInKeys = QKeySequence::keyBindings(QKeySequence::ZoomIn);
    const QKeySequence ctrlEquals(Qt::CTRL | Qt::Key_Equal);
    if (!zoomInKeys.contains(ctrlEquals))
        zoomInKeys.append(ctrlEquals);
    zoomInAction->setShortcuts(zoomInKeys);
    connect(zoomInAction, &QAction::triggered, this, [this]() { stepLogFontSize(1); });

    QAction *zoomOutAction = zoomMenu->addAction(tr("Zoom &Out"));
    zoomOutAction->setObjectName(QStringLiteral("zoomOutAction")); // findChild, for tests
    zoomOutAction->setShortcuts(QKeySequence::keyBindings(QKeySequence::ZoomOut));
    connect(zoomOutAction, &QAction::triggered, this, [this]() { stepLogFontSize(-1); });

    // Explicit, not a QKeySequence role: Qt has no "zoom reset" binding, and Ctrl+0 is
    // what every browser and editor uses for it.
    QAction *zoomResetAction = zoomMenu->addAction(tr("&Reset Size"));
    zoomResetAction->setObjectName(QStringLiteral("zoomResetAction")); // findChild, for tests
    zoomResetAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(zoomResetAction, &QAction::triggered, this, [this]() {
        if (resetLogFontPointSize()) {
            applyLogFontToViews();
            QSettings().remove(QLatin1String(kLogFontSizeKey));
        }
        announceLogFontSize();
    });

    buildTimeDisplayMenu();
    buildColumnWidthActions();

    // Return-to-bottom / follow control (SPEC.md §3, M6). Checked reflects whether
    // the view is currently following; triggering it re-attaches and jumps to the end.
    viewMenu->addSeparator();
    m_followAction = viewMenu->addAction(tr("&Follow Tail"));
    m_followAction->setObjectName(QStringLiteral("followAction")); // findChild, for tests
    m_followAction->setCheckable(true);
    m_followAction->setChecked(true);
    m_followAction->setEnabled(false);
    m_followAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_End));
    connect(m_followAction, &QAction::triggered, this, [this]() {
        if (LogView *v = activeLogView())
            v->followTail();
    });

    // Re-read the log from the beginning (SPEC.md §3 "Reloading by hand"). Everything
    // else about following a log is automatic and deliberately has no control — there is
    // no tail mode to turn on and no rotation notice to acknowledge — so this is not a
    // setting either: it is the one gesture for the case where what is on screen has
    // stopped agreeing with the file and the user should not have to work out why.
    //
    // F5 rather than Ctrl+R: QKeySequence::Refresh IS F5 on every platform loftail
    // targets, and it is what a person reaches for without being told.
    viewMenu->addSeparator();
    m_reloadAction = viewMenu->addAction(tr("&Reload"));
    m_reloadAction->setObjectName(QStringLiteral("reloadAction")); // findChild, for tests
    m_reloadAction->setShortcut(QKeySequence::Refresh);
    m_reloadAction->setEnabled(false);
    connect(m_reloadAction, &QAction::triggered, this, &MainWindow::reloadActiveDocument);

    // The one way back to an unfiltered view that does not mean visiting five axes by
    // hand — and the ONLY way, the pane having no Clear button of its own: the pane can
    // be closed outright (View ▸ Panes), and a filter left in force with no pane to
    // clear it from is the state this exists for.
    //
    // Which is also why its enablement is worth having. It is the one place the window
    // can say whether anything is being hidden without the Filters tab in view, so a
    // live item means "there is something to undo here" and a grey one answers the
    // question a reader of a short-looking log actually has. Off until a document with
    // filters in force is in front; updateClearFiltersState() is the only writer.
    viewMenu->addSeparator();
    m_clearFiltersAction = viewMenu->addAction(tr("&Clear Filters"));
    m_clearFiltersAction->setObjectName(QStringLiteral("clearFiltersAction"));
    m_clearFiltersAction->setEnabled(false);
    connect(m_clearFiltersAction, &QAction::triggered, this, [this] {
        if (m_filterPane)
            m_filterPane->clearAll();
    });

    // Panes are closable docks, so without this a closed pane could not be brought
    // back (SPEC.md §8). Qt's own toggleViewAction does the work.
    viewMenu->addSeparator();
    QMenu *panesMenu = viewMenu->addMenu(tr("&Panes"));
    for (QDockWidget *dock : std::as_const(m_paneDocks))
        panesMenu->addAction(dock->toggleViewAction());

    // Window: move between the open files (SPEC.md §3). The list of open views is
    // rebuilt each time the menu opens, since tabs come and go.
    m_windowMenu = menuBar()->addMenu(tr("&Window"));
    // Parented to the window, not the menu, so refreshWindowMenu()'s clear() does not
    // delete it out from under updateActionStates().
    m_newViewAction = new QAction(tr("&New View"), this);
    m_newViewAction->setObjectName(QStringLiteral("newViewAction")); // findChild, for tests
    m_newViewAction->setEnabled(false);
    connect(m_newViewAction, &QAction::triggered, this, &MainWindow::newViewOfActiveDocument);
    connect(m_windowMenu, &QMenu::aboutToShow, this, &MainWindow::refreshWindowMenu);
    refreshWindowMenu();

    // Help: where a running binary says which build it is (SPEC.md §1 "Which build
    // this is"). Until now that answer existed only on the command line, in
    // `--version` — which an installed .deb or an AppImage double-clicked from a file
    // manager never shows, so the one identity a bug report needs was the one identity
    // the application would not tell you. AboutRole moves it into the application menu
    // on macOS, exactly as PreferencesRole does for Preferences above.
    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));

    // Where loftail's own log lives (SPEC.md §3 "Diagnostics", DiagnosticLog.h). The file
    // is written whether or not anybody ever opens this, which is the point of it — but a
    // diagnostic log nobody can find is one nobody attaches to a bug report, and it lands
    // in a per-platform data directory that no user should be expected to know. Opens the
    // containing FOLDER rather than the file: there is a rolled-over copy beside it, both
    // are wanted, and no desktop reliably has an application registered for a .log.
    QAction *diagAction = helpMenu->addAction(tr("Show &Diagnostics Log"));
    diagAction->setObjectName(QStringLiteral("diagnosticsLogAction")); // findChild, for tests
    diagAction->setStatusTip(diagLogPath());
    diagAction->setToolTip(diagLogPath());
    connect(diagAction, &QAction::triggered, this, [this]() {
        const QString path = diagLogPath();
        // Flushed per line, so there is always something to look at — except on the very
        // first run of a session that has done nothing yet, where the file may not exist.
        diagLogSessionStart();
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()))) {
            // No file manager, or a headless session. The path itself is still the answer,
            // and it is the thing worth copying into a report.
            QMessageBox::information(this, tr("Diagnostics Log"),
                                     tr("loftail's own log is at:\n\n%1").arg(path));
        }
    });
    helpMenu->addSeparator();

    QAction *aboutAction = helpMenu->addAction(tr("&About loftail"));
    aboutAction->setObjectName(QStringLiteral("aboutAction")); // findChild, for tests
    aboutAction->setMenuRole(QAction::AboutRole);
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
}

void MainWindow::refreshWindowMenu()
{
    if (!m_windowMenu)
        return;
    m_windowMenu->clear();
    m_windowMenu->addAction(m_newViewAction);
    m_windowMenu->addSeparator();

    QAction *next = m_windowMenu->addAction(tr("&Next Tab"));
    next->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Tab));
    next->setEnabled(m_views.size() > 1);
    connect(next, &QAction::triggered, this, [this]() { cycleView(1); });

    QAction *prev = m_windowMenu->addAction(tr("&Previous Tab"));
    prev->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab));
    prev->setEnabled(m_views.size() > 1);
    connect(prev, &QAction::triggered, this, [this]() { cycleView(-1); });

    if (m_views.isEmpty())
        return;
    m_windowMenu->addSeparator();
    // In TAB order, so the menu reads down the bar rather than in the order the logs
    // happened to be opened.
    const QVector<DocumentView *> ordered = viewsInTabOrder();
    for (DocumentView *view : ordered) {
        const int index = m_tabs->indexOf(view);
        QAction *a = m_windowMenu->addAction(index >= 0 ? m_tabs->tabText(index) : QString());
        a->setCheckable(true);
        a->setChecked(view == m_activeView);
        connect(a, &QAction::triggered, this, [this, view]() { showView(view); });
    }
}

void MainWindow::cycleView(int delta)
{
    const int size = m_tabs->count();
    if (size < 2)
        return;
    const int current = qMax(0, m_tabs->currentIndex());
    m_tabs->setCurrentIndex(((current + delta % size) + size) % size);
    // The page that is now in front, whichever kind it is. Both kinds carry a focus
    // proxy, so this reaches the table or the text without asking which.
    if (QWidget *page = m_tabs->currentWidget())
        page->setFocus();
}

// --- Side panes ------------------------------------------------------------

QDockWidget *MainWindow::addPaneDock(QWidget *pane, const QString &objectName,
                                     const QString &title)
{
    auto *dock = new QDockWidget(title, this);
    dock->setObjectName(objectName); // restoreState() keys off this
    dock->setWidget(pane);

    // Painted by PaneTitleStyle: the panes ship tabbed, so the title bar would
    // otherwise repeat the tab's own label a row below it, above two hairline buttons
    // that are hard to see and harder to hit. Installed on the DOCK, never on the
    // window, so nothing outside the panes inherits any of it (PaneTitleStyle.h
    // explains why the bar cannot simply be hidden — Qt only starts a drag from its
    // own title bar).
    if (!m_paneStyle)
        m_paneStyle = new PaneTitleStyle(this);
    dock->setStyle(m_paneStyle);
    // Left or right only. SPEC.md §8 offers a pane "on either side", and a pane
    // dropped as a full-width strip above or below the log is a drag people make by
    // accident, not on purpose. (Restricting areas used to be forbidden here because
    // it broke GroupedDragging; that option is gone.)
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    QDockWidget::DockWidgetFeatures features =
        QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable;
    if (panesMayFloat())
        features |= QDockWidget::DockWidgetFloatable;
    dock->setFeatures(features);

    addDockWidget(Qt::RightDockWidgetArea, dock);
    m_paneDocks.append(dock);
    return dock;
}

// --- Document tabs ---------------------------------------------------------

void MainWindow::onCurrentTabChanged(int index)
{
    // The current page is the active view WHEN IT IS A LOG. An editor page leaves the
    // bound document exactly where it was, deliberately: setActiveView() ends in
    // stashPaneState(), which writes a per-log record, so unbinding on every flip to an
    // editor tab and back would cost a file write per flip — and the Filters and
    // Highlighters panes staying on the log being read is also what a reader editing
    // that log's config wants to see.
    //
    // The consequence is that `hasFile` stays true with an editor in front, which is why
    // every per-log action asks activePageIsLog() instead.
    if (auto *view = qobject_cast<DocumentView *>(m_tabs->widget(index)))
        setActiveView(view);
    else if (index < 0)
        setActiveView(nullptr); // the last tab closed: nothing to be looking at

    // UNCONDITIONALLY, and after the branch above rather than inside its else. What the
    // per-page actions may act on has moved whether or not the bound DOCUMENT did — and
    // setActiveView() early-returns when the view is unchanged, which is exactly what
    // happens coming back from an editor tab to the log that was already active. Put
    // this in the else and Save stays visible on the log tab, with every per-log action
    // still greyed out, until something else happens to refresh them.
    updateActionStates();
    updateStatus();
}

void MainWindow::onTabMoved(int /*from*/, int /*to*/)
{
    // NOTHING TO REORDER. m_views no longer carries tab order — viewsInTabOrder() reads
    // it off the bar, which has already moved by the time this runs — so the old
    // m_views.move(from, to) is not merely unnecessary, it is the bug: those arguments
    // are tab indices, and indexing m_views with one is only correct while every page
    // in the well is a DocumentView.
    //
    // Two views of one file are numbered by tab position, so moving a tab renumbers
    // its file's — and only a file with several views can be affected.
    for (auto &ctx : m_contexts) {
        if (ctx->views.size() > 1)
            updateTabTitles(ctx.get());
    }
}

QVector<DocumentView *> MainWindow::viewsInTabOrder() const
{
    QVector<DocumentView *> ordered;
    ordered.reserve(m_views.size());
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *view = qobject_cast<DocumentView *>(m_tabs->widget(i)))
            ordered.append(view);
    }
    return ordered;
}

void MainWindow::updateEmptyState()
{
    // Asked of the TAB BAR, not of m_views: the placeholder means "the well is empty",
    // and a well holding a page of some other kind is not empty even with no log in it.
    // On m_views this would hide a live page behind the "no file open" notice.
    m_centre->setCurrentWidget(m_tabs->count() == 0 ? static_cast<QWidget *>(m_placeholder)
                                                    : static_cast<QWidget *>(m_tabs));
}

void MainWindow::onViewDestroyed(QObject *obj)
{
    // `obj` is mid-destruction: compare it, never dereference it — and, since M17's
    // sanitizer work, never DOWNCAST it either. ~QObject runs after ~DocumentView, so by
    // the time this fires the object's dynamic type has degraded to QWidget and
    // static_cast<DocumentView *>(obj) is undefined behaviour on an object that is no
    // longer one. UBSan reports it as "downcast of address ... which does not point to an
    // object of type 'DocumentView'"; the pointer it produced happened to be right,
    // because the bases are all primary, which is exactly why it went unnoticed.
    //
    // Compare in the safe direction instead: converting each LIVE list entry up to
    // QObject * is a pointer adjustment that reads no vtable. See ARCHITECTURE.md §13.
    const auto isThisView = [obj](DocumentView *v) { return v == obj; };
    m_views.removeIf(isThisView);
    for (auto &ctx : m_contexts)
        ctx->views.removeIf(isThisView);
    if (m_activeView == obj)
        m_activeView = nullptr;

    // A file with no views left is closed: its index, workers and model go with it.
    const auto before = m_contexts.size();
    std::erase_if(m_contexts, [this](const auto &ctx) {
        if (!ctx->views.isEmpty())
            return false;
        // BEFORE the Document goes. The Filters pane may be holding a deferred edit
        // aimed at it, and landing that after the fact resolves names through a freed
        // intern table (FilterPane::documentClosing).
        if (m_filterPane)
            m_filterPane->documentClosing(ctx->doc.get());
        if (m_lastNotified == ctx.get())
            m_lastNotified = nullptr; // about to dangle
        return true;
    });
    // Closing the last log that wanted notifications takes the tray icon with it (M19).
    if (m_contexts.size() != before)
        updateTrayPresence();

    // A closed log takes its share of the ambiguity with it: the last two app.logs left
    // standing say which is which, and the last one standing is app.log again.
    relabelTabs();

    // A file down to its last view is a plain name again, not "name [1]".
    for (auto &ctx : m_contexts)
        updateTabTitles(ctx.get());

    updateEmptyState();
    if (m_activeView)
        return;

    // Removing the tab already moved the current page, and with it the active view;
    // reaching here with none means the last tab went. Unbind the panes (invariant
    // #7) and disable the per-file actions.
    if (m_views.isEmpty()) {
        emit activeDocumentChanged(nullptr);
        updateActionStates();
        updateStatus();
    }
}

// --- The config-file editor (SPEC.md §4) -----------------------------------

ConfigView *MainWindow::activeConfigView() const
{
    return qobject_cast<ConfigView *>(m_tabs->currentWidget());
}

bool MainWindow::activePageIsLog() const
{
    // NOT `activeContext() != nullptr`. The bound document deliberately does NOT move
    // when an editor tab comes to the front — setActiveView() ends in stashPaneState(),
    // which writes a per-log record, and a tab flip must not cost a file write — so
    // `hasFile` stays true with an editor in front. Every action that acts on
    // m_activeView therefore has to ask THIS instead, or it operates on a log the reader
    // is not looking at.
    //
    // AND IT IMPLIES `hasFile`, which is not decoration. Written as the cast alone this
    // answered true for a log page that was NOT the active view — which happens
    // transiently while tabs are being taken down — and updateActionStates() then
    // dereferenced a null context on the Reconnect line, whose guard this replaced.
    // That crashed on Windows and nowhere else: every case in tst_recordmenu passed on
    // its own and the binary segfaulted running them together, which is what a
    // teardown-ordering bug looks like from the outside. Asking the whole question here
    // is what makes every caller safe, rather than a null check at the one site that
    // happened to be found first.
    auto *page = qobject_cast<DocumentView *>(m_tabs->currentWidget());
    return page != nullptr && page == m_activeView && activeContext() != nullptr;
}

void MainWindow::openConfigEditor()
{
    DocumentContext *ctx = activeContext();
    if (!ctx || !ctx->doc)
        return;
    const QString logPath = ctx->doc->path();

    ConfigAddress target = resolveConfigAddress(logPath, resolvedProfile(logPath).configPath);

    if (target.state == ConfigAddress::State::Refused) {
        reportOpenRefusal(logSourceDisplayName(logPath), target.reason);
        return;
    }

    if (target.state == ConfigAddress::State::Unset) {
        // Nothing configured, so ASK — and the answer becomes this log's own setting,
        // through the ordinary write funnel, which means the redundancy rule applies to
        // it exactly as it does to everything else: if the chosen path is what the log
        // would have inherited anyway, no per-log entry is left behind.
        const QString dir = QFileInfo(logPath).absolutePath();
        const QString chosen = QFileDialog::getOpenFileName(
            this, tr("Config file for %1").arg(logSourceDisplayName(logPath)), dir,
            tr("Config files (*.properties *.ini *.conf *.cfg *.xml *.json);;All files (*)"));
        if (chosen.isEmpty())
            return; // cancelled: nothing chosen, nothing remembered

        LogProfile stored = resolvedProfile(logPath);
        stored.configPath = chosen;
        ctx->fileSettings.profile = stored;
        persistFileSettings(ctx);

        target = resolveConfigAddress(logPath, chosen);
        if (target.state != ConfigAddress::State::Resolved) {
            reportOpenRefusal(logSourceDisplayName(logPath), target.reason);
            return;
        }
    }

    openConfigAt(target.address);
}

void MainWindow::restartActiveApp()
{
    DocumentContext *ctx = activeContext();
    if (!ctx || !ctx->doc)
        return;
    const QString path = ctx->doc->path();
    const QString name = logSourceDisplayName(path);

    const RestartTarget target =
        resolveRestartTarget(path, resolvedProfile(path).restartScript);

    if (target.state == RestartTarget::State::Refused) {
        // The strip above the document well, never the status bar: updateStatus() rewrites
        // m_statusLabel from the active document on every ingest tick, so beside a live log
        // a reason written there is gone before it is read.
        reportOpenRefusal(name, target.reason);
        return;
    }

    if (target.state == RestartTarget::State::Unset) {
        // NOT a refusal, which is why the menu item is not disabled for it: a disabled
        // QAction swallows Ctrl+R with no feedback, and there is an answer to give. So it
        // explains itself, and offers the one place the answer can be written.
        QMessageBox box(this);
        box.setObjectName(QStringLiteral("restartNotConfiguredBox")); // findChild, for tests
        box.setWindowTitle(tr("Restart App"));
        box.setIcon(QMessageBox::Information); // Information, not Warning: nothing is wrong
        box.setTextFormat(Qt::PlainText);      // a log name is a path; `<` is not markup
        box.setText(tr("No restart script is configured for %1.").arg(name));
        box.setInformativeText(
            tr("A restart script is a shell script loftail runs to restart the application "
               "that writes this log. Set one in File ▸ Preferences, at whichever of the "
               "three levels it belongs to — one entry on a file pattern can serve every "
               "log it matches."));
        QPushButton *prefs = box.addButton(tr("Open &Preferences..."), QMessageBox::ActionRole);
        prefs->setObjectName(QStringLiteral("restartNotConfiguredPreferences")); // findChild
        QPushButton *close = box.addButton(QMessageBox::Close);
        close->setObjectName(QStringLiteral("restartNotConfiguredClose")); // findChild
        box.setDefaultButton(prefs);
        box.exec();
        // AFTER exec() returns, never from the button's own handler: Preferences opening
        // over a message box that is still up is two modals deep on one gesture.
        if (box.clickedButton() == prefs)
            showPreferences();
        return;
    }

    if (QString why; !restartTargetIsRunnable(target, &why)) {
        reportOpenRefusal(name, why);
        return;
    }

    RestartDialog dlg(name, target, this);
    dlg.run();
    dlg.exec();
}

ConfigView *MainWindow::openConfigAt(const QString &address)
{
    // Already open: RAISE it rather than opening a second tab onto one file, which is
    // openWithSettings()'s rule for a log and is more important here — two editors over
    // one buffer would let the reader save one over the other.
    for (ConfigView *existing : std::as_const(m_editors)) {
        if (existing->address() == address) {
            m_tabs->setCurrentWidget(existing);
            existing->setFocus();
            return existing;
        }
    }

    // A build with no SSH cannot reach a remote config at all, and that is a no-I/O
    // refusal — decidable without asking anybody — so it makes no tab, exactly as a
    // remote LOG open does in this configuration (§6.5).
    if (QString reason; !configAddressIsWritable(address, &reason)) {
        reportOpenRefusal(logSourceDisplayName(address), reason);
        return nullptr;
    }

    const bool remote = configAddressIsRemote(address);
    ConfigReadResult read;
    if (!remote) {
        read = readConfigFile(address);
        if (!read.ok) {
            reportOpenRefusal(logSourceDisplayName(address), read.error);
            return nullptr;
        }
    }

    auto *view = new ConfigView(address, this);
    if (!remote)
        view->setContents(read.bytes, read.existed);
    m_editors.append(view);

    connect(view, &ConfigView::zoomStepRequested, this, &MainWindow::stepLogFontSize);
    connect(view, &ConfigView::modifiedChanged, this,
            [this, view](bool) { updateConfigTabTitle(view); });
    connect(view, &QObject::destroyed, this, [this](QObject *obj) {
        m_editors.removeIf([obj](ConfigView *v) { return v == obj; });
    });

    m_tabs->addTab(view, QString());
    updateConfigTabTitle(view);
    m_tabs->setCurrentWidget(view);
    view->setFocus();
    updateEmptyState();

    if (remote) {
        // THE TAB IS UP BEFORE THE FAR END ANSWERS, which is M17's rule for a log and is
        // the same rule here: a connect is up to twenty seconds and may stop to ask for a
        // password, and the thread that opens must not be the thread that waits. A
        // refusal then KEEPS ITS TAB and says why, because a tab that appears and
        // vanishes is worse than one that explains itself.
        const auto host = RemoteLocation::parse(address);
        view->setBusy(true, host ? tr("Connecting to %1…").arg(host->displayHost())
                                 : tr("Connecting…"));
        auto *transfer = new ConfigTransfer(view); // destroying the tab abandons it
        connect(transfer, &ConfigTransfer::readFinished, view,
                [this, view, transfer](const ConfigReadResult &result) {
                    view->setBusy(false, QString());
                    if (!result.ok) {
                        view->showNotice(result.error);
                    } else {
                        view->setContents(result.bytes, result.existed);
                        if (result.existed)
                            view->clearNotice();
                        else
                            view->showNotice(tr("%1 does not exist yet. Saving will "
                                                "create it.")
                                                 .arg(logSourceDisplayPath(view->address())));
                    }
                    updateConfigTabTitle(view);
                    transfer->deleteLater();
                });
        // The transfer owns the relay that carries a host-key question or a password
        // prompt to this thread — see ConfigTransfer, where the lifetime argument is.
        transfer->startRead(address);
        return view;
    }

    if (!read.existed) {
        view->showNotice(tr("%1 does not exist yet. Saving will create it.")
                             .arg(logSourceDisplayPath(address)));
    }
    return view;
}

void MainWindow::updateConfigTabTitle(ConfigView *view)
{
    const int index = m_tabs->indexOf(view);
    if (index < 0)
        return;
    QString name = view->displayName();
    name.replace(u'&', QLatin1String("&&")); // the tab bar reads '&' as a mnemonic
    // A TRAILING mark, which is already this application's vocabulary for "something is
    // in force here" — the pane docks wear one. The two leading marks are taken and mean
    // something else: the hollow one is "not there yet" and the filled one is "something
    // arrived while you were away".
    const QString title = view->isModified() ? QStringLiteral("%1 •").arg(name) : name;
    // Only on a real change: QTabBar::setTabText relays the whole bar out whether or not
    // the text moved, and this runs on every keystroke that flips the modified flag.
    if (m_tabs->tabText(index) != title)
        m_tabs->setTabText(index, title);
    const QString tip = logSourceDisplayPath(view->address());
    if (m_tabs->tabToolTip(index) != tip)
        m_tabs->setTabToolTip(index, tip);
    updateActionStates();
}

void MainWindow::saveActiveConfig()
{
    ConfigView *view = activeConfigView();
    if (!view || view->isBusy())
        return;

    if (configAddressIsRemote(view->address())) {
        // THE BYTES ARE TAKEN NOW, and the revision with them. Clearing the modified flag
        // when the reply arrives is only honest if nothing was typed while it was in
        // flight — otherwise a keystroke made during a slow remote save is marked saved
        // and is lost at the next close with no prompt.
        const QByteArray payload = view->toBytes();
        const int sentAt = view->revision();
        view->setBusy(true, tr("Saving %1…").arg(view->displayName()));
        updateActionStates();
        auto *transfer = new ConfigTransfer(view);
        connect(transfer, &ConfigTransfer::writeFinished, view,
                [this, view, transfer, sentAt](const ConfigWriteResult &result) {
                    view->setBusy(false, QString());
                    if (!result.ok) {
                        view->showNotice(result.error);
                    } else {
                        if (view->revision() == sentAt)
                            view->setModified(false);
                        view->clearNotice();
                        statusBar()->showMessage(tr("Saved %1").arg(view->displayName()), 5000);
                    }
                    updateConfigTabTitle(view);
                    updateActionStates();
                    transfer->deleteLater();
                });
        transfer->startWrite(view->address(), payload);
        return;
    }

    const ConfigWriteResult result = writeConfigFile(view->address(), view->toBytes());
    if (!result.ok) {
        // A save failure names a directory or a permission the reader has to act on, so
        // it goes in the page's own notice, which stays — not the status bar's transient
        // channel and not m_statusLabel, which updateStatus() rewrites on every tick.
        view->showNotice(result.error);
        return;
    }
    view->setModified(false);
    updateConfigTabTitle(view);
    if (!result.error.isEmpty()) {
        // Saved, but with something worth saying — a permission that could not be put
        // back. Not a failure, so the flag is cleared above, and still not silent.
        view->showNotice(result.error);
        return;
    }
    view->clearNotice();
    statusBar()->showMessage(tr("Saved %1").arg(view->displayName()), 5000);
}

bool MainWindow::confirmDiscard(ConfigView *view)
{
    if (!view || !view->isModified())
        return true;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Unsaved changes"));
    box.setText(tr("%1 has unsaved changes.").arg(view->displayName()));
    box.setInformativeText(tr("Save them before closing?"));
    box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Save);
    box.setEscapeButton(QMessageBox::Cancel);
    switch (box.exec()) {
    case QMessageBox::Save: {
        const ConfigWriteResult result = writeConfigFile(view->address(), view->toBytes());
        if (!result.ok) {
            // A FAILED save is a Cancel. Closing anyway would throw the work away after
            // the reader explicitly asked to keep it, which is the one outcome this
            // dialog exists to prevent.
            view->showNotice(result.error);
            return false;
        }
        view->setModified(false);
        return true;
    }
    case QMessageBox::Discard:
        return true;
    default:
        return false;
    }
}

void MainWindow::closeViewAt(int index)
{
    // An editor page is closed here too. Without this branch the qobject_cast below
    // fails, the function returns, and the tab's own ✕ button silently does nothing.
    if (auto *editor = qobject_cast<ConfigView *>(m_tabs->widget(index))) {
        if (!confirmDiscard(editor))
            return; // the reader cancelled: the tab stays, with its edits
        m_tabs->removeTab(index);
        delete editor; // its destroyed handler takes it out of m_editors
        updateEmptyState();
        updateActionStates();
        return;
    }

    auto *view = qobject_cast<DocumentView *>(m_tabs->widget(index));
    if (!view)
        return;
    // Out of the tab widget first: that moves the current page (and so the active
    // view) to a surviving tab while this one is still whole. Deleting it then runs
    // onViewDestroyed, which reaps the file if this was its last view.
    m_tabs->removeTab(index);
    delete view;
}

void MainWindow::closeActiveView()
{
    closeViewAt(m_tabs->currentIndex());
}

// --- Active view / document ------------------------------------------------

DocumentContext *MainWindow::activeContext() const
{
    return m_activeView ? m_activeView->context() : nullptr;
}

DocumentView *MainWindow::viewOfPath(const QString &path) const
{
    for (DocumentView *view : m_views) {
        if (view->context()->doc->path() == path)
            return view;
    }
    return nullptr;
}

DocumentContext *MainWindow::contextOfPath(const QString &address) const
{
    // Compared through logSettingsKey(), unlike viewOfPath() above, because the callers
    // here arrive with a SETTINGS address — one that has been round-tripped through the
    // store — while a context holds the spelling the log was opened with. The two differ
    // over a symlink, a relative path and an `ssh://` URL with no port.
    const QString key = logSettingsKey(address);
    for (const auto &ctx : m_contexts) {
        if (ctx->doc && logSettingsKey(ctx->doc->path()) == key)
            return ctx.get();
    }
    return nullptr;
}

Document *MainWindow::activeDocument() const
{
    DocumentContext *ctx = activeContext();
    return ctx ? ctx->doc.get() : nullptr;
}

LogView *MainWindow::activeLogView() const
{
    return m_activeView ? m_activeView->logView() : nullptr;
}

LogModel *MainWindow::activeModel() const
{
    DocumentContext *ctx = activeContext();
    return ctx ? ctx->model : nullptr;
}

void MainWindow::setActiveView(DocumentView *view)
{
    if (m_activeView == view)
        return;

    DocumentContext *outgoing = activeContext();
    Document *before = activeDocument();
    m_activeView = view;
    Document *after = activeDocument();

    updateActionStates();

    // Rebind the panes only when the FILE changes: a second view onto the same log
    // shares its filters and highlighters, and a needless rebind would reset the
    // filter pane's discovered-value state (invariant #7, ARCHITECTURE.md §12.3).
    if (before != after) {
        stashPaneState(outgoing);
        emit activeDocumentChanged(after);
        hydratePanes(activeContext());
    }

    // Arriving at a log is what "seen" means (M19, SPEC.md §7). Outside the
    // file-changed branch above, because switching between two views of one file still
    // brings that file forward even though the panes do not rebind.
    clearUnseenMatch(activeContext());

    updateStatus();
}

void MainWindow::stashPaneState(DocumentContext *ctx)
{
    if (!ctx || !m_filterPane)
        return;
    ctx->filterState = m_filterPane->saveState();
    // A tab switched away persists what its pane held. applyActiveFilters() has usually
    // done it already — this is the case it cannot cover, where the pane's debounce had
    // not fired before the reader moved on.
    persistFileSettings(ctx);
}

void MainWindow::hydratePanes(DocumentContext *ctx)
{
    if (!ctx || !m_filterPane)
        return;
    // Unconditionally, including for an empty state: restoreState() falls back to the
    // pane's defaults key by key, which is exactly what a file with no stashed state
    // should show. Skipping it would leave the PREVIOUS file's filters on screen,
    // now bound to this one. This emits filtersChanged, which applies them to the
    // freshly-bound document.
    m_filterPane->restoreState(ctx->filterState);
}

void MainWindow::updateActionStates()
{
    DocumentContext *ctx = activeContext();
    const bool hasFile = ctx != nullptr;
    // `hasFile` says a LOG IS BOUND; `onLog` says the page in front is that log. They
    // used to be the same question and are not any more: the bound document deliberately
    // does not move when an editor tab comes forward (see onCurrentTabChanged), so every
    // action that acts on m_activeView must ask `onLog` or it acts on a log the reader is
    // not looking at.
    const bool onLog = activePageIsLog();
    ConfigView *editor = activeConfigView();

    if (m_copyAction)
        m_copyAction->setEnabled(onLog);
    if (m_copyColumnsAction)
        m_copyColumnsAction->setEnabled(onLog);
    if (m_selectAllAction)
        m_selectAllAction->setEnabled(onLog);
    // The config editor's own two. Save is VISIBLE only on an editor page, which is what
    // was asked for; Open Config File Editor needs a log to resolve a path against.
    if (m_openConfigAction)
        m_openConfigAction->setEnabled(hasFile);
    if (m_saveConfigAction) {
        m_saveConfigAction->setVisible(editor != nullptr);
        m_saveConfigAction->setEnabled(editor != nullptr && editor->isModified());
    }
    // hasFile, NOT onLog, and for openConfigAction's reason one line up: this acts on the
    // BOUND log's address and never on the view in front, so it stays live on that log's
    // config-editor page — which is exactly where somebody is standing when they decide to
    // bounce the service they have just reconfigured. It is live on a WAITING log too,
    // which is the case it is most wanted in: the application is down, which is why the
    // log has not turned up.
    //
    // Deliberately NOT gated on a script being configured. A disabled QAction swallows
    // Ctrl+R with no feedback, and there is an answer to give — see restartActiveApp().
    if (m_restartAppAction)
        m_restartAppAction->setEnabled(hasFile);
    // Find and its two navigations, on hasFile and nothing else. NOT on the query being
    // non-empty: these two carry F3 and Shift+F3, and a disabled QAction swallows its
    // shortcut with no feedback at all — so gating on the query would delete the only
    // place that can answer somebody who pressed F3 with an empty box. That answer is
    // runFind()'s empty-pattern branch instead. The query is also per VIEW (the bar is a
    // DocumentView child) and changes per keystroke, so it would put a window-wide menu
    // state on the typing path to tell the reader something the empty box in front of
    // them already says.
    // Find is the deliberate EXCEPTION to the onLog rule above: a config editor searches
    // too, so these stay enabled on either kind of page. The comment above still governs
    // why they are not gated on the query being non-empty.
    const bool canFind = hasFile || editor != nullptr;
    if (m_findAction)
        m_findAction->setEnabled(canFind);
    if (m_findNextAction)
        m_findNextAction->setEnabled(canFind);
    if (m_findPreviousAction)
        m_findPreviousAction->setEnabled(canFind);
    // Only a spooled log has a fetcher to poke; a local one is watched, not connected.
    if (m_reconnectAction) {
        m_reconnectAction->setEnabled(
            onLog && dynamic_cast<SpooledLogSource *>(ctx->doc->source()) != nullptr);
    }
    // Enabled for a WAITING document too: there it means "try now" rather than
    // "re-read", which is exactly what somebody staring at a tab that says a log has not
    // turned up wants the key to do.
    if (m_reloadAction)
        m_reloadAction->setEnabled(onLog);
    // Any page can be closed, log or editor — the editor branch in closeViewAt() is what
    // answers for one. Gated on hasFile it would take Ctrl+W away from an editor tab.
    if (m_closeTabAction)
        m_closeTabAction->setEnabled(m_tabs->count() > 0);
    if (m_closeAllAction)
        m_closeAllAction->setEnabled(m_tabs->count() > 0);
    if (m_newViewAction)
        m_newViewAction->setEnabled(onLog);
    if (m_followAction) {
        m_followAction->setEnabled(onLog);
        // With no file the next open follows again (SPEC.md §3); with one, the
        // checkbox tracks that view's own follow state.
        m_followAction->setChecked(hasFile ? m_activeView->logView()->following() : true);
    }
    if (m_toggleWrapAction)
        m_toggleWrapAction->setEnabled(onLog);
    if (m_wrapGroup) {
        // Wrap belongs to the view (invariant #7) and a log now opens in the mode its
        // settings name, so the checked entry has to follow whichever view is in front
        // rather than record one window-wide choice.
        const int mode =
            hasFile ? int(m_activeView->logView()->wrapMode()) : int(LogView::WrapMode::Off);
        for (QAction *a : m_wrapGroup->actions()) {
            a->setEnabled(onLog);
            if (a->data().toInt() == mode)
                a->setChecked(true);
        }
    }
    // The timestamp mode is per FILE, so the checkmark has to follow the active tab.
    updateTimeDisplayActions();
    // Filters are per FILE too (invariant #7), so switching tabs can move this either
    // way even though nothing about the filters themselves changed.
    updateClearFiltersState();
    if (m_progressBox) {
        m_progressBox->setVisible(hasFile && ctx->indexing);
        if (hasFile && ctx->indexing)
            m_progressBar->setValue(ctx->progressPercent);
    }

    // The title names WHAT IS IN FRONT, which with an editor page is the config file
    // and not the log still bound behind it. Naming the log there would be a title
    // describing a tab the reader is not looking at.
    if (editor)
        setWindowTitle(tr("loftail — %1").arg(editor->displayName()));
    else if (hasFile)
        setWindowTitle(tr("loftail — %1").arg(logSourceDisplayName(ctx->doc->path())));
    else
        setWindowTitle(QStringLiteral("loftail"));
}

void MainWindow::updateClearFiltersState()
{
    if (!m_clearFiltersAction)
        return;
    // One question, asked in one place. hasActiveFilters() is the resolved answer the
    // Filters tab's marker is already drawn from — an axis that is ticked but excludes
    // nothing does not count, because applyToDocument() collapses it — and it answers
    // false with no document bound, which is the no-file-open case as well.
    //
    // Context (M15's before/after N) counts, exactly as it does for the marker, and the
    // reason is what this action DOES rather than what the FilterSet holds: clearAll()
    // zeroes the two spinners, so a pane with context set and nothing else has work for
    // this item to do and a grey menu entry would be a lie. That it is inert without a
    // text axis is a statement about what the reader sees, not about whether the
    // setting is there to be cleared.
    //
    // The active-tab check is on the pane's binding rather than on activeContext():
    // setActiveView() calls updateActionStates() BEFORE it rebinds the panes, so
    // reading the context here would report the incoming tab's presence against the
    // outgoing tab's filters. The rebind emits activityChanged() whenever the answer
    // actually moves, so the pane's own state is what settles it either way.
    m_clearFiltersAction->setEnabled(m_filterPane && m_filterPane->hasActiveFilters());
}

void MainWindow::chooseFileToOpen()
{
    // getOpenFileNames, not getOpenFileName: several logs are open at once (SPEC.md
    // §3), and dragging them onto the window used to be the only way to ask for more
    // than one — which leaves out anyone without a file manager on screen.
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Open Log Files"), QString(),
        // The archive filter is offered whether or not libarchive is compiled in, so
        // the two builds' dialogs look alike: a file that simply vanished from the list
        // would read as "loftail cannot see this", where trying it explains itself.
        tr("Log files (*.log *.txt);;"
                       "Compressed and archived logs "
                       "(*.gz *.bz2 *.xz *.zst *.zip *.tar *.tgz *.tar.gz *.tar.bz2 "
                       "*.tar.xz *.txz *.tar.zst *.7z);;"
                       "All files (*)"));
    openFiles(paths);
}

void MainWindow::chooseRemoteToOpen()
{
    HostBookmarkStore store(HostBookmarkStore::defaultDir());
    OpenRemoteDialog dialog(&store, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    refreshRemoteHostsMenu(); // the dialog may have saved or removed a host
    const QString url = dialog.chosenUrl();
    if (!url.isEmpty())
        openFile(url);
}

void MainWindow::refreshRemoteHostsMenu()
{
    if (!m_remoteHostsMenu)
        return;
    m_remoteHostsMenu->clear();

    const HostBookmarkStore store(HostBookmarkStore::defaultDir());
    const QVector<HostBookmark> hosts = store.all();
    if (hosts.isEmpty()) {
        QAction *none = m_remoteHostsMenu->addAction(tr("(none saved)"));
        none->setEnabled(false);
        return;
    }

    for (const HostBookmark &host : hosts) {
        if (host.paths.isEmpty()) {
            // A host with no remembered log opens the dialog pre-filled rather than
            // guessing at a path.
            QAction *action = m_remoteHostsMenu->addAction(
                QStringLiteral("%1...").arg(host.displayName()));
            connect(action, &QAction::triggered, this, [this, host] {
                HostBookmarkStore store(HostBookmarkStore::defaultDir());
                OpenRemoteDialog dialog(&store, this);
                dialog.preset(host, QString());
                if (dialog.exec() == QDialog::Accepted && !dialog.chosenUrl().isEmpty())
                    openFile(dialog.chosenUrl());
                refreshRemoteHostsMenu();
            });
            continue;
        }
        // One flat entry per remembered log rather than a submenu per host: a host
        // usually has one or two logs worth reopening, and a submenu that deep costs
        // a hover and a second aim for what is a single click's worth of choice.
        for (const QString &path : host.paths) {
            QAction *action = m_remoteHostsMenu->addAction(
                QStringLiteral("%1: %2").arg(host.displayName(), path));
            const QString url = host.locationFor(path).toString();
            connect(action, &QAction::triggered, this, [this, url, host, path] {
                // Carry this host's poll cadence and tail-start choice into the
                // fetcher about to be built for it.
                setSshFetchOptions(host.locationFor(path), host.fetchOptions());
                openFile(url);
            });
        }
    }
}

void MainWindow::closeAllDocuments(Prompt prompt)
{
    // The editor pages first, and a Cancel on any of them ABANDONS the whole gesture —
    // File ▸ Close All means all of them or none, not "as many as happened to be clean".
    // AlreadyAsked is what keeps the quit path from asking twice: closeEvent() has to ask
    // BEFORE it saves the session, so by the time it reaches here the answer is in.
    if (prompt == Prompt::Ask) {
        const QVector<ConfigView *> editors = m_editors;
        for (ConfigView *editor : editors) {
            if (!confirmDiscard(editor))
                return;
        }
    }
    {
        const QSignalBlocker block(m_tabs);
        const QVector<ConfigView *> editors = m_editors;
        for (ConfigView *editor : editors) {
            if (const int index = m_tabs->indexOf(editor); index >= 0)
                m_tabs->removeTab(index);
            delete editor;
        }
        m_editors.clear();
    }

    if (m_contexts.empty()) {
        m_activeView = nullptr;
        updateEmptyState();
        updateActionStates();
        return;
    }

    // Take the whole set down at once, so the per-view reaping in onViewDestroyed —
    // which erases from the very containers being iterated here — stays out of it.
    const QVector<DocumentView *> views = m_views;
    m_views.clear();
    m_activeView = nullptr;
    // Empty the tab bar before deleting anything: removing tabs one at a time would
    // walk the current page through every surviving view on the way down.
    {
        const QSignalBlocker block(m_tabs);
        m_tabs->clear(); // removes the pages; ownership returns to us
    }
    for (DocumentView *view : views) {
        disconnect(view, &QObject::destroyed, this, &MainWindow::onViewDestroyed);
        delete view;
    }

    // Only now the contexts: a view references its context's model and Document,
    // and ~DocumentContext destroys both.
    for (auto &ctx : m_contexts) {
        ctx->views.clear();
        // As in onViewDestroyed: a deferred filter edit must not outlive the document
        // it was made against.
        if (m_filterPane)
            m_filterPane->documentClosing(ctx->doc.get());
    }
    m_contexts.clear(); // ~DocumentContext stops the workers and deletes the model

    updateEmptyState();

    // Unbind the panes from the now-gone document (invariant #7).
    emit activeDocumentChanged(nullptr);
    updateActionStates();
}

void MainWindow::primeRemoteCredentials(const QString &path)
{
    if (!RemoteLocation::isRemote(path))
        return;
    const auto location = RemoteLocation::parse(path);
    if (!location)
        return;

    const HostBookmarkStore store(HostBookmarkStore::defaultDir());
    bool found = false;
    const HostBookmark bookmark = HostBookmarkStore::find(store.all(), *location, &found);
    if (!found || !bookmark.savePassword || bookmark.password.isEmpty())
        return;

    // Never overwrite one the server has already accepted this session.
    const QString target = location->target();
    if (!SshCredentialCache::has(target))
        SshCredentialCache::remember(target, bookmark.password);

    // Only the FILE is primed here, deliberately. The cache is consulted after the agent
    // and the key files, so priming cannot cause a stored password to be sent to a host
    // that would have signed in with a key — and reading hosts.json is a silent local
    // file read. A keychain read is neither: it can raise an unlock dialog, so it stays
    // where the auth chain puts it, behind the agent (SshSession::authenticate).
}

void MainWindow::reportOpenRefusal(const QString &displayName, const QString &reason)
{
    m_openRefusals.append({displayName, reason});
    // Outside any gesture — a single openFile() from a menu item or a recent-files
    // entry — there is nothing to wait for, so say it now.
    if (m_openBatchDepth == 0)
        showOpenRefusals();
}

void MainWindow::beginOpenBatch()
{
    ++m_openBatchDepth;
}

void MainWindow::endOpenBatch()
{
    if (--m_openBatchDepth > 0)
        return;
    m_openBatchDepth = 0;
    showOpenRefusals();
    // The pool's MRU index, once per GESTURE rather than once per log. touch() moves a
    // tick in memory and nothing else, so a restored session of twenty tabs costs one
    // atomic map write here instead of twenty — which is the whole reason it defers.
    m_fileStore.flush();
}

void MainWindow::showOpenRefusals()
{
    if (m_openRefusals.isEmpty())
        return;

    // Capped, because one gesture can name any number of logs — a drop of a directory's
    // worth, a session of a hundred tabs — and a strip that grows with them would push
    // the log off the screen to say the same thing a hundred ways.
    constexpr int kMaxRefusalsShown = 8;

    QString text;
    if (m_openRefusals.size() == 1) {
        text = tr("Cannot open %1: %2")
                   .arg(m_openRefusals.first().first, m_openRefusals.first().second);
    } else {
        QStringList lines;
        for (const auto &refusal : std::as_const(m_openRefusals)) {
            if (lines.size() == kMaxRefusalsShown)
                break;
            // Not tr(): a name, a colon and a reason, both halves already translated.
            lines.append(QStringLiteral("%1: %2").arg(refusal.first, refusal.second));
        }
        if (const int more = int(m_openRefusals.size() - lines.size()); more > 0)
            lines.append(tr("… and %1 more").arg(more));
        text = tr("Cannot open these logs:") + u'\n' + lines.join(u'\n');
    }
    m_openRefusals.clear();

    m_openNoticeText->setText(text);
    // Taken from the CURRENT palette every time rather than at construction, so the
    // colour follows a theme changed under a running window (UiColors.h).
    QPalette notice = m_openNoticeText->palette();
    notice.setColor(QPalette::WindowText, errorColor(palette()));
    m_openNoticeText->setPalette(notice);
    m_openNotice->setVisible(true);
}

void MainWindow::clearOpenNotice()
{
    m_openRefusals.clear();
    m_openNoticeText->clear();
    m_openNotice->setVisible(false);
}

// openFiles() and openFile() call each other by design — picking several members out
// of one archive opens them through openFiles(), which is why beginOpenBatch() /
// endOpenBatch() nest and only the outermost renders. The recursion is one level deep
// and cannot become more: an archive inside an archive is refused in words rather than
// expanded (CLAUDE.md, M12).
// NOLINTNEXTLINE(misc-no-recursion)
bool MainWindow::openFiles(const QStringList &rawPaths, const QString &pattern)
{
    // ONE message for the lot, and the bracket is what makes it one: each refusal
    // inside the loop reports its own address and its own reason, and they are shown
    // together when the gesture ends. Reported one at a time, only the last one's
    // reason would be left on screen, with the earlier files silently missing from the
    // tab bar and nothing anywhere saying they had been asked for.
    beginOpenBatch();
    bool allOpened = true;
    for (const QString &raw : rawPaths) {
        // Each refusal has already named itself through reportOpenRefusal(); all that
        // is left to answer here is whether there were any.
        if (!openFile(raw, pattern))
            allOpened = false;
    }
    endOpenBatch();
    return allOpened;
}

// NOLINTNEXTLINE(misc-no-recursion): the other half of the openFiles() cycle above.
bool MainWindow::openFile(const QString &rawPath, const QString &pattern)
{
    // Normalize a remote URL to its one spelling FIRST, before it becomes a Document
    // path (RemoteLocation.h). Everything downstream compares these strings —
    // viewOfPath(), the recent-files dedupe, the format-cache key, the session — so
    // two spellings of one remote file would otherwise open two tabs on it and
    // remember its format twice. A local path passes through untouched.
    const QString path = normalizeLogPath(rawPath);

    // A saved host's remembered password, before anything tries to connect. Here because
    // openFile() is the single funnel every entry point goes through — the Open dialog,
    // Open Remote, the Remote Hosts menu, recent files, drag-and-drop, the command line
    // and session restore.
    primeRemoteCredentials(path);

    // An archive naming no member cannot be opened, so ask which log is wanted —
    // EXACTLY HERE and nowhere else. Document::prepare(), rescan() and session restore
    // must only ever see an address that already names one, or a dialog could appear
    // behind the user's back during a rotation or a relaunch. Cancelling abandons the
    // open silently, the same contract cancelling Preferences has.
    if (const auto archive = ArchiveLocation::split(path); archive && archive->needsMember()) {
        QString error;
        const QStringList members =
            OpenArchiveDialog::chooseMembers(archive->container, this, &error);
        if (!error.isEmpty()) {
            reportOpenRefusal(logSourceDisplayName(path), error);
            return false;
        }
        // Several picked logs open as several tabs, exactly as dropping several files
        // does (SPEC.md §3) — through the same funnel, so a member that will not open
        // is reported the way one of several dropped files is. A CANCELLED pick lands
        // here as an empty list and is a success: abandoning the open is silent.
        return openFiles(members, pattern);
    }

    // THREE LEVELS, ONE ANSWER (SPEC.md §4). The deepest node that names this log wins
    // WHOLE — its own per-log node, else the first file pattern that matches it, else
    // the defaults. The levels never mix: a node is taken entire, exactly as the
    // per-file cache and the default this replaced never merged.
    //
    // An explicitly supplied pattern (--pattern) overrides just the pattern, so a
    // command line naming one keeps the resolved encoding and source zone rather than
    // silently reverting them to auto-detect. It WINS over every level — that is what
    // the switch is for — but it is not BELIEVED: it goes on to be checked against the
    // file exactly as a resolved node is, and openWithSettings() persists it only if it
    // fits (SPEC.md §3, §4). An empty value carries no pattern and is the bare launch:
    // there is nothing to override with, so the resolved levels stand.
    FormatSettings settings = resolvedProfile(path).format;
    if (!pattern.isEmpty())
        settings.pattern = pattern;

    return openWithSettings(path, settings);
}

bool MainWindow::openWithSettings(const QString &path, FormatSettings settings,
                                  std::optional<RunSelection> runRestore)
{
    // Prepare the candidate document and settle its format BEFORE touching the
    // document currently on screen: cancelling the format dialog aborts the open
    // entirely (SPEC.md §4), and an aborted open must leave the open file alone.
    auto doc = std::make_unique<Document>();
    ManualFormatProvider provider(settings.pattern);
    {
        // No wait cursor, and nothing to wait for: opening a remote log no longer
        // connects on this thread (§6.3.3). It returns in microseconds with a tab that
        // says it is connecting, and the connect happens behind it.
        const bool prepared =
            doc->prepare(path, provider, settings.encoding, settings.sourceZone.toZone());
        if (!prepared) {
            reportOpenRefusal(logSourceDisplayName(path), doc->lastError());
            return false;
        }
    }
    doc->setTimeDisplay(settings.timeDisplay);

    // EVERY format is checked against the file, whatever supplied it — a resolved
    // per-log node, a file pattern, the defaults, or --pattern (SPEC.md §4). This is a
    // LOCAL and it starts true: it was a parameter, and the one caller that ever set it
    // false was the command line, which is how `--pattern` came to overwrite a working
    // remembered format with an unparseable one and SAVE it (bugs.md 15). A format that
    // does not fit costs a dialog; it never costs a wall of plain text, and it never
    // costs the settings that were already there.
    bool promptIfNoMatch = true;

    // Decide whether to remember this format on close of the flow. A dialog the user
    // accepted, or a format that actually matched, are worth persisting; one the user
    // declined is not (so reopen re-prompts rather than silently showing plain text),
    // and neither is one nothing has been judged against yet.
    bool persist = false;

    // A log with no bytes yet has nothing to preview, autodetect from, or seed a dialog
    // with — and asking about a format before anyone has seen a line of the file would
    // be asking the user to guess too. It opens anyway and settles its format from the
    // bytes that actually arrive (Document::resume). Nothing is persisted either: a
    // pattern never checked against a line of the log is not knowledge, and remembering
    // it would suppress the format prompt forever.
    //
    // TWO KINDS OF NO BYTES, one rule. A log that is not there yet opens WAITING; a log
    // that is there and EMPTY opens as an ordinary tab, because it exists and may stay
    // empty for ever — that is the file a service that has not logged yet leaves behind,
    // and the very one somebody opens to watch it start. Asking formatSettled() rather
    // than isWaiting() is what covers the second: before this it went to offerFormat()
    // with a 0-byte sample, so Preferences appeared over "No sample lines to preview."
    // with Detect greyed out, and cancelling it — the only sensible answer — refused the
    // open outright.
    const bool nothingToJudgeYet = doc->isWaiting() || !doc->formatSettled();
    const bool deferFormatPrompt = nothingToJudgeYet && promptIfNoMatch;
    if (nothingToJudgeYet) {
        promptIfNoMatch = false;
        persist = false;
    }

    if (promptIfNoMatch) {
        switch (offerFormat(doc.get(), path, &settings)) {
        case FormatOutcome::Matched:
            persist = true; // the default matched — remember it
            break;
        case FormatOutcome::Chosen: {
            ManualFormatProvider chosen(settings.pattern);
            if (!doc->prepare(path, chosen, settings.encoding, settings.sourceZone.toZone())) {
                reportOpenRefusal(logSourceDisplayName(path), doc->lastError());
                return false;
            }
            doc->setTimeDisplay(settings.timeDisplay);
            persist = true;
            break;
        }
        case FormatOutcome::Declined:
            // The only format we have is one the user just refused to confirm, and
            // opening with it would show a wall of unparsed plain text — so abort the
            // open instead (SPEC.md §4). Whatever was already open stays open, untouched.
            m_statusLabel->setText(tr("Open cancelled: %1").arg(logSourceDisplayName(path)));
            return false;
        }
    }

    // An open ADDS a file: several logs are open at once, each in its own tab
    // (SPEC.md §3). Reopening a file already open just raises its view.
    if (DocumentView *existing = viewOfPath(path)) {
        showView(existing);
        return true;
    }

    auto ctx = std::make_unique<DocumentContext>();
    ctx->doc = std::move(doc);
    ctx->settings = settings;
    // WHAT IS STORED FOR THIS LOG (M21), read once as the tab is made and kept EXACTLY
    // as it was read. It is the baseline persistFileSettings()'s change gate compares
    // against, so filling its profile in with the settings this open is about would make
    // the very first write look like no change — which is how a fitting `--pattern`
    // silently stopped being remembered.
    ctx->fileSettings = m_fileStore.read(path);
    // HOW THIS LOG WAS BEING READ, back from where it was left (SPEC.md §10). An ordinary
    // open restores it now, not only a session restore: the filters belong to the log, so
    // returning to a log a week later shows it the way it was being read rather than
    // unfiltered. Empty means the pane's defaults, which is what a log nobody has
    // filtered gets.
    ctx->filterState = ctx->fileSettings.filters;
    // AN OPEN is what the MRU counts, and this is one. In memory only; the map rides out
    // with endOpenBatch().
    m_fileStore.touch(path);
    // The caller's explicit choice wins; otherwise the log's own stored run, which is
    // why an ordinary open now restores one and not only a session restore. Inert
    // until a run-start pattern actually splits the log, so it costs nothing on a log
    // that has no runs.
    ctx->pendingRunRestore = runRestore;
    if (!ctx->pendingRunRestore && !settings.runStartPattern.isEmpty()
        && !ctx->fileSettings.run.saysNothing())
        ctx->pendingRunRestore = ctx->fileSettings.run;
    // The prompt this open could not raise, deferred to the first resume that has bytes.
    ctx->pendingFormatPrompt = deferFormatPrompt;

    // A log nothing has ever been saved for starts with the level colours (SPEC.md §7):
    // ERROR and FATAL rendered exactly like TRACE otherwise, which is the one thing a
    // reader opens a log to find. Here and in prepareContext(), which are the only two
    // routes to a new Document, and NOT in Document itself — a seed belongs where "has
    // anything been stored for this file?" can be asked, and putting it in core would
    // colour every document every test builds. The rules are the user's from this moment
    // on: deleting them all is saved as an empty list, and the restore route below reads
    // an empty list as an answer rather than as silence.
    //
    // AFTER the format has settled, never before: a rule is resolved against a format,
    // and the FormatOutcome::Chosen branch above re-prepares the document.
    //
    // PRESENCE, NEVER EMPTINESS. A record that has never spoken about rules gets the
    // seed; one carrying an EMPTY list is the user having deleted every rule, and it
    // stays deleted. Reading the emptiness instead re-seeds the level colours on every
    // launch, which is the shape of a bug nobody can get rid of.
    // Bound to a local first, and not `ctx->fileSettings.highlighters` twice over: the
    // check that reads an optional cannot follow the second mention back through
    // unique_ptr's operator-> to the first, and calls the dereference unguarded.
    const std::optional<QJsonArray> &storedRules = ctx->fileSettings.highlighters;
    ctx->doc->highlighters() = storedRules ? HighlighterSet::fromJson(*storedRules)
                                           : HighlighterSet::defaults();
    ctx->doc->resolveHighlighters();

    m_contexts.push_back(std::move(ctx));
    // Before the view exists, so the tab is titled right the first time — and so the
    // logs already open grow a parent directory now if this one shares their name.
    relabelTabs();

    buildViewAndIndex(m_contexts.back().get());

    if (persist)
        persistFileSettings(m_contexts.back().get());
    rememberRecentFile(path);
    return true;
}

DocumentView *MainWindow::createView(DocumentContext *ctx)
{
    auto *view = new DocumentView(ctx);
    ctx->views.append(view);
    connect(view, &DocumentView::findRequested, this, &MainWindow::runFind);

    LogView *logView = view->logView();
    // The mode a new view starts in comes from this log's settings node (M20, SPEC.md
    // §5) — a seed, not a per-file property: the view owns it from here, and the
    // session restores each view's own saved mode over this one.
    logView->setWrapMode(resolvedProfile(ctx->doc->path()).wrapMode);
    // A view made for a document that is ALREADY waiting — the first view of a waiting
    // open, a restored tab, or a second view onto one — needs the message now; the
    // waitingChanged signal it would otherwise learn from has already fired.
    if (ctx->doc->isWaiting())
        logView->setPlaceholderText(ctx->doc->waitReason());
    // Reflect follow state in the View menu (M6): the checkbox tracks the ACTIVE
    // view, and the overlay button/scroll gestures keep them in sync.
    connect(logView, &LogView::followingChanged, this, [this, view](bool following) {
        if (m_followAction && m_activeView == view)
            m_followAction->setChecked(following);
    });
    logView->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(logView->header(), &QWidget::customContextMenuRequested,
            this, &MainWindow::showColumnMenu);
    // Right-clicking a RECORD offers what that record can filter and highlight by
    // (SPEC.md §5). The view carries the click; the window builds the menu, because
    // it is the only thing that can reach both the record's fields and the panes the
    // items edit.
    connect(logView, &LogView::recordMenuRequested, this,
            [this, view](int row, int column, const QPoint &globalPos) {
                showRecordMenu(view, row, column, globalPos);
            });
    // Double-clicking a Subsystem or Thread cell shows only that value (SPEC.md §5) —
    // the same menu item, reached without the menu. Connected on the table only, for the
    // reason recordMenuRequested is: the digest strip's rows are its own index's, and
    // the window reads the DOCUMENT's when it resolves a view row.
    connect(logView, &LogView::recordDoubleClicked, this,
            [this, view](int row, int column) { activateRecordColumn(view, row, column); });
    // Ctrl+Alt+click and Alt+click on a cell are the record menu's Show Only and Hide,
    // reached without the menu (SPEC.md §5). Connected on the table only, for the reason
    // the two above are.
    connect(logView, &LogView::recordShowOnlyRequested, this,
            [this, view](int row, int column) {
                applyRecordFilter(view, row, column, RecordFilterCommand::ShowOnly);
            });
    connect(logView, &LogView::recordHideRequested, this,
            [this, view](int row, int column) {
                applyRecordFilter(view, row, column, RecordFilterCommand::Hide);
            });
    // Ctrl+wheel over either table asks for a size, and the WINDOW answers — one size
    // for the application, so a view that re-fonted itself would leave every other tab
    // behind. The strip is wired too: it is a log table under the pointer like any other.
    connect(logView, &LogView::zoomStepRequested, this, &MainWindow::stepLogFontSize);
    connect(view->digestView(), &LogView::zoomStepRequested, this,
            &MainWindow::stepLogFontSize);

    // Into the bookkeeping BEFORE the tab bar: adding the first tab makes it current
    // at once, and the active-view handling that fires from it reads m_views.
    m_views.append(view);
    connect(view, &QObject::destroyed, this, &MainWindow::onViewDestroyed);
    m_tabs->addTab(view, QString()); // titled by updateTabTitles below

    // First run only: the panes' size hints would otherwise claim about half the
    // window. A restored session brings its own proportions.
    if (!m_layoutRestored && m_contexts.size() == 1 && !m_paneDocks.isEmpty()) {
        m_layoutRestored = true;
        resizeDocks({m_paneDocks.first()}, {width() / 3}, Qt::Horizontal);
    }

    updateEmptyState();
    updateTabTitles(ctx); // numbers the tabs when a file has several views
    return view;
}

void MainWindow::showView(DocumentView *view)
{
    // Adding a tab does not raise it, so without this the status bar and panes would
    // describe a tab that is not on top.
    if (m_tabs->indexOf(view) >= 0)
        m_tabs->setCurrentWidget(view); // -> onCurrentTabChanged -> setActiveView
    setActiveView(view);
    view->logView()->setFocus();
}

void MainWindow::newViewOfActiveDocument()
{
    DocumentContext *ctx = activeContext();
    if (!ctx)
        return;

    // A second view onto the same file starts as a copy of the one it was made from
    // and diverges as the user scrolls, selects, wraps or resizes columns. Everything
    // else — index, filters, highlighters, run selection, live tail — is shared.
    LogView *source = m_activeView->logView();
    DocumentView *view = createView(ctx);
    LogView *fresh = view->logView();
    fresh->setWrapMode(source->wrapMode());
    fresh->restoreColumnState(source->saveColumnState());
    // Open where the source view is looking, rather than at record 0 — the new view
    // is a second window onto the same place, and the user splits it to compare
    // against what they were already reading.
    if (source->following())
        fresh->followTail();
    else if (source->currentRecord() >= 0)
        fresh->setCurrentRecord(source->currentRecord());
    showView(view);
}

DocumentContext *MainWindow::prepareContext(const SessionDocument &d, QString *error)
{
    // Resolved through the settings tree like any other open (M20), not carried in the
    // session: one home for a log's settings means a Preferences edit reaches a restored
    // tab, which it could not while the session held its own copy.
    const FormatSettings format = resolvedProfile(d.path).format;

    auto doc = std::make_unique<Document>();
    ManualFormatProvider provider(format.pattern);
    if (!doc->prepare(d.path, provider, format.encoding, format.sourceZone.toZone())) {
        if (error)
            *error = doc->lastError();
        return nullptr;
    }
    doc->setTimeDisplay(format.timeDisplay);

    auto owned = std::make_unique<DocumentContext>();
    DocumentContext *ctx = owned.get();
    ctx->doc = std::move(doc);
    ctx->settings = format;
    // The stored record, as openWithSettings() reads it — this and that function are the
    // only two routes to a new Document, and both have to ask (M21).
    ctx->fileSettings = m_fileStore.read(d.path);
    m_fileStore.touch(d.path);

    // The log's own record answers; the SESSION's copy is consulted only where it says
    // nothing, which is the one-time migration off the old home (M21). A session written
    // by an older build still carries these keys and this build no longer writes them, so
    // the first quit after upgrade takes them away and there is nothing to remember that
    // the migration ran. A record that already speaks WINS, so a crash before that quit
    // re-adopts the same values harmlessly and a later edit is never overwritten.
    ctx->filterState = ctx->fileSettings.filters.isEmpty() ? d.filters
                                                           : ctx->fileSettings.filters;
    if (!format.runStartPattern.isEmpty()) {
        // The record answers; the session's copy is the one-time migration, consulted
        // only where the record says nothing (see the filters above).
        ctx->pendingRunRestore = ctx->fileSettings.run.saysNothing()
            ? RunSelection{d.runAll, d.selectedRunStartOffset, d.selectedRunStartTimestamp}
            : ctx->fileSettings.run;
    }
    m_contexts.push_back(std::move(owned));
    // Restore builds its contexts one at a time, so every one of them relabels the set:
    // two same-named logs coming back from a session read exactly as they would if they
    // had just been opened by hand.
    relabelTabs();

    // Highlight rules go straight onto the Document rather than through the pane:
    // the pane holds one file's rules at a time, and every restored file needs its
    // own. HighlighterPane::setDocument reads them back out when this file is shown.
    //
    // PRESENCE, NEVER the array's emptiness — the same rule HighlightRule::fromJson
    // applies one level down to "actions" and LogProfile to "pattern". A stored EMPTY
    // list is the user having deleted every rule, which must stay deleted; only a store
    // that says nothing about this file's rules AT ALL gets the level colours seeded,
    // exactly as a first open does. Read empty as absent and a deleted default comes back
    // on every launch, which is the shape of a bug nobody can get rid of.
    //
    // The log's own record answers first; the session's copy is the one-time migration
    // (see the filters above), and asks the same question of the key it used to own.
    if (ctx->fileSettings.highlighters)
        ctx->doc->highlighters() = HighlighterSet::fromJson(*ctx->fileSettings.highlighters);
    else if (d.highlighters.contains(QStringLiteral("rules")))
        ctx->doc->highlighters() =
            HighlighterSet::fromJson(d.highlighters.value(QStringLiteral("rules")).toArray());
    else
        ctx->doc->highlighters() = HighlighterSet::defaults();
    // Rules match nothing until they have been resolved once. Indexing has not run
    // yet, so this binds only the format and display zone (and compiles each text
    // axis) — the intern-table pass follows when the scan reports names.
    ctx->doc->resolveHighlighters();

    // The model and the controller only. Views are created by the caller, which is
    // what lets session restore rebuild a file's several views in their saved tab
    // order; and indexing is not started, so every tab exists before any worker runs.
    buildContext(ctx);
    return ctx;
}

void MainWindow::buildContext(DocumentContext *ctx)
{
    Document *doc = ctx->doc.get();

    const bool dark = palette().base().color().lightness()
                      < palette().text().color().lightness();

    ctx->model = new LogModel(doc);
    ctx->model->setDarkTheme(dark);

    // The digest strip's model (M19): the same Document, read through its digest subset
    // instead of its filtered one, and coloured by the Digest action rather than the
    // Color one — which is what makes a digest row wear the colours of the rule that put
    // it there whether or not that rule also colours the log above.
    ctx->digestModel = new LogModel(doc);
    ctx->digestModel->setDarkTheme(dark);
    ctx->digestModel->setViewIndex(&doc->digest());
    ctx->digestModel->setHighlightAction(HighlightAction::Digest);

    // Configure the run-start matcher from the (remembered) format before binding the
    // panes, so the Run pane shows the pattern. Runs are detected once indexing
    // finishes (the index is empty here); see onIndexFinished (§3a).
    doc->setRunStart(ctx->settings.runStartPattern, ctx->settings.runStartIsRegex,
                     ctx->settings.runStartCaseSensitive ? Qt::CaseSensitive
                                                         : Qt::CaseInsensitive);

    buildIndexController(ctx);
}

// The scanning half of buildContext(), on its own because a RELOAD rebuilds exactly this
// and nothing else: the model and the digest model are what the live views hold, so they
// must survive, while the controller is destroyed by stopWorkers() and has to come back.
void MainWindow::buildIndexController(DocumentContext *ctx)
{
    ctx->controller = new IndexController(ctx->doc.get(), ctx->model);
    connect(ctx->controller, &IndexController::progress, this,
            [this, ctx](qint64 done, qint64 total) { onIndexProgress(ctx, done, total); });
    connect(ctx->controller, &IndexController::finished, this,
            [this, ctx](bool cancelled) { onIndexFinished(ctx, cancelled); });
    ctx->indexing = true;
    ctx->progressPercent = 0;
    m_progressBar->setRange(0, 100);
}

void MainWindow::buildViewAndIndex(DocumentContext *ctx)
{
    buildContext(ctx);

    // Column layout is per VIEW and lives in the session (SPEC.md §5). A newly
    // opened file starts on the format's own default columns.
    DocumentView *view = createView(ctx);

    // Show the file just opened and make it active, which binds the panes to its
    // Document (invariant #7). Their discovered subsystem/thread lists fill in as
    // indexing progresses.
    showView(view);
    ctx->controller->start();
}

// Does `settings->pattern` actually fit the bytes now readable in `doc`, and if not, what
// does the user want to do about it?
//
// ONE COPY, shared by the two moments this question arises: an ordinary open, and the
// first time a log that opened WAITING has real bytes to judge against
// (resumeOrSettleDocument). Those two used to be a copy of each other with a comment
// promising they agreed; since M17 every remote and archived log takes the second route,
// so a divergence would mean the format prompt behaving differently for local and remote
// logs — the sort of thing nobody would notice for a year.
// Does this document's compiled format actually match the bytes it can now read? The
// half of offerFormat() that asks nobody anything, for the callers that must not.
bool MainWindow::formatFits(Document *doc, const FormatSettings &settings)
{
    const qint64 sampleLen = qMin<qint64>(64LL * 1024, doc->source()->size());
    const QByteArray sample = sampleLen > 0
        ? doc->source()->bytes(0, sampleLen).toByteArray() : QByteArray();
    Decoder decoder = Decoder::detect(sample, settings.encoding);
    return FormatPreview::build(doc->format(), sample, decoder).matchedCount > 0;
}

MainWindow::FormatOutcome MainWindow::offerFormat(Document *doc, const QString &path,
                                                  FormatSettings *settings)
{
    if (formatFits(doc, *settings))
        return FormatOutcome::Matched;

    const qint64 sampleLen = qMin<qint64>(64LL * 1024, doc->source()->size());
    const QByteArray sample = sampleLen > 0
        ? doc->source()->bytes(0, sampleLen).toByteArray() : QByteArray();

    // What resolved for this log did not match. Autodetect (M8, ARCHITECTURE.md §9) and
    // PRE-FILL the node with the detected pattern for confirmation — never applied
    // silently. A no-detection result leaves it seeded with what was resolved.
    LogProfile seed = resolvedProfile(path);
    seed.format = *settings;
    DetectingFormatProvider detector(settings->encoding);
    detector.formatFor(QByteArrayView(sample.constData(), sample.size()));
    if (detector.detected())
        seed.format.pattern = detector.detectedPattern();

    // Preferences, opened on a node for THIS log. The whole tree is reachable from it,
    // so the answer can be given once for a class of logs (a file pattern) rather than
    // for this one — which is the level the two-store arrangement had no room for.
    PreferencesDialog dlg(m_logSettings, logSourceDisplayName(path), sample, this);
    dlg.selectLog(path, m_fileStore.read(path).profile, seed);
    if (dlg.exec() != QDialog::Accepted) {
        // Everything the dialog touched went with its working copy. Nothing is persisted
        // and nothing is applied.
        return FormatOutcome::Declined;
    }
    commitPreferences(dlg, path);
    *settings = resolvedProfile(path).format;
    return FormatOutcome::Chosen;
}

void MainWindow::showPreferences()
{
    // Preview the default against whichever log is open, so it can be checked against
    // real lines instead of typed blind. There may be none — the dialog is reachable
    // with an empty window, which is when someone most wants to set this up.
    QByteArray sample;
    if (DocumentContext *ctx = activeContext(); ctx && ctx->doc && ctx->doc->source()) {
        const qint64 sampleLen = qMin<qint64>(64LL * 1024, ctx->doc->source()->size());
        if (sampleLen > 0)
            sample = ctx->doc->source()->bytes(0, sampleLen).toByteArray();
    }

    // Re-read before showing rather than trusting the in-memory copy: another instance
    // may have written the tree since this window started (§8.1, last writer wins).
    m_logSettings = m_settingsStore.load();

    QString activePath;
    if (DocumentContext *ctx = activeContext(); ctx && ctx->doc)
        activePath = ctx->doc->path();

    PreferencesDialog dlg(m_logSettings,
                          activePath.isEmpty() ? QString() : logSourceDisplayName(activePath),
                          sample, this);
    if (!activePath.isEmpty()) {
        // Open on the log in front of the user, so the common errand — "this one is not
        // parsing" — needs no navigation. The row shows what the log has stored, or what
        // it inherits where it has stored nothing; OK stores that only if it ends up
        // saying something new.
        dlg.selectLog(activePath, m_fileStore.read(activePath).profile,
                      resolvedProfile(activePath));
        dlg.setApplyTarget(logSourceDisplayName(activePath));
    }
    if (dlg.exec() != QDialog::Accepted)
        return;

    commitPreferences(dlg, activePath);

    // LAST, because applying re-reads the log: it stops this document's workers, empties
    // its index and starts a fresh scan. The context and the Document itself now SURVIVE
    // that — a format change rebuilds them in place rather than replacing them (§6.6) —
    // but everything this function has read about them is stale afterwards, so nothing
    // that depends on the old format may follow.
    if (dlg.applyRequested())
        applyProfileToActive(dlg.applyProfile());
}

void MainWindow::applyProfileToActive(const LogProfile &p)
{
    DocumentContext *ctx = activeContext();
    if (!ctx || !ctx->doc)
        return;
    const QString path = ctx->doc->path();

    // Wrap first, into the record AND the views already on screen: applySettings() below
    // may reindex, and a view built afterwards seeds its mode from the record.
    // EVERY non-format field has to be named here by hand, and a field added to
    // LogProfile without a line of its own is one "Apply to current file" silently
    // ignores — the button appears to do nothing for that setting alone, which reads as
    // the whole button being broken. `stored` starts from resolvedProfile(), so an
    // unnamed field keeps its OLD value rather than being cleared, which is why the
    // omission is invisible until somebody changes that setting and presses Apply.
    LogProfile stored = resolvedProfile(path);
    stored.wrapMode = p.wrapMode;
    stored.configPath = p.configPath;
    stored.restartScript = p.restartScript;
    ctx->fileSettings.profile = stored;
    persistFileSettings(ctx);
    for (DocumentView *v : std::as_const(ctx->views))
        v->logView()->setWrapMode(p.wrapMode);

    // The format last: a pattern or encoding change re-reads the log in place from here
    // (§6.6), which is what makes this the one entry point Preferences has.
    applySettings(p.format);
}

QString MainWindow::aboutText()
{
    // The release and the build are shown on lines of their own rather than as the
    // single `0.1.0+100.g443daf4` token --version prints: that token is one word
    // because a shell grep and a Windows message box both wanted it to be, and here
    // there is room to say which half is which. Both are still present verbatim, so
    // pasting either into a bug report matches what CI stamped.
    const QString build = applicationBuildId();
    const QString buildLine = build.isEmpty()
        // Empty is the ordinary value for a build made on a developer's machine, not
        // a failure to read one (Version.h) — so it says what the binary IS, rather
        // than leaving a blank field that reads as a missing value.
        ? tr("Build: local build")
        : tr("Build: %1").arg(build);

    return QStringLiteral("loftail ") + applicationVersion() + QLatin1Char('\n')
        + buildLine + QLatin1Char('\n') + tr("A viewer for log4cplus logs.");
}

void MainWindow::showAbout()
{
    QMessageBox box(this);
    box.setWindowTitle(tr("About loftail"));
    box.setIcon(QMessageBox::NoIcon);
    // Plain text, because the build id is machine-written and a '<' arriving from a
    // stamped value must not be read as markup — QMessageBox auto-detects otherwise.
    box.setTextFormat(Qt::PlainText);
    box.setText(aboutText());
    // Selectable: the reason this dialog exists is to be copied into a bug report,
    // and a QMessageBox's text is not selectable by default.
    box.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    box.exec();
}

void MainWindow::applySettings(const FormatSettings &newSettings)
{
    DocumentContext *ctx = activeContext();
    if (!ctx)
        return;
    Document *doc = ctx->doc.get();

    const QString path = doc->path();
    const FormatSettings old = ctx->settings;
    const bool patternChanged  = newSettings.pattern != old.pattern;
    const bool encodingChanged = newSettings.encoding != old.encoding;
    const bool sourceChanged   = newSettings.sourceZone != old.sourceZone;
    const bool displayChanged  = newSettings.timeDisplay != old.timeDisplay;

    // A format change re-indexes (or reparses) but does not carry a persisted run
    // selection; the newest run is the default afterwards. A display-mode change
    // re-derives nothing, so it must NOT drop a restore still waiting on a scan.
    if (patternChanged || encodingChanged || sourceChanged)
        ctx->pendingRunRestore.reset();

    ctx->settings = newSettings;
    persistFileSettings(activeContext());

    // Pattern or encoding change alters record boundaries and byte offsets (§6.1,
    // invariant #3), so the index is invalid — read the file again through the new
    // format, IN PLACE.
    //
    // This used to call openWithSettings(), and had done nothing at all since M9. That
    // function begins "reopening a file already open just raises its view" — which is
    // right for an open and fatal here, because the file being reformatted is by
    // definition already open: the freshly prepared Document was discarded and the
    // existing tab merely raised. Before tabs (commit f197c0d) the same line read
    // teardownDocument(), which is what had made a format change reindex. So Preferences
    // ▸ Apply to current file, the timestamp header menu and the deferred format prompt
    // all silently did nothing for four milestones. It goes through the rebuild path now,
    // which keeps the tab, the views, the filters and the highlight rules rather than
    // replacing the document — the thing openWithSettings() could never have done.
    if (patternChanged || encodingChanged) {
        // A WAITING document has no bytes to re-read and must not have its workers torn
        // down (§6.5). It settles its format on the resume instead — but only if it is
        // asked to: a log that vanished AFTER it had been read is still "settled", so
        // without this it would come back wearing the format it had when it disappeared.
        if (doc->isWaiting()) {
            doc->unsettleFormat();
            diagLog("app", QStringLiteral("format changed while waiting for %1 — it will "
                                          "settle when the log arrives").arg(path));
            updateStatus();
            return;
        }
        diagLog("app", QStringLiteral("format changed for %1 — re-reading (%2)")
                           .arg(path, QString::fromLatin1(patternChanged ? "pattern"
                                                                         : "encoding")));
        rebuildDocument(ctx, KeepFormat::No);
        return;
    }

    // Source-zone change re-derives timestamps only, over the existing index (§5.1).
    if (sourceChanged)
        doc->reparseTimestamps(newSettings.sourceZone.toZone());

    // A display-mode change (or a source change, which moves the derived zone when
    // the mode is "as written") is a free reformat — just repaint (§5.1). This
    // branch can never lead to a rescan or a reparse.
    if (sourceChanged || displayChanged) {
        doc->setTimeDisplay(newSettings.timeDisplay);
        for (DocumentView *v : std::as_const(ctx->views))
            v->logView()->viewport()->update();
        // Run labels and the two panes' time-range editors all render in the display
        // zone, so they go stale with it if left alone. A highlight rule's time axis
        // holds wall clock exactly as a filter's does — leaving its digits alone would
        // silently re-point the rule at a different instant.
        if (m_runPane)
            m_runPane->refresh();
        if (m_filterPane)
            m_filterPane->refreshTimeBounds();
        if (m_highlighterPane)
            m_highlighterPane->refreshTimeBounds();
        updateTimeDisplayActions();
    }

    updateStatus();
}

void MainWindow::commitPreferences(const PreferencesDialog &dlg, const QString &address)
{
    // THE DIALOG APPLIES NOTHING and now writes to nothing either: it hands back a tree
    // and one log's own settings, and this is the single place both are stored. Two exits
    // reach it — offerFormat()'s mid-open confirmation and File ▸ Preferences — and they
    // used to write the one store between them with two spellings.
    //
    // The SWEEP is gated on the tree having actually moved, and that gate is not an
    // optimisation. A per-log record stops saying anything of its own just as surely when
    // the pattern above it is edited, added, reordered or deleted — and nothing writes
    // that record, so nothing re-tests it. But re-testing means reading every record in
    // the pool, which is affordable once on an OK and not at all on the keystroke-by-
    // keystroke rebuilds inside the dialog, where its predecessor used to live.
    const bool treeMoved = dlg.tree() != m_logSettings;
    m_logSettings = dlg.tree();
    m_settingsStore.save(m_logSettings);
    if (treeMoved)
        m_fileStore.pruneAgainst(m_logSettings);

    if (address.isEmpty())
        return;

    // What the log itself should have afterwards — nullopt when the row ended up saying
    // exactly what it inherits, which is also how Delete and Promote finish.
    //
    // Written STRAIGHT to the store rather than through persistFileSettings(), and
    // `ctx->settings` is deliberately left alone. That funnel builds the record's format
    // out of `ctx->settings`, which is what the tab is reading NOW — and this dialog does
    // not apply anything (SPEC.md §4): the settings reach the log on the next open, or
    // through "Apply to current file", which the caller performs after this returns.
    // Writing `ctx->settings` here would also make applySettings()'s diff — the thing
    // that decides between a rescan, a reparse and a repaint — see no change at all, so
    // the apply would store the new format and never re-read the log with it.
    DocumentContext *ctx = contextOfPath(address);
    LogFileSettings record = ctx ? ctx->fileSettings : m_fileStore.read(address);
    record.address = logSettingsKey(address);
    record.profile = dlg.fileProfile();
    m_fileStore.save(record, m_logSettings.inherited(address));
    if (ctx)
        ctx->fileSettings = record; // the change gate's baseline moves with the disk
}

LogProfile MainWindow::resolvedProfile(const QString &address)
{
    // AN OPEN LOG IS ANSWERED FROM ITS TAB. The context holds what that tab is actually
    // reading — a format the user has just changed reaches ctx->fileSettings before it
    // reaches the pool — so a second view created in between must not open on the older
    // answer, and a resolution taken mid-gesture must not disagree with the one on screen.
    const QString key = logSettingsKey(address);
    for (const auto &ctx : m_contexts) {
        if (!ctx->doc || logSettingsKey(ctx->doc->path()) != key)
            continue;
        // The tab's LIVE answer: the format it is actually reading, over the wrap seed its
        // own record or its pattern supplies. `settings` is the half that can be newer
        // than the disk — a `--pattern` override, or a format just confirmed in the
        // dialog — so a second view created in between must not open on the older one.
        LogProfile p = ctx->fileSettings.profile.value_or(m_logSettings.inherited(address));
        p.format = ctx->settings;
        return p;
    }

    // Otherwise the log's own record, and failing that what it inherits — the deepest
    // level that names it, taken WHOLE, exactly as the three-level tree always resolved
    // (SPEC.md §4). The two upper levels simply live in another file now.
    if (const auto own = m_fileStore.read(address).profile)
        return *own;
    return m_logSettings.inherited(address);
}

std::optional<RunSelection> MainWindow::runSelectionOf(const DocumentContext *ctx)
{
    // NOTHING TO READ YET, so nothing is written and the stored section is left exactly
    // as it was. While the scan is running — or while a restore is still armed, which is
    // the same thing one step later — runs() is empty and selectedRun() is -1, and -1
    // with a run-start pattern set is the "all runs" branch below. So a format change or
    // a resume arriving mid-scan would silently overwrite a PINNED run with "all runs",
    // which is the one state SPEC.md §3a says only the user moves.
    if (!ctx || !ctx->doc || ctx->indexing || ctx->pendingRunRestore)
        return std::nullopt;

    const Document *doc = ctx->doc.get();
    RunSelection out;

    // "LAST RUN" IS ASKED FIRST and names no run, so it saves no offset at all: storing
    // the run it currently resolves to would bring the log back PINNED to a run that has
    // since finished, which is the one thing the mode exists not to do.
    const int sel = doc->selectedRun();
    if (doc->followingLastRun()) {
        out.all = false;
        out.startOffset = -1;
    } else if (sel >= 0 && sel < doc->runs().size()) {
        out.all = false;
        out.startOffset = doc->runs().at(sel).startOffset;
        out.startTimestamp = doc->runs().at(sel).startTimestamp;
    } else {
        out.all = !ctx->settings.runStartPattern.isEmpty();
        out.startOffset = -1;
    }
    return out;
}

void MainWindow::persistFileSettings(DocumentContext *ctx)
{
    if (!ctx || !ctx->doc)
        return;

    LogFileSettings next = ctx->fileSettings;
    next.address = logSettingsKey(ctx->doc->path());
    // The wrap mode is carried over from what the record already holds, so a format
    // change does not quietly reset it: this is reached from the format dialog, the
    // timestamp header menu and the Run pane, none of which sets it.
    LogProfile p = next.profile.value_or(m_logSettings.inherited(next.address));
    p.format = ctx->settings;
    next.profile = p;

    // THE PANE ONLY WHERE IT IS THIS LOG'S. The Filters pane is global and follows the
    // active document, so reading it for a BACKGROUND context would persist the tab on
    // screen wearing another log's filters. Every other context is answered by its stash,
    // which is what stashPaneState() keeps up to date — the same split saveSession()
    // already makes.
    next.filters = (ctx == activeContext() && m_filterPane) ? m_filterPane->saveState()
                                                            : ctx->filterState;

    // Straight off the Document, which HighlighterPane keeps authoritative — it syncs on
    // every edit — so this is right for a background file just as much as for the one on
    // screen, and needs no stash of its own.
    next.highlighters = ctx->doc->highlighters().toJson();

    // Left exactly as stored while there is nothing to read: see runSelectionOf().
    if (const auto run = runSelectionOf(ctx))
        next.run = *run;

    // THE CHANGE GATE, and it is not an optimisation. This is reached on every resume of
    // a remote or archived log, so without it an identical record is rewritten — and the
    // map with it — once per poll.
    if (next == ctx->fileSettings)
        return;
    ctx->fileSettings = next;

    // Creates, updates or DELETES the record — one exists only while it says something
    // the log would not inherit anyway, which is what save() applies on the way in.
    m_fileStore.save(next, m_logSettings.inherited(next.address));
}

void MainWindow::updateTabTitles(DocumentContext *ctx)
{
    // A background file's scan has no claim on the status bar, so its progress shows
    // in its own tab title instead.
    //
    // The name comes from relabelTabs(), which decided it against every OTHER open log
    // (TabLabels.h) — two logs called app.log each bracket on the most prominent thing
    // that tells them apart. Read from the cache and never recomputed here: this runs on
    // every ingest tick, and the answer cannot have moved unless a log opened or closed.
    // The fallback keeps a context that has somehow not been labelled yet showing its own
    // name rather than an empty tab.
    QString name = ctx->tabLabel.isEmpty() ? logSourceDisplayName(ctx->doc->path())
                                           : ctx->tabLabel;
    name.replace(u'&', QLatin1String("&&")); // the tab bar reads '&' as a mnemonic
    QString base = name;
    if (ctx->indexing)
        base = tr("%1 — indexing %2%").arg(name).arg(ctx->progressPercent);
    else if (ctx->doc->isWaiting())
        // A tab with no records in it, so that a glance at the tab bar tells a log
        // that is empty from one that is not there (SPEC.md §3). The tooltip carries
        // the sentence; the title only has room for the mark.
        base = QStringLiteral("◦ %1").arg(name);
    else if (ctx->unseenMatch)
        // A highlight rule carrying the Tab action matched something while this log was
        // not on screen (M19, SPEC.md §7). Filled against the hollow mark above, so the
        // pair reads as "something arrived" against "not there yet" with no legend.
        //
        // The order of these three is not cosmetic. A LiveController only exists once
        // indexing has finished, and a waiting document ingests nothing — so both
        // earlier branches are states in which this marker cannot arise, and putting
        // this one first would only make the "indexing" text unreachable for a tab that
        // matched something on a previous open. tst_multidoc's waitUntilIndexed() polls
        // for the absence of "indexing" in every title, which "● " does not contain.
        base = QStringLiteral("● %1").arg(name);
    // Several views onto one file are numbered, so two identically-named tabs are
    // still tellable apart. The numbering runs left to right along the tab bar, not
    // in creation order, so a dragged tab does not end up as [2] left of [1].
    QVector<int> indices;
    indices.reserve(ctx->views.size());
    for (DocumentView *view : std::as_const(ctx->views)) {
        if (const int index = m_tabs->indexOf(view); index >= 0)
            indices.append(index);
    }
    std::ranges::sort(indices);
    const bool numbered = indices.size() > 1;
    for (int i = 0; i < indices.size(); ++i) {
        const QString title =
            numbered ? QStringLiteral("%1 [%2]").arg(base).arg(i + 1) : base;
        // Only on a real change. QTabBar::setTabText relays the whole bar out whether or
        // not the text moved, and this runs on every ingest tick of every open log — the
        // trap the Filters pane's dock title already guards against.
        if (m_tabs->tabText(indices.at(i)) != title)
            m_tabs->setTabText(indices.at(i), title);
        // Unchanged by the labelling above: the tooltip is the FULL address, which is
        // what makes shortening the label safe.
        const QString tip = ctx->doc->isWaiting()
            ? QStringLiteral("%1\n%2").arg(ctx->doc->path(), ctx->doc->waitReason())
            : ctx->doc->path();
        if (m_tabs->tabToolTip(indices.at(i)) != tip)
            m_tabs->setTabToolTip(indices.at(i), tip);
    }
}

void MainWindow::relabelTabs()
{
    QStringList addresses;
    addresses.reserve(int(m_contexts.size()));
    for (const auto &ctx : m_contexts)
        addresses.append(ctx->doc->path());

    // WHAT MAY NOT BE EVICTED (M21). The pool is bounded, so storing one log's settings
    // can cost another log theirs — and the one cost that is never acceptable is a log
    // somebody has open, whose tab is still reading the record and will rewrite it. This
    // is where the set of open logs changes, which is the whole definition of this
    // function, so this is where the pool is told.
    m_fileStore.setPinned(QSet<QString>(addresses.begin(), addresses.end()));

    // One pass over the whole set: what a log is called depends on which others are
    // open, so closing one of two app.logs has to shorten the survivor back again.
    const QStringList labels = tabLabelsFor(addresses);
    for (int i = 0; i < labels.size(); ++i) {
        if (m_contexts[i]->tabLabel == labels.at(i))
            continue;
        m_contexts[i]->tabLabel = labels.at(i);
        updateTabTitles(m_contexts[i].get()); // a file with no view yet is a no-op
    }
}

void MainWindow::onIndexProgress(DocumentContext *ctx, qint64 done, qint64 total)
{
    if (total > 0)
        ctx->progressPercent = int((done * 100) / total);
    updateTabTitles(ctx);
    // Guarded the way isBeingRead() is, and for the same reason: activeContext() is
    // null when no tab is current, and a bare `ctx == activeContext()` then reads as
    // true for a null ctx — which cannot happen (the handler is a lambda holding the
    // context that owns this controller) but which nothing in the signature says.
    if (ctx && ctx == activeContext()) {
        m_progressBar->setValue(ctx->progressPercent);
        updateStatus();
    }
}

void MainWindow::onIndexFinished(DocumentContext *ctx, bool cancelled)
{
    Document *doc = ctx->doc.get();
    ctx->indexing = false;
    updateTabTitles(ctx);
    const bool isActive = ctx == activeContext();
    if (isActive)
        m_progressBox->setVisible(false);

    // The full subsystem/thread value sets are known now, so fill the panes'
    // auto-discovered lists (SPEC.md §6) and re-run any active filter over the
    // completed index. The panes show the ACTIVE document, so only refresh them
    // when this is it; another file finishing in the background must not repaint
    // its values into the pane bound to a different log.
    if (isActive) {
        if (m_filterPane)
            m_filterPane->refreshDiscoveredLists();
        // Re-resolve highlight rules against the now-complete intern table so rules
        // naming subsystems discovered late in the scan take effect (SPEC.md §6, §7).
        if (m_highlighterPane)
            m_highlighterPane->refreshDiscoveredLists();
    }

    // The intern tables are complete too, so a Subsystem or Thread column can now be
    // seeded from the widest name in the file rather than from a guess (SPEC.md §5,
    // ARCHITECTURE.md §7.1). Here and nowhere else on this path: once per scan, never
    // per ingest tick, and the seed itself skips every column the user, a fit or a
    // restored session has already spoken for. Every view of the file, not just the
    // active one — a background tab is read the moment it is raised.
    for (DocumentView *v : std::as_const(ctx->views))
        v->logView()->seedColumnWidths();

    doc->resolveHighlighters();
    // Runs are detected now that the full index exists (§3a). Restore the
    // persisted selection if this open came from session restore, else default
    // to the last run — and to FOLLOWING it (decision: open a live log on the run
    // the application is in now, and stay on it across a restart).
    doc->detectRuns();
    if (ctx->pendingRunRestore) {
        if (ctx->pendingRunRestore->all)
            doc->selectRun(RunPane::kAllRuns);
        else if (ctx->pendingRunRestore->startOffset < 0)
            // No offset saved with runAll false is how "Follow the last" is written: it names
            // no run because it names none — see saveSession(), which is also why this
            // needed no schema bump.
            doc->selectLastRun();
        else
            doc->selectRunByStart(ctx->pendingRunRestore->startOffset,
                                  ctx->pendingRunRestore->startTimestamp);
        ctx->pendingRunRestore.reset();
    } else {
        doc->selectLastRun();
    }
    if (isActive && m_runPane)
        m_runPane->refresh();
    if (doc->filters().anyActive() || doc->viewRestricted())
        applyFiltersFor(ctx, KeepPosition::No); // every view is sent to the end below
    // AFTER the run selection settles, never with resolveHighlighters() above it: the
    // digest is bounded by the selected run, so building it before selectNewestRun()
    // would scan the whole file and then describe the wrong part of it.
    rebuildDigestFor(ctx);

    for (DocumentView *v : std::as_const(ctx->views)) {
        v->logView()->viewport()->update(); // repaint with resolved highlights
        v->logView()->scrollToEnd();        // open at the file's end (SPEC.md §3)
    }
    if (isActive)
        updateStatus();
    if (cancelled) {
        // Cancelling the scan cancels the FETCHING too. No LiveController is created
        // below, so nothing would ever read what a fetcher went on writing — and for
        // an archive that means expanding gigabytes into a cache for a log the user
        // has just said they are done with.
        if (auto *spooled = dynamic_cast<SpooledLogSource *>(doc->source())) {
            if (const auto &spool = spooled->spool())
                spool->cancel();
        }
        if (isActive) {
            m_statusLabel->setText(m_statusLabel->text()
                                   + tr("  (indexing cancelled)"));
        }
        return; // a cancelled scan is not watched — the user chose to stop reading it
    }

    startWatching(ctx);
}

// Activate the always-watched model (SPEC.md §3, M6): from here the file auto-updates as
// it grows. The scan that just finished captured the size at open, so the controller's
// first check catches up anything appended while it ran.
//
// Its own function because a RELOAD has to re-arm it too — stopWorkers() destroys the
// live controller along with the index worker, and a log that came back without its watch
// would look identical to one that is being watched right up until it next grew.
void MainWindow::startWatching(DocumentContext *ctx)
{
    {
        ctx->live = new LiveController(ctx->doc.get(), ctx->model);
        // So the digest's wholesale ordinal remap is bracketed by a model reset before
        // the mutation rather than after it (M19).
        ctx->live->setDigestModel(ctx->digestModel);
        connect(ctx->live, &LiveController::ingested, this, [this, ctx](qint64) {
            // ABOVE the early return, deliberately (M19). A background tab is the ONLY
            // case the tab marker and the notification exist for, so anything that
            // handles them below this line works perfectly with one tab open and never
            // fires in real use.
            handleAlerts(ctx);
            // ABOVE the early return for the same reason, and a stronger one: this
            // moves what a background tab is SHOWING (§3a). Below the line, a log that
            // restarts while its tab is in the background would stay on the finished
            // run until something else re-applied its view.
            followLastRunIfMoved(ctx);
            if (ctx != activeContext()) {
                updateStatus();
                return;
            }
            // Newly discovered subsystems/threads and the growing counts (SPEC.md §6).
            if (m_filterPane)
                m_filterPane->refreshDiscoveredLists();
            if (m_highlighterPane)
                m_highlighterPane->refreshDiscoveredLists();
            // A new run may have begun in the appended data (§3a): reflect it in the
            // Run pane. The Document already folded it into its run list during ingest.
            if (m_runPane)
                m_runPane->refresh();
            updateStatus();
        });
        // Expansion and fetch progress, and the failures SPEC.md §3 promises are
        // reported rather than popped up. Stored per file; only the active one shows.
        connect(ctx->live, &LiveController::sourceStatusChanged, this,
                [this, ctx](const QString &text) {
                    ctx->sourceStatus = text;
                    updateStatus();
                });
        // The log this document is waiting for is back — or a log that opened empty has
        // just been written to for the first time, which asks for exactly the same thing
        // and comes down the same signal. The pattern lives here, not in core, so this
        // is where the provider gets built and resume() gets called (invariant #3).
        // resume() may decline — the log can go again between the check and the open —
        // in which case the document stays waiting and the next tick tries once more.
        connect(ctx->live, &LiveController::resumeRequested, this,
                [this, ctx]() { resumeOrSettleDocument(ctx); });
        connect(ctx->live, &LiveController::waitingChanged, this,
                [this, ctx](bool, const QString &reason) {
                    for (DocumentView *v : std::as_const(ctx->views))
                        v->logView()->setPlaceholderText(reason);
                    updateTabTitles(ctx);
                    updateStatus();
                });
        connect(ctx->live, &LiveController::rescanned, this, [this, ctx]() {
            // A rotation replaced every record, so anything this log was owing a
            // notification about described records that no longer exist (M19).
            ctx->alerts.reset();
            // Rotation/truncation reloaded silently (SPEC.md §3): refresh the panes
            // against the fresh index and keep following if we were.
            if (ctx == activeContext()) {
                if (m_filterPane)
                    m_filterPane->refreshDiscoveredLists();
                if (m_highlighterPane)
                    m_highlighterPane->refreshDiscoveredLists();
                if (m_runPane)
                    m_runPane->refresh();
            }
            for (DocumentView *v : std::as_const(ctx->views)) {
                if (v->logView()->following())
                    v->logView()->followTail();
            }
            updateStatus();
        });
        connect(ctx->live, &LiveController::reloaded, this,
                [this, ctx](ReloadCause cause) {
                    // BELOW the guard, and nothing may be hoisted above it — unlike the
                    // ingest handler, where handleAlerts() and followLastRunIfMoved()
                    // sit above their identical line deliberately (M19). A transient
                    // message is window chrome and there is one status bar: a background
                    // tab's rotation would spend it on a log the reader is not looking
                    // at, and cover the count of the one they are.
                    // The `!ctx ||` half is what onIndexProgress()'s comment records:
                    // activeContext() is null with no current tab, so the bare
                    // inequality would let a null ctx through to the dereference below.
                    if (!ctx || ctx != activeContext())
                        return;
                    announceReload(cause, ctx->doc->path());
                });
        ctx->live->start();
    }
}

// The log `ctx` has been waiting for is back (M13, SPEC.md §3). Reopen it, index it,
// and — if this document has never yet seen a byte of it — settle its format from the
// bytes that have now arrived.
//
// This lives here rather than in LiveController because the PATTERN lives here: core
// holds a compiled LogFormat and never a pattern string (invariant #3), so only the
// window can build the provider that resume() needs.
void MainWindow::reloadActiveDocument()
{
    DocumentContext *ctx = activeContext();
    if (!ctx || !ctx->doc)
        return;

    // A log that is not there yet has nothing to re-read, and tearing its workers down
    // would stop the one thing that is making progress — the watch tick that brings it
    // back (§6.5). So a reload of a WAITING document means "try now" instead: poke a
    // spooled one's fetcher, and for a local one simply leave the tick alone, since it
    // is already retrying within 750 ms of the key being pressed.
    if (ctx->doc->isWaiting()) {
        diagLog("app", QStringLiteral("reload of %1 while waiting — poking instead")
                           .arg(ctx->doc->path()));
        if (auto *spooled = dynamic_cast<SpooledLogSource *>(ctx->doc->source())) {
            if (const auto &spool = spooled->spool())
                spool->poke();
        }
        return;
    }

    diagLog("app", QStringLiteral("reload requested: %1 (records=%2)")
                       .arg(ctx->doc->path()).arg(ctx->doc->index().records.size()));
    rebuildDocument(ctx, KeepFormat::Yes);
}

// THE one way an open document is rebuilt in place, and both callers matter:
//
//   KeepFormat::Yes — View ▸ Reload (F5). Re-read the same bytes with the same compiled
//                     format (invariant #3).
//   KeepFormat::No  — the conversion pattern or the encoding changed, so every record
//                     boundary and byte offset in the index is wrong and the file has to
//                     be read again through a new format (§6.1).
//
// One function because the two differ in exactly one call and in nothing else, and the
// half they share is the half that is easy to get wrong: stopping the workers in the
// right order, bracketing the wholesale row replacement, re-arming the watch on BOTH
// outcomes, and handing the scan to the worker thread so a large log does not freeze the
// window. Two functions that promised to agree is how one of them quietly stops entering
// the waiting state when the log has gone.
void MainWindow::rebuildDocument(DocumentContext *ctx, KeepFormat keep)
{
    Document *doc = ctx->doc.get();

    // Both workers first, and in that order (stopWorkers): the live watcher would
    // otherwise tick into a document whose source is being replaced, and a scan in flight
    // would go on appending batches into the index this is about to empty.
    ctx->stopWorkers();

    // The visible set is replaced wholesale, which is the same signal — and the same
    // bracket — a rotation uses (LiveController::doRescan). The digest goes with it,
    // because every ordinal it holds names a record that is about to stop existing.
    ctx->model->beginFilterReset();
    if (ctx->digestModel)
        ctx->digestModel->beginFilterReset();
    bool ok = false;
    if (keep == KeepFormat::Yes) {
        ok = doc->reopen();
    } else {
        ManualFormatProvider provider(ctx->settings.pattern);
        ok = doc->reformat(provider, ctx->settings.encoding,
                           ctx->settings.sourceZone.toZone());
        if (ok)
            doc->setTimeDisplay(ctx->settings.timeDisplay);
    }
    if (ctx->digestModel)
        ctx->digestModel->endFilterReset();
    ctx->model->endFilterReset();

    if (!ok) {
        // The log went away between the gesture and the reopen. reopen()/reformat() have
        // already put the document into the waiting state where that is what happened, so
        // the watch below is what brings it back — which is why the live controller is
        // rebuilt on this path too, rather than only on the successful one.
        for (DocumentView *v : std::as_const(ctx->views))
            v->logView()->setPlaceholderText(doc->waitReason());
        if (!doc->isWaiting())
            ctx->formatNotice = tr("could not re-read this log — %1").arg(doc->lastError());
        startWatching(ctx);
        updateTabTitles(ctx);
        updateStatus();
        return;
    }

    ctx->formatNotice.clear();

    // A NEW format means new answers to questions the panes asked when they bound: which
    // axes this log can even offer (a pattern with no %t has no Thread axis, one with no
    // %d has no Time range), and what the timestamp editors are written in. Nothing else
    // would ask them again — the Document pointer has not changed, so activeDocumentChanged
    // does not fire on its own — and a pane left holding the old answers offers an axis
    // the format cannot fill.
    //
    // Through stash/hydrate, exactly as a tab switch does, so the user's own filter state
    // survives the rebind: setDocument() clears the pane's discovered-value bookkeeping by
    // design, and restoreState() is what puts the choices back.
    if (keep == KeepFormat::No && ctx == activeContext()) {
        stashPaneState(ctx);
        emit activeDocumentChanged(doc);
        hydratePanes(ctx);
    }

    // Re-scanned on a worker thread, exactly as an ordinary open is, so a rebuild of a
    // very large log shows a progress bar rather than freezing the window. Everything that
    // has to happen afterwards — runs re-detected, filters re-applied, highlights
    // re-resolved against the rebuilt intern tables, the digest rebuilt, the views sent to
    // the end, and the live watch re-armed — is onIndexFinished()'s job already, and is
    // reached here by the same signal an open reaches it by.
    buildIndexController(ctx);
    updateTabTitles(ctx);
    updateStatus();
    ctx->controller->start();
}

void MainWindow::resumeOrSettleDocument(DocumentContext *ctx)
{
    Document *doc = ctx->doc.get();
    const bool settleFormat = !doc->formatSettled();
    ManualFormatProvider provider(ctx->settings.pattern);

    ctx->model->beginFilterReset();
    if (ctx->digestModel)
        ctx->digestModel->beginFilterReset();
    const bool ok = doc->resume(provider);
    if (ok) {
        // The intern tables were built from scratch, exactly as after a rotation — and
        // so was the digest, which resume() cleared along with the filtered subset.
        doc->refreshHighlighting();
        if (doc->filters().anyActive() || doc->viewRestricted())
            doc->applyFilters();
    }
    if (ctx->digestModel)
        ctx->digestModel->endFilterReset();
    ctx->model->endFilterReset();

    // The log can go again between the watch tick that saw it and this open. That is
    // ordinary, not an error: the document is still waiting and the next tick retries.
    if (!ok)
        return;

    for (DocumentView *v : std::as_const(ctx->views)) {
        v->logView()->setPlaceholderText(QString());
        if (v->logView()->following())
            v->logView()->followTail();
    }

    // `doc->formatSettled()` is the second half of the condition and the whole of the
    // fix: resume() settles a format only where there were BYTES to settle it against,
    // and a log that turned up empty — a file created a moment before its first record
    // is written, which is what a real logging application does — leaves it false. The
    // block below persists a pattern, raises Preferences and latches a notice, and every
    // one of those against a 0-byte sample is a judgement about a log nobody has seen:
    // the dialog previewed "No sample lines to preview." with Detect greyed out, and
    // declining it (or being a background tab, where nothing is asked) latched "format
    // not recognised" over a log whose every column then parsed perfectly. Doing none of
    // it leaves the prompt still owed and the flag still false, so the first growth tick
    // comes back through here with real bytes (LiveController::settleFirstBytes).
    if (settleFormat && doc->formatSettled()) {
        // CONSUMED UNCONDITIONALLY, before anything is decided with it. A document can
        // wait and resume any number of times — a host that comes and goes — and the
        // dialog is owed once, for the open the user actually performed.
        const bool owedADialog = ctx->pendingFormatPrompt;
        ctx->pendingFormatPrompt = false;

        // Whether the remembered pattern fits, decided by the same code an ordinary open
        // uses, so the two cannot disagree (offerFormat).
        //
        // The dialog is only ever raised for a log the user just opened AND is looking
        // at. §6.5's rule that nothing pops up on arrival is about the other case — a
        // watch tick bringing back a log in a tab that may not even be on screen — and
        // that case still raises nothing. For an interactive open the resume lands about
        // a second later, on the tab in front of them, which is the dialog they would
        // have got before the connect moved off this thread.
        const bool mayAsk = owedADialog && ctx == activeContext();
        FormatSettings settled = ctx->settings;
        // Statements rather than one expression, so formatFits() stays on the branch
        // that needs it: it reads and decodes 64 KB, offerFormat() asks it first thing
        // anyway, and this runs on every resume of a log that comes and goes. With
        // nobody to ask, the remembered format stands or falls on its own — Declined is
        // what aborts the open, the same answer a person gives by cancelling.
        FormatOutcome outcome = FormatOutcome::Declined;
        if (mayAsk)
            outcome = offerFormat(doc, doc->path(), &settled);
        else if (formatFits(doc, ctx->settings))
            outcome = FormatOutcome::Matched;

        switch (outcome) {
        case FormatOutcome::Matched:
            // It fits, and it has now been checked against real lines rather than
            // assumed — which is exactly the point at which it becomes worth
            // remembering. The waiting open deliberately persisted nothing.
            persistFileSettings(ctx);
            ctx->formatNotice.clear();
            break;
        case FormatOutcome::Chosen:
            // Reopened through the ordinary format-change path rather than by preparing
            // this document again: it is live, it has a controller and bound views, and
            // prepare() would reset its index and its waiting state underneath them.
            // Safe to use the active-context form, because the dialog was only offered
            // when this context IS the active one.
            ctx->formatNotice.clear();
            applySettings(settled);
            return; // ctx and doc are gone; applySettings reopened the file
        case FormatOutcome::Declined:
            // Either the user closed the dialog, or there was nobody to show one to. The
            // log stays readable as plain text and the status bar says where to fix it;
            // nothing is persisted, so reopening still offers the dialog properly.
            ctx->formatNotice = tr("format not recognised — File ▸ Preferences…");
            break;
        }
    }

    // The panes describe the ACTIVE document only; a log arriving in a background tab
    // must not repopulate the subsystem lists of the one being read.
    if (ctx == activeContext()) {
        if (m_filterPane)
            m_filterPane->refreshDiscoveredLists();
        if (m_highlighterPane)
            m_highlighterPane->refreshDiscoveredLists();
        if (m_runPane)
            m_runPane->refresh();
    }
    updateTabTitles(ctx);
    updateStatus();
}

void MainWindow::applyFiltersFor(DocumentContext *ctx, KeepPosition keep)
{
    if (!ctx || !ctx->model)
        return;
    // A filtered set is a wholesale row remap, so reset the model around the
    // recompute: the view/header/selection refresh over the new visible set and
    // LogView rebuilds its line geometry (invariant #6). The predicate chain inside
    // applyFilters runs integer axes first, message text last (invariant #4).
    //
    // That remap is also what would throw the reader's place away, so each view of the
    // file brackets the reset with its own anchor in source-record terms (SPEC.md §6).
    // Each keeps ITS OWN position — scroll and selection are per-view state (invariant
    // #7). The digest strip is deliberately untouched: applyFilters() does not move the
    // digest index.
    if (keep == KeepPosition::Yes) {
        for (DocumentView *v : std::as_const(ctx->views))
            v->logView()->beginFilterUpdate();
    }
    ctx->model->beginFilterReset();
    ctx->doc->applyFilters();
    ctx->model->endFilterReset();
    if (keep == KeepPosition::Yes) {
        for (DocumentView *v : std::as_const(ctx->views))
            v->logView()->endFilterUpdate();
    }
    for (DocumentView *v : std::as_const(ctx->views)) {
        v->logView()->updateGeometry();
        v->logView()->viewport()->update();
    }
    if (ctx == activeContext())
        updateStatus();
}

void MainWindow::applyActiveFilters()
{
    DocumentContext *ctx = activeContext();
    applyFiltersFor(ctx);
    // The pane's own DEBOUNCED notification, and the only signal that a filter edit has
    // landed. Persisting here is what makes a filter durable without waiting for a tab
    // switch or a clean quit; the debounce is what keeps it off every keystroke, and the
    // funnel's change gate absorbs the rest.
    //
    // Here and NOT in applyFiltersFor(), which is reached from onIndexFinished(),
    // onRunSelected(), followLastRunIfMoved() and the record menu — none of which is a
    // filter edit, and the third of which runs on every ingest tick of a restarting log.
    persistFileSettings(ctx);
}

void MainWindow::applyActiveHighlighters()
{
    DocumentContext *ctx = activeContext();
    if (!ctx)
        return;
    // The COLOUR action recolors visible rows in place (SPEC.md §7): no rows are added
    // or removed, so a viewport repaint is enough — no model reset, unlike filtering.
    // The DIGEST action is the opposite: its subset is a wholesale ordinal remap, so it
    // follows applyFiltersFor()'s shape and is bracketed by a model reset.
    if (ctx->digestModel)
        ctx->digestModel->beginFilterReset();
    ctx->doc->refreshHighlighting();
    if (ctx->digestModel)
        ctx->digestModel->endFilterReset();
    for (DocumentView *v : std::as_const(ctx->views))
        v->logView()->viewport()->update();
    // Whether any rule still asks to be notified may have changed with that edit.
    updateTrayPresence();
    // The pane's own notification that a rule edit has landed, and the only one there is.
    // NOT in refreshHighlighting() or on the ingest path: HighlighterPane::refreshTime-
    // Bounds() runs there and already writes nothing unless a rule actually moved, and
    // that guard is now load-bearing for a file write as well as for the tab's marker.
    persistFileSettings(ctx);
}

// --- Highlight actions beyond colour (M19, SPEC.md §7, ARCHITECTURE.md §7.5) ------

void MainWindow::rebuildDigestFor(DocumentContext *ctx)
{
    if (!ctx || !ctx->digestModel)
        return;
    ctx->digestModel->beginFilterReset();
    ctx->doc->rebuildDigest();
    ctx->digestModel->endFilterReset();
}

bool MainWindow::isBeingRead(const DocumentContext *ctx) const
{
    // "Being looked at" is both halves: the right tab AND the window in front. A tab in
    // the foreground of a window behind three others is not being read.
    return ctx && ctx == activeContext() && isActiveWindow();
}

void MainWindow::handleAlerts(DocumentContext *ctx)
{
    if (!ctx || !ctx->live)
        return;
    const LiveController::BatchAlerts &batch = ctx->live->lastBatchAlerts();
    if (batch.tabMatches == 0 && batch.notifyMatches == 0)
        return;
    // A match in the log the user is already reading is not news; the rows are on
    // screen, in their rule's colours, and interrupting would be noise.
    if (isBeingRead(ctx))
        return;

    if (batch.tabMatches > 0 && !ctx->unseenMatch) {
        ctx->unseenMatch = true;
        updateTabTitles(ctx);
    }

    if (batch.notifyMatches > 0) {
        // Where the desktop offers no notification service, a Notify rule behaves as if
        // it carried Tab rather than being silently ignored — the pane has already said
        // so, and dropping the event entirely would be the one outcome the user cannot
        // tell from a broken rule.
        if (!m_tray) {
            if (!ctx->unseenMatch) {
                ctx->unseenMatch = true;
                updateTabTitles(ctx);
            }
            return;
        }
        const AlertPolicy::Decision d =
            ctx->alerts.recordBatch(m_alertClock.elapsed(), batch.notifyMatches);
        if (!d.notify)
            return;
        m_lastNotified = ctx;
        const QString title = logSourceDisplayName(ctx->doc->path());
        const QString body = d.count == 1
                                 ? tr("A highlight rule matched a new record.")
                                 : tr("%n matching records.", "", d.count);
        m_tray->showMessage(title, body, QSystemTrayIcon::Information);
    }
}

void MainWindow::clearUnseenMatch(DocumentContext *ctx)
{
    // Edge-triggered. Re-setting a tab's title relays out the whole bar, and this runs
    // from every activation event and every tab change — the same discipline the panes'
    // activityChanged uses, and for the same reason.
    if (!ctx || !ctx->unseenMatch || !isBeingRead(ctx))
        return;
    ctx->unseenMatch = false;
    updateTabTitles(ctx);
}

bool MainWindow::notificationsAvailable()
{
    // isSystemTrayAvailable() is false on a stock GNOME/Wayland session — no
    // StatusNotifierWatcher and no XEmbed tray — which is the reference desktop, so
    // this returning false is the ORDINARY answer there rather than an edge case.
    return QSystemTrayIcon::isSystemTrayAvailable() && QSystemTrayIcon::supportsMessages();
}

bool MainWindow::anyRuleWantsNotifications() const
{
    for (const auto &ctx : m_contexts)
        if (ctx->doc && ctx->doc->highlighters().anyEnabled(HighlightAction::Notify))
            return true;
    return false;
}

void MainWindow::updateTrayPresence()
{
    const bool wanted = notificationsAvailable() && anyRuleWantsNotifications();

    if (!wanted) {
        // Destroyed rather than merely hidden: an icon sitting in the user's tray for a
        // feature nothing is currently asking for is a claim on their desktop that
        // loftail has not earned.
        delete m_tray;
        m_tray = nullptr;
        m_lastNotified = nullptr;
        if (m_alertPump)
            m_alertPump->stop();
        return;
    }
    if (m_tray)
        return;

    m_tray = new QSystemTrayIcon(this);
    QIcon icon = windowIcon();
    if (icon.isNull())
        icon = style()->standardIcon(QStyle::SP_MessageBoxInformation);
    m_tray->setIcon(icon);
    m_tray->setToolTip(tr("loftail — a highlight rule is watching for matches"));
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this] {
        // Raise the log the message was about. Qt gives a message no identity, so only
        // the most recent can be honoured — and the rate limiter makes that a
        // distinction without a difference.
        if (!m_lastNotified)
            return;
        for (DocumentView *v : std::as_const(m_lastNotified->views)) {
            if (const int index = m_tabs->indexOf(v); index >= 0) {
                m_tabs->setCurrentIndex(index);
                break;
            }
        }
        raise();
        activateWindow();
    });
    // showMessage() is a silent no-op on a hidden icon, so this show() is not
    // decoration — it is what makes the whole action work.
    m_tray->show();

    if (!m_alertClock.isValid())
        m_alertClock.start();
    if (!m_alertPump) {
        m_alertPump = new QTimer(this);
        m_alertPump->setInterval(5000);
        connect(m_alertPump, &QTimer::timeout, this, [this] {
            if (!m_tray)
                return;
            for (auto &ctx : m_contexts) {
                if (ctx->alerts.pending() <= 0 || isBeingRead(ctx.get()))
                    continue;
                const AlertPolicy::Decision d = ctx->alerts.poll(m_alertClock.elapsed());
                if (!d.notify)
                    continue;
                m_lastNotified = ctx.get();
                m_tray->showMessage(logSourceDisplayName(ctx->doc->path()),
                                    tr("%n matching records.", "", d.count),
                                    QSystemTrayIcon::Information);
            }
        });
    }
    m_alertPump->start();
}

void MainWindow::onRunStartChanged(const QString &pattern, bool regex, bool caseSensitive)
{
    DocumentContext *ctx = activeContext();
    if (!ctx)
        return;
    Document *doc = ctx->doc.get();

    // The run-start pattern is part of the per-file format (persisted like it, §3a).
    ctx->settings.runStartPattern = pattern;
    ctx->settings.runStartIsRegex = regex;
    ctx->settings.runStartCaseSensitive = caseSensitive;
    persistFileSettings(ctx);

    // Reconfigure + re-detect over the existing index (no rescan — offsets are
    // unchanged, invariant #3), defaulting to the newest run, then re-apply the view.
    doc->setRunStart(pattern, regex, caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
    if (m_runPane)
        m_runPane->refresh();
    applyActiveFilters();
    for (DocumentView *v : std::as_const(ctx->views)) {
        if (v->logView()->following())
            v->logView()->followTail();
    }
}

void MainWindow::onRunSelected(int runIndex)
{
    DocumentContext *ctx = activeContext();
    if (!ctx)
        return;
    Document *doc = ctx->doc.get();

    // "Follow the last" is not an ordinal, so it cannot travel as one: it is the standing
    // instruction the document keeps and re-points itself by as runs appear (§3a).
    if (runIndex == RunPane::kLastRun)
        doc->selectLastRun();
    else
        doc->selectRun(runIndex);
    // The user PINNING a run, or letting go of one, is a choice about this log and is
    // remembered as one. Here and not in applyFiltersFor() below, which
    // followLastRunIfMoved() reaches on every ingest tick of a restarting log.
    persistFileSettings(ctx);
    // No anchor: every view is positioned explicitly at the end of this function, so
    // anchoring first would measure a wrapped selection and scroll to a place the very
    // next statement overwrites — one wasted pass and one visible jump.
    applyFiltersFor(ctx, KeepPosition::No);
    if (m_runPane)
        m_runPane->refresh();

    // In "seconds from run start" the two panes' time bounds are counted from the
    // SELECTED run's baseline (AxisEditor::secondsBaseMs), because a bound has to name
    // one instant and that is the run whose records are on screen. So moving the
    // selection moves what those digits mean, exactly as a display-mode change does —
    // and this re-renders them to keep naming the instant they named before. A no-op in
    // every other display mode, where the baseline is not consulted at all.
    if (m_filterPane)
        m_filterPane->refreshTimeBounds();
    if (m_highlighterPane)
        m_highlighterPane->refreshTimeBounds();

    // EVERY choice in this pane opens at the END of what it selects — a run, "All runs"
    // or "Follow the last" alike (§3a): a run is picked because of how it went, and what went
    // wrong is the last thing in it, since a crash writes its stack and stops. Opening
    // at the first record put the one part nobody is looking for on screen and left the
    // reader scrolling the whole run to reach the part they are.
    //
    // The last record is SELECTED and not merely scrolled to, so the reader lands on the
    // record they came for and walks back up from it with the keyboard — and it is the
    // same gesture for all three choices, because "what happened at the end" is the same
    // question whichever of them was picked.
    //
    // What still differs is only what the end is worth afterwards. The newest run (or
    // "all runs") is still being written, so it FOLLOWS: the end moves and the view goes
    // with it. A finished run has a fixed end and nothing is ever appended to it, so
    // follow is left to the view's own rule (at the bottom = following) rather than
    // forced — there is nothing there for it to follow.
    const int newest = doc->runs().isEmpty() ? -1 : int(doc->runs().size()) - 1;
    const bool isLive = runIndex < 0 || runIndex == newest;
    for (DocumentView *v : std::as_const(ctx->views)) {
        LogView *log = v->logView();
        log->setCurrentRecord(log->recordCount() - 1);
        if (isLive)
            log->followTail(); // ...and stay on the end as it moves
    }
}

void MainWindow::followLastRunIfMoved(DocumentContext *ctx)
{
    if (!ctx || !ctx->doc->retargetLastRun())
        return;

    applyFiltersFor(ctx, KeepPosition::No);
    // Rebuilt, unlike on an explicit run switch: the digest is bounded by the selected
    // run, and here the run changed under a strip nobody asked to change — its rows
    // would go on being read as this run's matches while describing the previous one.
    rebuildDigestFor(ctx);

    // The panes describe the ACTIVE document only (as everywhere else on the ingest
    // path): a background log starting a new run must not repaint the pane the user is
    // reading. Its own pane state is rebuilt when its tab is next selected.
    if (ctx == activeContext()) {
        if (m_runPane)
            m_runPane->refresh();
        // "Seconds from run start" is counted from the SELECTED run, and the selected
        // run just moved — exactly the case onRunSelected() re-renders these for.
        if (m_filterPane)
            m_filterPane->refreshTimeBounds();
        if (m_highlighterPane)
            m_highlighterPane->refreshTimeBounds();
    }

    // The follow state is left alone rather than re-attached, exactly as after a
    // rotation (rescanned) — which is the same event from the view's side: every row
    // it held was replaced. A reader who had scrolled back stays detached.
    for (DocumentView *v : std::as_const(ctx->views)) {
        if (v->logView()->following())
            v->logView()->followTail();
    }
}

void MainWindow::updateModelTheme()
{
    const bool dark = palette().base().color().lightness() < palette().text().color().lightness();
    for (auto &ctx : m_contexts) {
        if (!ctx->model || dark == ctx->model->darkTheme())
            continue;
        ctx->model->setDarkTheme(dark);
        if (ctx->digestModel)
            ctx->digestModel->setDarkTheme(dark); // the strip paints rule colours too
        for (DocumentView *v : std::as_const(ctx->views)) {
            v->logView()->viewport()->update();
            v->digestView()->viewport()->update();
        }
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        updateModelTheme();
    // Raising the window is the other half of "being looked at" (M19): a marked tab
    // that was already current must lose its mark when loftail comes to the front, not
    // only when the user clicks another tab and back.
    if (event->type() == QEvent::ActivationChange && isActiveWindow())
        clearUnseenMatch(activeContext());
}

void MainWindow::runFind(bool forward, bool fromStart)
{
    // Find runs over the view whose bar asked for it. Focusing that bar already made
    // its view active, so the active view IS the requesting one.
    // The page in front decides what is searched. FIRST, above everything, because the
    // log view below is still bound while an editor page is current — searching it would
    // move the cursor in a tab the reader is not looking at and report into its hidden
    // bar.
    if (ConfigView *editor = activeConfigView()) {
        editor->runFind(forward, fromStart);
        return;
    }

    LogView *logView = activeLogView();
    LogModel *model = activeModel();
    FindBar *findBar = m_activeView ? m_activeView->findBar() : nullptr;
    if (!logView || !model || !findBar)
        return;

    // The bar goes on screen BEFORE anything is written into it. Every branch below —
    // `no search text`, `bad regex`, `no records`, `no match` and the match report
    // itself — answers into the bar's own label, and F3 can be pressed with the bar
    // closed (the shortcut is a window action, and Escape closes the bar without
    // touching the query): without this, a search runs, the cursor moves, the table
    // re-arms its marks, and the sentence explaining all of it is written to a widget
    // nobody can see (SPEC.md §5).
    //
    // reveal() and NOT activate(): activate() opens by clearing the status, so calling
    // it here would wipe the very report this exists to show, and it selects the query
    // for replacement, which on an already-open bar would arm the reader's next
    // keystroke to delete the text they are stepping through. reveal() is a no-op on an
    // open bar, so the focus a `no match` or a successful find leaves inside the bar
    // stays exactly where it was.
    findBar->reveal();

    const QString pattern = findBar->pattern();
    if (pattern.isEmpty()) {
        // Which of the two empty-query cases this is, is exactly `fromStart`. It is true
        // only when the query itself changed (typing, or a Regex/Case toggle), where the
        // reader has just deleted their own text and being told about it is a nag; it is
        // false for every deliberate navigation — F3, Shift+F3, Enter in the box, the two
        // arrow buttons — where the gesture asked a question and used to get silence.
        findBar->setStatus(fromStart ? QString() : tr("no search text"));
        logView->clearFindMatcher(); // an empty query marks everything, which marks nothing
        return;
    }

    // Find over the message column of the CURRENTLY-VISIBLE rows (the filtered
    // subset when a filter is active), reusing the filter's text matcher (SPEC.md
    // §5). Changes no filter state — it only moves the selection.
    TextMatcher matcher;
    matcher.set(pattern, findBar->regex(),
                findBar->caseSensitive() ? Qt::CaseSensitive : Qt::CaseInsensitive);
    if (!matcher.isValid()) {
        findBar->setStatus(tr("bad regex"));
        logView->clearFindMatcher();
        return;
    }

    const int count = model->rowCount();
    if (count == 0) {
        findBar->setStatus(tr("no records"));
        logView->clearFindMatcher();
        return;
    }

    // Search every visible column's text so Find matches anything on screen, but
    // fall back to a message-only scan when the format defines no columns.
    const int cols = model->columnCount();
    auto rowMatches = [model, &matcher, cols](int row) {
        if (cols == 0)
            return matcher.matches(model->cellText(row, 0));
        for (int c = 0; c < cols; ++c)
            if (matcher.matches(model->cellText(row, c)))
                return true;
        return false;
    };

    const int from = fromStart ? -1 : logView->currentRecord();
    const int hit = Find::search(count, from, forward, /*wrap=*/true, rowMatches);
    if (hit < 0) {
        findBar->setStatus(tr("no match"));
        logView->clearFindMatcher(); // nothing matched, so there is nothing to point at
        return;
    }
    logView->setCurrentRecord(hit);
    // Mark what matched, in the table this search ran over (SPEC.md §5). The MATCHER is
    // handed over, not a list of positions: the view re-runs it over the cells it paints
    // anyway, so the marks cost nothing off screen and survive every repaint
    // (ARCHITECTURE.md §7.1.4). The same object the search just used, so a mark can
    // never disagree with a hit about the regex or the case option.
    logView->setFindMatcher(matcher);

    // The search wraps (SPEC.md §5), and a wrap that says nothing is a teleport: F3 at
    // the last match jumps to the top and the reader has no way to tell it apart from
    // an ordinary step. Forward, the walk starts one row PAST the cursor and backward
    // one row before it, so landing at or behind it is exactly a wrap — and a search
    // that started from the end (from < 0) never wrapped.
    const bool wrapped = from >= 0 && (forward ? hit <= from : hit >= from);

    // Where this match sits among the others. Bounded (ARCHITECTURE.md §7.1.3): the
    // count decodes every visible record's text, so on a log too big to count in the
    // moment the total is a floor ("47+") and, when the match itself lies past where
    // counting stopped, there is no position to give and the bar just says it found one.
    const Find::Tally t = Find::tally(count, hit, kFindTallyRows, kFindTallyMs, rowMatches);
    // The wording lives on FindBar, shared with the config editor's own bar, so the two
    // cannot come to describe one gesture in two vocabularies.
    const QString status = FindBar::describeMatch(t.index, t.total, t.complete, wrapped, forward);
    findBar->setStatus(status); // the bar's own label: focus stays in it for the next F3
}

void MainWindow::updateStatus()
{
    if (ConfigView *editor = activeConfigView()) {
        // The page in front is a config file, so the status line describes THAT rather
        // than the log still bound behind it. Above the document branch, because
        // activeDocument() is deliberately still non-null here.
        QString text = editor->displayName();
        if (!editor->fileExisted())
            text = tr("%1 — new file").arg(text);
        if (editor->isModified())
            text = tr("%1 — unsaved changes").arg(text);
        m_statusLabel->setText(text);
        return;
    }

    Document *doc = activeDocument();
    if (!doc) {
        m_statusLabel->setText(tr("No file open"));
        return;
    }
    if (doc->isWaiting()) {
        // A record count for a log that is not there would be an honest zero and a
        // useless one. Say what is actually going on instead (SPEC.md §3).
        QString text = doc->waitReason();
        if (const DocumentContext *ctx = activeContext();
            ctx && !ctx->sourceStatus.isEmpty() && ctx->sourceStatus != text) {
            // Not when they are the same sentence, which for a spooled document is the
            // ordinary case rather than a coincidence: both come from sourceStatusText()
            // over the same fetcher. Every remote open would otherwise read
            // "connecting…  |  connecting…".
            text += QStringLiteral("  |  ") + ctx->sourceStatus;
        }
        m_statusLabel->setText(text);
        return;
    }

    const int total = int(doc->index().records.size());
    // Filtered/total counts (SPEC.md §5, §6): show the shown-vs-total pair only
    // when a filter narrows the view, otherwise a plain record count.
    QString text;
    if (doc->filters().anyActive()) {
        text = tr("%1  |  %2 of %3 records shown")
                   .arg(logSourceDisplayName(doc->path()))
                   .arg(doc->filtered().recordCount())
                   .arg(total);
        // With filter context on, "shown" counts the neighbours too. Say how many, or
        // the pair reads as a filter that matched far more than it did (SPEC.md §6).
        if (const int ctx = doc->filtered().contextCount(); ctx > 0)
            text += tr(" (%1 as context)").arg(ctx);
    } else {
        text = tr("%1  |  %2 records")
                   .arg(logSourceDisplayName(doc->path()))
                   .arg(total);
    }

    // What the source is doing, when it is doing anything worth mentioning: expanding
    // an archive, priming a remote log, or having failed at either. Appended rather
    // than replacing, so the record count stays visible throughout.
    if (const DocumentContext *ctx = activeContext()) {
        if (!ctx->sourceStatus.isEmpty())
            text += QStringLiteral("  |  ") + ctx->sourceStatus;
        // A log that arrived while being waited for, whose remembered pattern did not
        // fit it. Separate from sourceStatus so a fetcher's progress cannot erase it.
        if (!ctx->formatNotice.isEmpty())
            text += QStringLiteral("  |  ") + ctx->formatNotice;
    }

    m_statusLabel->setText(text);
}

void MainWindow::buildTimeDisplayMenu()
{
    // Parented to the window, NOT to a context menu: showColumnMenu builds its QMenu
    // on the stack per invocation, so this one is borrowed and must outlive it. The
    // actions being window children is also what lets tests findChild them without
    // opening a modal menu (precedent: newViewAction).
    m_timeDisplayMenu = new QMenu(tr("&Timestamp Format"), this);
    m_timeDisplayMenu->setObjectName(QStringLiteral("timeDisplayMenu"));
    auto *group = new QActionGroup(this);

    struct Item
    {
        TimeDisplay mode;
        const char *text;
        const char *name;
    };
    // QT_TR_NOOP marks the text for extraction where it is WRITTEN, since the tr() that
    // actually translates it is in the loop below and sees only `item.text`. This is the
    // one menu in the window built from a table rather than from a run of addAction
    // calls, and therefore the one a sweep for translatable strings walks straight past.
    // The object names beside them are the test contract and stay untranslated.
    static constexpr Item kItems[] = {
        { TimeDisplay::AsWritten,    QT_TR_NOOP("As &Written in the File"),
          "timeDisplayAsWrittenAction" },
        { TimeDisplay::LocalTime,    QT_TR_NOOP("&Local Time"), "timeDisplayLocalAction" },
        { TimeDisplay::Utc,          QT_TR_NOOP("&UTC"),        "timeDisplayUtcAction" },
        { TimeDisplay::EpochSeconds, QT_TR_NOOP("&Seconds"),    "timeDisplaySecondsAction" },
        { TimeDisplay::RunSeconds,   QT_TR_NOOP("Seconds from &Run Start"),
          "timeDisplayRunSecondsAction" },
        { TimeDisplay::SincePrevious, QT_TR_NOOP("Seconds Since &Previous Record"),
          "timeDisplaySincePreviousAction" },
    };
    for (const Item &item : kItems) {
        QAction *a = m_timeDisplayMenu->addAction(tr(item.text));
        a->setObjectName(QLatin1String(item.name));
        a->setCheckable(true);
        group->addAction(a);
        m_timeDisplayActions[int(item.mode)] = a;
        const TimeDisplay mode = item.mode;
        connect(a, &QAction::triggered, this, [this, mode]() { setTimeDisplay(mode); });
    }
    m_timeDisplayActions[int(TimeDisplay::AsWritten)]->setChecked(true);
}

void MainWindow::setTimeDisplay(TimeDisplay mode)
{
    DocumentContext *ctx = activeContext();
    if (!ctx || ctx->settings.timeDisplay == mode)
        return;
    // Through applySettings, exactly like Preferences: that is what puts
    // the choice in the settings tree, so it survives a restart.
    FormatSettings s = ctx->settings;
    s.timeDisplay = mode;
    applySettings(s);
}

void MainWindow::updateTimeDisplayActions()
{
    if (!m_timeDisplayMenu)
        return;
    DocumentContext *ctx = activeContext();
    // With no file open the menu is unreachable anyway; leave it on the default so a
    // fresh open starts from a sane checkmark.
    const TimeDisplay mode = ctx ? ctx->settings.timeDisplay : TimeDisplay::AsWritten;
    if (QAction *a = m_timeDisplayActions[int(mode)])
        a->setChecked(true);
}

// The two width commands on the column header menu (SPEC.md §5). Window-owned actions
// with object names, exactly like the timestamp submenu above and for the same two
// reasons: showColumnMenu builds its menu on the stack per invocation, and a test can
// then reach them without opening a modal menu.
//
// They act on the ACTIVE view rather than on a header remembered from the click, on the
// same argument the record menu makes: a right-click can only land in the visible tab.
void MainWindow::buildColumnWidthActions()
{
    m_fitColumnsAction = new QAction(tr("&Fit to Contents"), this);
    m_fitColumnsAction->setObjectName(QStringLiteral("fitColumnsAction"));
    connect(m_fitColumnsAction, &QAction::triggered, this, [this]() {
        if (m_activeView)
            m_activeView->logView()->fitColumnsToContents();
    });

    m_resetColumnWidthsAction = new QAction(tr("&Reset Widths"), this);
    m_resetColumnWidthsAction->setObjectName(QStringLiteral("resetColumnWidthsAction"));
    connect(m_resetColumnWidthsAction, &QAction::triggered, this, [this]() {
        if (m_activeView)
            m_activeView->logView()->resetColumnWidths();
    });
}

void MainWindow::showColumnMenu(const QPoint &pos)
{
    // The menu belongs to the header that asked for it, which may be any view's.
    auto *header = qobject_cast<QHeaderView *>(sender());
    LogModel *model = activeModel();
    if (!header || !model)
        return;
    QMenu menu(this);

    // Right-clicking the TIMESTAMP column also offers its display mode (SPEC.md §4).
    // A submenu rather than five more inline entries: the column toggles below are
    // independent checkboxes and these are an exclusive radio group, and flattened
    // together they would read as ten equivalent checkmarks. A click past the last
    // section gives -1 and simply yields the plain column menu.
    const int logical = header->logicalIndexAt(pos);
    const Document *doc = activeDocument();
    if (doc && logical >= 0 && logical < doc->format().fields.size()
        && doc->format().fields.at(logical).role == FieldRole::Date) {
        updateTimeDisplayActions();
        menu.addMenu(m_timeDisplayMenu); // borrowed; addMenu does not take ownership
        menu.addSeparator();
    }

    // Widths above the visibility list, and separated from it: one pair acts on every
    // column at once, the list acts on one column each, and run together they would read
    // as two more entries of the same kind.
    menu.addAction(m_fitColumnsAction);
    menu.addAction(m_resetColumnWidthsAction);
    menu.addSeparator();

    menu.addSection(tr("Columns"));
    for (int c = 0; c < model->columnCount(); ++c) {
        const QString name = model->headerData(c, Qt::Horizontal).toString();
        QAction *a = menu.addAction(name);
        a->setCheckable(true);
        a->setChecked(!header->isSectionHidden(c));
        connect(a, &QAction::toggled, this, [header, c](bool visible) {
            header->setSectionHidden(c, !visible);
        });
    }
    menu.exec(header->mapToGlobal(pos));
}

// --- The record context menu (SPEC.md §5) ----------------------------------
//
// Right-clicking a record turns that record's own field values into filter and
// highlight criteria. Every item is a `MatchCriteria` the panes already build and
// persist (M10) — the menu is an input method for them, not a second filtering
// system, which is what keeps presets, session restore and portability across a
// re-index applying to it with no work of its own.
//
// Two rules hold the design together. Items are OMITTED, never greyed, when the
// record or the format cannot speak for that axis. And every edit goes through the
// panes, so what the menu did is visible in the ticks — there is no undo stack, and
// the pane IS the undo.

void MainWindow::buildRecordMenu(QMenu *menu, DocumentView *view, int viewRow, int column)
{
    if (!menu || !view)
        return;
    // A right-click can only land in the visible tab, so the clicked view is the
    // active one and the panes below are bound to its file. Refuse rather than edit
    // some other file's filters if that ever stops being true (invariant #7).
    if (view != m_activeView)
        return;
    DocumentContext *ctx = view->context();
    if (!ctx || !ctx->doc)
        return;

    const Document *doc = ctx->doc.get();
    const int src = doc->filtered().sourceRow(viewRow);
    if (src < 0 || src >= doc->index().records.size())
        return;

    const Record    &rec = doc->index().records.at(src);
    const LogFormat &fmt = doc->format();
    const QString subsystem = doc->index().loggers.name(rec.loggerId);
    const QString thread = doc->index().threads.name(rec.threadId);
    const Priority prio = rec.priorityEnum();

    // An axis is offered only when THIS record can speak for it. An unparsed
    // plain-text line has no subsystem, thread, level or timestamp (SPEC.md §4), and
    // a pattern without %t or %d has none for any record (§6).
    const bool hasSubsystem = fmt.loggerGroup > 0 && !subsystem.isEmpty();
    const bool hasThread = fmt.threadGroup > 0 && !thread.isEmpty();
    const bool hasPriority = prio != Priority::Unknown;
    const bool hasTime = fmt.dateGroup > 0 && rec.timestamp != Record::kNoTimestamp;

    // A multi-record selection contributes exactly one item: the two bounds it
    // already names. Everything else reads the record that was clicked, because the
    // union of five records' subsystems is not a gesture anyone means.
    qint64 selLo = 0;
    qint64 selHi = 0;
    int    selTimed = 0;
    for (const QModelIndex &i : view->logView()->selectionModel()->selectedRows(0)) {
        const int s = doc->filtered().sourceRow(i.row());
        if (s < 0 || s >= doc->index().records.size())
            continue;
        const qint64 ts = doc->index().records.at(s).timestamp;
        if (ts == Record::kNoTimestamp)
            continue;
        selLo = selTimed ? qMin(selLo, ts) : ts;
        selHi = selTimed ? qMax(selHi, ts) : ts;
        ++selTimed;
    }
    const bool hasSelectedRange = fmt.dateGroup > 0 && selTimed > 1 && selLo < selHi;

    // Which axis the clicked column names leads the menu. The CONTENTS do not depend
    // on the column: a menu whose items move is learnable, one whose items appear and
    // disappear with the column is not.
    enum class Axis { Subsystem, Thread, Priority, Time };
    QVector<Axis> order{Axis::Subsystem, Axis::Thread, Axis::Priority, Axis::Time};
    if (column >= 0 && column < fmt.fields.size()) {
        std::optional<Axis> clicked;
        switch (fmt.fields.at(column).role) {
        case FieldRole::Logger:   clicked = Axis::Subsystem; break;
        case FieldRole::Thread:   clicked = Axis::Thread; break;
        case FieldRole::Priority: clicked = Axis::Priority; break;
        case FieldRole::Date:     clicked = Axis::Time; break;
        default: break;
        }
        if (clicked) {
            order.removeAll(*clicked);
            order.prepend(*clicked);
        }
    }

    // Sections are added on first use so a record with nothing to offer produces an
    // empty menu the caller can decline to pop up, rather than two bare headings.
    bool filterSection = false;
    bool highlightSection = false;
    auto add = [this, menu](const QString &text, const char *name, auto &&fn) {
        QAction *act = menu->addAction(text);
        act->setObjectName(QLatin1String(name));
        connect(act, &QAction::triggered, this, fn);
    };

    for (Axis axis : order) {
        auto section = [&] {
            if (!filterSection) {
                menu->addSection(tr("Filter"));
                filterSection = true;
            }
        };
        switch (axis) {
        case Axis::Subsystem:
            if (!hasSubsystem)
                break;
            section();
            add(tr("Show Only Subsystem \"%1\"").arg(subsystem),
                "recordShowOnlySubsystem", [this, subsystem] {
                    m_filterPane->showOnlyValue(ValueAxis::Subsystem, subsystem);
                });
            add(tr("Hide Subsystem \"%1\"").arg(subsystem),
                "recordHideSubsystem", [this, subsystem] {
                    m_filterPane->hideValue(ValueAxis::Subsystem, subsystem);
                });
            break;
        case Axis::Thread:
            if (!hasThread)
                break;
            section();
            add(tr("Show Only Thread \"%1\"").arg(thread),
                "recordShowOnlyThread", [this, thread] {
                    m_filterPane->showOnlyValue(ValueAxis::Thread, thread);
                });
            add(tr("Hide Thread \"%1\"").arg(thread),
                "recordHideThread", [this, thread] {
                    m_filterPane->hideValue(ValueAxis::Thread, thread);
                });
            break;
        case Axis::Priority:
            if (!hasPriority)
                break;
            section();
            // The record's own level as the MINIMUM, which is the only shape the
            // priority axis has (SPEC.md §6): "at least this bad".
            add(tr("Show %1 and Above").arg(QString(priorityName(prio))),
                "recordPriorityFloor",
                [this, prio] { m_filterPane->setMinimumPriority(prio); });
            break;
        case Axis::Time:
            if (!hasTime && !hasSelectedRange)
                break;
            section();
            if (hasTime) {
                const qint64 ts = rec.timestamp;
                add(tr("Start Time Range Here"), "recordTimeStart",
                    [this, ts] { m_filterPane->setTimeBound(TimeBound::Start, ts); });
                add(tr("End Time Range Here"), "recordTimeEnd",
                    [this, ts] { m_filterPane->setTimeBound(TimeBound::End, ts); });
            }
            if (hasSelectedRange) {
                add(tr("Filter to Selected Time Range"), "recordTimeRange",
                    [this, selLo, selHi] { m_filterPane->setTimeRange(selLo, selHi); });
            }
            break;
        }
    }

    // Highlighting takes the same values and does the opposite with them: a one-axis
    // rule, appended so existing rules keep their precedence (SPEC.md §7).
    auto highlight = [&](const QString &text, const char *name, const MatchCriteria &c) {
        if (!highlightSection) {
            menu->addSection(tr("Highlight"));
            highlightSection = true;
        }
        add(text, name, [this, c] { m_highlighterPane->addRule(c); });
    };
    for (Axis axis : order) {
        switch (axis) {
        case Axis::Subsystem: {
            if (!hasSubsystem)
                break;
            MatchCriteria c;
            c.loggerEnabled = true;
            c.loggerNames = QStringList{subsystem};
            c.loggerCoversAll = false;
            c.loggerRestrictive = true; // these names exactly, whatever turns up later
            highlight(tr("Highlight This Subsystem"), "recordHighlightSubsystem", c);
            break;
        }
        case Axis::Thread: {
            if (!hasThread)
                break;
            MatchCriteria c;
            c.threadEnabled = true;
            c.threadNames = QStringList{thread};
            c.threadCoversAll = false;
            c.threadRestrictive = true;
            highlight(tr("Highlight This Thread"), "recordHighlightThread", c);
            break;
        }
        case Axis::Priority: {
            if (!hasPriority)
                break;
            MatchCriteria c;
            c.priorityEnabled = true;
            c.minPriority = prio;
            highlight(tr("Highlight %1 and Above").arg(QString(priorityName(prio))),
                      "recordHighlightPriority", c);
            break;
        }
        case Axis::Time:
            break; // one record names one instant, and a rule needs two bounds
        }
    }

    // The clipboard actions live here too — a record menu is where people look for
    // them, and these are the window's own, so the shortcuts stay visible beside them.
    if (m_copyAction && m_copyColumnsAction) {
        if (filterSection || highlightSection)
            menu->addSeparator();
        menu->addAction(m_copyAction);
        menu->addAction(m_copyColumnsAction);
    }
}

void MainWindow::showRecordMenu(DocumentView *view, int viewRow, int column,
                                const QPoint &globalPos)
{
    QMenu menu(this);
    buildRecordMenu(&menu, view, viewRow, column);
    if (!menu.isEmpty())
        menu.exec(globalPos);
}

// The one double-click gesture (SPEC.md §5). A Subsystem or a Thread cell is the only
// place in the table where a cell names a VALUE of an axis the filters have, so it is
// the only place a double-click has something obvious to mean: show only this one. Every
// other column does nothing, deliberately — a gesture invented for the Message or the
// Time column would be a second thing to learn and a second thing to undo.
//
// It builds the record menu and triggers the item by object name, and never touches a
// pane itself. That is the whole design: the gating is the menu's (an unparsed line, or
// a format with no %t, offers no item and the double-click is inert), the edit is the
// pane's, and the filter is applied by the same applyFiltersFor() the menu reaches — so
// the reader keeps their place (SPEC.md §6) with nothing here knowing that they do.
//
// Deliberately NOT a toggle. A second double-click on the same cell re-applies the same
// "show only", which is idempotent; undoing it would mean recognising the pane's current
// state as "exactly what my last double-click set" — untrue as soon as the user has
// touched the pane — and the pane is where the menu's edits are taken back.
void MainWindow::activateRecordColumn(DocumentView *view, int viewRow, int column)
{
    if (!view || !view->context() || !view->context()->doc)
        return;
    const QVector<Field> &fields = view->context()->doc->format().fields;
    if (column < 0 || column >= fields.size())
        return;
    // Its OWN two-role gate, not applyRecordFilter()'s three: a double-click on a
    // Priority cell has always done nothing and goes on doing nothing, because the
    // gesture was invented for the columns where a cell names a value of a value axis.
    const char *wanted = nullptr;
    switch (fields.at(column).role) {
    case FieldRole::Logger: wanted = "recordShowOnlySubsystem"; break;
    case FieldRole::Thread: wanted = "recordShowOnlyThread"; break;
    default: return;
    }
    triggerRecordMenuItem(view, viewRow, column, wanted);
}

// The two filter chords (SPEC.md §5). Ctrl+Alt+click is the Filters pane's own Ctrl+click
// — "show only this one" — moved onto the record, and Alt+click is unticking that value,
// which is the edit the pane is next most often wanted for. Priority takes the first and
// not the second: its axis is a MINIMUM level, so "at least this bad" is a thing to point
// at and "not this level" is not one the axis can express.
//
// Deliberately NOT toggles, for activateRecordColumn()'s own reason: undoing would mean
// recognising the pane's state as exactly what the last chord set, which stops being true
// the moment the reader touches the pane — and the pane is where these edits are undone.
void MainWindow::applyRecordFilter(DocumentView *view, int viewRow, int column,
                                   RecordFilterCommand command)
{
    if (!view || !view->context() || !view->context()->doc)
        return;
    const QVector<Field> &fields = view->context()->doc->format().fields;
    if (column < 0 || column >= fields.size())
        return;
    const bool only = command == RecordFilterCommand::ShowOnly;
    const char *wanted = nullptr;
    switch (fields.at(column).role) {
    case FieldRole::Logger:
        wanted = only ? "recordShowOnlySubsystem" : "recordHideSubsystem";
        break;
    case FieldRole::Thread:
        wanted = only ? "recordShowOnlyThread" : "recordHideThread";
        break;
    case FieldRole::Priority:
        if (!only)
            return;
        wanted = "recordPriorityFloor";
        break;
    default:
        return;
    }
    triggerRecordMenuItem(view, viewRow, column, wanted);
}

void MainWindow::triggerRecordMenuItem(DocumentView *view, int viewRow, int column,
                                       const char *objectName)
{
    QMenu menu(this);
    buildRecordMenu(&menu, view, viewRow, column);
    for (QAction *a : menu.actions()) {
        if (a->objectName() == QLatin1String(objectName)) {
            a->trigger();
            return;
        }
    }
}

// --- Recent files ----------------------------------------------------------

void MainWindow::rememberRecentFile(const QString &path)
{
    QSettings settings;
    QStringList recent = settings.value(QLatin1String(kRecentFilesKey)).toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > kMaxRecentFiles)
        recent.removeLast();
    settings.setValue(QLatin1String(kRecentFilesKey), recent);
    refreshRecentFilesMenu();
}

void MainWindow::refreshRecentFilesMenu()
{
    if (!m_recentMenu)
        return;
    m_recentMenu->clear();
    const QStringList recent = QSettings().value(QLatin1String(kRecentFilesKey)).toStringList();
    if (recent.isEmpty()) {
        QAction *none = m_recentMenu->addAction(tr("(none)"));
        none->setObjectName(QStringLiteral("recentEmptyAction"));
        none->setEnabled(false);
    } else {
        // What an entry is CALLED is decided against the whole list at once (TabLabels.h):
        // the log's own name, grown by the nearest parent directory that differs wherever
        // two entries would otherwise read alike. A raw address is unbounded in width —
        // an `ssh://` URL or a path continuing through an archive especially — and a menu
        // is as wide as its widest item, so the list used to size itself to the longest
        // path ever opened. The full address is not lost: it is the entry's tooltip,
        // which is also what makes shortening the label safe, exactly as it is on a tab.
        //
        // This is the PREFIX rule and not the tab's bracket rule, deliberately: an entry
        // is read against the other nine rather than against the logs open beside it, and
        // a path is what a person recognises a remembered file by.
        const QStringList labels = prefixedLabelsFor(recent);
        for (int i = 0; i < recent.size(); ++i) {
            const QString &path = recent.at(i);
            QString label = labels.at(i);
            label.replace(u'&', QLatin1String("&&")); // a menu reads '&' as a mnemonic
            QAction *a = m_recentMenu->addAction(label);
            // The address, for a test and for anything that has to say WHICH entry this
            // is without reading its visible text.
            a->setData(path);
            // The address VERBATIM, exactly as a tab's tooltip carries its document's
            // path: this string is what triggering the entry opens, so anything
            // re-spelled here would describe a different open.
            a->setToolTip(path);
            connect(a, &QAction::triggered, this, [this, path]() { openFile(path); });
        }
    }
    // Forgetting the list is the one thing the menu could not do, and a list of ten
    // addresses is exactly the kind of thing somebody wants off the screen. Always
    // present so it can be found, disabled while there is nothing to forget.
    //
    // Made ONCE and parented to the window, unlike every entry above it: triggering it
    // rebuilds this menu, and the rebuild starts with QMenu::clear(), which deletes the
    // actions the menu owns — this action, while its own triggered() is being emitted.
    if (!m_clearRecentAction) {
        m_clearRecentAction = new QAction(tr("Clear Recent Files"), this);
        // findChild, for tests
        m_clearRecentAction->setObjectName(QStringLiteral("clearRecentFilesAction"));
        connect(m_clearRecentAction, &QAction::triggered, this, [this]() {
            QSettings().remove(QLatin1String(kRecentFilesKey));
            refreshRecentFilesMenu();
        });
    }
    m_clearRecentAction->setEnabled(!recent.isEmpty());
    m_recentMenu->addSeparator();
    m_recentMenu->addAction(m_clearRecentAction);
}

// --- Drag and drop ---------------------------------------------------------

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    // Every dropped file opens, each in its own tab (SPEC.md §3). A drop out of a
    // file manager's SSH mount arrives as an sftp:// (or ssh://) URL rather than a
    // local file, and opens as a remote log — openFile() normalizes the spelling.
    const QList<QUrl> urls = event->mimeData()->urls();
    QStringList paths;
    paths.reserve(urls.size());
    for (const QUrl &url : urls) {
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
        else if (RemoteLocation::isRemote(url.toString()))
            paths.append(url.toString());
    }
    openFiles(paths);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // THE PROMPT COMES FIRST, above saveSession(), and the order is the whole point. A
    // cancelled quit must leave the window exactly as it was — and saveSession() does not
    // merely write the session: it calls persistFileSettings() for every context and
    // flushes the per-log pool. Asking afterwards would mean a "Cancel" that had already
    // performed half a quit.
    //
    // In tab order, so the questions arrive left to right the way the tabs read.
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *editor = qobject_cast<ConfigView *>(m_tabs->widget(i))) {
            if (!confirmDiscard(editor)) {
                event->ignore();
                return;
            }
        }
    }

    // Persist the full session BEFORE teardown drops the view and unbinds the panes
    // (SPEC.md §10). Global state is last-writer-wins across instances (§8.1).
    saveSession();
    closeAllDocuments(Prompt::AlreadyAsked);
    event->accept();
}

// --- Log text size (SPEC.md §5, ARCHITECTURE.md §7.1.5) --------------------

void MainWindow::applyLogFontToViews()
{
    // Every open view at once, the DIGEST STRIPS INCLUDED: the strip's claim is that a
    // row is rendered exactly as it is in the log above it, and a strip in a different
    // size would read as a different kind of row rather than as a copy of one. Each view
    // invalidates its own geometry from the font change (LogView::applyFontChange).
    const QFont f = logTextFont();
    for (DocumentView *view : std::as_const(m_views)) {
        view->logView()->setFont(f);
        view->digestView()->setFont(f);
    }
    // The config editors too, or a zoom leaves every open config file behind at the old
    // size while the logs move — and "the editor font is the same as in logs" would then
    // be true only until somebody pressed Ctrl+=.
    for (ConfigView *editor : std::as_const(m_editors))
        editor->setLogFont(f);
}

void MainWindow::setLogFontSize(int points)
{
    if (!setLogFontPointSize(points)) {
        announceLogFontSize(); // unmoved, at a bound or already there — still say where
        return;
    }
    applyLogFontToViews();
    // Written immediately rather than at closeEvent: this is not window state, and a
    // second window opened meanwhile should come up at the size the reader just chose.
    QSettings().setValue(QLatin1String(kLogFontSizeKey), logFontPointSize());
    announceLogFontSize();
}

void MainWindow::stepLogFontSize(int steps)
{
    setLogFontSize(logFontPointSize() + steps);
}

void MainWindow::announceLogFontSize()
{
    // Through the status BAR rather than m_statusLabel: that label is rebuilt from the
    // document on every tick by updateStatus(), so a size written into it would be gone
    // by the next one. A transient message covers it and clears itself.
    statusBar()->showMessage(tr("Log text size: %1 pt").arg(logFontPointSize()), 2000);
}

void MainWindow::announceReload(ReloadCause cause, const QString &path)
{
    // Through the status BAR for the reason announceLogFontSize() gives above, and with
    // more force here: this fires FROM a watch tick, and updateStatus() runs on that same
    // tick from the rescanned handler — so a sentence written into m_statusLabel would be
    // overwritten by the very reload that produced it.
    //
    // It names the log even though only the active tab announces, because the temporary
    // message covers m_statusLabel, which is the one place the name otherwise appears.
    const QString name = logSourceDisplayName(path);
    const QString text = cause == ReloadCause::Truncated
                             ? tr("%1 was truncated — reloaded").arg(name)
                             : tr("%1 was replaced — reloaded").arg(name);
    // A writer rewriting in place faster than the notice expires would otherwise re-arm
    // the timer on every reload and keep m_statusLabel hidden for as long as it kept
    // going. Re-showing the same sentence says nothing new, so the FIRST timer is left to
    // run and the record count comes back five seconds after the first notice rather than
    // five seconds after the last.
    if (statusBar()->currentMessage() == text)
        return;
    statusBar()->showMessage(text, kReloadNoticeMs);
}

// --- Session persistence ---------------------------------------------------

void MainWindow::saveSession()
{
    // The active file's pane state lives in the widgets, not in its context, until
    // the user switches away — so fold it back before reading the contexts.
    stashPaneState(activeContext());

    Session session;
    session.geometry = saveGeometry();
    // The pane layout: which panes are open, where, and how they are tabbed, split or
    // floated (SPEC.md §8, §10). Only the panes are docks, so this blob no longer
    // describes the open files — those are the `views` array below. Must be taken
    // while the docks still exist, i.e. before any teardown.
    session.windowState = saveState();

    // WHICH LOGS WERE OPEN, and in the views array which view showed which — and that is
    // all the session says about a file now (invariant #7 / §12.4). Its filters, its
    // highlight rules and its run are per-FILE state and live in its own record (M21),
    // which is what makes them survive closing the tab: the session only ever remembered
    // them while the log was open in one.
    //
    // The last flush of each record goes here, so a quit stores what the panes were
    // holding. It carries the three-way run branch that used to be written out in this
    // loop, one function over (runSelectionOf()), so the two cannot drift.
    QHash<const DocumentContext *, int> documentIndex;
    for (const auto &ctx : m_contexts) {
        persistFileSettings(ctx.get());

        SessionDocument d;
        d.path = ctx->doc->path();
        documentIndex.insert(ctx.get(), int(session.documents.size()));
        session.documents.append(d);
    }
    // Once for the gesture, after every record has had its say.
    m_fileStore.flush();

    // THE SAVED ORDER IS THE BAR'S ORDER, read off the bar. The `views` array carries
    // the tab layout and nothing else does, so this is where a dragged tab has to be
    // observed — and m_views is no longer the place to observe it from.
    const QVector<DocumentView *> ordered = viewsInTabOrder();
    for (DocumentView *view : ordered) {
        SessionView v;
        v.documentIndex = documentIndex.value(view->context(), 0);
        v.columnState = view->logView()->saveColumnState();
        v.wrapMode = int(view->logView()->wrapMode());
        session.views.append(v);
    }
    // Into the ORDERED list, for the same reason: activeView indexes `views`.
    session.activeView = qMax(0, int(ordered.indexOf(m_activeView)));

    // The editor pages, each with WHERE ON THE BAR it sat. The views array's order is
    // the whole layout a session used to need; with a second kind of page, order alone
    // cannot say how the two interleave, so an absolute position is what carries it.
    // Walked over the bar so the positions are the real ones.
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *editor = qobject_cast<ConfigView *>(m_tabs->widget(i));
        if (!editor)
            continue;
        SessionEditor e;
        e.address = editor->address();
        e.tabIndex = i;
        e.syntaxChosen = editor->syntaxWasChosen();
        e.syntax = int(editor->syntax());
        session.editors.append(e);
    }
    session.activeTab = qMax(0, m_tabs->currentIndex());

    QSettings store;
    SessionStore::save(store, session);
}

// Hold the restore's bulk-prompt mode open until every restored remote log has stopped
// being able to ask anything, then let it go.
//
// A fetcher can only prompt on the attempt the document's own opening granted it a
// prompter for; once it has left Connecting it has either got in, given up, or gone to
// its unattended retry loop, and none of those asks again without File ▸ Reconnect. So
// "still Connecting" is exactly the set worth waiting for.
//
// Polled rather than signalled, like everything else that watches a fetcher, and capped
// so that one wedged host cannot leave the mode armed for the session.
void MainWindow::armBulkRestore()
{
    if (!m_sshPrompter)
        return;

    const auto stillConnecting = [this]() {
        for (const auto &ctx : m_contexts) {
            if (!ctx->doc)
                continue;
            const auto *spooled = dynamic_cast<const SpooledLogSource *>(ctx->doc->source());
            if (!spooled)
                continue;
            const FetchStatus::State state = spooled->fetchStatus().state;
            if (state == FetchStatus::State::Connecting || state == FetchStatus::State::Idle)
                return true;
        }
        return false;
    };

    if (!stillConnecting()) {
        m_sshPrompter->endBulkRestore();
        return;
    }

    auto *timer = new QTimer(this);
    timer->setObjectName(QStringLiteral("bulkRestoreWatch")); // findChild, for tests
    timer->setInterval(kBulkRestoreWatchMs);
    auto since = std::make_shared<QElapsedTimer>();
    since->start();
    connect(timer, &QTimer::timeout, this, [this, timer, since, stillConnecting]() {
        if (since->elapsed() < kBulkRestoreCapMs && stillConnecting())
            return;
        if (m_sshPrompter)
            m_sshPrompter->endBulkRestore();
        timer->stop();
        timer->deleteLater();
    });
    timer->start();
}

void MainWindow::restoreSession()
{
    QSettings store;
    const Session session = SessionStore::load(store);

    if (!session.geometry.isEmpty())
        restoreGeometry(session.geometry);

    // The pane layout goes back while only the pane docks exist — which is all of
    // them now that the documents are tabs, so there is nothing left to place
    // afterwards.
    if (!session.windowState.isEmpty()) {
        restoreState(session.windowState);
        m_layoutRestored = true;
        // A layout saved where panes could float — another platform, or this one
        // before the pane got wedged mid-drag — must not bring back a window this
        // platform cannot place. Pull any such pane back into the dock area.
        if (!panesMayFloat()) {
            for (QDockWidget *dock : std::as_const(m_paneDocks)) {
                if (dock->isFloating())
                    dock->setFloating(false);
            }
        }
    }

    // Rebuild the files and their views in the saved order, which is the saved TAB
    // order. Opening is split: the synchronous half (open the source, compile the
    // format, build the model and the tab) runs for everything first, so every tab
    // exists before any of them starts indexing on a worker thread.
    // A restored session reopens every remote log too, prompting where a host needs
    // it. The storm that could be is contained structurally rather than by refusing
    // to restore: the credential cache means one prompt PER HOST however many of its
    // files were open, and the prompt grows "Skip This Host" / "Skip All Remaining"
    // so a host that is not available today cannot hold the launch hostage.
    if (m_sshPrompter)
        m_sshPrompter->beginBulkRestore();

    // Every refusal in the restore is one gesture's worth, exactly as a multi-file
    // open is: reported together, once, when the loop is done.
    beginOpenBatch();
    QStringList missing;
    QHash<int, DocumentContext *> byDocument;
    DocumentView *toActivate = nullptr;
    for (int i = 0; i < session.views.size(); ++i) {
        const SessionView &sv = session.views.at(i);
        const SessionDocument *d = session.documentFor(sv);
        if (!d || d->path.isEmpty())
            continue;

        DocumentContext *ctx = byDocument.value(sv.documentIndex, nullptr);
        if (!ctx) {
            // A file that is not there is no longer dropped from the restore. It comes
            // back as a WAITING tab, and the reason is not tidiness: saveSession()
            // writes only the files that are open, so a file skipped at launch was
            // silently forgotten at the next quit — an unmounted share or a host that
            // was down for an afternoon cost you the tab permanently. Waiting for it
            // keeps it in the session and picks it up when it returns (SPEC.md §3, §10).
            //
            // prepareContext() reaches Document::prepare(), which decides waitable vs
            // fatal for itself, so there is nothing to pre-check here any more.

            // The user asked to stop reopening remote logs: honor it for the rest of
            // the restore instead of asking again per file.
            if (RemoteLocation::isRemote(d->path) && m_sshPrompter
                && m_sshPrompter->restoreCancelled()) {
                if (!missing.contains(d->path)) {
                    missing.append(d->path);
                    reportOpenRefusal(logSourceDisplayPath(d->path),
                                      tr("Reopening remote logs was cancelled."));
                }
                continue;
            }
            QString reason;
            ctx = prepareContext(*d, &reason);
            if (!ctx) {
                // Genuinely refused rather than merely absent — a changed host key, an
                // archive naming no member, a dependency not built in. Listed rather
                // than errored, exactly as before. This now covers LOCAL failures too:
                // one used to vanish here without appearing in the list at all.
                if (!missing.contains(d->path)) {
                    missing.append(d->path);
                    reportOpenRefusal(logSourceDisplayPath(d->path), reason);
                }
                continue;
            }
            byDocument.insert(sv.documentIndex, ctx);
        }

        // Every saved view is created here — including the file's first, which is why
        // prepareContext() makes none.
        DocumentView *view = createView(ctx);
        view->logView()->setWrapMode(static_cast<LogView::WrapMode>(sv.wrapMode));
        if (!sv.columnState.isEmpty())
            view->logView()->restoreColumnState(sv.columnState);
        // Which view was active is a saved INDEX, and skipped files shift every index
        // after them — so resolve it here, against the views actually created.
        if (i == session.activeView)
            toActivate = view;
    }

    // BULK MODE STAYS ARMED until the last connect has settled, not until this loop
    // finishes. The loop used to be the whole restore — every connect ran inside it — and
    // now it merely creates the tabs, in milliseconds, while the connects run behind them.
    // Disarming here would take "Skip This Host" and "Skip All Remaining" off the very
    // dialogs they exist for, since none of them has appeared yet.
    armBulkRestore();

    if (!missing.isEmpty()) {
        // A local file that has gone and a remote host that would not answer are
        // different problems, so say which one this is rather than telling someone
        // their server's log "no longer exists".
        const bool anyRemote = std::ranges::any_of(missing,
                                           [](const QString &p) {
                                               return RemoteLocation::isRemote(p);
                                           });
        QStringList shown;
        shown.reserve(missing.size());
        for (const QString &p : std::as_const(missing))
            shown.append(logSourceDisplayPath(p));
        // "Could not be reopened", not "no longer available" — a file that is merely
        // absent now restores as a waiting tab and never reaches this list, so
        // everything in it was actively refused.
        m_placeholder->setText(QStringLiteral("%1\n%2")
                                   .arg(anyRemote
                                            ? tr("These logs could not be reopened:")
                                            : tr("These files could not be reopened:"),
                                        shown.join(u'\n')));
        // No status-bar line beside it: the strip above the well already names every
        // one of them WITH its reason and stays there until dismissed, while this
        // placeholder is only on screen while nothing else opened at all.
    }
    // The config-file editor pages, put back WHERE THEY SAT. In ascending tab order and
    // inserted rather than appended, which is what reproduces the interleaving exactly:
    // the log tabs are already in place, so inserting each editor at its recorded
    // position walks the bar back to the shape it had.
    //
    // The FILE is re-read from disk, never restored from the session: a session is not a
    // backing store for unsaved work, and resurrecting a buffer over a file somebody
    // changed in the meantime would be worse than losing it.
    QVector<SessionEditor> editors = session.editors;
    std::ranges::sort(editors,
              [](const SessionEditor &a, const SessionEditor &b) {
                  return a.tabIndex < b.tabIndex;
              });
    for (const SessionEditor &e : std::as_const(editors)) {
        const ConfigReadResult read = readConfigFile(e.address);
        if (!read.ok) {
            // Listed with its reason like any other refused restore, rather than an
            // error dialog on every launch (SPEC.md §10).
            reportOpenRefusal(logSourceDisplayName(e.address), read.error);
            continue;
        }
        auto *view = new ConfigView(e.address, this);
        view->setContents(read.bytes, read.existed);
        if (e.syntaxChosen)
            view->setSyntax(static_cast<ConfigSyntax>(e.syntax), /*chosen=*/true);
        m_editors.append(view);
        connect(view, &ConfigView::zoomStepRequested, this, &MainWindow::stepLogFontSize);
        connect(view, &ConfigView::modifiedChanged, this,
                [this, view](bool) { updateConfigTabTitle(view); });
        connect(view, &QObject::destroyed, this, [this](QObject *obj) {
            m_editors.removeIf([obj](ConfigView *v) { return v == obj; });
        });
        m_tabs->insertTab(qMin(e.tabIndex, m_tabs->count()), view, QString());
        updateConfigTabTitle(view);
    }

    endOpenBatch();
    updateEmptyState();
    if (m_contexts.empty() && m_editors.isEmpty())
        return;

    // Activate the saved view, which binds the panes to its file, then start every
    // scan. Indexing goes last so worker batches never race the layout settling.
    if (!m_contexts.empty())
        showView(toActivate ? toActivate : m_views.first());
    // The saved tab last, so an editor page that was in front comes back in front. After
    // showView() rather than instead of it: showView() is what binds the panes to a log,
    // and that has to happen whichever page ends up current.
    if (session.activeTab >= 0 && session.activeTab < m_tabs->count())
        m_tabs->setCurrentIndex(session.activeTab);
    if (m_contexts.empty())
        return;

    // Restored rules go straight onto their Documents rather than through the pane, so
    // nothing above has asked whether any of them wants notifications (M19).
    updateTrayPresence();

    for (auto &ctx : m_contexts) {
        if (ctx->controller)
            ctx->controller->start();
    }
}

} // namespace loftail
