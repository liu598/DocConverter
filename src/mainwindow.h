#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QThread>

#include "converter.h"

class QListWidget;
class QListWidgetItem;
class QComboBox;
class QLineEdit;
class QCheckBox;
class QPushButton;
class QPlainTextEdit;
class QProgressBar;
class QLabel;
class ConverterWorker;
class InstallerWorker;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onAddFiles();
    void onAddFolder();
    void onRemoveSelected();
    void onClearList();
    void onChooseOutputDir();
    void onConvert();
    void onCancel();
    void onInstallDependencies();
    void onOpenOutputDir();
    void onSaveLog();
    void startEngineCheck();

    // 安装 worker 回调
    void onInstallLog(const QString &text);
    void onInstallStepResult(bool ok, const QString &message);
    void onInstallFinished(int succeeded, int failed);

    // worker 回调
    void onFileStarted(int index, const QString &path);
    void onFileSkipped(int index, const QString &reason);
    void onFileSucceeded(int index, const QString &outputPath, const QString &note);
    void onFileFailed(int index, const QString &reason);
    void onWorkerFinished(int success, int failed, int skipped, bool canceled);

private:
    void buildUi();
    void loadSettings();
    void saveSettings();

    void appendLog(const QString &text);
    void addFilesToList(const QStringList &paths);
    QStringList selectedFilePaths() const;
    void setBusy(bool busy);
    void updateControls();
    void setItemStatus(int index, int state, const QString &tooltip);
    void updateEngineLabel(const QString &pandoc, const QString &soffice, bool pdf2docx);

    bool isBusy() const { return m_busy || m_installing; }
    bool ensureEnginesOrWarn();

    // UI
    QListWidget *m_fileList = nullptr;
    QComboBox *m_formatCombo = nullptr;
    QLineEdit *m_outputDirEdit = nullptr;
    QCheckBox *m_outputToSourceCheck = nullptr;
    QPushButton *m_addFilesBtn = nullptr;
    QPushButton *m_addFolderBtn = nullptr;
    QPushButton *m_removeBtn = nullptr;
    QPushButton *m_clearBtn = nullptr;
    QPushButton *m_browseBtn = nullptr;
    QPushButton *m_convertBtn = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    QPushButton *m_openDirBtn = nullptr;
    QPushButton *m_installBtn = nullptr;
    QPushButton *m_saveLogBtn = nullptr;
    QPushButton *m_clearLogBtn = nullptr;
    QPlainTextEdit *m_logEdit = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QLabel *m_engineStatusLabel = nullptr;
    QLabel *m_progressLabel = nullptr;

    // 状态
    QPointer<QThread> m_engineThread;
    QPointer<QThread> m_workerThread;
    QPointer<QThread> m_installThread;
    ConverterWorker *m_worker = nullptr;
    InstallerWorker *m_installWorker = nullptr;
    Converter::CancelToken m_cancelToken;
    bool m_busy = false;
    bool m_installing = false;
    bool m_enginesChecked = false;
    bool m_pandocReady = false;
    bool m_sofficeReady = false;
    bool m_pdf2docxReady = false;
    int m_totalCount = 0;

    // 逐项统计，用于最后汇报
    int m_successCount = 0;
    int m_failedCount = 0;
    int m_skippedCount = 0;
};
