#include "installer.h"

#include <QProcess>
#include <QStandardPaths>

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

    QProcess process;
    const QStringList args = {
        "install", "--id", packageId, "-e",
        "--silent",
        "--accept-package-agreements",
        "--accept-source-agreements"
    };
    process.start("winget", args);

    if (!process.waitForStarted(5000)) {
        r.message = QString("无法启动 winget 来安装 %1").arg(displayName);
        return r;
    }

    // 安装包体积可能较大（尤其 LibreOffice 约 300MB），给足够长的超时时间
    if (!process.waitForFinished(10 * 60 * 1000)) {
        process.kill();
        r.message = QString("安装 %1 超时，请检查网络后重试，或手动下载安装").arg(displayName);
        return r;
    }

    const int code = process.exitCode();
    if (code != 0) {
        r.message = QString("安装 %1 可能未成功（退出码 %2）：%3")
                         .arg(displayName)
                         .arg(code)
                         .arg(QString::fromLocal8Bit(process.readAllStandardError()));
        return r;
    }

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

static bool verifyPythonWorks(const QString &exePath)
{
    QProcess p;
    p.start(exePath, {"--version"});
    if (!p.waitForStarted(3000))
        return false;
    if (!p.waitForFinished(5000))
        return false;
    return p.exitCode() == 0;
}

static QString findPythonExe()
{
    const QStringList candidates = {"python", "python3", "py"};
    for (const QString &c : candidates) {
        const QString p = QStandardPaths::findExecutable(c);
        if (!p.isEmpty() && verifyPythonWorks(p))
            return p;
    }
    return QString();
}

Installer::Result Installer::installPdf2docx()
{
    Result r;
    QString python = findPythonExe();

    if (python.isEmpty()) {
        // 本机没有 Python，先通过 winget 装一个
        const Result pyResult = runWingetInstall("Python.Python.3.12", "Python 3.12");
        if (!pyResult.success) {
            r.message = "安装 Python 失败，无法继续安装 pdf2docx：" + pyResult.message;
            return r;
        }
        python = findPythonExe();
        if (python.isEmpty()) {
            r.message = "Python 安装完成，但当前进程还检测不到（PATH 未刷新）。"
                         "请重启本程序（甚至重启电脑）后，再次点击安装按钮继续安装 pdf2docx。";
            return r;
        }
    }

    QProcess process;
    const QStringList args = {"-m", "pip", "install", "--user", "--upgrade", "pdf2docx"};
    process.start(python, args);

    if (!process.waitForStarted(5000)) {
        r.message = "无法启动 pip 来安装 pdf2docx";
        return r;
    }
    if (!process.waitForFinished(5 * 60 * 1000)) {
        process.kill();
        r.message = "安装 pdf2docx 超时";
        return r;
    }
    if (process.exitCode() != 0) {
        r.message = QString("安装 pdf2docx 失败：%1")
                        .arg(QString::fromLocal8Bit(process.readAllStandardError()));
        return r;
    }

    r.success = true;
    r.message = "pdf2docx 安装完成（用于提升 PDF 转 Word 的精度）";
    return r;
}
