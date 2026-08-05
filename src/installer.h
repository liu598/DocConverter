#pragma once
#include <QString>

// Installer 负责在缺少 Pandoc / LibreOffice 时，
// 通过 Windows 自带的包管理器 winget 自动静默安装。
// winget 是微软官方工具，Windows 10(1809+)/11 基本自带，
// 若没有可在微软商店搜索安装「应用安装程序」获得。
class Installer
{
public:
    struct Result
    {
        bool success = false;
        QString message;
    };

    // 检测系统是否有 winget
    static bool isWingetAvailable();

    static Result installPandoc();
    static Result installLibreOffice();
    static Result installPdf2docx();

private:
    static Result runWingetInstall(const QString &packageId, const QString &displayName);
};
