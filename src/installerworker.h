#pragma once

#include <QObject>
#include <QString>

// InstallerWorker 把 winget 安装过程放到后台线程。
// LibreOffice 安装包约 300MB，winget 静默安装动辄几分钟，
// 在主线程同步等 waitForFinished 会把窗口卡成「未响应」。
class InstallerWorker : public QObject
{
    Q_OBJECT
public:
    // 三个 bool 表示「这一项是否需要安装」
    InstallerWorker(bool needPandoc,
                    bool needSoffice,
                    bool needPdf2docx,
                    QObject *parent = nullptr);

public slots:
    void run();

signals:
    void logMessage(const QString &text);
    void stepResult(bool ok, const QString &message);
    void finished(int succeeded, int failed);

private:
    bool m_needPandoc = false;
    bool m_needSoffice = false;
    bool m_needPdf2docx = false;
};
