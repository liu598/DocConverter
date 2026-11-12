#include <QApplication>
#include <QIcon>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QString>

#include "mainwindow.h"
#include "DocConverterVersion.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // applicationName 决定 QSettings 的注册表位置和配置目录，
    // 这里刻意用 ASCII 名字，避免中文路径带来的各种边界问题。
    app.setOrganizationName("LocalTools");
    app.setApplicationName("DocConverter");
    app.setApplicationVersion(DOC_CONVERTER_VERSION);
    // 标题栏会显示成「文档格式转换工具 — v1.4.0」，displayName 只补版本号，
    // 避免和 windowTitle 重复两遍名字
    app.setApplicationDisplayName(QString("v%1").arg(DOC_CONVERTER_VERSION));
    app.setWindowIcon(QIcon(":/icons/app_icon.ico"));

    // 单实例：双开会让两个进程同时往同一个输出目录写文件，
    // 而且「记住上次设置」也会互相覆盖。
    // QLockFile 会自动识别进程崩溃后残留的陈旧锁文件，不会把人锁在门外。
    QLockFile lock(QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                   + "/DocConverter_single_instance.lock");
    if (!lock.tryLock(100)) {
        QMessageBox::information(nullptr, "文档格式转换工具",
            "程序已经在运行了。\n\n如果没看到窗口，请检查一下任务栏。");
        return 0;
    }

    MainWindow window;
    window.show();

    return app.exec();
}
