#include "MainWindow.h"

#include "Decoder.h"
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
#include "SourceSpool.h"
#include "SpooledLogSource.h"
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
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <utility>

namespace loftail {

namespace {
// The default log4cplus layout used until the M3 Log Format dialog exists. A file
// that does not match still opens with unparsed lines as plain text (SPEC.md §4).
constexpr auto kDefaultPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
constexpr int  kMaxRecentFiles = 10;
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
    : QMainWindow(parent), m_defaultPattern(QString::fromLatin1(kDefaultPattern))
{
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
    m_sshPrompter->setPasswordStorePath(HostBookmarkStore(HostBookmarkStore::defaultDir()).filePath());
    setSshPrompter(m_sshPrompter.get());

    m_progressBar = new QProgressBar(this);
    m_progressBar->setMaximumWidth(200);
    m_progressBar->setVisible(false);
    m_statusLabel = new QLabel(QStringLiteral("No file open"), this);
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
    m_placeholder = new QLabel(QStringLiteral("No file open. Open a log file to begin."), this);
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
                                          QStringLiteral("Filters"));
    connect(this, &MainWindow::activeDocumentChanged, m_filterPane, &FilterPane::setDocument);
    connect(m_filterPane, &FilterPane::filtersChanged, this, &MainWindow::applyActiveFilters);

    m_highlighterPane = new HighlighterPane(this);
    QDockWidget *highlightDock = addPaneDock(m_highlighterPane,
                                             QStringLiteral("highlightersDock"),
                                             QStringLiteral("Highlighters"));
    connect(this, &MainWindow::activeDocumentChanged, m_highlighterPane, &HighlighterPane::setDocument);
    connect(m_highlighterPane, &HighlighterPane::highlightersChanged,
            this, &MainWindow::applyActiveHighlighters);

    m_presetPane = new PresetPane(m_filterPane, m_highlighterPane, this);
    QDockWidget *presetDock = addPaneDock(m_presetPane, QStringLiteral("presetsDock"),
                                          QStringLiteral("Presets"));

    // Run selection pane (§3a): a run-start regexp splits the file into runs and the
    // user views one at a time. Binds to the active document by signal like the rest.
    m_runPane = new RunPane(this);
    QDockWidget *runDock = addPaneDock(m_runPane, QStringLiteral("runsDock"),
                                       QStringLiteral("Runs"));
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
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction *openAction = fileMenu->addAction(QStringLiteral("&Open..."));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::chooseFileToOpen);

    // Remote logs (M11, SPEC.md §3). Both entries are present whether or not SSH was
    // compiled in — a disabled item with a tooltip explains the situation, where a
    // missing one would just look like the feature does not exist.
    m_openRemoteAction = fileMenu->addAction(QStringLiteral("Open &Remote..."));
    m_openRemoteAction->setObjectName(QStringLiteral("openRemoteAction")); // findChild, for tests
    m_openRemoteAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    connect(m_openRemoteAction, &QAction::triggered, this, &MainWindow::chooseRemoteToOpen);

    m_recentMenu = fileMenu->addMenu(QStringLiteral("Open &Recent"));
    refreshRecentFilesMenu();

    m_remoteHostsMenu = fileMenu->addMenu(QStringLiteral("Remote &Hosts"));
    m_remoteHostsMenu->setObjectName(QStringLiteral("remoteHostsMenu"));
    refreshRemoteHostsMenu();

#if !defined(LOFTAIL_HAVE_SSH)
    const QString noSsh = QStringLiteral(
        "This copy of loftail was built without SSH support, so remote logs cannot "
        "be opened. Rebuild with libssh2 available to enable it.");
    m_openRemoteAction->setEnabled(false);
    m_openRemoteAction->setToolTip(noSsh);
    m_remoteHostsMenu->setEnabled(false);
    m_remoteHostsMenu->setToolTip(noSsh);
#endif

    fileMenu->addSeparator();
    m_closeTabAction = fileMenu->addAction(QStringLiteral("&Close Tab"));
    m_closeTabAction->setObjectName(QStringLiteral("closeTabAction")); // findChild, for tests
    m_closeTabAction->setShortcut(QKeySequence::Close); // Ctrl+W
    m_closeTabAction->setEnabled(false);
    connect(m_closeTabAction, &QAction::triggered, this, &MainWindow::closeActiveView);

    m_closeAllAction = fileMenu->addAction(QStringLiteral("Close &All"));
    m_closeAllAction->setObjectName(QStringLiteral("closeAllAction")); // findChild, for tests
    m_closeAllAction->setEnabled(false);
    connect(m_closeAllAction, &QAction::triggered, this, &MainWindow::closeAllDocuments);

    // Ask a spooled log's fetcher to try again NOW rather than at its next backoff.
    // The one case it is required for, rather than merely convenient: a reconnect that
    // needs a password cannot prompt from the fetcher thread, so it stops trying and
    // says so — and this is how the user says "ask me again" (§6.5).
    m_reconnectAction = fileMenu->addAction(QStringLiteral("&Reconnect"));
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
    m_formatAction = fileMenu->addAction(QStringLiteral("&Log Format..."));
    m_formatAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    m_formatAction->setEnabled(false);
    connect(m_formatAction, &QAction::triggered, this, &MainWindow::showFormatDialog);

    fileMenu->addSeparator();
    m_cancelAction = fileMenu->addAction(QStringLiteral("&Cancel Indexing"));
    m_cancelAction->setEnabled(false);
    connect(m_cancelAction, &QAction::triggered, this, [this]() {
        if (DocumentContext *ctx = activeContext(); ctx && ctx->controller)
            ctx->controller->cancel();
    });

    fileMenu->addSeparator();
    QAction *quitAction = fileMenu->addAction(QStringLiteral("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    QMenu *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    m_copyAction = editMenu->addAction(QStringLiteral("&Copy"));
    m_copyAction->setShortcut(QKeySequence::Copy);
    m_copyAction->setEnabled(false);
    connect(m_copyAction, &QAction::triggered, this, [this]() {
        if (LogView *v = activeLogView())
            v->copySelectionRaw();
    });
    m_copyColumnsAction = editMenu->addAction(QStringLiteral("Copy as &Columns"));
    m_copyColumnsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    m_copyColumnsAction->setEnabled(false);
    connect(m_copyColumnsAction, &QAction::triggered, this, [this]() {
        if (LogView *v = activeLogView())
            v->copySelectionAsColumns();
    });

    // Find / Find Next / Find Previous (SPEC.md §5). Find opens the bar; F3 /
    // Shift+F3 navigate the current query over the visible rows.
    editMenu->addSeparator();
    QAction *findAction = editMenu->addAction(QStringLiteral("&Find..."));
    findAction->setShortcut(QKeySequence::Find);
    connect(findAction, &QAction::triggered, this, [this]() {
        if (m_activeView)
            m_activeView->activateFind();
    });
    QAction *findNextAction = editMenu->addAction(QStringLiteral("Find &Next"));
    findNextAction->setShortcut(QKeySequence::FindNext); // F3
    connect(findNextAction, &QAction::triggered, this, [this]() { runFind(true, false); });
    QAction *findPrevAction = editMenu->addAction(QStringLiteral("Find Pre&vious"));
    findPrevAction->setShortcut(QKeySequence::FindPrevious); // Shift+F3
    connect(findPrevAction, &QAction::triggered, this, [this]() { runFind(false, false); });

    QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    QMenu *wrapMenu = viewMenu->addMenu(QStringLiteral("Line &Wrap"));
    auto *wrapGroup = new QActionGroup(this);
    QAction *wrapOff = wrapMenu->addAction(QStringLiteral("&Off"));
    QAction *wrapSel = wrapMenu->addAction(QStringLiteral("&Selected Record Only"));
    QAction *wrapAll = wrapMenu->addAction(QStringLiteral("&Always On"));
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
    m_followAction = viewMenu->addAction(QStringLiteral("&Follow Tail"));
    m_followAction->setCheckable(true);
    m_followAction->setChecked(true);
    m_followAction->setEnabled(false);
    m_followAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_End));
    connect(m_followAction, &QAction::triggered, this, [this]() {
        if (LogView *v = activeLogView())
            v->followTail();
    });

    // Panes are closable docks, so without this a closed pane could not be brought
    // back (SPEC.md §8). Qt's own toggleViewAction does the work.
    viewMenu->addSeparator();
    QMenu *panesMenu = viewMenu->addMenu(QStringLiteral("&Panes"));
    for (QDockWidget *dock : std::as_const(m_paneDocks))
        panesMenu->addAction(dock->toggleViewAction());

    // Window: move between the open files (SPEC.md §3). The list of open views is
    // rebuilt each time the menu opens, since tabs come and go.
    m_windowMenu = menuBar()->addMenu(QStringLiteral("&Window"));
    // Parented to the window, not the menu, so refreshWindowMenu()'s clear() does not
    // delete it out from under updateActionStates().
    m_newViewAction = new QAction(QStringLiteral("&New View"), this);
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

    QAction *next = m_windowMenu->addAction(QStringLiteral("&Next Tab"));
    next->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Tab));
    next->setEnabled(m_views.size() > 1);
    connect(next, &QAction::triggered, this, [this]() { cycleView(1); });

    QAction *prev = m_windowMenu->addAction(QStringLiteral("&Previous Tab"));
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
    // `obj` is mid-destruction: compare it, never dereference it.
    auto *view = static_cast<DocumentView *>(obj);
    m_views.removeAll(view);
    for (auto &ctx : m_contexts)
        ctx->views.removeAll(view);
    if (m_activeView == view)
        m_activeView = nullptr;

    // A file with no views left is closed: its index, workers and model go with it.
    std::erase_if(m_contexts, [](const auto &ctx) { return ctx->views.isEmpty(); });

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
                       ? QStringLiteral("loftail — %1").arg(logSourceDisplayName(ctx->doc->path()))
                       : QStringLiteral("loftail"));
}

void MainWindow::chooseFileToOpen()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Log File"), QString(),
        // The archive filter is offered whether or not libarchive is compiled in, so
        // the two builds' dialogs look alike: a file that simply vanished from the list
        // would read as "loftail cannot see this", where trying it explains itself.
        QStringLiteral("Log files (*.log *.txt);;"
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
        QAction *none = m_remoteHostsMenu->addAction(QStringLiteral("(none saved)"));
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
        QMenu *hostMenu = m_remoteHostsMenu->addMenu(host.displayName());
        for (const QString &path : host.paths) {
            QAction *action = hostMenu->addAction(path);
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

void MainWindow::openFile(const QString &rawPath, const QString &pattern)
{
    // Normalize a remote URL to its one spelling FIRST, before it becomes a Document
    // path (RemoteLocation.h). Everything downstream compares these strings —
    // viewOfPath(), the recent-files dedupe, the format-cache key, the session — so
    // two spellings of one remote file would otherwise open two tabs on it and
    // remember its format twice. A local path passes through untouched.
    const QString path = normalizeLogPath(rawPath);

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
            m_statusLabel->setText(QStringLiteral("Cannot open %1: %2")
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
    // saved format and no prompt. A never-seen file gets the supplied (or default)
    // pattern, and the dialog is offered when that pattern does not match.
    FormatSettings settings;
    QSettings store;
    bool cached = false;
    if (auto loaded = FormatCache::load(store, path)) {
        settings = *loaded;
        cached = true;
    } else {
        settings.pattern = pattern.isEmpty() ? m_defaultPattern : pattern;
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
        // Opening a remote log connects, which blocks this thread for as long as the
        // handshake and authentication take (bounded by the SSH timeout). Say so with
        // the cursor — the prompts that may appear during it are modal dialogs, which
        // is also what stops a second open from starting on top of this one.
        const bool remote = RemoteLocation::isRemote(path);
        if (remote)
            QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        const bool prepared =
            doc->prepare(path, provider, settings.encoding, settings.sourceZone.toZone());
        if (remote)
            QGuiApplication::restoreOverrideCursor();
        if (!prepared) {
            m_statusLabel->setText(QStringLiteral("Cannot open %1: %2")
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
    if (doc->isWaiting()) {
        promptIfNoMatch = false;
        persist = false;
    }

    if (promptIfNoMatch) {
        const qint64 sampleLen = qMin<qint64>(64 * 1024, doc->source()->size());
        const QByteArray sample = sampleLen > 0
            ? doc->source()->bytes(0, sampleLen).toByteArray() : QByteArray();
        Decoder decoder = Decoder::detect(sample, settings.encoding);
        const PreviewResult pv = FormatPreview::build(doc->format(), sample, decoder);

        if (pv.matchedCount > 0) {
            persist = true; // the default matched — remember it
        } else {
            // The default did not match and no format is cached (M3 unchanged: a
            // cached file never reaches here). Autodetect (M8, ARCHITECTURE.md §9)
            // and PRE-FILL the dialog with the detected pattern for confirmation —
            // never applied silently. A no-detection result leaves the dialog
            // seeded with the fallback default, i.e. it opens as it does today.
            FormatSettings seed = settings;
            DetectingFormatProvider detector(settings.encoding);
            detector.formatFor(QByteArrayView(sample.constData(), sample.size()));
            if (detector.detected())
                seed.pattern = detector.detectedPattern();

            LogFormatDialog dlg(logSourceDisplayName(path), sample, seed, this);
            if (dlg.exec() == QDialog::Accepted) {
                settings = dlg.settings();
                ManualFormatProvider chosen(settings.pattern);
                if (!doc->prepare(path, chosen, settings.encoding, settings.sourceZone.toZone())) {
                    m_statusLabel->setText(QStringLiteral("Cannot open %1: %2")
                                               .arg(logSourceDisplayName(path), doc->lastError()));
                    return false;
                }
                doc->setTimeDisplay(settings.timeDisplay);
                persist = true;
            } else {
                // Cancelled. The only format we have is one the user just refused
                // to confirm, and opening with it would show a wall of unparsed
                // plain text — so abort the open instead (SPEC.md §4). Whatever
                // was already open stays open, untouched.
                m_statusLabel->setText(QStringLiteral("Open cancelled: %1")
                                           .arg(logSourceDisplayName(path)));
                return false;
            }
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

    ctx->model = new LogModel(doc);
    ctx->model->setDarkTheme(palette().base().color().lightness()
                             < palette().text().color().lightness());

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
        // The dialog does not edit the run-start axis; carry it through unchanged so
        // accepting a format change never clears the run-start pattern (§3a).
        FormatSettings s = dlg.settings();
        s.runStartPattern = ctx->settings.runStartPattern;
        s.runStartIsRegex = ctx->settings.runStartIsRegex;
        s.runStartCaseSensitive = ctx->settings.runStartCaseSensitive;
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
        base = QStringLiteral("%1 — indexing %2%").arg(name).arg(ctx->progressPercent);
    else if (ctx->doc->isWaiting())
        // A tab with no records in it, so that a glance at the tab bar tells a log
        // that is empty from one that is not there (SPEC.md §3). The tooltip carries
        // the sentence; the title only has room for the mark.
        base = QStringLiteral("◦ %1").arg(name);
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
                                   + QStringLiteral("  (indexing cancelled)"));
        }
        return; // a cancelled scan is not watched — the user chose to stop reading it
    }

    // Activate the always-watched model (SPEC.md §3, M6): from here the file
    // auto-updates as it grows. The initial scan captured the size at open, so the
    // controller's first check catches up anything appended while it ran.
    {
        ctx->live = new LiveController(doc, ctx->model);
        connect(ctx->live, &LiveController::ingested, this, [this, ctx](qint64) {
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
    const bool ok = doc->resume(provider);
    if (ok) {
        // The intern tables were built from scratch, exactly as after a rotation.
        doc->resolveHighlighters();
        if (doc->filters().anyActive() || doc->viewRestricted())
            doc->applyFilters();
    }
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
        // Whether the remembered pattern actually fits, decided the same way an
        // ordinary open decides it (openWithSettings) so the two cannot disagree.
        const qint64 sampleLen = qMin<qint64>(64 * 1024, doc->source()->size());
        const QByteArray sample =
            sampleLen > 0 ? doc->source()->bytes(0, sampleLen).toByteArray() : QByteArray();
        Decoder decoder = Decoder::detect(sample, ctx->settings.encoding);
        const PreviewResult pv = FormatPreview::build(doc->format(), sample, decoder);

        if (pv.matchedCount > 0) {
            // It fits, and it has now been checked against real lines rather than
            // assumed — which is exactly the point at which it becomes worth
            // remembering. The waiting open deliberately persisted nothing.
            persistFormat(doc->path(), ctx->settings);
            ctx->formatNotice.clear();
        } else {
            // It does not, and there is NO DIALOG here on purpose: this runs from a
            // watch tick for a tab that may not even be on screen, which is the
            // "behind the user's back" case openFile() takes such care to avoid. The
            // log stays readable as plain text and the status bar says where to fix
            // it; nothing is persisted, so reopening still offers the dialog properly.
            ctx->formatNotice = QStringLiteral("format not recognised — Log ▸ Format…");
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
    // Highlighting recolors visible rows in place (SPEC.md §7): no rows are added or
    // removed, so a viewport repaint is enough — no model reset, unlike filtering.
    ctx->doc->resolveHighlighters();
    for (DocumentView *v : std::as_const(ctx->views))
        v->logView()->viewport()->update();
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
        for (DocumentView *v : std::as_const(ctx->views))
            v->logView()->viewport()->update();
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        updateModelTheme();
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
        findBar->setStatus(QStringLiteral("bad regex"));
        return;
    }

    const int count = model->rowCount();
    if (count == 0) {
        findBar->setStatus(QStringLiteral("no records"));
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
        findBar->setStatus(QStringLiteral("no match"));
        return;
    }
    logView->setCurrentRecord(hit);
    findBar->setStatus(QString()); // keep focus in the bar for repeated Enter/F3
}

void MainWindow::updateStatus()
{
    Document *doc = activeDocument();
    if (!doc) {
        m_statusLabel->setText(QStringLiteral("No file open"));
        return;
    }
    if (doc->isWaiting()) {
        // A record count for a log that is not there would be an honest zero and a
        // useless one. Say what is actually going on instead (SPEC.md §3).
        QString text = doc->waitReason();
        if (const DocumentContext *ctx = activeContext(); ctx && !ctx->sourceStatus.isEmpty())
            text += QStringLiteral("  |  ") + ctx->sourceStatus;
        m_statusLabel->setText(text);
        return;
    }

    const int total = doc->index().records.size();
    // Filtered/total counts (SPEC.md §5, §6): show the shown-vs-total pair only
    // when a filter narrows the view, otherwise a plain record count.
    QString text;
    if (doc->filters().anyActive()) {
        text = QStringLiteral("%1  |  %2 of %3 records shown")
                   .arg(logSourceDisplayName(doc->path()))
                   .arg(doc->filtered().recordCount())
                   .arg(total);
    } else {
        text = QStringLiteral("%1  |  %2 records")
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
    m_timeDisplayMenu = new QMenu(QStringLiteral("&Timestamp Format"), this);
    m_timeDisplayMenu->setObjectName(QStringLiteral("timeDisplayMenu"));
    auto *group = new QActionGroup(this);

    struct Item
    {
        TimeDisplay mode;
        const char *text;
        const char *name;
    };
    static constexpr Item kItems[] = {
        { TimeDisplay::AsWritten,    "As &Written in the File", "timeDisplayAsWrittenAction" },
        { TimeDisplay::LocalTime,    "&Local Time",             "timeDisplayLocalAction" },
        { TimeDisplay::Utc,          "&UTC",                    "timeDisplayUtcAction" },
        { TimeDisplay::EpochSeconds, "&Seconds",                "timeDisplaySecondsAction" },
        { TimeDisplay::RunSeconds,   "Seconds from &Run Start", "timeDisplayRunSecondsAction" },
    };
    for (const Item &item : kItems) {
        QAction *a = m_timeDisplayMenu->addAction(QLatin1String(item.text));
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

    menu.addSection(QStringLiteral("Columns"));
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
        QAction *none = m_recentMenu->addAction(QStringLiteral("(none)"));
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

    if (m_sshPrompter)
        m_sshPrompter->endBulkRestore();

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
                                            ? QStringLiteral("These logs could not be reopened:")
                                            : QStringLiteral("These files could not be reopened:"),
                                        shown.join(u'\n')));
        m_statusLabel->setText(
            QStringLiteral("%1 file(s) from the last session unavailable").arg(missing.size()));
    }
    updateEmptyState();
    if (m_contexts.empty())
        return;

    // Activate the saved view, which binds the panes to its file, then start every
    // scan. Indexing goes last so worker batches never race the layout settling.
    showView(toActivate ? toActivate : m_views.first());

    for (auto &ctx : m_contexts) {
        if (ctx->controller)
            ctx->controller->start();
    }
}

} // namespace loftail
