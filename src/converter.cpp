#include "converter.h"

#include <QProcess>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include <QCoreApplication>

QString Converter::findPandoc()
{
    QString path = QStandardPaths::findExecutable("pandoc");
    if (!path.isEmpty())
        return path;

    const QStringList candidates = {
        "C:/Program Files/Pandoc/pandoc.exe",
        QDir::homePath() + "/AppData/Local/Pandoc/pandoc.exe"
    };
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c))
            return c;
    }
    return QString();
}

QString Converter::findLibreOffice()
{
    QString path = QStandardPaths::findExecutable("soffice");
    if (!path.isEmpty())
        return path;

    const QStringList candidates = {
        "C:/Program Files/LibreOffice/program/soffice.exe",
        "C:/Program Files (x86)/LibreOffice/program/soffice.exe"
    };
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c))
            return c;
    }
    return QString();
}

QStringList Converter::supportedTargetFormats()
{
    return {"pdf", "docx", "odt", "rtf", "txt", "html", "md", "epub"};
}

static bool verifyPythonWorks(const QString &exePath)
{
    QProcess p;
    p.start(exePath, {"--version"});
    if (!p.waitForStarted(3000))
        return false;
    if (!p.waitForFinished(5000))
        return false;
    // Windows 下如果没真正安装 Python，"python" 可能只是微软商店的占位存根，
    // 运行后会弹出商店而不是打印版本号，这种情况下退出码不会是 0。
    return p.exitCode() == 0;
}

QString Converter::findPython()
{
    const QStringList candidates = {"python", "python3", "py"};
    for (const QString &c : candidates) {
        const QString path = QStandardPaths::findExecutable(c);
        if (!path.isEmpty() && verifyPythonWorks(path))
            return path;
    }
    return QString();
}

bool Converter::hasPdf2docx()
{
    const QString python = findPython();
    if (python.isEmpty())
        return false;

    QProcess process;
    process.start(python, {"-c", "import pdf2docx"});
    if (!process.waitForStarted(3000))
        return false;
    if (!process.waitForFinished(8000))
        return false;
    return process.exitCode() == 0;
}

Converter::Result Converter::runProcess(const QString &program,
                                         const QStringList &args,
                                         int timeoutMs)
{
    Result r;
    QProcess process;
    process.start(program, args);

    if (!process.waitForStarted(5000)) {
        r.message = QString("无法启动程序: %1").arg(program);
        return r;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        r.message = "转换超时（可能文件过大或程序无响应）";
        return r;
    }
    if (process.exitCode() != 0) {
        r.message = QString("转换失败(退出码 %1): %2")
                         .arg(process.exitCode())
                         .arg(QString::fromLocal8Bit(process.readAllStandardError()));
        return r;
    }
    r.success = true;
    return r;
}

Converter::Result Converter::convertFile(const QString &inputPath,
                                          const QString &targetFormat,
                                          const QString &outputDir)
{
    Result result;
    QFileInfo fi(inputPath);
    if (!fi.exists()) {
        result.message = "源文件不存在: " + inputPath;
        return result;
    }

    const QString ext = fi.suffix().toLower();
    const QString target = targetFormat.toLower();

    if (ext == target) {
        result.message = "源格式与目标格式相同，无需转换";
        return result;
    }

    const QString pandoc = findPandoc();
    const QString soffice = findLibreOffice();
    const QString baseName = fi.completeBaseName();
    const QString outPath = QDir(outputDir).filePath(baseName + "." + target);

    auto convertWithSoffice = [&](const QString &input, const QString &fmt,
                                   const QString &destDir, const QString &infilter = QString()) -> Result {
        Result r;
        if (soffice.isEmpty()) {
            r.message = "未检测到 LibreOffice（soffice），请先安装。";
            return r;
        }
        QStringList args = {"--headless"};
        if (!infilter.isEmpty())
            args << ("--infilter=" + infilter);
        args << "--convert-to" << fmt << "--outdir" << destDir << input;
        r = runProcess(soffice, args);
        return r;
    };

    auto convertWithPandoc = [&](const QString &input, const QString &output) -> Result {
        Result r;
        if (pandoc.isEmpty()) {
            r.message = "未检测到 Pandoc，请先安装。";
            return r;
        }
        const QStringList args = {input, "-o", output};
        r = runProcess(pandoc, args);
        return r;
    };

    // 情况一：目标格式是 pdf
    if (target == "pdf") {
        if (ext == "md") {
            // Markdown 直接转 PDF 依赖 LaTeX 环境，成功率不稳定。
            // 更稳妥的做法：先用 Pandoc 把 md 转成 docx，再用 LibreOffice 转成 pdf。
            QTemporaryDir tmp;
            if (!tmp.isValid()) {
                result.message = "创建临时目录失败";
                return result;
            }
            const QString tmpDocx = tmp.filePath(baseName + ".docx");
            Result r1 = convertWithPandoc(inputPath, tmpDocx);
            if (!r1.success)
                return r1;
            Result r2 = convertWithSoffice(tmpDocx, "pdf", outputDir);
            if (r2.success)
                r2.outputPath = outPath;
            return r2;
        } else {
            Result r = convertWithSoffice(inputPath, "pdf", outputDir);
            if (r.success)
                r.outputPath = outPath;
            return r;
        }
    }

    // 情况二：源文件是 pdf，目标不是 pdf
    if (ext == "pdf") {
        // 优先尝试 pdf2docx（Python库）：专门针对 PDF -> Word 做了排版还原，
        // 精度明显好于 LibreOffice 自带的 PDF 导入功能。仅当目标是 docx 且
        // 本机检测到可用的 pdf2docx 时才走这条路，失败则自动回退到下面的 LibreOffice 方案。
        if (target == "docx" && hasPdf2docx()) {
            const QString python = findPython();
            const QString script = QCoreApplication::applicationDirPath() + "/scripts/pdf2docx_convert.py";
            if (QFileInfo::exists(script)) {
                const QStringList args = {script, inputPath, outPath};
                Result r = runProcess(python, args, 5 * 60 * 1000); // 大文件可能较慢，给5分钟超时
                if (r.success) {
                    r.outputPath = outPath;
                    return r;
                }
                // 失败则继续往下走 LibreOffice 兜底方案，不直接返回
            }
        }

        // 关键点：LibreOffice 默认会把 PDF 当图形文件用 Draw 模块打开，
        // Draw 没有导出到 docx/odt/txt 等文字处理格式的功能，会报 "no export filter"。
        // 必须显式指定 writer_pdf_import 过滤器，强制用 Writer 模块以可编辑文本方式打开。
        const QString infilter = "writer_pdf_import";

        if (target == "md") {
            // LibreOffice 本身没有 Markdown 导出过滤器，
            // 所以走 pdf -> docx（LibreOffice） -> md（Pandoc）的中转方案。
            QTemporaryDir tmp;
            if (!tmp.isValid()) {
                result.message = "创建临时目录失败";
                return result;
            }
            Result r1 = convertWithSoffice(inputPath, "docx", tmp.path(), infilter);
            if (!r1.success)
                return r1;
            const QString tmpDocx = tmp.filePath(baseName + ".docx");
            Result r2 = convertWithPandoc(tmpDocx, outPath);
            if (r2.success)
                r2.outputPath = outPath;
            return r2;
        }

        Result r = convertWithSoffice(inputPath, target, outputDir, infilter);
        if (r.success)
            r.outputPath = outPath;
        return r;
    }

    // 情况三：其余文档格式互转，优先用 Pandoc（速度快、保真度高），失败则回退 LibreOffice
    Result r = convertWithPandoc(inputPath, outPath);
    if (r.success) {
        r.outputPath = outPath;
        return r;
    }

    Result r2 = convertWithSoffice(inputPath, target, outputDir);
    if (r2.success) {
        r2.outputPath = outPath;
    } else if (!r.message.isEmpty()) {
        r2.message = "Pandoc: " + r.message + " | LibreOffice: " + r2.message;
    }
    return r2;
}
