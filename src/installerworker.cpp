#include "installerworker.h"

#include "converter.h"
#include "installer.h"

#include <functional>

InstallerWorker::InstallerWorker(bool needPandoc,
                                 bool needSoffice,
                                 bool needPdf2docx,
                                 QObject *parent)
    : QObject(parent)
    , m_needPandoc(needPandoc)
    , m_needSoffice(needSoffice)
    , m_needPdf2docx(needPdf2docx)
{
}

void InstallerWorker::run()
{
    int succeeded = 0;
    int failed = 0;

    auto doStep = [&](const QString &title, const std::function<Installer::Result()> &fn) {
        emit logMessage(title);
        const Installer::Result r = fn();
        if (r.success)
            ++succeeded;
        else
            ++failed;
        emit stepResult(r.success, r.message);
    };

    if (m_needPandoc)
        doStep("开始安装 Pandoc…", [] { return Installer::installPandoc(); });

    if (m_needSoffice)
        doStep("开始安装 LibreOffice（约 300MB，请耐心等待）…",
               [] { return Installer::installLibreOffice(); });

    if (m_needPdf2docx)
        doStep("开始安装 Python + pdf2docx（提升 PDF 转 Word 精度）…",
               [] { return Installer::installPdf2docx(); });

    // 清掉引擎路径缓存并刷新 PATH，主线程随后的状态检测才能看到新装的组件
    Converter::refreshEngines();

    emit logMessage("安装流程结束，重新检测组件状态…");
    emit finished(succeeded, failed);
}
