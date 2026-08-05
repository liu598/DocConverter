#pragma once
#include <QMainWindow>
#include <QListWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QLabel>

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void onAddFiles();
    void onRemoveSelected();
    void onClearList();
    void onChooseOutputDir();
    void onConvert();
    void onInstallDependencies();

private:
    void appendLog(const QString &text);
    void checkEngines();
    void addFilesToList(const QStringList &paths);

    QListWidget *m_fileList;
    QComboBox *m_formatCombo;
    QLineEdit *m_outputDirEdit;
    QPushButton *m_convertBtn;
    QPlainTextEdit *m_logEdit;
    QProgressBar *m_progressBar;
    QLabel *m_engineStatusLabel;
    QPushButton *m_installBtn;
};
