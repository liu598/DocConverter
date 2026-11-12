#pragma once

#include <QJsonArray>
#include <QMainWindow>
#include <QPointer>
#include <QThread>

#include "converter.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QToolButton;
class QWidget;
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

    // 高级选项 / 预设
    void onToggleAdvanced();
    void onSaveProfile();
    void onDeleteProfile();
    void onProfileActivated(int index);

    // 预览 / 历史 / 外观
    void onPreviewSelected();
    void onListContextMenu(const QPoint &pos);
    void showHistory();
    void onChooseEnginePath(const QString &engine);

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
    void buildMenus();
    QWidget *buildAdvancedPanel();
    void loadSettings();
    void saveSettings();

    void appendLog(const QString &text);
    void addFilesToList(const QStringList &paths);
    QStringList selectedFilePaths() const;
    void setBusy(bool busy);
    void updateControls();
    void setItemStatus(int index, int state, const QString &tooltip);
    void updateEngineLabel(const QString &pandoc, const QString &soffice, bool pdf2docx);
    void updateFormatDependentOptions();

    // 高级选项
    Converter::Options collectOptions() const;
    void applyOptions(const Converter::Options &o);
    void refreshProfileCombo(const QString &selectName = QString());

    // 主题 / 历史
    void applyTheme(const QString &theme);
    void loadHistory();
    void saveHistory();
    void addHistory(const QString &source, const QString &target, const QString &output,
                    const QString &status, const QString &message);

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

    // 高级选项
    QToolButton *m_advancedToggle = nullptr;
    QWidget *m_advancedPanel = nullptr;
    QCheckBox *m_tocCheck = nullptr;
    QCheckBox *m_numberCheck = nullptr;
    QLineEdit *m_refDocEdit = nullptr;
    QPushButton *m_refDocBrowseBtn = nullptr;
    QPushButton *m_refDocClearBtn = nullptr;
    QComboBox *m_mdFlavorCombo = nullptr;
    QSpinBox *m_dpiSpin = nullptr;
    QLineEdit *m_titleEdit = nullptr;
    QLineEdit *m_authorEdit = nullptr;
    QComboBox *m_profileCombo = nullptr;

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

    QString m_theme = "system";
    QString m_currentTarget;      // 本轮转换的目标格式，写历史时用
    QJsonArray m_history;

    // 逐项统计，用于最后汇报
    int m_successCount = 0;
    int m_failedCount = 0;
    int m_skippedCount = 0;
};
