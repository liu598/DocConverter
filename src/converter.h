#pragma once
#include <QString>
#include <QStringList>

// Converter 负责实际的格式转换工作。
// 它本身不实现任何编解码逻辑，而是调用系统上已安装的
// Pandoc 和 LibreOffice(soffice) 命令行工具来完成转换，
// 这样可以稳定支持 docx/doc/odt/rtf/txt/html/md/epub/pdf
// 之间的相互转换，且转换质量有保障。
class Converter
{
public:
    struct Result
    {
        bool success = false;
        QString message;     // 失败原因，或成功提示
        QString outputPath;  // 成功时生成的文件路径
    };

    // 检测本机是否安装了 Pandoc / LibreOffice，返回可执行文件完整路径；
    // 找不到则返回空字符串。
    static QString findPandoc();
    static QString findLibreOffice();
    static QString findPython();

    // 检测 Python 环境里是否安装了 pdf2docx 库（用于高精度 PDF -> Word）
    static bool hasPdf2docx();

    // 界面下拉框可选的目标格式列表
    static QStringList supportedTargetFormats();

    // 执行单个文件的转换
    static Result convertFile(const QString &inputPath,
                               const QString &targetFormat,
                               const QString &outputDir);

private:
    static Result runProcess(const QString &program,
                              const QStringList &args,
                              int timeoutMs = 120000);
};
