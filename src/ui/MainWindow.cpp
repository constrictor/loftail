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
#include "PresetPane.h"
#include "RunPane.h"
#include "SessionStore.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QProgressBar>
#include <QScrollBar>
#include <QSettings>
#include <QStatusBar>
#include <QVBoxLayout>

#include <utility>

namespace loftail {

namespace {
// The default log4cplus layout used until the M3 Log Format dialog exists. A file
// that does not match still opens with unparsed lines as plain text (SPEC.md §4).
constexpr auto kDefaultPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
constexpr int  kMaxRecentFiles = 10;
constexpr auto kRecentFilesKey = "recentFiles";
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_defaultPattern(QString::fromLatin1(kDefaultPattern))
{
    setWindowTitle(QStringLiteral("loftail"));
    resize(1100, 720);
    setAcceptDrops(true);

    // The whole window is a dock layout: open files and the side panes are all dock
    // widgets, so either can be dragged into a tab group, split against the other, or
    // pulled out into a floating window (SPEC.md §8).
    //
    // Two Qt constraints govern this and are easy to break:
    //   * dock options must be set BEFORE any dock widget is added;
    //   * GroupedDragging misbehaves for docks that restrict their allowed areas, so
    //     no dock here may call setAllowedAreas().
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks
                   | QMainWindow::AllowTabbedDocks | QMainWindow::GroupedDragging);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setMaximumWidth(200);
    m_progressBar->setVisible(false);
    m_statusLabel = new QLabel(QStringLiteral("No file open"), this);
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_progressBar);

    // The empty state. Open files live in dock widgets, so the central widget holds
    // nothing but this notice; hiding it collapses the centre to zero and lets the
    // document docks fill the window.
    m_placeholder = new QLabel(QStringLiteral("No file open. Open a log file to begin."), this);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    setCentralWidget(m_placeholder);

    // Three side panes (SPEC.md §8): filters, highlighters, presets. Each binds to
    // the active document by signal (invariant #7 / §12.3), never a fixed Document.
    m_filterPane = new FilterPane(this);
    auto *filterDock = new QDockWidget(QStringLiteral("Filters"), this);
    filterDock->setObjectName(QStringLiteral("filtersDock"));
    filterDock->setWidget(m_filterPane);
    addDockWidget(Qt::RightDockWidgetArea, filterDock);
    m_paneDocks.append(filterDock);
    connect(this, &MainWindow::activeDocumentChanged, m_filterPane, &FilterPane::setDocument);
    connect(m_filterPane, &FilterPane::filtersChanged, this, &MainWindow::applyActiveFilters);

    m_highlighterPane = new HighlighterPane(this);
    auto *highlightDock = new QDockWidget(QStringLiteral("Highlighters"), this);
    highlightDock->setObjectName(QStringLiteral("highlightersDock"));
    highlightDock->setWidget(m_highlighterPane);
    addDockWidget(Qt::RightDockWidgetArea, highlightDock);
    m_paneDocks.append(highlightDock);
    connect(this, &MainWindow::activeDocumentChanged, m_highlighterPane, &HighlighterPane::setDocument);
    connect(m_highlighterPane, &HighlighterPane::highlightersChanged,
            this, &MainWindow::applyActiveHighlighters);

    m_presetPane = new PresetPane(m_filterPane, m_highlighterPane, this);
    auto *presetDock = new QDockWidget(QStringLiteral("Presets"), this);
    presetDock->setObjectName(QStringLiteral("presetsDock"));
    presetDock->setWidget(m_presetPane);
    addDockWidget(Qt::RightDockWidgetArea, presetDock);
    m_paneDocks.append(presetDock);

    // Run selection pane (§3a): a run-start regexp splits the file into runs and the
    // user views one at a time. Binds to the active document by signal like the rest.
    m_runPane = new RunPane(this);
    auto *runDock = new QDockWidget(QStringLiteral("Runs"), this);
    runDock->setObjectName(QStringLiteral("runsDock"));
    runDock->setWidget(m_runPane);
    addDockWidget(Qt::RightDockWidgetArea, runDock);
    m_paneDocks.append(runDock);
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

    // Which document is active follows the keyboard focus (invariant #7): the panes
    // rebind to whichever file the user is reading.
    connect(qApp, &QApplication::focusChanged, this, &MainWindow::onFocusChanged);

    // Restore the previous working state last, once every pane dock exists with its
    // object name (restoreState keys off those) — SPEC.md §10.
    restoreSession();
}

MainWindow::~MainWindow()
{
    // Destroying the views moves the keyboard focus, which would call back into a
    // half-destroyed MainWindow. QObject's own disconnect happens in ~QObject, i.e.
    // AFTER this body, so the application-wide connection must be dropped by hand.
    disconnect(qApp, nullptr, this, nullptr);
    closeAllDocuments();
}

void MainWindow::buildMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction *openAction = fileMenu->addAction(QStringLiteral("&Open..."));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::chooseFileToOpen);

    m_recentMenu = fileMenu->addMenu(QStringLiteral("Open &Recent"));
    refreshRecentFilesMenu();

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
        QDockWidget *dock = dockOf(view);
        QAction *a = m_windowMenu->addAction(dock ? dock->windowTitle() : QString());
        a->setCheckable(true);
        a->setChecked(view == m_activeView);
        connect(a, &QAction::triggered, this, [this, view]() {
            if (QDockWidget *d = dockOf(view))
                d->raise();
            setActiveView(view);
            view->logView()->setFocus();
        });
    }
}

void MainWindow::cycleView(int delta)
{
    if (m_views.size() < 2)
        return;
    const int current = m_views.indexOf(m_activeView);
    const int size = m_views.size();
    const int next = ((current < 0 ? 0 : current) + delta % size + size) % size;
    DocumentView *view = m_views.at(next);
    if (QDockWidget *dock = dockOf(view))
        dock->raise();
    setActiveView(view);
    view->logView()->setFocus();
}

// --- Document docks --------------------------------------------------------

QDockWidget *MainWindow::dockOf(DocumentView *view)
{
    return view ? qobject_cast<QDockWidget *>(view->parentWidget()) : nullptr;
}

QDockWidget *MainWindow::addViewDock(DocumentView *view, const QString &dockName)
{
    view->setDockName(dockName);

    auto *dock = new QDockWidget(this);
    dock->setObjectName(dockName);
    dock->setWindowTitle(QFileInfo(view->context()->doc->path()).fileName());
    dock->setToolTip(view->context()->doc->path());
    // The close button destroys the dock and, with it, the view. Deleting on close
    // rather than from inside the close event is what keeps a dock being dragged or
    // floated from being deleted underneath Qt.
    dock->setAttribute(Qt::WA_DeleteOnClose);
    // Deliberately NO setAllowedAreas(): a dock that restricts its areas breaks
    // GroupedDragging, and an open file must be draggable anywhere.
    dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable
                      | QDockWidget::DockWidgetFloatable);
    dock->setWidget(view);

    // If the saved layout has a slot under this object name — a tab group, a split,
    // a floating window — the dock goes straight back into it. restoreState() leaves
    // such slots as placeholders for docks that do not exist yet, and this is the
    // documented way to claim one. It must be tried BEFORE any addDockWidget(): a
    // dock already placed in the layout no longer matches its placeholder.
    //
    // It returns false for a dock the saved layout never knew (a newly opened file,
    // a brand-new view, or a first run), and then the default placement applies:
    // documents share the left area, the panes sit on the right, and a further file
    // joins the existing ones as a tab, to be dragged out into a split if wanted.
    if (!restoreDockWidget(dock)) {
        addDockWidget(Qt::LeftDockWidgetArea, dock);
        if (!m_views.isEmpty()) {
            if (QDockWidget *previous = dockOf(m_views.last()))
                tabifyDockWidget(previous, dock);
        }
    }
    m_views.append(view);

    // Raising a tab does not necessarily move focus, so track visibility too.
    connect(dock, &QDockWidget::visibilityChanged, this, [this, view](bool visible) {
        if (visible)
            setActiveView(view);
    });
    connect(view, &QObject::destroyed, this, &MainWindow::onViewDestroyed);

    // First run only: the panes' size hints would otherwise claim about half the
    // window. A restored session brings its own proportions.
    if (!m_layoutRestored && m_contexts.size() == 1 && !m_paneDocks.isEmpty()) {
        m_layoutRestored = true;
        resizeDocks({dock, m_paneDocks.first()}, {width() * 2 / 3, width() / 3}, Qt::Horizontal);
    }
    return dock;
}

void MainWindow::updateEmptyState()
{
    m_placeholder->setVisible(m_views.isEmpty());
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

    updateEmptyState();
    if (m_activeView)
        return;

    if (!m_views.isEmpty()) {
        setActiveView(m_views.last());
    } else {
        // Nothing left open: unbind the panes (invariant #7) and disable the
        // per-file actions, exactly as closing the only file used to do.
        emit activeDocumentChanged(nullptr);
        updateActionStates();
        updateStatus();
    }
}

void MainWindow::closeActiveView()
{
    if (QDockWidget *dock = dockOf(m_activeView))
        dock->close(); // WA_DeleteOnClose -> onViewDestroyed
}

void MainWindow::onFocusChanged(QWidget *, QWidget *now)
{
    for (QWidget *w = now; w; w = w->parentWidget()) {
        if (auto *view = qobject_cast<DocumentView *>(w)) {
            setActiveView(view);
            return;
        }
    }
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
    if (m_formatAction)
        m_formatAction->setEnabled(hasFile);
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
    if (m_cancelAction)
        m_cancelAction->setEnabled(hasFile && ctx->indexing);
    if (m_progressBar) {
        m_progressBar->setVisible(hasFile && ctx->indexing);
        if (hasFile && ctx->indexing)
            m_progressBar->setValue(ctx->progressPercent);
    }

    setWindowTitle(hasFile
                       ? QStringLiteral("loftail — %1").arg(QFileInfo(ctx->doc->path()).fileName())
                       : QStringLiteral("loftail"));
}

void MainWindow::chooseFileToOpen()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Log File"), QString(),
        QStringLiteral("Log files (*.log *.txt);;All files (*)"));
    if (!path.isEmpty())
        openFile(path);
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
    for (DocumentView *view : views) {
        disconnect(view, &QObject::destroyed, this, &MainWindow::onViewDestroyed);
        delete dockOf(view); // the dock owns the view
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

void MainWindow::openFile(const QString &path, const QString &pattern)
{
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
    if (!doc->prepare(path, provider, settings.encoding,
                      settings.sourceZone.toZone(), settings.displayZone.toZone())) {
        m_statusLabel->setText(QStringLiteral("Cannot open %1: %2")
                                   .arg(QFileInfo(path).fileName(), doc->lastError()));
        return false;
    }

    // Decide whether to remember this format on close of the flow. A cached open, a
    // dialog the user accepted, or a default that actually matched are all worth
    // persisting; a non-matching default the user declined is not (so reopen
    // re-prompts rather than silently showing plain text).
    bool persist = !promptIfNoMatch;

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

            LogFormatDialog dlg(QFileInfo(path).fileName(), sample, seed, this);
            if (dlg.exec() == QDialog::Accepted) {
                settings = dlg.settings();
                ManualFormatProvider chosen(settings.pattern);
                if (!doc->prepare(path, chosen, settings.encoding,
                                  settings.sourceZone.toZone(), settings.displayZone.toZone())) {
                    m_statusLabel->setText(QStringLiteral("Cannot open %1: %2")
                                               .arg(QFileInfo(path).fileName(), doc->lastError()));
                    return false;
                }
                persist = true;
            } else {
                // Cancelled. The only format we have is one the user just refused
                // to confirm, and opening with it would show a wall of unparsed
                // plain text — so abort the open instead (SPEC.md §4). Whatever
                // was already open stays open, untouched.
                m_statusLabel->setText(QStringLiteral("Open cancelled: %1")
                                           .arg(QFileInfo(path).fileName()));
                return false;
            }
        }
    }

    // An open ADDS a file: several logs are open at once, each in its own tab
    // (SPEC.md §3). Reopening a file already open just raises its view.
    if (DocumentView *existing = viewOfPath(path)) {
        setActiveView(existing);
        if (QDockWidget *dock = dockOf(existing))
            dock->raise();
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

DocumentView *MainWindow::createView(DocumentContext *ctx, const QString &dockName)
{
    auto *view = new DocumentView(ctx);
    ctx->views.append(view);
    connect(view, &DocumentView::findRequested, this, &MainWindow::runFind);

    LogView *logView = view->logView();
    logView->setWrapMode(m_wrapMode);
    // Reflect follow state in the View menu (M6): the checkbox tracks the ACTIVE
    // view, and the overlay button/scroll gestures keep them in sync.
    connect(logView, &LogView::followingChanged, this, [this, view](bool following) {
        if (m_followAction && m_activeView == view)
            m_followAction->setChecked(following);
    });
    logView->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(logView->header(), &QWidget::customContextMenuRequested,
            this, &MainWindow::showColumnMenu);

    addViewDock(view, dockName);
    updateEmptyState();
    updateDockTitles(ctx); // numbers the tabs when a file has several views
    return view;
}

void MainWindow::showView(DocumentView *view)
{
    // tabifyDockWidget leaves the PREVIOUS tab raised, so without this the status bar
    // and panes would describe a tab that is not on top.
    if (QDockWidget *dock = dockOf(view)) {
        dock->show();
        dock->raise();
    }
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
    DocumentView *view = createView(ctx, DocumentView::makeDockName());
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
    if (!doc->prepare(d.path, provider, d.format.encoding, d.format.sourceZone.toZone(),
                      d.format.displayZone.toZone())) {
        return nullptr;
    }

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

    // The model and the controller only. Views are created by the caller, which is
    // what lets session restore give each one its SAVED dock name; and indexing is
    // not started, so every dock exists before any worker runs.
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
    DocumentView *view = createView(ctx, DocumentView::makeDockName());

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

    LogFormatDialog dlg(QFileInfo(doc->path()).fileName(), sample, ctx->settings, this);
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

    // A format change re-indexes (or reparses) but does not carry a persisted run
    // selection; the newest run is the default afterwards.
    ctx->pendingRunRestore.reset();

    // Copy the path: a rescan tears the Document down, so `doc` and `ctx` must not
    // be read after openWithSettings() runs.
    const QString path = doc->path();
    const FormatSettings old = ctx->settings;
    const bool patternChanged  = newSettings.pattern != old.pattern;
    const bool encodingChanged = newSettings.encoding != old.encoding;
    const bool sourceChanged   = newSettings.sourceZone != old.sourceZone;
    const bool displayChanged  = newSettings.displayZone != old.displayZone;

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

    // Display-zone change (or a source change while display "follows source") is a
    // free reformat — just repaint (§5.1).
    if (sourceChanged || displayChanged) {
        const QTimeZone display = newSettings.displayZone.kind == ZoneChoice::Kind::Default
            ? doc->sourceZone() // "as written" == the (possibly updated) source zone
            : newSettings.displayZone.toZone();
        doc->setDisplayZone(display);
        for (DocumentView *v : std::as_const(ctx->views))
            v->logView()->viewport()->update();
    }

    updateStatus();
}

void MainWindow::persistFormat(const QString &path, const FormatSettings &s)
{
    QSettings store;
    FormatCache::save(store, path, s);
}

void MainWindow::updateDockTitles(DocumentContext *ctx)
{
    // A background file's scan has no claim on the status bar, so its progress shows
    // in its own tab title instead.
    const QString name = QFileInfo(ctx->doc->path()).fileName();
    const QString base = ctx->indexing
        ? QStringLiteral("%1 — indexing %2%").arg(name).arg(ctx->progressPercent)
        : name;
    // Several views onto one file are numbered, so two identically-named tabs are
    // still tellable apart.
    const bool numbered = ctx->views.size() > 1;
    for (int i = 0; i < ctx->views.size(); ++i) {
        if (QDockWidget *dock = dockOf(ctx->views.at(i)))
            dock->setWindowTitle(numbered ? QStringLiteral("%1 [%2]").arg(base).arg(i + 1) : base);
    }
}

void MainWindow::onIndexProgress(DocumentContext *ctx, qint64 done, qint64 total)
{
    if (total > 0)
        ctx->progressPercent = int((done * 100) / total);
    updateDockTitles(ctx);
    if (ctx == activeContext()) {
        m_progressBar->setValue(ctx->progressPercent);
        updateStatus();
    }
}

void MainWindow::onIndexFinished(DocumentContext *ctx, bool cancelled)
{
    Document *doc = ctx->doc.get();
    ctx->indexing = false;
    updateDockTitles(ctx);
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
    const int total = doc->index().records.size();
    // Filtered/total counts (SPEC.md §5, §6): show the shown-vs-total pair only
    // when a filter narrows the view, otherwise a plain record count.
    if (doc->filters().anyActive()) {
        m_statusLabel->setText(QStringLiteral("%1  |  %2 of %3 records shown")
                                   .arg(QFileInfo(doc->path()).fileName())
                                   .arg(doc->filtered().recordCount())
                                   .arg(total));
    } else {
        m_statusLabel->setText(QStringLiteral("%1  |  %2 records")
                                   .arg(QFileInfo(doc->path()).fileName())
                                   .arg(total));
    }
}

void MainWindow::showColumnMenu(const QPoint &pos)
{
    // The menu belongs to the header that asked for it, which may be any view's.
    auto *header = qobject_cast<QHeaderView *>(sender());
    LogModel *model = activeModel();
    if (!header || !model)
        return;
    QMenu menu(this);
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
    // Every dropped file opens, each in its own tab (SPEC.md §3).
    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl &url : urls) {
        if (url.isLocalFile())
            openFile(url.toLocalFile());
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
    // The whole dock layout: tab groups, splits and floating windows, for the panes
    // AND for every open file (SPEC.md §8, §10). Must be taken while the docks still
    // exist, i.e. before any teardown.
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

    for (DocumentView *view : std::as_const(m_views)) {
        SessionView v;
        v.documentIndex = documentIndex.value(view->context(), 0);
        v.dockName = view->dockName();
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

    // The dock layout goes back FIRST, while only the pane docks exist. Docks named
    // in the saved state but not yet created become placeholders, and each document
    // dock claims its own slot below via restoreDockWidget() — the documented way to
    // place a dock created after restoreState().
    if (!session.windowState.isEmpty()) {
        restoreState(session.windowState);
        m_layoutRestored = true;
    }

    // Rebuild the files and their views in the saved order. Opening is split: the
    // synchronous half (open the source, compile the format, build the model and the
    // dock) runs for everything first, so every dock exists before any of them starts
    // indexing on a worker thread.
    QStringList missing;
    QHash<int, DocumentContext *> byDocument;
    for (const SessionView &sv : session.views) {
        const SessionDocument *d = session.documentFor(sv);
        if (!d || d->path.isEmpty())
            continue;

        DocumentContext *ctx = byDocument.value(sv.documentIndex, nullptr);
        if (!ctx) {
            // A missing/unreadable file must not error every launch (SPEC.md §10):
            // skip it with an inline notice, no dialog. Its dock never appears, which
            // restoreState() tolerates.
            const QFileInfo info(d->path);
            if (!info.exists() || !info.isReadable()) {
                if (!missing.contains(d->path))
                    missing.append(d->path);
                continue;
            }
            ctx = prepareContext(*d);
            if (!ctx)
                continue;
            byDocument.insert(sv.documentIndex, ctx);
        }

        // Every saved view is created here, under its OWN saved dock name — including
        // the file's first, which is why prepareContext() makes none.
        DocumentView *view = createView(ctx, sv.dockName);
        // createView() has already put the dock back in its saved slot, keying off
        // the dock name set above (or fallen back to the default placement).
        view->logView()->setWrapMode(static_cast<LogView::WrapMode>(sv.wrapMode));
        if (!sv.columnState.isEmpty())
            view->logView()->restoreColumnState(sv.columnState);
    }

    if (!missing.isEmpty()) {
        m_placeholder->setText(
            QStringLiteral("These files are no longer available:\n%1").arg(missing.join(u'\n')));
        m_statusLabel->setText(
            QStringLiteral("%1 file(s) from the last session unavailable").arg(missing.size()));
    }
    updateEmptyState();
    if (m_contexts.empty())
        return;

    // Activate the saved view, which binds the panes to its file, then start every
    // scan. Indexing goes last so worker batches never race the layout settling.
    const SessionView *activeSaved = session.active();
    DocumentView *toActivate = nullptr;
    if (activeSaved) {
        for (DocumentView *view : std::as_const(m_views)) {
            if (view->dockName() == activeSaved->dockName) {
                toActivate = view;
                break;
            }
        }
    }
    showView(toActivate ? toActivate : m_views.first());

    for (auto &ctx : m_contexts) {
        if (ctx->controller)
            ctx->controller->start();
    }
}

} // namespace loftail
