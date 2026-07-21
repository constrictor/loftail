#include "MainWindow.h"

#include "Decoder.h"
#include "Document.h"
#include "Filter.h"
#include "FilterPane.h"
#include "FindBar.h"
#include "FormatCache.h"
#include "FormatPreview.h"
#include "IndexController.h"
#include "LogFormat.h"
#include "LogFormatDialog.h"
#include "LogModel.h"
#include "LogSource.h"
#include "ManualFormatProvider.h"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
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

namespace loftail {

namespace {
// The default log4cplus layout used until the M3 Log Format dialog exists. A file
// that does not match still opens with unparsed lines as plain text (SPEC.md §4).
constexpr auto kDefaultPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
constexpr int  kMaxRecentFiles = 10;
constexpr auto kRecentFilesKey = "recentFiles";
constexpr auto kColumnStateKey = "view/columnState";
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_defaultPattern(QString::fromLatin1(kDefaultPattern))
{
    setWindowTitle(QStringLiteral("loftail"));
    resize(1100, 720);
    setAcceptDrops(true);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setMaximumWidth(200);
    m_progressBar->setVisible(false);
    m_statusLabel = new QLabel(QStringLiteral("No file open"), this);
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_progressBar);

    // Central area is a container holding the record view above the (hidden) Find
    // bar, so Find can dock at the bottom of the view without a modal dialog.
    auto *central = new QWidget(this);
    m_centralLayout = new QVBoxLayout(central);
    m_centralLayout->setContentsMargins(0, 0, 0, 0);
    m_centralLayout->setSpacing(0);
    m_findBar = new FindBar(central);
    m_centralLayout->addWidget(m_findBar);
    setCentralWidget(central);
    connect(m_findBar, &FindBar::findRequested, this, &MainWindow::runFind);

    // Filters side pane (SPEC.md §8), bound to the active document by signal
    // (invariant #7) rather than constructed against a fixed Document.
    m_filterPane = new FilterPane(this);
    auto *filterDock = new QDockWidget(QStringLiteral("Filters"), this);
    filterDock->setObjectName(QStringLiteral("filtersDock"));
    filterDock->setWidget(m_filterPane);
    addDockWidget(Qt::RightDockWidgetArea, filterDock);
    connect(this, &MainWindow::activeDocumentChanged, m_filterPane, &FilterPane::setDocument);
    connect(m_filterPane, &FilterPane::filtersChanged, this, &MainWindow::applyActiveFilters);

    buildMenus();
}

MainWindow::~MainWindow()
{
    teardownDocument();
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
    m_formatAction = fileMenu->addAction(QStringLiteral("&Log Format..."));
    m_formatAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    m_formatAction->setEnabled(false);
    connect(m_formatAction, &QAction::triggered, this, &MainWindow::showFormatDialog);

    fileMenu->addSeparator();
    m_cancelAction = fileMenu->addAction(QStringLiteral("&Cancel Indexing"));
    m_cancelAction->setEnabled(false);
    connect(m_cancelAction, &QAction::triggered, this, [this]() {
        if (m_controller)
            m_controller->cancel();
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
        if (m_view)
            m_view->copySelectionRaw();
    });
    m_copyColumnsAction = editMenu->addAction(QStringLiteral("Copy as &Columns"));
    m_copyColumnsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    m_copyColumnsAction->setEnabled(false);
    connect(m_copyColumnsAction, &QAction::triggered, this, [this]() {
        if (m_view)
            m_view->copySelectionAsColumns();
    });

    // Find / Find Next / Find Previous (SPEC.md §5). Find opens the bar; F3 /
    // Shift+F3 navigate the current query over the visible rows.
    editMenu->addSeparator();
    QAction *findAction = editMenu->addAction(QStringLiteral("&Find..."));
    findAction->setShortcut(QKeySequence::Find);
    connect(findAction, &QAction::triggered, this, [this]() {
        if (m_findBar)
            m_findBar->activate();
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
    connect(wrapOff, &QAction::triggered, this, [this]() {
        m_wrapMode = LogView::WrapMode::Off;
        if (m_view)
            m_view->setWrapMode(m_wrapMode);
    });
    connect(wrapSel, &QAction::triggered, this, [this]() {
        m_wrapMode = LogView::WrapMode::SelectedRecordOnly;
        if (m_view)
            m_view->setWrapMode(m_wrapMode);
    });
    connect(wrapAll, &QAction::triggered, this, [this]() {
        m_wrapMode = LogView::WrapMode::AlwaysOn;
        if (m_view)
            m_view->setWrapMode(m_wrapMode);
    });
}

Document *MainWindow::activeDocument() const
{
    if (m_activeIndex < 0 || m_activeIndex >= int(m_documents.size()))
        return nullptr;
    return m_documents[m_activeIndex].get();
}

void MainWindow::chooseFileToOpen()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Log File"), QString(),
        QStringLiteral("Log files (*.log *.txt);;All files (*)"));
    if (!path.isEmpty())
        openFile(path);
}

void MainWindow::teardownDocument()
{
    if (m_controller) {
        m_controller->cancel();
        delete m_controller; // dtor joins the worker thread
        m_controller = nullptr;
    }
    if (m_view) {
        // Persist the header layout before dropping the view (SPEC.md §5).
        QSettings().setValue(QLatin1String(kColumnStateKey), m_view->saveColumnState());
        m_centralLayout->removeWidget(m_view);
        delete m_view; // no longer setCentralWidget-owned; the container persists
    }
    m_view = nullptr;
    delete m_model;
    m_model = nullptr;
    m_documents.clear();
    m_activeIndex = -1;

    // Unbind the panes from the now-gone document (invariant #7).
    emit activeDocumentChanged(nullptr);

    if (m_copyAction)
        m_copyAction->setEnabled(false);
    if (m_copyColumnsAction)
        m_copyColumnsAction->setEnabled(false);
    if (m_formatAction)
        m_formatAction->setEnabled(false);
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

void MainWindow::openWithSettings(const QString &path, FormatSettings settings, bool promptIfNoMatch)
{
    teardownDocument();

    auto doc = std::make_unique<Document>();
    ManualFormatProvider provider(settings.pattern);
    if (!doc->prepare(path, provider, settings.encoding,
                      settings.sourceZone.toZone(), settings.displayZone.toZone())) {
        m_statusLabel->setText(QStringLiteral("Cannot open %1: %2")
                                   .arg(QFileInfo(path).fileName(), doc->lastError()));
        return;
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
            LogFormatDialog dlg(QFileInfo(path).fileName(), sample, settings, this);
            if (dlg.exec() == QDialog::Accepted) {
                settings = dlg.settings();
                ManualFormatProvider chosen(settings.pattern);
                if (!doc->prepare(path, chosen, settings.encoding,
                                  settings.sourceZone.toZone(), settings.displayZone.toZone())) {
                    m_statusLabel->setText(QStringLiteral("Cannot open %1: %2")
                                               .arg(QFileInfo(path).fileName(), doc->lastError()));
                    return;
                }
                persist = true;
            } else {
                persist = false; // declined: open as plain text, do not remember
            }
        }
    }

    m_currentSettings = settings;
    m_documents.push_back(std::move(doc));
    m_activeIndex = 0;

    buildViewAndIndex(path);

    if (persist)
        persistFormat(path, settings);
    rememberRecentFile(path);
}

void MainWindow::buildViewAndIndex(const QString &path)
{
    Document *active = activeDocument();
    if (!active)
        return;

    m_model = new LogModel(active);
    m_view = new LogView(active, m_model);
    m_view->setWrapMode(m_wrapMode);
    m_centralLayout->insertWidget(0, m_view); // above the Find bar
    m_view->setFocus();

    // Column layout persistence (SPEC.md §5): restore the saved header state.
    const QByteArray colState = QSettings().value(QLatin1String(kColumnStateKey)).toByteArray();
    if (!colState.isEmpty())
        m_view->restoreColumnState(colState);

    m_view->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view->header(), &QWidget::customContextMenuRequested, this, &MainWindow::showColumnMenu);

    m_copyAction->setEnabled(true);
    m_copyColumnsAction->setEnabled(true);
    m_formatAction->setEnabled(true);

    m_controller = new IndexController(active, m_model, this);
    connect(m_controller, &IndexController::progress, this, &MainWindow::onIndexProgress);
    connect(m_controller, &IndexController::finished, this, &MainWindow::onIndexFinished);

    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(true);
    m_cancelAction->setEnabled(true);

    setWindowTitle(QStringLiteral("loftail — %1").arg(QFileInfo(path).fileName()));

    // Bind the filter pane to the new document (invariant #7). Its discovered
    // subsystem/thread lists fill in as indexing progresses (refreshed on finish).
    emit activeDocumentChanged(active);
    updateStatus();

    m_controller->start();
}

void MainWindow::showFormatDialog()
{
    Document *doc = activeDocument();
    if (!doc || !doc->source())
        return;

    const qint64 sampleLen = qMin<qint64>(64 * 1024, doc->source()->size());
    const QByteArray sample = sampleLen > 0
        ? doc->source()->bytes(0, sampleLen).toByteArray() : QByteArray();

    LogFormatDialog dlg(QFileInfo(doc->path()).fileName(), sample, m_currentSettings, this);
    if (dlg.exec() == QDialog::Accepted)
        applySettings(dlg.settings());
}

void MainWindow::applySettings(const FormatSettings &newSettings)
{
    Document *doc = activeDocument();
    if (!doc)
        return;

    // Copy the path: a rescan tears the Document down, so `doc` must not be read
    // after openWithSettings() runs.
    const QString path = doc->path();
    const FormatSettings old = m_currentSettings;
    const bool patternChanged  = newSettings.pattern != old.pattern;
    const bool encodingChanged = newSettings.encoding != old.encoding;
    const bool sourceChanged   = newSettings.sourceZone != old.sourceZone;
    const bool displayChanged  = newSettings.displayZone != old.displayZone;

    m_currentSettings = newSettings;
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
        if (m_view)
            m_view->viewport()->update();
    }

    updateStatus();
}

void MainWindow::persistFormat(const QString &path, const FormatSettings &s)
{
    QSettings store;
    FormatCache::save(store, path, s);
}

void MainWindow::onIndexProgress(qint64 done, qint64 total)
{
    if (total > 0)
        m_progressBar->setValue(int((done * 100) / total));
    updateStatus();
}

void MainWindow::onIndexFinished(bool cancelled)
{
    m_progressBar->setVisible(false);
    m_cancelAction->setEnabled(false);
    // The full subsystem/thread value sets are known now, so fill the pane's
    // auto-discovered lists (SPEC.md §6) and re-run any active filter over the
    // completed index.
    if (m_filterPane)
        m_filterPane->refreshDiscoveredLists();
    if (activeDocument() && activeDocument()->filters().anyActive())
        applyActiveFilters();
    if (m_view)
        m_view->scrollToEnd(); // open at the file's end, following (SPEC.md §3)
    updateStatus();
    if (cancelled)
        m_statusLabel->setText(m_statusLabel->text() + QStringLiteral("  (indexing cancelled)"));
}

void MainWindow::applyActiveFilters()
{
    Document *doc = activeDocument();
    if (!doc || !m_model)
        return;
    // A filtered set is a wholesale row remap, so reset the model around the
    // recompute: the view/header/selection refresh over the new visible set and
    // LogView rebuilds its line geometry (invariant #6). The predicate chain inside
    // applyFilters runs integer axes first, message text last (invariant #4).
    m_model->beginFilterReset();
    doc->applyFilters();
    m_model->endFilterReset();
    if (m_view) {
        m_view->updateGeometry();
        m_view->viewport()->update();
    }
    updateStatus();
}

void MainWindow::runFind(bool forward, bool fromStart)
{
    if (!m_view || !m_model || !m_findBar)
        return;
    const QString pattern = m_findBar->pattern();
    if (pattern.isEmpty()) {
        m_findBar->setStatus(QString());
        return;
    }

    // Find over the message column of the CURRENTLY-VISIBLE rows (the filtered
    // subset when a filter is active), reusing the filter's text matcher (SPEC.md
    // §5). Changes no filter state — it only moves the selection.
    TextMatcher matcher;
    matcher.set(pattern, m_findBar->regex(),
                m_findBar->caseSensitive() ? Qt::CaseSensitive : Qt::CaseInsensitive);
    if (!matcher.isValid()) {
        m_findBar->setStatus(QStringLiteral("bad regex"));
        return;
    }

    const int count = m_model->rowCount();
    if (count == 0) {
        m_findBar->setStatus(QStringLiteral("no records"));
        return;
    }

    // Search every visible column's text so Find matches anything on screen, but
    // fall back to a message-only scan when the format defines no columns.
    const int cols = m_model->columnCount();
    auto rowMatches = [this, &matcher, cols](int row) {
        if (cols == 0)
            return matcher.matches(m_model->cellText(row, 0));
        for (int c = 0; c < cols; ++c)
            if (matcher.matches(m_model->cellText(row, c)))
                return true;
        return false;
    };

    const int from = fromStart ? -1 : m_view->currentRecord();
    const int hit = Find::search(count, from, forward, /*wrap=*/true, rowMatches);
    if (hit < 0) {
        m_findBar->setStatus(QStringLiteral("no match"));
        return;
    }
    m_view->setCurrentRecord(hit);
    m_findBar->setStatus(QString()); // keep focus in the bar for repeated Enter/F3
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
    if (!m_view || !m_model)
        return;
    QHeaderView *header = m_view->header();
    QMenu menu(this);
    for (int c = 0; c < m_model->columnCount(); ++c) {
        const QString name = m_model->headerData(c, Qt::Horizontal).toString();
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
    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            openFile(url.toLocalFile());
            break; // single-file view in M2b; multi-file is FUTURE.md
        }
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    teardownDocument();
    event->accept();
}

} // namespace loftail
