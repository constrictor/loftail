#include "MainWindow.h"

#include "Decoder.h"
#include "DefaultFormatStore.h"
#include "DetectingFormatProvider.h"
#include "Document.h"
#include "DocumentContext.h"
#include "DocumentView.h"
#include "Filter.h"
#include "FilterPane.h"
#include "FindBar.h"
#include "FormatCache.h"
#include "FormatPreview.h"
#include "HighlighterPane.h"
#include "IndexController.h"
#include "LiveController.h"
#include "LogFormat.h"
#include "LogFormatDialog.h"
#include "LogModel.h"
#include "LogSource.h"
#include "ManualFormatProvider.h"
#include "HostBookmarkStore.h"
#include "ArchiveLocation.h"
#include "OpenArchiveDialog.h"
#include "PreferencesDialog.h"
#include "SourceSpool.h"
#include "SpooledLogSource.h"

#include <QElapsedTimer>
#include <QTimer>
#include "OpenRemoteDialog.h"
#include "PresetPane.h"
#include "PaneTitleStyle.h"
#include "RemoteLocation.h"
#include "RunPane.h"
#include "SshFetcher.h"
#include "SshPromptDialogs.h"
#include "SessionStore.h"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
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
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

namespace loftail {

namespace {
// The default format a never-seen file is tried with now lives in the core
// DefaultFormatStore (M18): it is a user setting, and the built-in log4cplus layout is
// only what it falls back to. A file that matches neither still opens with unparsed
// lines as plain text (SPEC.md §4).
constexpr int  kMaxRecentFiles = 10;

// How often the restore checks whether its remote logs have finished connecting, and the
// ceiling on how long it will keep the bulk-prompt mode armed waiting for them. The cap
// exists so that one host that neither answers nor fails cannot leave "Skip All
// Remaining" on every password dialog for the rest of the session.
constexpr int  kBulkRestoreWatchMs = 500;
constexpr int  kBulkRestoreCapMs = 60000;
constexpr auto kRecentFilesKey = "recentFiles";

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
        // The saved default format (M18), or the built-in when the user has saved none.
        // Read once here and kept in step by showPreferences()/showFormatDialog(), so an
        // open never touches QSettings for it.
        QSettings store;
        m_defaultFormat = DefaultFormatStore::load(store);
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

    m_progressBar = new QProgressBar(this);
    m_progressBar->setMaximumWidth(200);
    m_progressBar->setVisible(false);
    m_statusLabel = new QLabel(tr("No file open"), this);
    m_statusLabel->setObjectName(QStringLiteral("statusLabel")); // findChild, for tests
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_progressBar);

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
    setCentralWidget(m_centre);

    // Three side panes (SPEC.md §8): filters, highlighters, presets. Each binds to
    // the active document by signal (invariant #7 / §12.3), never a fixed Document.
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
    // "Present", not "in force" — a rule in the list is something the user put there,
    // where a filter axis can be switched on and still exclude nothing.
    m_highlightersDock = highlightDock;
    connect(m_highlighterPane, &HighlighterPane::activityChanged, this, [this](bool active) {
        if (m_highlightersDock)
            m_highlightersDock->setWindowTitle(active ? tr("Highlighters •") : tr("Highlighters"));
    });

    m_presetPane = new PresetPane(m_filterPane, m_highlighterPane, this);
    QDockWidget *presetDock = addPaneDock(m_presetPane, QStringLiteral("presetsDock"),
                                          tr("Presets"));

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
    tabifyDockWidget(filterDock, highlightDock);
    tabifyDockWidget(highlightDock, presetDock);
    tabifyDockWidget(presetDock, runDock);
    filterDock->raise();

    buildMenus();

    // Restore the previous working state last, once every pane dock exists with its
    // object name (restoreState keys off those) — SPEC.md §10.
    restoreSession();
}

MainWindow::~MainWindow()
{
    closeAllDocuments();
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
    m_closeTabAction = fileMenu->addAction(tr("&Close Tab"));
    m_closeTabAction->setObjectName(QStringLiteral("closeTabAction")); // findChild, for tests
    m_closeTabAction->setShortcut(QKeySequence::Close); // Ctrl+W
    m_closeTabAction->setEnabled(false);
    connect(m_closeTabAction, &QAction::triggered, this, &MainWindow::closeActiveView);

    m_closeAllAction = fileMenu->addAction(tr("Close &All"));
    m_closeAllAction->setObjectName(QStringLiteral("closeAllAction")); // findChild, for tests
    m_closeAllAction->setEnabled(false);
    connect(m_closeAllAction, &QAction::triggered, this, &MainWindow::closeAllDocuments);

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

    fileMenu->addSeparator();
    m_formatAction = fileMenu->addAction(tr("&Log Format..."));
    m_formatAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    m_formatAction->setEnabled(false);
    connect(m_formatAction, &QAction::triggered, this, &MainWindow::showFormatDialog);

    fileMenu->addSeparator();
    m_cancelAction = fileMenu->addAction(tr("&Cancel Indexing"));
    m_cancelAction->setEnabled(false);
    connect(m_cancelAction, &QAction::triggered, this, [this]() {
        if (DocumentContext *ctx = activeContext(); ctx && ctx->controller)
            ctx->controller->cancel();
    });

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

    // Find / Find Next / Find Previous (SPEC.md §5). Find opens the bar; F3 /
    // Shift+F3 navigate the current query over the visible rows.
    editMenu->addSeparator();
    QAction *findAction = editMenu->addAction(tr("&Find..."));
    findAction->setShortcut(QKeySequence::Find);
    connect(findAction, &QAction::triggered, this, [this]() {
        if (m_activeView)
            m_activeView->activateFind();
    });
    QAction *findNextAction = editMenu->addAction(tr("Find &Next"));
    findNextAction->setShortcut(QKeySequence::FindNext); // F3
    connect(findNextAction, &QAction::triggered, this, [this]() { runFind(true, false); });
    QAction *findPrevAction = editMenu->addAction(tr("Find Pre&vious"));
    findPrevAction->setShortcut(QKeySequence::FindPrevious); // Shift+F3
    connect(findPrevAction, &QAction::triggered, this, [this]() { runFind(false, false); });

    // M18 — application-wide settings. Deliberately NOT disabled when no log is open:
    // the default format is what the NEXT open uses, so the moment before there is a
    // document is exactly when someone wants to set it. PreferencesRole is what moves it
    // into the application menu on macOS, where the Edit menu is the wrong place for it.
    editMenu->addSeparator();
    QAction *preferencesAction = editMenu->addAction(tr("&Preferences..."));
    preferencesAction->setObjectName(QStringLiteral("preferencesAction")); // findChild, for tests
    preferencesAction->setShortcut(QKeySequence::Preferences);
    preferencesAction->setMenuRole(QAction::PreferencesRole);
    connect(preferencesAction, &QAction::triggered, this, &MainWindow::showPreferences);

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    QMenu *wrapMenu = viewMenu->addMenu(tr("Line &Wrap"));
    auto *wrapGroup = new QActionGroup(this);
    QAction *wrapOff = wrapMenu->addAction(tr("&Off"));
    QAction *wrapSel = wrapMenu->addAction(tr("&Selected Record Only"));
    QAction *wrapAll = wrapMenu->addAction(tr("&Always On"));
    for (QAction *a : {wrapOff, wrapSel, wrapAll}) {
        a->setCheckable(true);
        wrapGroup->addAction(a);
    }
    wrapOff->setChecked(true);
    auto setWrap = [this](LogView::WrapMode mode) {
        m_wrapMode = mode; // the default for views created from here on
        if (LogView *v = activeLogView())
            v->setWrapMode(mode);
    };
    connect(wrapOff, &QAction::triggered, this, [setWrap]() { setWrap(LogView::WrapMode::Off); });
    connect(wrapSel, &QAction::triggered, this,
            [setWrap]() { setWrap(LogView::WrapMode::SelectedRecordOnly); });
    connect(wrapAll, &QAction::triggered, this,
            [setWrap]() { setWrap(LogView::WrapMode::AlwaysOn); });

    buildTimeDisplayMenu();

    // Return-to-bottom / follow control (SPEC.md §3, M6). Checked reflects whether
    // the view is currently following; triggering it re-attaches and jumps to the end.
    viewMenu->addSeparator();
    m_followAction = viewMenu->addAction(tr("&Follow Tail"));
    m_followAction->setCheckable(true);
    m_followAction->setChecked(true);
    m_followAction->setEnabled(false);
    m_followAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_End));
    connect(m_followAction, &QAction::triggered, this, [this]() {
        if (LogView *v = activeLogView())
            v->followTail();
    });

    // The one way back to an unfiltered view that does not mean visiting five axes by
    // hand. On the menu as well as on the pane's own header because the pane can be
    // closed outright (View ▸ Panes), and a filter left in force with no pane to
    // clear it from is the state this exists for.
    viewMenu->addSeparator();
    m_clearFiltersAction = viewMenu->addAction(tr("&Clear Filters"));
    m_clearFiltersAction->setObjectName(QStringLiteral("clearFiltersAction"));
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
    for (DocumentView *view : std::as_const(m_views)) {
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
    if (m_activeView)
        m_activeView->logView()->setFocus();
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
    // The current page IS the active view; there is no other way to be looking at a
    // log. A -1 (the last tab just closed) makes the window file-less, which
    // setActiveView handles by unbinding the panes.
    setActiveView(qobject_cast<DocumentView *>(m_tabs->widget(index)));
}

void MainWindow::onTabMoved(int from, int to)
{
    // m_views is the session's view order and Ctrl+Tab's walk order, so a dragged
    // tab has to move with it.
    if (from < 0 || from >= m_views.size() || to < 0 || to >= m_views.size())
        return;
    m_views.move(from, to);
    // Two views of one file are numbered by tab position, so moving a tab renumbers
    // its file's — and only a file with several views can be affected.
    for (auto &ctx : m_contexts) {
        if (ctx->views.size() > 1)
            updateTabTitles(ctx.get());
    }
}

void MainWindow::updateEmptyState()
{
    m_centre->setCurrentWidget(m_views.isEmpty() ? static_cast<QWidget *>(m_placeholder)
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
        if (m_lastNotified == ctx.get())
            m_lastNotified = nullptr; // about to dangle
        return true;
    });
    // Closing the last log that wanted notifications takes the tray icon with it (M19).
    if (m_contexts.size() != before)
        updateTrayPresence();

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

void MainWindow::closeViewAt(int index)
{
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
    if (ctx && m_filterPane)
        ctx->filterState = m_filterPane->saveState();
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

    if (m_copyAction)
        m_copyAction->setEnabled(hasFile);
    if (m_copyColumnsAction)
        m_copyColumnsAction->setEnabled(hasFile);
    // The format dialog samples the source's first 64 KB, and a waiting document has
    // no source and no bytes to show — there is literally nothing to configure a format
    // against. It settles its own format from the log when it arrives, and the action
    // comes back with it (SPEC.md §3, §4).
    if (m_formatAction)
        m_formatAction->setEnabled(hasFile && !ctx->doc->isWaiting());
    // Only a spooled log has a fetcher to poke; a local one is watched, not connected.
    if (m_reconnectAction) {
        m_reconnectAction->setEnabled(
            hasFile && dynamic_cast<SpooledLogSource *>(ctx->doc->source()) != nullptr);
    }
    if (m_closeTabAction)
        m_closeTabAction->setEnabled(hasFile);
    if (m_closeAllAction)
        m_closeAllAction->setEnabled(!m_views.isEmpty());
    if (m_newViewAction)
        m_newViewAction->setEnabled(hasFile);
    if (m_followAction) {
        m_followAction->setEnabled(hasFile);
        // With no file the next open follows again (SPEC.md §3); with one, the
        // checkbox tracks that view's own follow state.
        m_followAction->setChecked(hasFile ? m_activeView->logView()->following() : true);
    }
    // The timestamp mode is per FILE, so the checkmark has to follow the active tab.
    updateTimeDisplayActions();
    if (m_cancelAction)
        m_cancelAction->setEnabled(hasFile && ctx->indexing);
    if (m_progressBar) {
        m_progressBar->setVisible(hasFile && ctx->indexing);
        if (hasFile && ctx->indexing)
            m_progressBar->setValue(ctx->progressPercent);
    }

    setWindowTitle(hasFile
                       ? tr("loftail — %1").arg(logSourceDisplayName(ctx->doc->path()))
                       : QStringLiteral("loftail"));
}

void MainWindow::chooseFileToOpen()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Log File"), QString(),
        // The archive filter is offered whether or not libarchive is compiled in, so
        // the two builds' dialogs look alike: a file that simply vanished from the list
        // would read as "loftail cannot see this", where trying it explains itself.
        tr("Log files (*.log *.txt);;"
                       "Compressed and archived logs "
                       "(*.gz *.bz2 *.xz *.zst *.zip *.tar *.tgz *.tar.gz *.tar.bz2 "
                       "*.tar.xz *.txz *.tar.zst *.7z);;"
                       "All files (*)"));
    if (!path.isEmpty())
        openFile(path);
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

void MainWindow::closeAllDocuments()
{
    if (m_contexts.empty()) {
        m_activeView = nullptr;
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
    for (auto &ctx : m_contexts)
        ctx->views.clear();
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

void MainWindow::openFile(const QString &rawPath, const QString &pattern)
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
    // open silently, the same contract cancelling the Log Format dialog has.
    if (const auto archive = ArchiveLocation::split(path); archive && archive->needsMember()) {
        QString error;
        const QStringList members =
            OpenArchiveDialog::chooseMembers(archive->container, this, &error);
        if (!error.isEmpty()) {
            m_statusLabel->setText(tr("Cannot open %1: %2")
                                       .arg(logSourceDisplayName(path), error));
            return;
        }
        // Several picked logs open as several tabs, exactly as dropping several files
        // does (SPEC.md §3).
        for (const QString &member : members)
            openFile(member, pattern);
        return;
    }

    // Per-file recall (SPEC.md §4): a file already configured reopens with its
    // saved format and no prompt. A never-seen file gets the supplied pattern, or the
    // user's DEFAULT format (M18), and the dialog is offered when that does not match.
    //
    // The two levels never mix: a cached entry is taken whole, and the default is
    // consulted only when there is none. The default brings its encoding and source zone
    // along with its pattern, since those are the same three things the Log Format dialog
    // sets — but an explicitly supplied pattern still overrides just the pattern, so a
    // command line naming one keeps the default's encoding rather than silently reverting
    // it to auto-detect.
    FormatSettings settings;
    QSettings store;
    bool cached = false;
    if (auto loaded = FormatCache::load(store, path)) {
        settings = *loaded;
        cached = true;
    } else {
        settings = m_defaultFormat;
        if (!pattern.isEmpty())
            settings.pattern = pattern;
    }
    // Offer the dialog only on an interactive open of a never-seen file that the
    // fallback default fails to parse. An explicitly-supplied pattern (command
    // line) is taken as the user's intent — a wrong one opens as plain text
    // without a blocking prompt, which also keeps headless/scripted opens safe.
    const bool promptIfNoMatch = !cached && pattern.isEmpty();
    openWithSettings(path, settings, promptIfNoMatch);
}

bool MainWindow::openWithSettings(const QString &path, FormatSettings settings,
                                  bool promptIfNoMatch, std::optional<RunRestore> runRestore)
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
            m_statusLabel->setText(tr("Cannot open %1: %2")
                                       .arg(logSourceDisplayName(path), doc->lastError()));
            return false;
        }
    }
    doc->setTimeDisplay(settings.timeDisplay);

    // Decide whether to remember this format on close of the flow. A cached open, a
    // dialog the user accepted, or a default that actually matched are all worth
    // persisting; a non-matching default the user declined is not (so reopen
    // re-prompts rather than silently showing plain text).
    bool persist = !promptIfNoMatch;

    // A log that is not there yet has no bytes to preview, autodetect from, or seed a
    // dialog with — and asking about a format before anyone has seen a line of the file
    // would be asking the user to guess too. It opens as a waiting tab and settles its
    // format from the bytes that actually arrive (Document::resume). Nothing is
    // persisted either: a pattern never checked against a line of the log is not
    // knowledge, and remembering it would suppress the format prompt forever.
    const bool deferFormatPrompt = doc->isWaiting() && promptIfNoMatch;
    if (doc->isWaiting()) {
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
                m_statusLabel->setText(tr("Cannot open %1: %2")
                                           .arg(logSourceDisplayName(path), doc->lastError()));
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
    ctx->pendingRunRestore = std::move(runRestore);
    // The prompt this open could not raise, deferred to the first resume that has bytes.
    ctx->pendingFormatPrompt = deferFormatPrompt;
    m_contexts.push_back(std::move(ctx));

    buildViewAndIndex(m_contexts.back().get());

    if (persist)
        persistFormat(path, settings);
    rememberRecentFile(path);
    return true;
}

DocumentView *MainWindow::createView(DocumentContext *ctx)
{
    auto *view = new DocumentView(ctx);
    ctx->views.append(view);
    connect(view, &DocumentView::findRequested, this, &MainWindow::runFind);

    LogView *logView = view->logView();
    logView->setWrapMode(m_wrapMode);
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

DocumentContext *MainWindow::prepareContext(const SessionDocument &d)
{
    auto doc = std::make_unique<Document>();
    ManualFormatProvider provider(d.format.pattern);
    if (!doc->prepare(d.path, provider, d.format.encoding, d.format.sourceZone.toZone()))
        return nullptr;
    doc->setTimeDisplay(d.format.timeDisplay);

    auto owned = std::make_unique<DocumentContext>();
    DocumentContext *ctx = owned.get();
    ctx->doc = std::move(doc);
    ctx->settings = d.format;
    ctx->filterState = d.filters;
    if (!d.format.runStartPattern.isEmpty()) {
        ctx->pendingRunRestore =
            RunRestore{d.runAll, d.selectedRunStartOffset, d.selectedRunStartTimestamp};
    }
    m_contexts.push_back(std::move(owned));

    // Highlight rules go straight onto the Document rather than through the pane:
    // the pane holds one file's rules at a time, and every restored file needs its
    // own. HighlighterPane::setDocument reads them back out when this file is shown.
    ctx->doc->highlighters() =
        HighlighterSet::fromJson(d.highlighters.value(QStringLiteral("rules")).toArray());
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

    ctx->controller = new IndexController(doc, ctx->model);
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
// (resumeWaitingDocument). Those two used to be a copy of each other with a comment
// promising they agreed; since M17 every remote and archived log takes the second route,
// so a divergence would mean the format prompt behaving differently for local and remote
// logs — the sort of thing nobody would notice for a year.
// Does this document's compiled format actually match the bytes it can now read? The
// half of offerFormat() that asks nobody anything, for the callers that must not.
bool MainWindow::formatFits(Document *doc, const FormatSettings &settings)
{
    const qint64 sampleLen = qMin<qint64>(64 * 1024, doc->source()->size());
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

    const qint64 sampleLen = qMin<qint64>(64 * 1024, doc->source()->size());
    const QByteArray sample = sampleLen > 0
        ? doc->source()->bytes(0, sampleLen).toByteArray() : QByteArray();

    // The default did not match and no format is cached (M3 unchanged: a cached file
    // never reaches here). Autodetect (M8, ARCHITECTURE.md §9) and PRE-FILL the dialog
    // with the detected pattern for confirmation — never applied silently. A no-detection
    // result leaves the dialog seeded with the fallback default.
    FormatSettings seed = *settings;
    DetectingFormatProvider detector(settings->encoding);
    detector.formatFor(QByteArrayView(sample.constData(), sample.size()));
    if (detector.detected())
        seed.pattern = detector.detectedPattern();

    LogFormatDialog dlg(logSourceDisplayName(path), sample, seed, this);
    if (dlg.exec() != QDialog::Accepted)
        return FormatOutcome::Declined;
    *settings = dlg.settings();
    // The checkbox is honoured here as well as in showFormatDialog(), because this is
    // the dialog most users meet first: it is the one that appears by itself when a log
    // does not parse, and having just fixed it against real lines is the moment the
    // answer is worth keeping.
    if (dlg.useAsDefault())
        rememberDefaultFormat(*settings);
    return FormatOutcome::Chosen;
}

void MainWindow::rememberDefaultFormat(const FormatSettings &s)
{
    QSettings store;
    DefaultFormatStore::save(store, s);
    // Re-read rather than assigning `s`: the store keeps three fields on purpose, and
    // reading back is what guarantees the in-memory copy holds the same three rather
    // than quietly carrying this file's timestamp mode and run-start pattern into the
    // next open (DefaultFormatStore::save).
    m_defaultFormat = DefaultFormatStore::load(store);
}

void MainWindow::showPreferences()
{
    // Preview the default against whichever log is open, so it can be checked against
    // real lines instead of typed blind. There may be none — the dialog is reachable
    // with an empty window, which is when someone most wants to set this up.
    QByteArray sample;
    if (DocumentContext *ctx = activeContext(); ctx && ctx->doc && ctx->doc->source()) {
        const qint64 sampleLen = qMin<qint64>(64 * 1024, ctx->doc->source()->size());
        if (sampleLen > 0)
            sample = ctx->doc->source()->bytes(0, sampleLen).toByteArray();
    }

    PreferencesDialog dlg(sample, m_defaultFormat, this);
    if (dlg.exec() == QDialog::Accepted)
        rememberDefaultFormat(dlg.defaultFormat());
    // dlg.formatCacheCleared() needs no action: nothing here caches the per-file store,
    // and open documents deliberately keep the format they are displaying.
}

void MainWindow::showFormatDialog()
{
    DocumentContext *ctx = activeContext();
    if (!ctx || !ctx->doc->source())
        return;
    Document *doc = ctx->doc.get();

    const qint64 sampleLen = qMin<qint64>(64 * 1024, doc->source()->size());
    const QByteArray sample = sampleLen > 0
        ? doc->source()->bytes(0, sampleLen).toByteArray() : QByteArray();

    LogFormatDialog dlg(logSourceDisplayName(doc->path()), sample, ctx->settings, this);
    if (dlg.exec() == QDialog::Accepted) {
        // The dialog does not edit the run-start axis. FormatEditor carries it through,
        // but restate it from the context anyway: this is the one place that KNOWS what
        // the run-start pattern is, and the cost of the belt is a line (§3a).
        FormatSettings s = dlg.settings();
        s.runStartPattern = ctx->settings.runStartPattern;
        s.runStartIsRegex = ctx->settings.runStartIsRegex;
        s.runStartCaseSensitive = ctx->settings.runStartCaseSensitive;
        // Ticked "also use for new files" (M18). Saved BEFORE applySettings(), which
        // rescans and destroys `ctx` and `doc` when the pattern changed.
        if (dlg.useAsDefault())
            rememberDefaultFormat(s);
        applySettings(s);
    }
}

void MainWindow::applySettings(const FormatSettings &newSettings)
{
    DocumentContext *ctx = activeContext();
    if (!ctx)
        return;
    Document *doc = ctx->doc.get();

    // Copy the path: a rescan tears the Document down, so `doc` and `ctx` must not
    // be read after openWithSettings() runs.
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
    persistFormat(path, newSettings);

    // Pattern or encoding change alters record boundaries and byte offsets (§6.1,
    // invariant #3), so the index is invalid — full rescan.
    if (patternChanged || encodingChanged) {
        openWithSettings(path, newSettings, /*promptIfNoMatch=*/false);
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

void MainWindow::persistFormat(const QString &path, const FormatSettings &s)
{
    QSettings store;
    FormatCache::save(store, path, s);
}

void MainWindow::updateTabTitles(DocumentContext *ctx)
{
    // A background file's scan has no claim on the status bar, so its progress shows
    // in its own tab title instead.
    QString name = logSourceDisplayName(ctx->doc->path());
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
    std::sort(indices.begin(), indices.end());
    const bool numbered = indices.size() > 1;
    for (int i = 0; i < indices.size(); ++i) {
        m_tabs->setTabText(indices.at(i),
                           numbered ? QStringLiteral("%1 [%2]").arg(base).arg(i + 1) : base);
        m_tabs->setTabToolTip(indices.at(i),
                              ctx->doc->isWaiting()
                                  ? QStringLiteral("%1\n%2").arg(ctx->doc->path(),
                                                                 ctx->doc->waitReason())
                                  : ctx->doc->path());
    }
}

void MainWindow::onIndexProgress(DocumentContext *ctx, qint64 done, qint64 total)
{
    if (total > 0)
        ctx->progressPercent = int((done * 100) / total);
    updateTabTitles(ctx);
    if (ctx == activeContext()) {
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
    if (isActive) {
        m_progressBar->setVisible(false);
        m_cancelAction->setEnabled(false);
    }

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

    doc->resolveHighlighters();
    // Runs are detected now that the full index exists (§3a). Restore the
    // persisted selection if this open came from session restore, else default
    // to the newest run (decision: open a live log on its current run).
    doc->detectRuns();
    if (ctx->pendingRunRestore) {
        if (ctx->pendingRunRestore->all)
            doc->selectRun(-1);
        else
            doc->selectRunByStart(ctx->pendingRunRestore->startOffset,
                                  ctx->pendingRunRestore->startTimestamp);
        ctx->pendingRunRestore.reset();
    } else {
        doc->selectNewestRun();
    }
    if (isActive && m_runPane)
        m_runPane->refresh();
    if (doc->filters().anyActive() || doc->viewRestricted())
        applyFiltersFor(ctx);
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

    // Activate the always-watched model (SPEC.md §3, M6): from here the file
    // auto-updates as it grows. The initial scan captured the size at open, so the
    // controller's first check catches up anything appended while it ran.
    {
        ctx->live = new LiveController(doc, ctx->model);
        // So the digest's wholesale ordinal remap is bracketed by a model reset before
        // the mutation rather than after it (M19).
        ctx->live->setDigestModel(ctx->digestModel);
        connect(ctx->live, &LiveController::ingested, this, [this, ctx](qint64) {
            // ABOVE the early return, deliberately (M19). A background tab is the ONLY
            // case the tab marker and the notification exist for, so anything that
            // handles them below this line works perfectly with one tab open and never
            // fires in real use.
            handleAlerts(ctx);
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
        // The log this document is waiting for is back. The pattern lives here, not in
        // core, so this is where the provider gets built and resume() gets called
        // (invariant #3). resume() may decline — the log can go again between the
        // check and the open — in which case the document stays waiting and the next
        // tick tries once more.
        connect(ctx->live, &LiveController::resumeRequested, this,
                [this, ctx]() { resumeWaitingDocument(ctx); });
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
void MainWindow::resumeWaitingDocument(DocumentContext *ctx)
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

    if (settleFormat) {
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
        const FormatOutcome outcome =
            mayAsk ? offerFormat(doc, doc->path(), &settled)
                   : (formatFits(doc, ctx->settings) ? FormatOutcome::Matched
                                                     : FormatOutcome::Declined);

        switch (outcome) {
        case FormatOutcome::Matched:
            // It fits, and it has now been checked against real lines rather than
            // assumed — which is exactly the point at which it becomes worth
            // remembering. The waiting open deliberately persisted nothing.
            persistFormat(doc->path(), ctx->settings);
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
            ctx->formatNotice = tr("format not recognised — Log ▸ Format…");
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

void MainWindow::applyFiltersFor(DocumentContext *ctx)
{
    if (!ctx || !ctx->model)
        return;
    // A filtered set is a wholesale row remap, so reset the model around the
    // recompute: the view/header/selection refresh over the new visible set and
    // LogView rebuilds its line geometry (invariant #6). The predicate chain inside
    // applyFilters runs integer axes first, message text last (invariant #4).
    ctx->model->beginFilterReset();
    ctx->doc->applyFilters();
    ctx->model->endFilterReset();
    for (DocumentView *v : std::as_const(ctx->views)) {
        v->logView()->updateGeometry();
        v->logView()->viewport()->update();
    }
    if (ctx == activeContext())
        updateStatus();
}

void MainWindow::applyActiveFilters()
{
    applyFiltersFor(activeContext());
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
    persistFormat(doc->path(), ctx->settings);

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

    doc->selectRun(runIndex);
    applyActiveFilters();
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

    // Follow only makes sense for the live tail: the newest run (or "all runs") jumps
    // to the end and keeps following; an earlier, finished run scrolls to its start,
    // which detaches follow so the history stays put while the file grows (§3a).
    const int newest = doc->runs().isEmpty() ? -1 : int(doc->runs().size()) - 1;
    const bool isLive = runIndex < 0 || runIndex == newest;
    for (DocumentView *v : std::as_const(ctx->views)) {
        if (isLive)
            v->logView()->followTail();
        else
            v->logView()->setCurrentRecord(0); // top of the run; detaches follow
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
    LogView *logView = activeLogView();
    LogModel *model = activeModel();
    FindBar *findBar = m_activeView ? m_activeView->findBar() : nullptr;
    if (!logView || !model || !findBar)
        return;
    const QString pattern = findBar->pattern();
    if (pattern.isEmpty()) {
        findBar->setStatus(QString());
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
        return;
    }

    const int count = model->rowCount();
    if (count == 0) {
        findBar->setStatus(tr("no records"));
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
        return;
    }
    logView->setCurrentRecord(hit);
    findBar->setStatus(QString()); // keep focus in the bar for repeated Enter/F3
}

void MainWindow::updateStatus()
{
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

    const int total = doc->index().records.size();
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
    // Through applySettings, exactly like the Log Format dialog: that is what puts
    // the choice in the FormatCache and the session, so it survives a restart.
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
    qint64 selLo = 0, selHi = 0;
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
        none->setEnabled(false);
        return;
    }
    for (const QString &path : recent) {
        QAction *a = m_recentMenu->addAction(path);
        connect(a, &QAction::triggered, this, [this, path]() { openFile(path); });
    }
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
    for (const QUrl &url : urls) {
        if (url.isLocalFile())
            openFile(url.toLocalFile());
        else if (RemoteLocation::isRemote(url.toString()))
            openFile(url.toString());
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Persist the full session BEFORE teardown drops the view and unbinds the panes
    // (SPEC.md §10). Global state is last-writer-wins across instances (§8.1).
    saveSession();
    closeAllDocuments();
    event->accept();
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

    // Every open file goes into the documents array; every view into the views array,
    // pointing back at its file (invariant #7 / §12.4). Everything here is portable
    // name/index JSON, so restore is the same path as applying a preset.
    QHash<const DocumentContext *, int> documentIndex;
    for (const auto &ctx : m_contexts) {
        Document *doc = ctx->doc.get();
        SessionDocument d;
        d.path = doc->path();
        d.format = ctx->settings;
        d.filters = ctx->filterState;
        // Highlighter rules are read straight off the Document, which HighlighterPane
        // keeps authoritative (it syncs on every edit) — so this is correct for a
        // background file just as much as for the active one.
        d.highlighters.insert(QStringLiteral("rules"), doc->highlighters().toJson());

        // Run selection (§3a). The run-start pattern rides in d.format; here we save
        // WHICH run was viewed by its stable start offset/timestamp (not the ordinal,
        // which shifts as the file grows). selectedRun() == -1 means "all runs" when a
        // pattern is set, otherwise nothing meaningful (restore falls back to newest).
        const int sel = doc->selectedRun();
        if (sel >= 0 && sel < doc->runs().size()) {
            d.runAll = false;
            d.selectedRunStartOffset = doc->runs().at(sel).startOffset;
            d.selectedRunStartTimestamp = doc->runs().at(sel).startTimestamp;
        } else {
            d.runAll = !ctx->settings.runStartPattern.isEmpty();
            d.selectedRunStartOffset = -1;
        }

        documentIndex.insert(ctx.get(), int(session.documents.size()));
        session.documents.append(d);
    }

    // m_views is kept in tab order, so saving it in order is what puts the tabs back
    // left to right — including after the user has dragged them around.
    for (DocumentView *view : std::as_const(m_views)) {
        SessionView v;
        v.documentIndex = documentIndex.value(view->context(), 0);
        v.columnState = view->logView()->saveColumnState();
        v.wrapMode = int(view->logView()->wrapMode());
        session.views.append(v);
    }
    session.activeView = qMax(0, m_views.indexOf(m_activeView));

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
                if (!missing.contains(d->path))
                    missing.append(d->path);
                continue;
            }
            ctx = prepareContext(*d);
            if (!ctx) {
                // Genuinely refused rather than merely absent — a changed host key, an
                // archive naming no member, a dependency not built in. Listed rather
                // than errored, exactly as before. This now covers LOCAL failures too:
                // one used to vanish here without appearing in the list at all.
                if (!missing.contains(d->path))
                    missing.append(d->path);
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
        const bool anyRemote = std::any_of(missing.cbegin(), missing.cend(),
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
        m_statusLabel->setText(
            tr("%1 file(s) from the last session unavailable").arg(missing.size()));
    }
    updateEmptyState();
    if (m_contexts.empty())
        return;

    // Activate the saved view, which binds the panes to its file, then start every
    // scan. Indexing goes last so worker batches never race the layout settling.
    showView(toActivate ? toActivate : m_views.first());

    // Restored rules go straight onto their Documents rather than through the pane, so
    // nothing above has asked whether any of them wants notifications (M19).
    updateTrayPresence();

    for (auto &ctx : m_contexts) {
        if (ctx->controller)
            ctx->controller->start();
    }
}

} // namespace loftail
