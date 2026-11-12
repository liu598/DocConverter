#include "converterworker.h"

#include <QDir>
#include <QFileInfo>

ConverterWorker::ConverterWorker(QStringList files,
                                 QString targetFormat,
                                 QString outputDir,
                                 bool outputToSourceDir,
                                 Converter::Options options,
                                 Converter::CancelToken *token,
                                 QObject *parent)
    : QObject(parent)
    , m_files(std::move(files))
    , m_targetFormat(std::move(targetFormat))
    , m_outputDir(std::move(outputDir))
    , m_outputToSourceDir(outputToSourceDir)
    , m_options(std::move(options))
    , m_token(token)
{
}

void ConverterWorker::run()
{
    int success = 0;
    int failed = 0;
    int skipped = 0;
    bool canceled = false;

    for (int i = 0; i < m_files.size(); ++i) {
        if (m_token && m_token->isCanceled()) {
            canceled = true;
            break;
        }

        const QString input = m_files.at(i);
        emit fileStarted(i, input);

        const QString dir = m_outputToSourceDir
                                ? QFileInfo(input).absolutePath()
                                : m_outputDir;

        const Converter::Result r = Converter::convertFile(input, m_targetFormat, dir,
                                                           m_token, m_options);

        if (r.canceled) {
            canceled = true;
            break;
        }
        if (r.skipped) {
            ++skipped;
            emit fileSkipped(i, r.message);
        } else if (r.success) {
            ++success;
            emit fileSucceeded(i, r.outputPath, r.message);
        } else {
            ++failed;
            emit fileFailed(i, r.message);
        }
    }

    if (m_token && m_token->isCanceled())
        canceled = true;

    emit finished(success, failed, skipped, canceled);
}
