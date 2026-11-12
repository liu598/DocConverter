#pragma once

#include <QAtomicInt>
#include <QString>
#include <QStringList>

// Converter 负责实际的格式转换工作。
// 它本身不实现任何编解码逻辑，而是调用系统上已安装的
// Pandoc 和 LibreOffice(soffice) 命令行工具来完成转换，
// 这样可以稳定支持 docx/doc/odt/rtf/txt/html/md/epub/pdf
// 之间的相互转换，且转换质量有保障。
//
// 设计要点（相对于最初版本的改动）：
//  1. 引擎路径只在启动时探测一次并缓存，批量转换时不再反复扫 PATH；
//  2. 所有转换先落到临时暂存目录，校验产物确实存在且非空后，
//     再以「不覆盖已有文件」的唯一名字移入输出目录；
//  3. 每次调用都带独立的 LibreOffice 用户配置目录，避免用户自己开着
//     LibreOffice 时 --convert-to 被转发给已有实例而静默失败；
//  4. 子进程输出边跑边排空，避免大量日志把管道/内存顶爆；
//  5. 支持取消令牌，超时按文件大小动态计算；
//  6. 错误信息按 UTF-8 解码（pandoc / python 的输出都是 UTF-8）。
class Converter
{
public:
    struct Result
    {
        bool success = false;   // 转换成功且产物校验通过
        bool skipped = false;   // 无需转换（源格式 == 目标格式）
        bool canceled = false;  // 被用户取消
        QString message;        // 失败原因；成功时可能是补充提示（如降级）
        QString outputPath;     // 成功时生成的文件路径
        QString output;         // 引擎 stdout/stderr 的尾部，便于排查与识别引擎警告
    };

    // 取消令牌。用一个原子标志在 UI 线程与工作线程之间传递取消请求，
    // 不依赖事件循环，线程安全。
    struct CancelToken
    {
        QAtomicInt flag{0};
        void cancel() { flag.storeRelease(1); }
        bool isCanceled() const { return flag.loadAcquire() != 0; }
        void reset() { flag.storeRelease(0); }
    };

    // ---- 引擎探测 ----
    // 首次调用会探测并缓存；之后直接返回缓存结果。
    static QString pandocPath();
    static QString sofficePath();
    static QString pythonPath();

    // 检测 Python 环境里是否安装了 pdf2docx 库（用于高精度 PDF -> Word）
    static bool pdf2docxAvailable();

    // 清空缓存并重新探测。安装完组件后调用——因为 Windows 上
    // 当前进程的环境变量（PATH）不会自动刷新，需要从注册表重新读取。
    static void refreshEngines();

    // ---- 元信息 ----
    static QStringList supportedTargetFormats();
    // 是否是我们支持的源文件格式（用于拖拽时给出明确提示）
    static bool isSupportedSource(const QString &path);

    // 若 path 已存在，则返回一个不冲突的 "名字 (1).后缀" 路径
    static QString uniqueOutputPath(const QString &path);

    // ---- 转换 ----
    static Result convertFile(const QString &inputPath,
                              const QString &targetFormat,
                              const QString &outputDir,
                              CancelToken *token = nullptr);

private:
    static Result runProcess(const QString &program,
                             const QStringList &args,
                             int timeoutMs,
                             CancelToken *token = nullptr);
};
