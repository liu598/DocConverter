#include "converter.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

// ===========================================================================
// 引擎探测：路径只算一次，之后全程复用
// ===========================================================================
namespace {

struct EngineCache
{
    bool probed = false;
    QString pandoc;
    QString soffice;
    QString python;
    bool pdf2docx = false;
};

EngineCache &cache()
{
    static EngineCache c;
    return c;
}

QMutex &engineMutex()
{
    static QMutex m;
    return m;
}

// 在 dir 下按名字找文件，递归深度受限（避免扫整块磁盘）
QString searchDir(const QString &dirPath, const QString &fileName, int depth)
{
    if (depth < 0)
        return QString();
    QDir dir(dirPath);
    if (!dir.exists())
        return QString();

    const QString direct = dir.filePath(fileName);
    if (QFileInfo(direct).isFile())
        return direct;

    const QStringList subs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &sub : subs) {
        const QString found = searchDir(dir.filePath(sub), fileName, depth - 1);
        if (!found.isEmpty())
            return found;
    }
    return QString();
}

// winget 用 --silent 安装时，包会落在 %LOCALAPPDATA%\Microsoft\WinGet\Packages\ 下，
// 且目录名带哈希后缀，只能靠前缀匹配 + 递归找。
QString searchWingetPackage(const QStringList &nameFilters, const QString &fileName, int depth)
{
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (local.isEmpty())
        return QString();

    const QString root = local + "/Microsoft/WinGet/Packages";
    QDir dir(root);
    if (!dir.exists())
        return QString();

    const QStringList pkgs = dir.entryList(nameFilters, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &pkg : pkgs) {
        const QString found = searchDir(dir.filePath(pkg), fileName, depth);
        if (!found.isEmpty())
            return found;
    }
    return QString();
}

QString probePandoc()
{
    const QString onPath = QStandardPaths::findExecutable("pandoc");
    if (!onPath.isEmpty())
        return onPath;

    const QStringList fixed = {
        "C:/Program Files/Pandoc/pandoc.exe",
        "C:/Program Files (x86)/Pandoc/pandoc.exe",
        QDir::homePath() + "/AppData/Local/Pandoc/pandoc.exe"
    };
    for (const QString &c : fixed) {
        if (QFileInfo(c).isFile())
            return c;
    }
    return searchWingetPackage({"JohnMacFarlane.Pandoc*"}, "pandoc.exe", 3);
}

QString probeSoffice()
{
    const QString onPath = QStandardPaths::findExecutable("soffice");
    if (!onPath.isEmpty())
        return onPath;

    const QStringList fixed = {
        "C:/Program Files/LibreOffice/program/soffice.exe",
        "C:/Program Files (x86)/LibreOffice/program/soffice.exe"
    };
    for (const QString &c : fixed) {
        if (QFileInfo(c).isFile())
            return c;
    }
    return searchWingetPackage({"TheDocumentFoundation.LibreOffice*"}, "soffice.exe", 4);
}

// Windows 下 "python" 可能只是微软商店的占位存根：运行后不打印版本而是弹商店，
// 退出码非 0。所以必须真的跑一次 --version 验证。
bool pythonWorks(const QString &exePath)
{
    if (exePath.isEmpty())
        return false;
    QProcess p;
    p.setStandardInputFile(QProcess::nullDevice());
    p.start(exePath, {"--version"});
    if (!p.waitForStarted(3000))
        return false;
    if (!p.waitForFinished(5000)) {
        p.kill();
        return false;
    }
    return p.exitCode() == 0;
}

// 列出所有看起来可用的 python 解释器。
// 一台机器上装好几个 Python 很常见（系统级、用户级、各种工具自带），
// 而 pdf2docx 往往只装在其中一个里，所以要把候选都收集起来再挑。
QStringList probePythons()
{
    QStringList found;

    auto addIfWorks = [&found](const QString &exe) {
        if (exe.isEmpty() || found.contains(exe))
            return;
        if (pythonWorks(exe))
            found << exe;
    };

    // 1. PATH 上的
    for (const QString &n : {"python", "python3", "py"})
        addIfWorks(QStandardPaths::findExecutable(n));

    // 2. 常见安装目录：C:/Python312、%LOCALAPPDATA%/Programs/Python/Python312、
    //    %ProgramFiles%/Python312 之类
    QStringList roots;
    for (const QString &drive : {"C:/", "D:/"})
        roots << drive;
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (!local.isEmpty())
        roots << (local + "/Programs/Python");
    const QString pf = qEnvironmentVariable("ProgramFiles");
    if (!pf.isEmpty())
        roots << pf + "/Python";

    for (const QString &root : roots) {
        QDir dir(root);
        if (!dir.exists())
            continue;
        // 只认 Python3x 这种目录名，避免把整个盘扫一遍
        const QStringList dirs = dir.entryList({"Python3*"}, QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &d : dirs)
            addIfWorks(dir.filePath(d + "/python.exe"));
    }
    return found;
}

bool probePdf2docx(const QString &python)
{
    if (python.isEmpty())
        return false;
    QProcess p;
    p.setStandardInputFile(QProcess::nullDevice());
    p.start(python, {"-c", "import pdf2docx"});
    if (!p.waitForStarted(3000))
        return false;
    if (!p.waitForFinished(15000)) {
        p.kill();
        return false;
    }
    return p.exitCode() == 0;
}

// 调用前必须已持有 engineMutex
void ensureProbed()
{
    EngineCache &c = cache();
    if (c.probed)
        return;
    c.pandoc = probePandoc();
    c.soffice = probeSoffice();

    // 优先挑装了 pdf2docx 的那个解释器：PDF 转 Word/文本的还原质量差很多
    const QStringList pythons = probePythons();
    for (const QString &py : pythons) {
        if (probePdf2docx(py)) {
            c.python = py;
            c.pdf2docx = true;
            break;
        }
    }
    if (c.python.isEmpty() && !pythons.isEmpty())
        c.python = pythons.first();

    c.probed = true;
}

#ifdef Q_OS_WIN
// Qt 读注册表时拿到的是未展开的字符串，需要自己替换 %VAR%
QString expandEnvVars(const QString &value)
{
    if (value.isEmpty())
        return value;
    wchar_t buffer[32768];
    const DWORD n = ExpandEnvironmentStringsW(
        reinterpret_cast<const wchar_t *>(value.utf16()), buffer, 32768);
    if (n == 0 || n > 32768)
        return value;
    return QString::fromWCharArray(buffer);
}

// 安装程序（winget）只改了注册表，当前进程的 PATH 还是启动时的旧值，
// 所以装完组件后必须重新读一遍注册表并写回本进程环境。
void reloadPathFromRegistry()
{
    QStringList parts;
    const QStringList keys = {
        "HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
        "HKEY_CURRENT_USER\\Environment"
    };
    for (const QString &key : keys) {
        QSettings s(key, QSettings::NativeFormat);
        const QVariant v = s.value("Path");
        QString text;
        if (v.metaType().id() == QMetaType::QStringList)
            text = v.toStringList().join(';');
        else
            text = v.toString();
        if (!text.isEmpty())
            parts << text;
    }
    if (parts.isEmpty())
        return;

    QString merged = parts.join(';');
    // 去掉空项和重复的分号
    QStringList cleaned;
    for (const QString &p : merged.split(';', Qt::SkipEmptyParts))
        cleaned << p.trimmed();
    merged = expandEnvVars(cleaned.join(';'));

    SetEnvironmentVariableW(L"PATH",
                            reinterpret_cast<const wchar_t *>(merged.utf16()));
}
#endif

// ---- 通用小工具 ----

void terminateProcessTree(QProcess &p)
{
    if (p.state() == QProcess::NotRunning)
        return;
#ifdef Q_OS_WIN
    // 只 kill 直接子进程会留下 soffice.bin 之类的孙进程，
    // 用 taskkill /T 整棵树一起收掉。
    const qint64 pid = p.processId();
    if (pid > 0) {
        QProcess killer;
        killer.setStandardInputFile(QProcess::nullDevice());
        killer.start("taskkill", {"/PID", QString::number(pid), "/T", "/F"});
        killer.waitForFinished(3000);
    }
#endif
    if (p.state() != QProcess::NotRunning) {
        p.kill();
        p.waitForFinished(2000);
    }
}

QString normalizeExt(const QString &ext)
{
    if (ext == "htm" || ext == "xhtml")
        return "html";
    if (ext == "markdown" || ext == "mdown" || ext == "mkd" || ext == "mdwn")
        return "md";
    if (ext == "text")
        return "txt";
    if (ext == "latex")
        return "tex";
    if (ext == "asciidoc" || ext == "ad")
        return "adoc";
    if (ext == "rest")
        return "rst";
    return ext;
}

// 目标格式 -> Pandoc 的 writer 名。
// 表里没有的目标（目前只有 doc）说明 Pandoc 写不出来，直接交给 LibreOffice。
QString pandocWriterFor(const QString &target)
{
    static const QHash<QString, QString> map = {
        {"docx", "docx"},
        {"odt",  "odt"},
        {"rtf",  "rtf"},
        {"txt",  "plain"},
        {"html", "html"},
        {"md",   "markdown"},
        {"epub", "epub"},
        {"pptx", "pptx"},
        {"tex",  "latex"},
        {"rst",  "rst"},
        {"org",  "org"},
        {"adoc", "asciidoc"}
    };
    return map.value(target);
}

// 这些格式是「标记/纯文本」，直接交给 Pandoc 排版成 PDF 会依赖 LaTeX，
// 因此统一先转 docx 再让 LibreOffice 出 PDF，成功率更高。
bool isMarkupSource(const QString &ext)
{
    static const QStringList markup = {"md", "txt", "epub", "odt",
                                       "tex", "rst", "org", "adoc"};
    return markup.contains(ext);
}

// Pandoc 读不了的源格式：老的二进制 .doc，以及只适合简单版面的 .pptx。
// 这两类交给 LibreOffice，别白跑一次 Pandoc（还会在日志里留下失败噪音）。
bool pandocCanRead(const QString &ext)
{
    static const QStringList unreadable = {"doc", "pptx"};
    return !unreadable.contains(ext);
}

// pandoc 2.19 起 --self-contained 改名为 --embed-resources
bool pandocUsesEmbedResources(const QString &pandoc)
{
    static QHash<QString, bool> memo;
    if (memo.contains(pandoc))
        return memo.value(pandoc);

    bool modern = false;
    QProcess p;
    p.setStandardInputFile(QProcess::nullDevice());
    p.start(pandoc, {"--version"});
    if (p.waitForStarted(3000) && p.waitForFinished(5000)) {
        const QString firstLine = QString::fromUtf8(p.readAllStandardOutput())
                                      .section('\n', 0, 0).trimmed();
        const QStringList parts = firstLine.split(' ', Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            const QStringList nums = parts.at(1).split('.');
            if (nums.size() >= 2) {
                const int major = nums.at(0).toInt();
                const int minor = nums.at(1).toInt();
                modern = (major > 2) || (major == 2 && minor >= 19);
            }
        }
    }
    memo.insert(pandoc, modern);
    return modern;
}

bool fileReady(const QString &path)
{
    const QFileInfo fi(path);
    return fi.exists() && fi.isFile() && fi.size() > 0;
}

int timeoutFor(const QFileInfo &fi, const QString &target, const QString &ext)
{
    const bool heavy = (target == "pdf") || (ext == "pdf");
    const double mb = double(fi.size()) / (1024.0 * 1024.0);
    const int baseMs = heavy ? 300000 : 120000;   // 5 分钟 / 2 分钟
    const int perMb = heavy ? 6000 : 3000;        // 每 MB 追加
    const int cap = 20 * 60 * 1000;
    const int ms = baseMs + int(mb * perMb);
    return qMin(ms, cap);
}

bool moveFile(const QString &from, const QString &to)
{
    if (QFile::rename(from, to))
        return true;
    // 跨盘符时 rename 会失败，退化成复制 + 删除
    if (QFile::copy(from, to)) {
        QFile::remove(from);
        return true;
    }
    return false;
}

bool copyDirRecursive(const QString &from, const QString &to)
{
    QDir src(from);
    if (!src.exists())
        return false;
    if (!QDir().mkpath(to))
        return false;

    const QFileInfoList entries = src.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &e : entries) {
        const QString dest = QDir(to).filePath(e.fileName());
        if (e.isDir()) {
            if (!copyDirRecursive(e.absoluteFilePath(), dest))
                return false;
        } else if (!QFile::copy(e.absoluteFilePath(), dest)) {
            return false;
        }
    }
    return true;
}

} // namespace

// ===========================================================================
// 对外接口
// ===========================================================================
QString Converter::pandocPath()
{
    QMutexLocker locker(&engineMutex());
    ensureProbed();
    return cache().pandoc;
}

QString Converter::sofficePath()
{
    QMutexLocker locker(&engineMutex());
    ensureProbed();
    return cache().soffice;
}

QString Converter::pythonPath()
{
    QMutexLocker locker(&engineMutex());
    ensureProbed();
    return cache().python;
}

bool Converter::pdf2docxAvailable()
{
    QMutexLocker locker(&engineMutex());
    ensureProbed();
    return cache().pdf2docx;
}

void Converter::refreshEngines()
{
#ifdef Q_OS_WIN
    reloadPathFromRegistry();
#endif
    QMutexLocker locker(&engineMutex());
    cache() = EngineCache();     // 清空，下次访问重新探测
}

QStringList Converter::supportedTargetFormats()
{
    // 顺序即下拉框顺序：常用的放前面
    return {"pdf", "docx", "doc", "odt", "rtf", "txt", "html", "md", "epub",
            "pptx", "tex", "rst", "org", "adoc"};
}

QStringList Converter::supportedSourceExtensions()
{
    // 不含通配符、不含点，UI 自己拼 *.xxx
    return {"pdf", "docx", "doc", "odt", "rtf", "txt", "html", "htm",
            "md", "markdown", "epub", "pptx", "tex", "rst", "org",
            "adoc", "asciidoc"};
}

bool Converter::isSupportedSource(const QString &path)
{
    const QString ext = normalizeExt(QFileInfo(path).suffix().toLower());
    static const QStringList known = {
        "pdf", "docx", "doc", "odt", "rtf", "txt", "html", "md", "epub",
        "pptx", "tex", "rst", "org", "adoc"
    };
    return known.contains(ext);
}

QString Converter::uniqueOutputPath(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.exists())
        return path;

    const QString dir = fi.absolutePath();
    const QString base = fi.completeBaseName();
    const QString suffix = fi.suffix();
    for (int i = 1; i < 10000; ++i) {
        const QString candidate = QDir(dir).filePath(
            suffix.isEmpty() ? QString("%1 (%2)").arg(base).arg(i)
                             : QString("%1 (%2).%3").arg(base).arg(i).arg(suffix));
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    return path;
}

Converter::Result Converter::runProcess(const QString &program,
                                        const QStringList &args,
                                        int timeoutMs,
                                        CancelToken *token,
                                        const QString &workingDir)
{
    Result r;
    if (program.isEmpty()) {
        r.message = "内部错误：未指定要执行的程序";
        return r;
    }
    if (token && token->isCanceled()) {
        r.canceled = true;
        r.message = "已取消";
        return r;
    }

    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    if (!workingDir.isEmpty())
        p.setWorkingDirectory(workingDir);
    p.setStandardInputFile(QProcess::nullDevice());
    p.start();

    if (!p.waitForStarted(8000)) {
        r.message = QString("无法启动程序：%1").arg(QFileInfo(program).fileName());
        return r;
    }

    // 边跑边把输出读走：pdf2docx 的进度条、pandoc 的警告都可能刷出大量内容，
    // 不排空会让缓冲区无限膨胀。
    QByteArray out, err;
    QElapsedTimer timer;
    timer.start();

    auto drain = [&]() {
        out += p.readAllStandardOutput();
        err += p.readAllStandardError();
        const int keep = 64 * 1024;
        if (out.size() > keep)
            out = out.right(keep);
        if (err.size() > keep)
            err = err.right(keep);
    };

    while (p.state() != QProcess::NotRunning) {
        if (p.waitForFinished(100))
            break;
        drain();

        if (token && token->isCanceled()) {
            terminateProcessTree(p);
            r.canceled = true;
            r.message = "已取消";
            return r;
        }
        if (timer.elapsed() > timeoutMs) {
            terminateProcessTree(p);
            r.message = QString("处理超时（超过 %1 秒），文件可能过大或转换引擎无响应")
                            .arg(timeoutMs / 1000);
            return r;
        }
    }
    drain();
    r.output = (QString::fromUtf8(out) + QString::fromUtf8(err)).right(4000);

    if (p.exitStatus() == QProcess::CrashExit) {
        QString detail = QString::fromUtf8(err).trimmed();
        r.message = detail.isEmpty() ? QString("外部程序异常退出")
                                     : ("外部程序异常退出：" + detail.right(400));
        return r;
    }
    if (p.exitCode() != 0) {
        // pandoc / python 的输出都是 UTF-8，用 fromLocal8Bit 会得到乱码
        QString detail = QString::fromUtf8(err).trimmed();
        if (detail.isEmpty())
            detail = QString::fromUtf8(out).trimmed();
        if (detail.size() > 400)
            detail = detail.right(400);
        r.message = QString("转换失败（退出码 %1）%2")
                        .arg(p.exitCode())
                        .arg(detail.isEmpty() ? QString() : "：" + detail);
        return r;
    }

    r.success = true;
    return r;
}

Converter::Result Converter::convertFile(const QString &inputPath,
                                         const QString &targetFormat,
                                         const QString &outputDir,
                                         CancelToken *token)
{
    Result result;

    const QFileInfo fi(inputPath);
    if (!fi.exists() || !fi.isFile()) {
        result.message = "源文件不存在：" + inputPath;
        return result;
    }

    const QString ext = normalizeExt(fi.suffix().toLower());
    const QString target = normalizeExt(targetFormat.toLower());
    if (ext.isEmpty()) {
        result.message = "无法识别源文件格式（没有扩展名）";
        return result;
    }
    if (ext == target) {
        result.skipped = true;
        result.message = "源格式与目标格式相同，已跳过";
        return result;
    }
    if (!isSupportedSource(inputPath)) {
        result.message = QString("不支持的源格式：.%1").arg(ext);
        return result;
    }

    QDir outDir(outputDir);
    if (!outDir.exists() && !outDir.mkpath(".")) {
        result.message = "输出目录不可用，且无法创建：" + outputDir;
        return result;
    }

    const QString pandoc = pandocPath();
    const QString soffice = sofficePath();
    const QString base = fi.completeBaseName();
    const bool targetIsPdf = (target == "pdf");
    const bool sourceIsPdf = (ext == "pdf");

    if (pandoc.isEmpty() && soffice.isEmpty()) {
        result.message = "未检测到 Pandoc 和 LibreOffice，无法转换。请先点击「一键安装缺失组件」。";
        return result;
    }

    // 先确定最终落地的文件名，这样抽取图片的目录名才能和 markdown 里的相对链接对齐
    const QString finalPath = uniqueOutputPath(outDir.filePath(base + "." + target));
    const QString finalBase = QFileInfo(finalPath).completeBaseName();

    // 所有转换都在暂存目录里完成，校验通过后才搬进输出目录
    QTemporaryDir staging;
    if (!staging.isValid()) {
        result.message = "无法创建临时目录";
        return result;
    }

    const int timeoutMs = timeoutFor(fi, target, ext);
    const QString stagedOut = staging.filePath(finalBase + "." + target);
    const QString mediaDirName = finalBase + "_media";

    QStringList notes;      // 降级 / 重试之类的补充说明
    QString produced;       // 暂存目录中最终产物的路径

    // --- 用 LibreOffice 转换 ---
    auto sofficeConvert = [&](const QString &input, const QString &fmt,
                              const QString &destDir, const QString &infilter) -> Result {
        if (soffice.isEmpty()) {
            Result r;
            r.message = "未检测到 LibreOffice（soffice），无法完成该转换";
            return r;
        }
        // 关键：给每次调用一个独立的用户配置目录。
        // 否则用户自己开着 LibreOffice 时，--convert-to 会被转发给已运行的实例，
        // 命令立刻退出（退出码 0）但根本不产出文件。
        QTemporaryDir profile;
        QStringList args = {"--headless", "--norestore", "--invisible"};
        if (profile.isValid())
            args << ("-env:UserInstallation=" + QUrl::fromLocalFile(profile.path()).toString());
        if (!infilter.isEmpty())
            args << ("--infilter=" + infilter);
        args << "--convert-to" << fmt << "--outdir" << destDir << input;
        return runProcess(soffice, args, timeoutMs, token);
    };

    // --- 用 Pandoc 转换 ---
    auto pandocConvert = [&](const QString &input, const QString &output,
                             const QString &writer) -> Result {
        if (pandoc.isEmpty()) {
            Result r;
            r.message = "未检测到 Pandoc，无法完成该转换";
            return r;
        }
        QStringList args = {input, "-o", output};
        if (!writer.isEmpty())
            args << "-t" << writer;

        if (writer == "html") {
            // 不加 -s 得到的是 HTML 片段：没有 <head>、没有样式，浏览器打开一片白。
            args << "-s";
            args << (pandocUsesEmbedResources(pandoc) ? "--embed-resources"
                                                      : "--self-contained");
            args << "--metadata" << ("title=" + finalBase);
        } else if (writer == "latex" || writer == "rst" || writer == "org"
                   || writer == "asciidoc") {
            // 同样是片段问题：LaTeX 缺 \documentclass、rst 缺标题，
            // 拿去编译/发布都得自己补，加 -s 才是能直接用的完整文档。
            args << "-s" << "--metadata" << ("title=" + finalBase);
        } else if (writer == "markdown") {
            // 把 docx/pdf 里的图片抽出来，否则 markdown 里只剩指向不存在文件的链接。
            // 这里必须用相对路径（配合下面的工作目录），否则 pandoc 会把链接写成
            // 临时目录的绝对路径，文件搬到输出目录后链接就全断了。
            args << ("--extract-media=" + mediaDirName);
        }
        // 工作目录设成暂存目录：-o 仍然用绝对路径，
        // 但 --extract-media 的相对路径会落在暂存目录里
        return runProcess(pandoc, args, timeoutMs, token, staging.path());
    };

    // -----------------------------------------------------------------
    // 情况一：目标是 PDF
    // -----------------------------------------------------------------
    if (targetIsPdf) {
        // md / txt / epub / tex / rst / org / adoc 直接转 PDF 依赖 LaTeX 环境，
        // 成功率不稳定；先用 Pandoc 转成 docx，再交给 LibreOffice 排版成 PDF。
        if (isMarkupSource(ext)) {
            const QString mid = staging.filePath("_dc_stage.docx");
            Result r1 = pandocConvert(inputPath, mid, "docx");
            if (r1.canceled)
                return r1;
            if (!r1.success) {
                notes << ("Pandoc 转 docx 失败：" + r1.message);
            } else if (!fileReady(mid)) {
                notes << "Pandoc 未生成中间 docx";
            } else {
                Result r2 = sofficeConvert(mid, "pdf", staging.path(), QString());
                if (r2.canceled)
                    return r2;
                if (!r2.success) {
                    r2.message = notes.join("；") + "；LibreOffice 转 PDF 失败：" + r2.message;
                    return r2;
                }
                produced = staging.filePath("_dc_stage.pdf");
            }
        }

        // 其余格式（docx/doc/rtf/html/pdf），以及上面 Pandoc 路线失败的情况，
        // 都再交给 LibreOffice 直转一次
        if (produced.isEmpty()) {
            Result r = sofficeConvert(inputPath, "pdf", staging.path(), QString());
            if (r.canceled)
                return r;
            if (r.success) {
                produced = staging.filePath(base + ".pdf");
            } else {
                result.message = notes.isEmpty()
                                     ? r.message
                                     : (notes.join("；") + "；" + r.message);
                return result;
            }
        }
    }
    // -----------------------------------------------------------------
    // 情况二：源文件是 PDF
    // -----------------------------------------------------------------
    else if (sourceIsPdf) {
        // LibreOffice 默认用 Draw 打开 PDF，Draw 没有导出 docx/txt 的能力，
        // 会报 "no export filter"，必须显式指定 writer_pdf_import。
        const QString infilter = "writer_pdf_import";

        // PDF → docx 是所有「PDF 转文档/文本」的公共第一步。
        // 优先 pdf2docx：它能把正文还原成真正的段落；LibreOffice 的 PDF 导入
        // 会把正文塞进文本框，Pandoc 读 docx 时直接看不到（只剩标题）。
        // pdf2docx 没装或失败时再退回 LibreOffice，保证链路还能走通。
        auto pdfToDocx = [&](const QString &dest) -> Result {
            Result ok;

            if (pdf2docxAvailable()) {
                const QString python = pythonPath();
                const QString script = QCoreApplication::applicationDirPath()
                                       + "/scripts/pdf2docx_convert.py";
                if (!python.isEmpty() && QFileInfo(script).isFile()) {
                    // -X utf8 让 python 的 stdout/stderr 也用 UTF-8，
                    // 否则 Windows 下默认 GBK，我们按 UTF-8 解码就会乱码
                    const QStringList args = {"-X", "utf8", "-u", script, inputPath, dest};
                    // pdf2docx 比命令行工具慢得多，超时给两倍
                    Result r = runProcess(python, args, timeoutMs * 2, token);
                    if (r.canceled) {
                        ok.canceled = true;
                        return ok;
                    }
                    if (r.success && fileReady(dest)) {
                        ok.success = true;
                        // 脚本在结果几乎没文字时会打 SCANNED_PDF 标记
                        if (r.output.contains("SCANNED_PDF"))
                            notes << "该 PDF 可能是扫描件（图片型），结果几乎不含文字，需要先 OCR";
                        return ok;
                    }
                    // 不再静默降级：把失败原因带出去，日志里能看到实际用了哪个引擎
                    notes << ("pdf2docx 失败，已退回 LibreOffice：" +
                              (r.message.isEmpty() ? QString("未产出文件") : r.message));
                    QFile::remove(dest);
                }
            }

            Result r = sofficeConvert(inputPath, "docx", staging.path(), infilter);
            if (r.canceled) {
                ok.canceled = true;
                return ok;
            }
            // LibreOffice 的输出名跟着输入基名走，不能自己编个名字
            const QString loOut = staging.filePath(base + ".docx");
            if (!r.success) {
                notes << ("PDF 转 docx 失败：" + r.message);
                return ok;
            }
            if (!fileReady(loOut)) {
                notes << "PDF 转 docx 未产出文件（可能是纯图片扫描件）";
                return ok;
            }
            if (loOut != dest && !moveFile(loOut, dest)) {
                notes << "PDF 转 docx 的中间文件无法重命名";
                return ok;
            }
            ok.success = true;
            return ok;
        };

        // 2.1 目标就是 docx
        if (target == "docx") {
            Result r = pdfToDocx(stagedOut);
            if (r.canceled)
                return r;
            if (r.success)
                produced = stagedOut;
        }

        // 2.2 Pandoc 写得出来的目标（md/html/epub/txt/tex/rst/org/adoc/pptx）：
        //     LibreOffice 只有 docx/txt/rtf 等少数导出过滤器，统一走
        //     pdf -> docx -> 目标(Pandoc)。
        //     失败时不直接放弃，落到 2.3 再让 LibreOffice 试一次，并把原因记在 notes 里。
        const QString pdfWriter = pandocWriterFor(target);
        if (produced.isEmpty() && !pdfWriter.isEmpty()
            && target != "docx" && target != "odt" && target != "rtf") {
            const QString mid = staging.filePath("_dc_pdf_stage.docx");
            Result r1 = pdfToDocx(mid);
            if (r1.canceled)
                return r1;

            if (r1.success) {
                Result r2 = pandocConvert(mid, stagedOut, pdfWriter);
                if (r2.canceled)
                    return r2;
                if (r2.success && fileReady(stagedOut)) {
                    produced = stagedOut;
                } else {
                    notes << ("Pandoc 转 " + target + " 失败：" +
                              (r2.message.isEmpty() ? QString("未产出文件") : r2.message));
                    QFile::remove(stagedOut);
                }
            }
        }

        // 2.3 其余目标格式交给 LibreOffice
        if (produced.isEmpty()) {
            Result r = sofficeConvert(inputPath, target, staging.path(), infilter);
            if (r.canceled)
                return r;
            if (!r.success) {
                r.message = QStringList(notes + QStringList{r.message}).join("；");
                return r;
            }
            produced = staging.filePath(base + "." + target);
        }
    }
    // -----------------------------------------------------------------
    // 情况三：普通文档互转，优先 Pandoc，失败回退 LibreOffice
    // -----------------------------------------------------------------
    else {
        const QString writer = pandocWriterFor(target);

        // pptx：Impress 只能导出演示格式 / PDF / 图片，写不出 docx 这类文档格式
        // （直接转会报 "no export filter"）。先落成 PDF，再复用整条「PDF → 目标」的链路。
        if (ext == "pptx") {
            Result r = sofficeConvert(inputPath, "pdf", staging.path(), QString());
            if (r.canceled)
                return r;
            if (!r.success) {
                result.message = r.message;
                return result;
            }
            const QString midPdf = staging.filePath(base + ".pdf");
            if (!fileReady(midPdf)) {
                result.message = "LibreOffice 未能把 PPTX 转成 PDF，无法继续转换";
                return result;
            }
            // 递归调用自己的「PDF 源」分支：输出目录用外层暂存目录，
            // 这样产物在本函数返回前一直有效
            Result sub = convertFile(midPdf, target, staging.path(), token);
            if (sub.canceled)
                return sub;
            if (!sub.success) {
                result.message = "PPTX 需要先转成 PDF 再转目标格式，中途失败：" + sub.message;
                return result;
            }
            produced = sub.outputPath;
            if (!sub.message.isEmpty())
                notes << sub.message;
            notes << "PPTX 已先按页转成 PDF，再转成目标格式";
        }

        // writer 为空说明 Pandoc 写不出这个格式（目前只有 doc），
        // 那就别白试一次，直接交给 LibreOffice。
        if (produced.isEmpty() && !writer.isEmpty() && pandocCanRead(ext)) {
            Result r = pandocConvert(inputPath, stagedOut, writer);
            if (r.canceled)
                return r;
            if (r.success && fileReady(stagedOut)) {
                produced = stagedOut;
            } else {
                if (!r.success)
                    notes << ("Pandoc 失败：" + r.message);
                else
                    notes << "Pandoc 未产出文件";
                QFile::remove(stagedOut);   // 清掉可能的半成品，避免干扰下一步
            }
        }

        if (produced.isEmpty()) {
            Result r2 = sofficeConvert(inputPath, target, staging.path(), QString());
            if (r2.canceled)
                return r2;
            if (!r2.success) {
                r2.message = notes.isEmpty()
                                 ? r2.message
                                 : (notes.join("；") + "；LibreOffice 失败：" + r2.message);
                return r2;
            }
            produced = staging.filePath(base + "." + target);
        }
    }

    // -----------------------------------------------------------------
    // 收尾：校验产物，然后以「不覆盖」的方式落到输出目录
    // -----------------------------------------------------------------
    if (produced.isEmpty() || !fileReady(produced)) {
        result.message = "转换已执行，但没有生成有效的输出文件"
                         "（该格式组合可能不被当前引擎支持）";
        if (!notes.isEmpty())
            result.message += "：" + notes.join("；");
        return result;
    }

    if (!moveFile(produced, finalPath)) {
        result.message = "无法写入输出目录：" + finalPath +
                         "（可能没有写入权限，或文件正被其他程序占用）";
        return result;
    }

    // markdown 抽出来的图片目录跟着一起搬，名字与最终文件名保持一致，
    // 这样 markdown 里的相对链接仍然有效
    const QString stagedMedia = staging.filePath(mediaDirName);
    if (QDir(stagedMedia).exists()) {
        const QString finalMedia = outDir.filePath(mediaDirName);
        if (!QDir(finalMedia).exists())
            copyDirRecursive(stagedMedia, finalMedia);
    }

    // 扫描件（图片型）PDF 转文字几乎必然是空的。
    // 与其让用户以为「转换坏了」，不如直接告诉他原因和出路。
    if (sourceIsPdf && fi.size() > 100 * 1024
        && (target == "txt" || target == "md" || target == "html")) {
        QFile f(finalPath);
        if (f.open(QIODevice::ReadOnly)) {
            QString text = QString::fromUtf8(f.readAll());
            f.close();
            static const QRegularExpression tags("<[^>]*>");
            static const QRegularExpression spaces("\\s");
            text.remove(tags);
            text.remove(spaces);
            if (text.size() < 40) {
                notes << "输出几乎不含文字：该 PDF 可能是扫描件（图片型），"
                         "本工具不含 OCR，需要先用 OCR 软件识别";
            }
        }
    }

    result.success = true;
    result.outputPath = finalPath;
    result.message = notes.join("；");
    return result;
}
