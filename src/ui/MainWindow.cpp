#include "MainWindow.h"

#include "Document.h"
#include "IndexController.h"
#include "LogModel.h"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QProgressBar>
#include <QScrollBar>
#include <QSettings>
#include <QStatusBar>

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

    QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    QMenu *wrapMenu = viewMenu->addMenu(QStringLiteral("Line &Wrap"));
    auto *wrapGroup = new QActionGroup(this);
    QAction *wrapOff = wrapMenu->addAction(QStringLiteral("&Off"));
    QAction *wrapSel = wrapMenu->addAction(QStringLiteral("&Selected Record Only"));
    for (QAction *a : {wrapOff, wrapSel}) {
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
    }
    setCentralWidget(nullptr); // deletes m_view
    m_view = nullptr;
    delete m_model;
    m_model = nullptr;
    m_documents.clear();
    m_activeIndex = -1;
}

void MainWindow::openFile(const QString &path, const QString &pattern)
{
    const QString effectivePattern = pattern.isEmpty() ? m_defaultPattern : pattern;

    teardownDocument();

    auto doc = std::make_unique<Document>();
    if (!doc->prepare(path, effectivePattern)) {
        m_statusLabel->setText(QStringLiteral("Cannot open %1: %2")
                                   .arg(QFileInfo(path).fileName(), doc->lastError()));
        return;
    }

    m_documents.push_back(std::move(doc));
    m_activeIndex = 0;
    Document *active = activeDocument();

    m_model = new LogModel(active);
    m_view = new LogView(active, m_model);
    m_view->setWrapMode(m_wrapMode);
    setCentralWidget(m_view);

    // Column layout persistence (SPEC.md §5): restore the saved header state.
    const QByteArray colState = QSettings().value(QLatin1String(kColumnStateKey)).toByteArray();
    if (!colState.isEmpty())
        m_view->restoreColumnState(colState);

    m_view->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view->header(), &QWidget::customContextMenuRequested, this, &MainWindow::showColumnMenu);

    m_copyAction->setEnabled(true);
    m_copyColumnsAction->setEnabled(true);

    m_controller = new IndexController(active, m_model, this);
    connect(m_controller, &IndexController::progress, this, &MainWindow::onIndexProgress);
    connect(m_controller, &IndexController::finished, this, &MainWindow::onIndexFinished);

    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(true);
    m_cancelAction->setEnabled(true);

    setWindowTitle(QStringLiteral("loftail — %1").arg(QFileInfo(path).fileName()));
    rememberRecentFile(path);
    updateStatus();

    m_controller->start();
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
    if (m_view)
        m_view->scrollToEnd(); // open at the file's end, following (SPEC.md §3)
    updateStatus();
    if (cancelled)
        m_statusLabel->setText(m_statusLabel->text() + QStringLiteral("  (indexing cancelled)"));
}

void MainWindow::updateStatus()
{
    Document *doc = activeDocument();
    if (!doc) {
        m_statusLabel->setText(QStringLiteral("No file open"));
        return;
    }
    m_statusLabel->setText(QStringLiteral("%1  |  %2 records")
                               .arg(QFileInfo(doc->path()).fileName())
                               .arg(doc->index().records.size()));
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
