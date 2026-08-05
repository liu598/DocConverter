#include <QApplication>
#include <QIcon>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("文档格式转换工具");
    app.setOrganizationName("LocalTools");
    app.setWindowIcon(QIcon(":/icons/app_icon.ico"));

    MainWindow window;
    window.show();

    return app.exec();
}
