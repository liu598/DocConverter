#pragma once

#include <QDialog>
#include <QPointer>
#include <QThread>

class QLabel;
class QPlainTextEdit;
class QPushButton;

// 文档预览窗口：只读地展示文件开头的文字内容，用于转换前确认「拿到的确实是这份文件」。
// 文字抽取放在后台线程里跑（docx/epub 要调 Pandoc，冷启动可能几百毫秒到几秒），
// 避免界面在打开预览时卡住。
class PreviewDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PreviewDialog(const QString &path, QWidget *parent = nullptr);
    ~PreviewDialog() override;

private:
    void startLoading();

    QString m_path;
    QLabel *m_status = nullptr;
    QPlainTextEdit *m_text = nullptr;
    QPushButton *m_openBtn = nullptr;
    QPointer<QThread> m_thread;
};
