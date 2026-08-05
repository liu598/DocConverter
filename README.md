# 文档格式转换工具（DocConverter）

一个基于 C++ / Qt 的 Windows 桌面小工具，支持在常见文档格式之间批量转换：
`docx / doc / odt / rtf / txt / html / md / epub / pdf`

界面：文件列表（支持拖拽添加）+ 目标格式下拉框 + 输出目录 + 一键批量转换 + 实时日志。

## 工作原理

工具本身不重复造轮子去解析每一种文档格式，而是在后台调用两个成熟、**免费开源**的转换引擎：

- **Pandoc**：负责 txt / md / html / docx / odt / rtf / epub 之间的相互转换，速度快、保真度高。
- **LibreOffice（headless 模式）**：负责涉及 PDF 的转换（转入/转出），以及 Pandoc 不擅长的复杂排版文档。
- **pdf2docx（可选，Python库）**：专门针对 PDF → docx 做了排版还原，精度明显好于 LibreOffice 自带的 PDF 导入功能，适合"文字版"PDF（不是扫描图片版）。检测到就优先用它，检测不到自动退回 LibreOffice 方案。

程序启动时会自动检测这两个工具是否已安装，并在界面顶部显示检测状态。**这两个软件都需要你提前在电脑上装好**（都是免费的）：

- Pandoc 下载地址：https://pandoc.org/installing.html
- LibreOffice 下载地址：https://www.libreoffice.org/download/download/

装好之后建议保证它们在系统 PATH 中（Pandoc 安装程序一般会自动加，LibreOffice 需要手动确认 `soffice.exe` 所在的 `program` 目录），这样程序才能自动找到它们。

### 一键安装（推荐）

界面顶部有一个「一键安装缺失组件」按钮，点击后程序会通过 Windows 自带的包管理器 **winget** 自动静默下载安装缺失的 Pandoc / LibreOffice，不需要用户手动去官网下载。

前提：系统里要有 `winget`（Windows 10 1809 及以上、Windows 11 一般都自带；如果没有，可在「Microsoft Store」搜索安装「应用安装程序」App Installer 获得）。

安装过程可能需要几分钟（LibreOffice 安装包约 300MB），期间系统可能会弹出安全确认框，点击"是"放行即可。安装完成后程序会自动重新检测状态；如果状态没变绿，尝试重启程序或重启电脑让 PATH 生效。

## 编译方法（Windows）

需要准备：

1. **Qt 6**（建议 6.5 及以上），选择 `msvc2019_64` 或 `msvc2022_64` 组件，用 [Qt 在线安装器](https://www.qt.io/download-qt-installer) 安装。
2. **CMake**（3.16+）：https://cmake.org/download/
3. **Visual Studio 2019/2022**（勾选"使用 C++ 的桌面开发"工作负载），或单独的 MSVC 生成工具。

拿到本项目源码后，在项目根目录下打开"适用于 VS 的开发人员命令提示符"，执行：

```bat
mkdir build
cd build
cmake .. -DCMAKE_PREFIX_PATH="C:/Qt/6.7.0/msvc2019_64" -A x64
cmake --build . --config Release
```

（`C:/Qt/6.7.0/msvc2019_64` 换成你实际的 Qt 安装路径）

编译成功后，可执行文件在 `build/Release/DocConverter.exe`。

首次运行前，需要把 Qt 的运行库拷到同目录，最简单的办法是在 Qt 安装目录下找到 `windeployqt.exe`，然后执行：

```bat
C:\Qt\6.7.0\msvc2019_64\bin\windeployqt.exe build\Release\DocConverter.exe
```

它会自动把所需的 dll 都拷贝过去，之后这个文件夹就可以整体打包分发了。

## 已知限制

- PDF → 其他格式（尤其是 PDF → docx）依赖 LibreOffice 的解析能力，遇到复杂排版、扫描件（图片型PDF）效果有限，扫描件需要 OCR 才能提取文字，本工具暂不包含 OCR。
- Markdown → PDF 走的是 `md → docx（Pandoc）→ pdf（LibreOffice）` 的链路，样式较为朴素。
- 批量转换是顺序执行（一个个来），如果文件很多或很大，请耐心等待，过程中界面日志会实时显示进度。

## 图标

`resources/app_icon.ico` 是我给这个工具设计的图标（蓝色文档 + 橙色循环转换箭头，寓意"文档格式转换"），已经通过 `resources/app.rc`（exe文件图标）和 `resources/icons.qrc`（标题栏/任务栏图标）接入编译流程，编译出的 exe 会自带图标，无需额外操作。如果想换成自己的图标，直接替换 `resources/app_icon.ico` 这一个文件即可（保持文件名不变）。

## 目录结构

```
DocConverter/
├── CMakeLists.txt
├── README.md
└── src/
    ├── main.cpp          # 程序入口
    ├── mainwindow.h/.cpp # 主界面：文件列表、拖拽、进度、日志
    └── converter.h/.cpp  # 转换引擎：封装对 Pandoc / LibreOffice 的调用
```

## 后续可扩展方向

- 加入 OCR（如 Tesseract）支持扫描版 PDF 转文字。
- 并行转换多个文件以提速。
- 记住上次使用的目标格式和输出目录（QSettings）。
- 打包成安装程序（可用 Inno Setup 或 NSIS）。
