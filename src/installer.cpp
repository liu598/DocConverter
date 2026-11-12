#include "installer.h"
#include "converter.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace {

// 边跑边排空输出：winget / pip 都会输出大量进度信息，
// 不读走会让缓冲区无限膨胀。
int runAndCollect(const QString &program,
                  const QStringList &args,
                  int timeoutMs,
                  QString *mergedOutput)
{
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.setStandardInputFile(QProcess::nullDevice());
    p.start();

    if (!p.waitForStarted(8000))
        return -1;

    QByteArray buffer;
    QElapsedTimer timer;
    timer.start();

    while (p.state() != QProcess::NotRunning) {
        if (p.waitForFinished(200))
            break;
        buffer += p.readAllStandardOutput();
        buffer += p.readAllStandardError();
        if (buffer.size() > 128 * 1024)
            buffer = buffer.right(64 * 1024);
        if (timer.elapsed() > timeoutMs) {
            p.kill();
            p.waitForFinished(2000);
            if (mergedOutput)
                *mergedOutput = QString::fromUtf8(buffer.right(800));
            return -2;   // 超时
        }
    }
    buffer += p.readAllStandardOutput();
    buffer += p.readAllStandardError();

    if (mergedOutput)
        *mergedOutput = QString::fromUtf8(buffer.right(800));

    return p.exitStatus() == QProcess::CrashExit ? -3 : p.exitCode();
}

// 把 winget / pip 的尾部输出整理成一行可读的原因
QString tailLine(const QString &output)
{
    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QString line = lines.at(i).trimmed();
        // 过滤掉 winget 进度条（█ 字符）和「正在…」这类无信息量的行
        if (!line.isEmpty() && !line.startsWith('-')
            && !line.contains(QChar(0x2588))
            && !line.contains(QStringLiteral("正在")) && line.size() > 3)
            return line.right(200);
    }
    return QString();
}

} // namespace

bool Installer::isWingetAvailable()
{
    return !QStandardPaths::findExecutable("winget").isEmpty();
}

Installer::Result Installer::runWingetInstall(const QString &packageId, const QString &displayName)
{
    Result r;

    if (!isWingetAvailable()) {
        r.message = QString("未检测到 winget（Windows 包管理器），无法自动安装 %1。"
                            "请到微软商店搜索安装「应用安装程序」，或手动下载安装。")
                        .arg(displayName);
        return r;
    }

    const QStringList args = {
        "install", "--id", packageId, "-e",
        "--silent",
        "--accept-package-agreements",
        "--accept-source-agreements",
        "--disable-interactivity"
    };

    QString output;
    // 安装包体积可能很大（LibreOffice 约 300MB），给足 15 分钟
    const int code = runAndCollect("winget", args, 15 * 60 * 1000, &output);

    if (code == -1) {
        r.message = QString("无法启动 winget 来安装 %1").arg(displayName);
        return r;
    }
    if (code == -2) {
        r.message = QString("安装 %1 超时，请检查网络后重试，或手动下载安装").arg(displayName);
        return r;
    }
    // winget 用 0x8A150061 等 HRESULT 表示「已安装最新版」，这种情况算成功
    const bool alreadyInstalled = output.contains(QStringLiteral("已安装"), Qt::CaseInsensitive)
                                  || output.contains(QStringLiteral("already installed"),
                                                     Qt::CaseInsensitive);
    if (code != 0 && !alreadyInstalled) {
        r.message = QString("安装 %1 失败（退出码 %2）：%3")
                        .arg(displayName)
                        .arg(code)
                        .arg(tailLine(output));
        return r;
    }

    // 装完立刻刷新：winget 只改了注册表，本进程的 PATH 还是旧值，
    // 不刷新的话紧接着的状态检测依然会显示「未找到」。
    Converter::refreshEngines();

    r.success = true;
    r.message = QString("%1 安装完成").arg(displayName);
    return r;
}

Installer::Result Installer::installPandoc()
{
    return runWingetInstall("JohnMacFarlane.Pandoc", "Pandoc");
}

Installer::Result Installer::installLibreOffice()
{
    return runWingetInstall("TheDocumentFoundation.LibreOffice", "LibreOffice");
}

Installer::Result Installer::installPdf2docx()
{
    Result r;

    // 复用 Converter 的探测逻辑，不再维护第二份 Python 查找代码
    QString python = Converter::pythonPath();

    if (python.isEmpty()) {
        const Result pyResult = runWingetInstall("Python.Python.3.12", "Python 3.12");
        if (!pyResult.success) {
            r.message = "安装 Python 失败，无法继续安装 pdf2docx：" + pyResult.message;
            return r;
        }
        Converter::refreshEngines();
        python = Converter::pythonPath();
        if (python.isEmpty()) {
            r.message = "Python 已安装，但当前进程还找不到它（环境变量未生效）。"
                        "请重启本程序（必要时重启电脑）后再点一次安装。";
            return r;
        }
    }

    const QStringList baseArgs = {
        "-m", "pip", "install", "--user", "--upgrade",
        "--disable-pip-version-check", "--no-warn-script-location"
    };

    QString output;
    int code = runAndCollect(python, QStringList(baseArgs) << "pdf2docx",
                             10 * 60 * 1000, &output);

    // 国内网络访问 pypi.org 经常超时，失败后自动换清华镜像再试一次
    if (code != 0) {
        const QStringList mirrorArgs = QStringList(baseArgs)
                                       << "-i" << "https://pypi.tuna.tsinghua.edu.cn/simple"
                                       << "pdf2docx";
        QString mirrorOutput;
        const int mirrorCode = runAndCollect(python, mirrorArgs,
                                             10 * 60 * 1000, &mirrorOutput);
        if (mirrorCode == -2) {
            r.message = "安装 pdf2docx 超时（已尝试官方源和清华镜像），请检查网络";
            return r;
        }
        if (mirrorCode != 0) {
            r.message = QString("安装 pdf2docx 失败：%1").arg(tailLine(mirrorOutput));
            return r;
        }
        code = 0;
        output = mirrorOutput;
    }

    if (code == -2) {
        r.message = "安装 pdf2docx 超时，请检查网络后重试";
        return r;
    }

    r.success = true;
    r.message = "pdf2docx 安装完成（用于提升 PDF 转 Word 的精度）";
    return r;
}
