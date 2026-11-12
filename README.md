# 文档格式转换工具（DocConverter）

一个基于 C++ / Qt 6 的 Windows 桌面工具，支持在常见文档格式之间批量转换：
`docx / doc / odt / rtf / txt / html / md / epub / pdf`

工具本身不解析任何文档格式，而是调度两个成熟的开源引擎（Pandoc、LibreOffice），
外加可选的 pdf2docx，把「格式组合」交给最擅长的那个引擎去做。

## 界面能力

- **拖拽即用**：拖文件、拖整个文件夹（递归查找支持的格式）都行
- **批量转换**：列表内逐个处理，每项独立标记成功 / 跳过 / 失败，鼠标悬停看详情
- **可取消**：转换在后台线程跑，随时点「取消」，界面不卡死
- **两种输出位置**：指定目录，或「输出到源文件所在目录」
- **不覆盖已有文件**：同名时自动生成 `报告 (1).pdf`，绝不静默盖掉你的文件
- **记住上次设置**：目标格式、输出目录、窗口位置都会记忆（QSettings）
- **实时日志**：带时间戳，失败原因（引擎原始报错）会完整打印出来，可一键「保存日志」为 txt
- **单实例运行**：重复启动不会开出第二个窗口，避免两个实例同时写同一个输出目录
- **扫描件会提醒**：PDF 转出的文字几乎为空时，日志会明确提示「可能是图片型扫描件，需要先 OCR」

## 转换链路

| 源 → 目标 | 走的路径 |
|---|---|
| 任意文档 → PDF | LibreOffice 直接转；md / txt / epub / odt 先由 Pandoc 转 docx 再排版成 PDF（避开 LaTeX 依赖） |
| PDF → docx | 装了 pdf2docx 就用它（排版还原好），失败自动退回 LibreOffice |
| PDF → md / html / epub | PDF → docx（LibreOffice）→ 目标格式（Pandoc） |
| PDF → txt / odt / rtf 等 | LibreOffice + `writer_pdf_import` 过滤器 |
| 其余文档互转 | 优先 Pandoc（快、保真），失败回退 LibreOffice |

几个容易踩的坑已经处理掉了：

- PDF 必须显式指定 `writer_pdf_import`，否则 LibreOffice 用 Draw 打开，报 `no export filter`
- HTML 输出加了 `--standalone --embed-resources`，得到带样式的完整页面并把图片内嵌进去（原来是缺 `<head>` 的裸片段）
- 转 Markdown 时用 `--extract-media` 把图片抽到 `文件名_media/` 目录，链接不会失效
- LibreOffice 每次调用都给独立的 `-env:UserInstallation` 配置目录，**你开着 LibreOffice 也不影响转换**
- 转换结果先落临时目录，**校验文件确实存在且非空**才搬进输出目录（LibreOffice 有时退出码 0 但什么都没生成）

## 依赖的引擎

程序启动后会在后台检测，界面顶部显示状态：

- **Pandoc**：https://pandoc.org/installing.html
- **LibreOffice**：https://www.libreoffice.org/download/download/
- **pdf2docx**（可选，提升 PDF→Word 精度）：`pip install pdf2docx`

路径探测顺序：系统 PATH → 常见安装位置 → winget 包目录（`%LOCALAPPDATA%\Microsoft\WinGet\Packages`）。

**一键安装**：点界面上的「一键安装缺失组件」，会通过 winget 静默安装缺的那个。
安装跑在后台线程，日志实时滚动、界面不假死；装完会重新读取注册表里的 PATH 并清掉引擎缓存，**不用重启程序**即可识别。

## 编译

需要 Qt 6.2+、CMake 3.21+、MSVC 2022（或 VS 生成工具）。

**最省事的方式**——一键脚本会自动找 Qt / MSVC / Windows SDK，编译、跑测试、部署、打包全做完：

```powershell
.\scripts\build_release.ps1                 # 只编译 + 部署到 .\deploy
.\scripts\build_release.ps1 -Test -Package  # 编译 + CTest + 部署进安装包目录 + 重新打包
.\scripts\build_release.ps1 -Clean          # 先清空构建目录再编译
```

如果工具装在非标准位置，用 `-QtPath D:\Qt\6.11.1\msvc2022_64` 显式指定。

也可以手动来：

```bat
:: 方式一：用预设（推荐，路径按需修改 CMakePresets.json）
cmake --preset windows-msvc
cmake --build --preset release

:: 方式二：手动指定
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_PREFIX_PATH="D:/Qt/6.11.1/msvc2022_64"
cmake --build build
```

产物在 `build/windows-msvc/DocConverter.exe`。本地直接运行需要 Qt 运行库，用 `windeployqt` 或直接走下面的安装步骤。

### 测试

```bat
ctest --test-dir build/windows-msvc --output-on-failure
```

`tests/test_converter.cpp` 覆盖转换链路：同格式跳过、目标文件不存在、唯一命名（`a (1).docx`）、
md→docx、docx→html 是否含完整 `<head>`、取消令牌等。纯逻辑用例任何机器都能跑；
需要真实引擎的用例在本机没装 Pandoc / LibreOffice 时会自动跳过，不会误报失败。

## 打包成安装目录

```bat
cmake --install build/windows-msvc --prefix D:/out/DocConverter
```

这一步会：

1. 拷贝 exe 与 `scripts/`
2. 调 `windeployqt` 部署 Qt 运行时（扁平布局，dll 与 exe 同级）
3. **只带简体中文翻译**，跳过 20MB 的软件 OpenGL 后备实现和 D3D 着色器编译器
4. 剔除用不到的 Qt6Network 及其插件
5. 带上 VC++ 运行库（msvcp140 / vcruntime140），目标机器没装 VC++ Redistributable 也能跑

产出约 **34MB**（旧版本是 62MB），且目录里没有任何 CMake 中间文件。

## 生成安装包

用 Qt Installer Framework：

```bat
cd DocConverterInstaller
D:\Qt\Tools\QtInstallerFramework\4.11\bin\binarycreator.exe ^
    -c config/config.xml -p packages DocConverter_Setup.exe
```

`packages/com.docconverter.app/data` 就是上一步 `cmake --install` 的输出目录，
`meta/package.xml` 与 `meta/installscript.qs` 定义安装行为（桌面 + 开始菜单快捷方式）。

版本号只有一处来源：`DocConverter/CMakeLists.txt` 里的 `project(DocConverter VERSION x.y.z)`。
它会同时作用于 exe 的版本资源、程序标题和「属性 → 详细信息」。

## 已知限制

- **扫描件 PDF（图片型）** 没有 OCR，转出来基本是空的——日志会提示需要先做 OCR，但工具本身不提供识别能力。
- Markdown → PDF 的样式比较朴素（Pandoc 生成 docx 的默认样式）。
- 批量转换是**串行**的：LibreOffice 本身不适合并发起多个实例，串行反而更快更稳。
- 没有代码签名，首次运行可能被 SmartScreen 拦一下（点「更多信息 → 仍要运行」）。

## 目录结构

```
DocConverter/
├── CMakeLists.txt              # 版本号唯一来源 + 构建/部署规则
├── CMakePresets.json           # 三套预设：Ninja Release / Debug / VS2022
├── cmake/DeployApp.cmake.in    # cmake --install 时的部署与裁剪脚本
├── resources/
│   ├── app.rc.in               # 图标 + VERSIONINFO（由 CMake 生成）
│   ├── app_icon.ico
│   └── icons.qrc
├── scripts/
│   ├── pdf2docx_convert.py     # 调 pdf2docx 的薄封装
│   └── build_release.ps1       # 一键编译 / 测试 / 部署 / 打包
├── tests/
│   ├── CMakeLists.txt          # CTest 目标
│   └── test_converter.cpp      # 转换链路用例
└── src/
    ├── main.cpp                # 入口，应用元信息 + 单实例锁
    ├── version.h.in            # 版本宏（由 CMake 生成）
    ├── mainwindow.h/.cpp       # 界面：列表、进度、日志、设置、拖拽
    ├── converterworker.h/.cpp  # 后台线程里执行批量转换
    ├── installerworker.h/.cpp  # 后台线程里跑 winget 安装
    ├── converter.h/.cpp        # 引擎探测 + 转换链路 + 产物校验
    └── installer.h/.cpp        # winget 安装与 PATH 刷新
```

## 变更记录

### 1.1.0
- 组件安装改为后台线程：winget 装 LibreOffice 期间界面不再假死，日志实时输出
- 新增单实例锁（QLockFile），重复启动不会开出第二个窗口互相覆盖输出
- PDF 转出内容几乎为空时提示「可能是扫描件，需要 OCR」
- 日志支持一键保存为 txt，反馈问题时可直接附上
- 测试工程化接入 CTest，无引擎的机器自动跳过集成用例
- 新增 `scripts/build_release.ps1`：自动发现工具链，一条命令走完编译→测试→部署→打包
- 部署脚本会清掉 windeployqt 在 `bin/` 下重复铺的一份 VC 运行库，最终目录里只有根目录那一套
- 版本号提升到 1.1.0（exe 属性、安装包、标题栏同步）

### 1.0.1
- 转换改为后台线程执行，界面不再假死，支持中途取消
- 输出不再覆盖同名文件，改为自动加序号
- 转换后校验产物真实存在，避免「显示成功但没生成文件」
- LibreOffice 使用独立配置目录，不再受用户已打开的 LibreOffice 干扰
- 修正错误信息乱码（stderr 按 UTF-8 解码）
- PDF → md/html/epub 走 docx 中转而不再失败
- HTML 输出为完整独立文档，图片内嵌；Markdown 导出自动抽图
- 支持拖入文件夹、记住上次设置、打开输出目录
- 安装组件后自动刷新 PATH，无需重启程序
- 安装包从 62MB 精简到 34MB，清理掉混入的 CMake 构建产物
- 修复 `CMakePresets.json` 内容被无关代码污染的问题
