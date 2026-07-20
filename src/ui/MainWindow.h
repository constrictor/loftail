#pragma once

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
QT_END_NAMESPACE

namespace loftail {

class Document;
class LogModel;
class IndexController;

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

    // Open `path` with `pattern` (the M3 Log Format dialog will replace the fixed
    // pattern; until then the caller supplies one, defaulting to a common
    // log4cplus layout). Safe to call repeatedly; it replaces the open document.
    void openFile(const QString &path, const QString &pattern = QString());

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void chooseFileToOpen();
    void onIndexProgress(qint64 done, qint64 total);
    void onIndexFinished(bool cancelled);
    void showColumnMenu(const QPoint &pos);

private:
    void buildMenus();
    void refreshRecentFilesMenu();
    void rememberRecentFile(const QString &path);
    void teardownDocument();
    void updateStatus();

    Document *activeDocument() const;

    std::vector<std::unique_ptr<Document>> m_documents;
    int m_activeIndex = -1;

    LogModel        *m_model = nullptr;
    LogView         *m_view = nullptr;
    IndexController *m_controller = nullptr;

    QMenu   *m_recentMenu = nullptr;
    QAction *m_cancelAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_copyColumnsAction = nullptr;
    QLabel       *m_statusLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;

    QString m_defaultPattern;
    LogView::WrapMode m_wrapMode = LogView::WrapMode::Off;
};

} // namespace loftail
