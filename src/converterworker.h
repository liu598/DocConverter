#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include "converter.h"

// ConverterWorker 在独立线程里逐个执行批量转换。
// 之所以不放在主线程：QProcess 阻塞等待会把窗口卡到「未响应」，
// 而且那样没办法在中途取消。
class ConverterWorker : public QObject
{
    Q_OBJECT
public:
    ConverterWorker(QStringList files,
                    QString targetFormat,
                    QString outputDir,
                    bool outputToSourceDir,
                    Converter::CancelToken *token,
                    QObject *parent = nullptr);

public slots:
    void run();

signals:
    void fileStarted(int index, const QString &path);
    void fileSkipped(int index, const QString &reason);
    void fileSucceeded(int index, const QString &outputPath, const QString &note);
    void fileFailed(int index, const QString &reason);
    void finished(int success, int failed, int skipped, bool canceled);

private:
    QStringList m_files;
    QString m_targetFormat;
    QString m_outputDir;
    bool m_outputToSourceDir = false;
    Converter::CancelToken *m_token = nullptr;
};
