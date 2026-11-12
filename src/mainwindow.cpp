#include "mainwindow.h"
#include "converterworker.h"
#include "installerworker.h"
#include "installer.h"
#include "previewdialog.h"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileInfoList>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleHints>
#include <QTableWidget>
#include <QThread>
#include <QTime>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace {

// 文件在列表里的处理状态
enum ItemState {
    StatePending = 0,
    StateRunning,
    StateOk,
    StateFailed,
    StateSkipped
};

const char *kSettingGeometry = "window/geometry";
const char *kSettingFormat = "convert/targetFormat";
const char *kSettingOutputDir = "convert/outputDir";
const char *kSettingOutputToSource = "convert/outputToSource";
const char *kSettingTheme = "ui/theme";
const char *kSettingAdvancedOpen = "ui/advancedOpen";
const QString kSettingProfiles = "profiles";
const char *kSettingLastProfile = "profiles/last";
const char *kOptionsNode = "options";

QString desktopDir()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (dir.isEmpty())
        dir = QDir::homePath();
    return dir;
}

QString humanSize(qint64 bytes)
{
    if (bytes < 1024)
        return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

QString historyFilePath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + "/.DocConverter";
    QDir().mkpath(dir);
    return dir + "/history.json";
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle("文档格式转换工具");
    setAcceptDrops(true);
    resize(900, 800);

    buildUi();
    buildMenus();
    loadSettings();
    loadHistory();

    appendLog("就绪。把文件（或文件夹）拖进窗口，选择目标格式后点「开始转换」。"
              "双击列表里的文件可以先预览内容。");
    QMetaObject::invokeMethod(this, &MainWindow::startEngineCheck, Qt::QueuedConnection);
}

MainWindow::~MainWindow()
{
    if (m_workerThread && m_workerThread->isRunning()) {
        m_cancelToken.cancel();
        m_workerThread->wait(15000);
    }
    if (m_engineThread && m_engineThread->isRunning())
        m_engineThread->wait(20000);
    if (m_installThread && m_installThread->isRunning())
        m_installThread->wait(5000);
}

// ===========================================================================
// 界面搭建
// ===========================================================================
void MainWindow::buildUi()
{
    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    auto *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(12, 10, 12, 12);
    mainLayout->setSpacing(8);

    // ---- 引擎状态条 ----
    auto *engineLayout = new QHBoxLayout();
    m_engineStatusLabel = new QLabel("正在检测转换引擎…", this);
    m_engineStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    engineLayout->addWidget(m_engineStatusLabel, 1);
    m_installBtn = new QPushButton("一键安装缺失组件", this);
    engineLayout->addWidget(m_installBtn);
    mainLayout->addLayout(engineLayout);

    auto *hint = new QLabel(
        "把文件或整个文件夹拖到下面的列表，也可用「添加文件 / 添加文件夹」。双击文件可预览内容。\n"
        "支持 docx / doc / odt / rtf / txt / html / md / epub / pdf / pptx / tex / rst / org / adoc 互转。",
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: palette(mid);");
    mainLayout->addWidget(hint);

    // ---- 文件列表 ----
    m_fileList = new QListWidget(this);
    m_fileList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_fileList->setAlternatingRowColors(true);
    mainLayout->addWidget(m_fileList, 3);

    auto *fileBtnLayout = new QHBoxLayout();
    m_addFilesBtn = new QPushButton("添加文件", this);
    m_addFolderBtn = new QPushButton("添加文件夹", this);
    m_removeBtn = new QPushButton("移除所选", this);
    m_clearBtn = new QPushButton("清空列表", this);
    fileBtnLayout->addWidget(m_addFilesBtn);
    fileBtnLayout->addWidget(m_addFolderBtn);
    fileBtnLayout->addWidget(m_removeBtn);
    fileBtnLayout->addWidget(m_clearBtn);
    fileBtnLayout->addStretch();
    mainLayout->addLayout(fileBtnLayout);

    // ---- 输出选项 ----
    auto *optionGroup = new QGroupBox("输出设置", this);
    auto *optionLayout = new QVBoxLayout(optionGroup);
    optionLayout->setSpacing(6);

    // 预设方案：把「目标格式 + 高级选项」存成一套，下次一键切换
    auto *presetRow = new QHBoxLayout();
    presetRow->addWidget(new QLabel("预设方案：", this));
    m_profileCombo = new QComboBox(this);
    m_profileCombo->setMinimumWidth(180);
    presetRow->addWidget(m_profileCombo);
    auto *saveProfileBtn = new QPushButton("保存为预设…", this);
    auto *delProfileBtn = new QPushButton("删除", this);
    presetRow->addWidget(saveProfileBtn);
    presetRow->addWidget(delProfileBtn);
    presetRow->addStretch();
    optionLayout->addLayout(presetRow);

    auto *row1 = new QHBoxLayout();
    row1->addWidget(new QLabel("目标格式：", this));
    m_formatCombo = new QComboBox(this);
    // 格式缩写对普通用户不直观，配一句说明放在悬停提示里
    static const QHash<QString, QString> formatTips = {
        {"pdf",  "PDF：版式固定的便携文档，适合分发与打印"},
        {"docx", "Word 文档（2007 及以后）"},
        {"doc",  "Word 97-2003 兼容格式，老系统/老软件需要"},
        {"odt",  "OpenDocument 文本，LibreOffice / WPS 通用"},
        {"rtf",  "富文本格式，跨软件兼容性最好"},
        {"txt",  "纯文本，只保留文字，丢掉所有格式"},
        {"html", "网页：单文件，样式与图片都内嵌"},
        {"md",   "Markdown：适合写作、笔记与版本管理"},
        {"epub", "电子书格式，适合阅读器"},
        {"pptx", "PowerPoint 幻灯片：按标题层级自动分页"},
        {"tex",  "LaTeX 源码：论文排版用"},
        {"rst",  "reStructuredText：Python 文档常用"},
        {"org",  "Org-mode：Emacs 生态的纯文本格式"},
        {"adoc", "AsciiDoc：技术文档常用"}
    };
    for (const QString &fmt : Converter::supportedTargetFormats()) {
        m_formatCombo->addItem(fmt.toUpper(), fmt);
        const auto tip = formatTips.constFind(fmt);
        if (tip != formatTips.constEnd())
            m_formatCombo->setItemData(m_formatCombo->count() - 1, tip.value(), Qt::ToolTipRole);
    }
    m_formatCombo->setMinimumWidth(90);
    row1->addWidget(m_formatCombo);
    row1->addSpacing(16);

    m_outputToSourceCheck = new QCheckBox("输出到源文件所在目录", this);
    row1->addWidget(m_outputToSourceCheck);
    row1->addStretch();
    optionLayout->addLayout(row1);

    auto *row2 = new QHBoxLayout();
    row2->addWidget(new QLabel("输出目录：", this));
    m_outputDirEdit = new QLineEdit(this);
    m_outputDirEdit->setPlaceholderText("选择转换后的文件放在哪里");
    row2->addWidget(m_outputDirEdit, 1);
    m_browseBtn = new QPushButton("浏览…", this);
    row2->addWidget(m_browseBtn);
    m_openDirBtn = new QPushButton("打开目录", this);
    row2->addWidget(m_openDirBtn);
    optionLayout->addLayout(row2);

    mainLayout->addWidget(optionGroup);

    // ---- 高级选项（默认折叠，避免一屏塞满）----
    m_advancedToggle = new QToolButton(this);
    m_advancedToggle->setCheckable(true);
    m_advancedToggle->setAutoRaise(true);
    m_advancedToggle->setArrowType(Qt::RightArrow);
    m_advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_advancedToggle->setText("高级选项：目录 / 编号 / Word 模板 / Markdown 方言 / 元数据");
    mainLayout->addWidget(m_advancedToggle, 0, Qt::AlignLeft);

    m_advancedPanel = buildAdvancedPanel();
    m_advancedPanel->setVisible(false);
    mainLayout->addWidget(m_advancedPanel);

    // ---- 操作按钮 ----
    auto *actionLayout = new QHBoxLayout();
    m_convertBtn = new QPushButton("开始转换", this);
    m_convertBtn->setMinimumHeight(38);
    m_convertBtn->setDefault(true);
    m_cancelBtn = new QPushButton("取消", this);
    m_cancelBtn->setMinimumHeight(38);
    m_cancelBtn->setVisible(false);
    actionLayout->addWidget(m_convertBtn, 3);
    actionLayout->addWidget(m_cancelBtn, 1);
    mainLayout->addLayout(actionLayout);

    // ---- 进度 ----
    auto *progressLayout = new QHBoxLayout();
    m_progressBar = new QProgressBar(this);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(false);
    m_progressLabel = new QLabel("", this);
    m_progressLabel->setMinimumWidth(120);
    progressLayout->addWidget(m_progressBar, 1);
    progressLayout->addWidget(m_progressLabel);
    mainLayout->addLayout(progressLayout);

    // ---- 日志 ----
    auto *logHeader = new QHBoxLayout();
    auto *logTitle = new QLabel("运行日志", this);
    logTitle->setStyleSheet("color: palette(mid);");
    logHeader->addWidget(logTitle);
    logHeader->addStretch();
    m_saveLogBtn = new QPushButton("保存日志", this);
    m_saveLogBtn->setFlat(true);
    m_clearLogBtn = new QPushButton("清空", this);
    m_clearLogBtn->setFlat(true);
    logHeader->addWidget(m_saveLogBtn);
    logHeader->addWidget(m_clearLogBtn);
    mainLayout->addLayout(logHeader);

    m_logEdit = new QPlainTextEdit(this);
    m_logEdit->setReadOnly(true);
    m_logEdit->setMaximumBlockCount(3000);
    m_logEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    mainLayout->addWidget(m_logEdit, 2);

    // ---- 信号 ----
    connect(m_addFilesBtn, &QPushButton::clicked, this, &MainWindow::onAddFiles);
    connect(m_addFolderBtn, &QPushButton::clicked, this, &MainWindow::onAddFolder);
    connect(m_removeBtn, &QPushButton::clicked, this, &MainWindow::onRemoveSelected);
    connect(m_clearBtn, &QPushButton::clicked, this, &MainWindow::onClearList);
    connect(m_browseBtn, &QPushButton::clicked, this, &MainWindow::onChooseOutputDir);
    connect(m_openDirBtn, &QPushButton::clicked, this, &MainWindow::onOpenOutputDir);
    connect(m_convertBtn, &QPushButton::clicked, this, &MainWindow::onConvert);
    connect(m_cancelBtn, &QPushButton::clicked, this, &MainWindow::onCancel);
    connect(m_installBtn, &QPushButton::clicked, this, &MainWindow::onInstallDependencies);
    connect(m_saveLogBtn, &QPushButton::clicked, this, &MainWindow::onSaveLog);
    connect(m_clearLogBtn, &QPushButton::clicked, m_logEdit, &QPlainTextEdit::clear);

    connect(m_outputToSourceCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_outputDirEdit->setEnabled(!on && !m_busy);
        m_browseBtn->setEnabled(!on && !m_busy);
    });

    // 预设 / 高级选项
    connect(saveProfileBtn, &QPushButton::clicked, this, &MainWindow::onSaveProfile);
    connect(delProfileBtn, &QPushButton::clicked, this, &MainWindow::onDeleteProfile);
    connect(m_profileCombo, &QComboBox::activated, this, &MainWindow::onProfileActivated);
    connect(m_advancedToggle, &QToolButton::toggled, this, &MainWindow::onToggleAdvanced);
    connect(m_formatCombo, &QComboBox::currentIndexChanged, this, [this]() {
        updateFormatDependentOptions();
    });

    // 双击预览、右键菜单
    m_fileList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_fileList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *) { onPreviewSelected(); });
    connect(m_fileList, &QListWidget::customContextMenuRequested,
            this, &MainWindow::onListContextMenu);

    // 快捷键
    auto *delKey = new QShortcut(QKeySequence::Delete, this);
    connect(delKey, &QShortcut::activated, this, &MainWindow::onRemoveSelected);
    auto *previewKey = new QShortcut(QKeySequence("Ctrl+P"), this);
    connect(previewKey, &QShortcut::activated, this, &MainWindow::onPreviewSelected);
    auto *convertKey = new QShortcut(QKeySequence("Ctrl+Return"), this);
    connect(convertKey, &QShortcut::activated, this, &MainWindow::onConvert);
}

// ===========================================================================
// 菜单栏
// ===========================================================================
void MainWindow::buildMenus()
{
    QMenu *fileMenu = menuBar()->addMenu("文件(&F)");
    QAction *addFiles = fileMenu->addAction("添加文件…", this, &MainWindow::onAddFiles);
    addFiles->setShortcut(QKeySequence::Open);
    fileMenu->addAction("添加文件夹…", this, &MainWindow::onAddFolder);
    fileMenu->addSeparator();
    fileMenu->addAction("保存日志…", this, &MainWindow::onSaveLog);
    fileMenu->addAction("清空列表", this, &MainWindow::onClearList);
    fileMenu->addSeparator();
    fileMenu->addAction("退出", qApp, &QApplication::quit, QKeySequence::Quit);

    QMenu *viewMenu = menuBar()->addMenu("查看(&V)");
    viewMenu->addAction("预览所选文件", this, &MainWindow::onPreviewSelected);
    viewMenu->addAction("转换为 PDF", this, [this]() {
        const int idx = m_formatCombo->findData("pdf");
        if (idx >= 0)
            m_formatCombo->setCurrentIndex(idx);
        onConvert();
    });
    viewMenu->addSeparator();
    viewMenu->addAction("转换历史…", this, &MainWindow::showHistory);

    QMenu *settingMenu = menuBar()->addMenu("设置(&S)");
    settingMenu->addAction("指定 Pandoc 路径…", this, [this]() { onChooseEnginePath("pandoc"); });
    settingMenu->addAction("指定 LibreOffice 路径…", this, [this]() { onChooseEnginePath("soffice"); });
    settingMenu->addAction("恢复自动探测", this, [this]() {
        Converter::setEngineOverride("pandoc", QString());
        Converter::setEngineOverride("soffice", QString());
        appendLog("已恢复引擎路径自动探测。");
        startEngineCheck();
    });
    settingMenu->addSeparator();

    // 外观：Qt 6.8+ 可以直接告诉系统「我要深色」，标题栏与控件一起变
    QMenu *themeMenu = settingMenu->addMenu("外观");
    auto *themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    const QList<QPair<QString, QString>> themes = {
        {"system", "跟随系统"}, {"light", "浅色"}, {"dark", "深色"}};
    for (const auto &t : themes) {
        QAction *a = themeMenu->addAction(t.second);
        a->setCheckable(true);
        a->setData(t.first);
        if (t.first == m_theme)
            a->setChecked(true);
        themeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, key = t.first]() {
            m_theme = key;
            applyTheme(key);
            QSettings().setValue(kSettingTheme, key);
        });
    }

    settingMenu->addAction("重新检测引擎", this, &MainWindow::startEngineCheck);

    QMenu *helpMenu = menuBar()->addMenu("帮助(&H)");
    helpMenu->addAction("已知限制", this, [this]() {
        QMessageBox::information(this, "已知限制",
            "· 图片型扫描件（PDF 里没有文字层）转出来是空的，需要先做 OCR。\n"
            "  转换时日志会逐页告诉你哪几页没有文字层。\n\n"
            "· PDF → Word / Markdown 的排版还原依赖 pdf2docx；\n"
            "  没装时退回 LibreOffice，正文可能只剩标题。\n\n"
            "· PPTX → docx / odt / rtf 是「按页拍平成 PDF 再转文档」，保版面但不是可编辑的幻灯片大纲。\n\n"
            "· Markdown → PDF 的样式比较朴素；想控制排版，可以在高级选项里指定 Word 样式模板。\n\n"
            "· 批量转换是串行的：LibreOffice 不适合同时起多个实例，串行反而更快更稳。");
    });
    helpMenu->addAction("关于", this, [this]() {
        QString pandoc = Converter::pandocPath();
        if (pandoc.isEmpty())
            pandoc = "未检测到";
        QString soffice = Converter::sofficePath();
        if (soffice.isEmpty())
            soffice = "未检测到";

        QMessageBox box(this);
        box.setWindowTitle("关于");
        box.setTextFormat(Qt::RichText);
        box.setText(QString(
            "<b>文档格式转换工具</b> v%1<br><br>"
            "调度 Pandoc 与 LibreOffice 完成文档格式互转，本身不解析任何格式。<br><br>"
            "<span style='color:gray'>Pandoc：%2<br>LibreOffice：%3</span>")
            .arg(QCoreApplication::applicationVersion(), pandoc.toHtmlEscaped(),
                 soffice.toHtmlEscaped()));
        box.exec();
    });
}

// ===========================================================================
// 高级选项面板
// ===========================================================================
QWidget *MainWindow::buildAdvancedPanel()
{
    auto *box = new QGroupBox("高级转换选项", this);
    auto *grid = new QGridLayout(box);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(1, 1);

    m_tocCheck = new QCheckBox("生成目录（TOC）", box);
    m_tocCheck->setToolTip("在文档开头插入目录。对 PDF / docx / HTML / epub / Markdown 等有效；\n"
                           "txt 与 pptx 没有目录概念，会自动忽略。");
    m_numberCheck = new QCheckBox("章节自动编号", box);
    m_numberCheck->setToolTip("按标题层级给章节加编号（1、1.1、1.1.1…）。");
    grid->addWidget(m_tocCheck, 0, 0);
    grid->addWidget(m_numberCheck, 0, 1, 1, 2);

    grid->addWidget(new QLabel("Word 样式模板：", box), 1, 0);
    m_refDocEdit = new QLineEdit(box);
    m_refDocEdit->setPlaceholderText("可选：选一个 .docx，输出会套用它的字体与标题样式");
    grid->addWidget(m_refDocEdit, 1, 1);
    auto *refBtnRow = new QHBoxLayout();
    m_refDocBrowseBtn = new QPushButton("浏览…", box);
    m_refDocClearBtn = new QPushButton("清除", box);
    refBtnRow->addWidget(m_refDocBrowseBtn);
    refBtnRow->addWidget(m_refDocClearBtn);
    grid->addLayout(refBtnRow, 1, 2);

    connect(m_refDocBrowseBtn, &QPushButton::clicked, this, [this]() {
        const QString f = QFileDialog::getOpenFileName(
            this, "选择 Word 样式模板", QString(), "Word 文档 (*.docx)");
        if (!f.isEmpty())
            m_refDocEdit->setText(QDir::toNativeSeparators(f));
    });
    connect(m_refDocClearBtn, &QPushButton::clicked, m_refDocEdit, &QLineEdit::clear);

    grid->addWidget(new QLabel("Markdown 方言：", box), 2, 0);
    m_mdFlavorCombo = new QComboBox(box);
    m_mdFlavorCombo->addItem("Pandoc Markdown（默认，功能最全）", "");
    m_mdFlavorCombo->addItem("GitHub 风格（GFM，表格/任务列表）", "gfm");
    m_mdFlavorCombo->addItem("CommonMark（最严格、最通用）", "commonmark");
    grid->addWidget(m_mdFlavorCombo, 2, 1);

    auto *dpiRow = new QHBoxLayout();
    dpiRow->addWidget(new QLabel("图片 DPI：", box));
    m_dpiSpin = new QSpinBox(box);
    m_dpiSpin->setRange(0, 1200);
    m_dpiSpin->setSpecialValueText("默认");
    m_dpiSpin->setToolTip("转换时假定图片的 DPI，影响 Word/PDF 里图片的物理尺寸。0 表示交给引擎判断。");
    dpiRow->addWidget(m_dpiSpin);
    dpiRow->addStretch();
    grid->addLayout(dpiRow, 2, 2);

    grid->addWidget(new QLabel("文档标题：", box), 3, 0);
    m_titleEdit = new QLineEdit(box);
    m_titleEdit->setPlaceholderText("留空则用文件名");
    grid->addWidget(m_titleEdit, 3, 1);

    auto *authorRow = new QHBoxLayout();
    authorRow->addWidget(new QLabel("作者：", box));
    m_authorEdit = new QLineEdit(box);
    m_authorEdit->setPlaceholderText("写入文档属性");
    authorRow->addWidget(m_authorEdit);
    grid->addLayout(authorRow, 3, 2);

    return box;
}

void MainWindow::loadSettings()
{
    QSettings s;
    if (s.contains(kSettingGeometry))
        restoreGeometry(s.value(kSettingGeometry).toByteArray());

    const QString fmt = s.value(kSettingFormat, "pdf").toString();
    const int idx = m_formatCombo->findData(fmt);
    if (idx >= 0)
        m_formatCombo->setCurrentIndex(idx);

    m_outputDirEdit->setText(s.value(kSettingOutputDir, desktopDir()).toString());
    m_outputToSourceCheck->setChecked(s.value(kSettingOutputToSource, false).toBool());
    m_outputDirEdit->setEnabled(!m_outputToSourceCheck->isChecked());
    m_browseBtn->setEnabled(!m_outputToSourceCheck->isChecked());

    // ---- 外观 ----
    m_theme = s.value(kSettingTheme, "system").toString();
    applyTheme(m_theme);

    // ---- 高级选项：先恢复上次手工调的，再套用上次选的预设 ----
    applyOptions(Converter::Options());
    s.beginGroup(kOptionsNode);
    Converter::Options saved;
    saved.toc = s.value("toc", false).toBool();
    saved.numberSections = s.value("numberSections", false).toBool();
    saved.referenceDoc = s.value("referenceDoc").toString();
    saved.markdownFlavor = s.value("markdownFlavor").toString();
    saved.dpi = s.value("dpi", 0).toInt();
    saved.title = s.value("title").toString();
    saved.author = s.value("author").toString();
    s.endGroup();
    applyOptions(saved);

    refreshProfileCombo();
    const QString lastProfile = s.value(kSettingLastProfile).toString();
    if (!lastProfile.isEmpty()) {
        const int pIdx = m_profileCombo->findData(lastProfile);
        if (pIdx > 0) {
            QSignalBlocker block(m_profileCombo);
            m_profileCombo->setCurrentIndex(pIdx);
            const QByteArray raw = s.value("profiles/" + lastProfile).toString().toUtf8();
            const QJsonObject obj = QJsonDocument::fromJson(raw).object();
            if (!obj.isEmpty())
                applyOptions(Converter::Options{obj.value("toc").toBool(),
                                                obj.value("numberSections").toBool(),
                                                obj.value("referenceDoc").toString(),
                                                obj.value("markdownFlavor").toString(),
                                                obj.value("title").toString(),
                                                obj.value("author").toString(),
                                                obj.value("dpi").toInt(), {}});
        }
    }

    m_advancedToggle->setChecked(s.value(kSettingAdvancedOpen, false).toBool());
    updateFormatDependentOptions();
}

void MainWindow::saveSettings()
{
    QSettings s;
    s.setValue(kSettingGeometry, saveGeometry());
    s.setValue(kSettingFormat, m_formatCombo->currentData().toString());
    s.setValue(kSettingOutputDir, m_outputDirEdit->text());
    s.setValue(kSettingOutputToSource, m_outputToSourceCheck->isChecked());
    s.setValue(kSettingTheme, m_theme);
    s.setValue(kSettingAdvancedOpen, m_advancedToggle->isChecked());
    s.setValue(kSettingLastProfile, m_profileCombo->currentData().toString());

    const Converter::Options o = collectOptions();
    s.beginGroup(kOptionsNode);
    s.setValue("toc", o.toc);
    s.setValue("numberSections", o.numberSections);
    s.setValue("referenceDoc", o.referenceDoc);
    s.setValue("markdownFlavor", o.markdownFlavor);
    s.setValue("dpi", o.dpi);
    s.setValue("title", o.title);
    s.setValue("author", o.author);
    s.endGroup();
}

// ===========================================================================
// 高级选项与预设方案
// ===========================================================================
Converter::Options MainWindow::collectOptions() const
{
    Converter::Options o;
    o.toc = m_tocCheck->isChecked();
    o.numberSections = m_numberCheck->isChecked();
    o.referenceDoc = m_refDocEdit->text().trimmed();
    o.markdownFlavor = m_mdFlavorCombo->currentData().toString();
    o.dpi = m_dpiSpin->value();
    o.title = m_titleEdit->text().trimmed();
    o.author = m_authorEdit->text().trimmed();
    return o;
}

void MainWindow::applyOptions(const Converter::Options &o)
{
    m_tocCheck->setChecked(o.toc);
    m_numberCheck->setChecked(o.numberSections);
    m_refDocEdit->setText(o.referenceDoc);
    int mdIdx = m_mdFlavorCombo->findData(o.markdownFlavor);
    m_mdFlavorCombo->setCurrentIndex(mdIdx >= 0 ? mdIdx : 0);
    m_dpiSpin->setValue(o.dpi);
    m_titleEdit->setText(o.title);
    m_authorEdit->setText(o.author);
}

void MainWindow::onToggleAdvanced()
{
    const bool open = m_advancedToggle->isChecked();
    m_advancedPanel->setVisible(open);
    m_advancedToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
}

void MainWindow::refreshProfileCombo(const QString &selectName)
{
    const QString keep = selectName.isEmpty()
                             ? m_profileCombo->currentData().toString()
                             : selectName;

    QSignalBlocker block(m_profileCombo);
    m_profileCombo->clear();
    m_profileCombo->addItem("（未使用预设）", "");

    QSettings s;
    s.beginGroup(kSettingProfiles);
    QStringList names = s.childKeys();
    s.endGroup();
    names.removeAll("last");
    names.sort(Qt::CaseInsensitive);
    for (const QString &n : names)
        m_profileCombo->addItem(n, n);

    const int idx = m_profileCombo->findData(keep);
    m_profileCombo->setCurrentIndex(idx > 0 ? idx : 0);
}

void MainWindow::onSaveProfile()
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, "保存预设", "预设名称（例如「论文 PDF」「给同事的 Markdown」）：",
        QLineEdit::Normal, m_profileCombo->currentText().isEmpty()
                               || m_profileCombo->currentIndex() == 0
                           ? QString("我的预设")
                           : m_profileCombo->currentText(),
        &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    if (name == "last") {
        QMessageBox::information(this, "换个名字", "「last」是内部保留名，请换一个。");
        return;
    }

    const Converter::Options o = collectOptions();
    QJsonObject obj;
    obj["toc"] = o.toc;
    obj["numberSections"] = o.numberSections;
    obj["referenceDoc"] = o.referenceDoc;
    obj["markdownFlavor"] = o.markdownFlavor;
    obj["dpi"] = o.dpi;
    obj["title"] = o.title;
    obj["author"] = o.author;
    obj["format"] = m_formatCombo->currentData().toString();

    QSettings s;
    s.setValue(kSettingProfiles + "/" + name,
               QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    refreshProfileCombo(name);
    appendLog(QString("已保存预设「%1」").arg(name));
}

void MainWindow::onDeleteProfile()
{
    const QString name = m_profileCombo->currentData().toString();
    if (name.isEmpty()) {
        QMessageBox::information(this, "提示", "当前没有选中预设。");
        return;
    }
    if (QMessageBox::question(this, "删除预设", QString("确定删除预设「%1」？").arg(name))
        != QMessageBox::Yes)
        return;

    QSettings s;
    s.remove(kSettingProfiles + "/" + name);
    refreshProfileCombo();
    appendLog(QString("已删除预设「%1」").arg(name));
}

void MainWindow::onProfileActivated(int index)
{
    const QString name = m_profileCombo->itemData(index).toString();
    if (name.isEmpty())
        return;

    QSettings s;
    const QByteArray raw = s.value(kSettingProfiles + "/" + name).toString().toUtf8();
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    if (obj.isEmpty()) {
        QMessageBox::warning(this, "预设损坏", "这个预设读不出来，可能已被外部修改。");
        return;
    }

    Converter::Options o;
    o.toc = obj.value("toc").toBool();
    o.numberSections = obj.value("numberSections").toBool();
    o.referenceDoc = obj.value("referenceDoc").toString();
    o.markdownFlavor = obj.value("markdownFlavor").toString();
    o.dpi = obj.value("dpi").toInt();
    o.title = obj.value("title").toString();
    o.author = obj.value("author").toString();
    applyOptions(o);

    const QString fmt = obj.value("format").toString();
    const int fmtIdx = m_formatCombo->findData(fmt);
    if (fmtIdx >= 0)
        m_formatCombo->setCurrentIndex(fmtIdx);

    if (!m_advancedToggle->isChecked())
        m_advancedToggle->setChecked(true);   // 让用户看到预设改了什么
    appendLog(QString("已套用预设「%1」").arg(name));
}

// 高级选项里有一部分只对特定目标格式有意义，其余时候灰掉，
// 免得用户勾了「目录」却发现转成 txt 时没生效。
void MainWindow::updateFormatDependentOptions()
{
    const QString target = m_formatCombo->currentData().toString();
    const bool docx = (target == "docx");
    const bool markdown = (target == "md");
    const bool structured = target != "txt" && target != "pptx";

    m_tocCheck->setEnabled(structured);
    m_numberCheck->setEnabled(structured);
    m_refDocEdit->setEnabled(docx);
    m_refDocBrowseBtn->setEnabled(docx);
    m_refDocClearBtn->setEnabled(docx);
    m_mdFlavorCombo->setEnabled(markdown);
    m_dpiSpin->setEnabled(structured);

    m_refDocEdit->setToolTip(docx ? QString()
                                  : "只有目标格式是 docx 时，Word 模板才会生效");
    m_mdFlavorCombo->setToolTip(markdown ? QString()
                                         : "只有目标格式是 md 时，方言选项才会生效");
}

// ===========================================================================
// 外观
// ===========================================================================
void MainWindow::applyTheme(const QString &theme)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    Qt::ColorScheme scheme = Qt::ColorScheme::Unknown;
    if (theme == "light")
        scheme = Qt::ColorScheme::Light;
    else if (theme == "dark")
        scheme = Qt::ColorScheme::Dark;
    QGuiApplication::styleHints()->setColorScheme(scheme);
#else
    Q_UNUSED(theme)
#endif
}

// ===========================================================================
// 转换历史
// ===========================================================================
void MainWindow::loadHistory()
{
    QFile f(historyFilePath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (doc.isArray())
        m_history = doc.array();
}

void MainWindow::saveHistory()
{
    QFile f(historyFilePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        appendLog("⚠️ 无法写入转换历史：" + historyFilePath());
        return;
    }
    f.write(QJsonDocument(m_history).toJson(QJsonDocument::Indented));
    f.close();
}

void MainWindow::addHistory(const QString &source, const QString &target,
                            const QString &output, const QString &status,
                            const QString &message)
{
    QJsonObject obj;
    obj["time"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    obj["source"] = QDir::toNativeSeparators(source);
    obj["target"] = target.toUpper();
    obj["output"] = QDir::toNativeSeparators(output);
    obj["status"] = status;
    if (!message.isEmpty())
        obj["message"] = message;

    m_history.insert(0, obj);
    while (m_history.size() > 300)
        m_history.removeLast();
}

void MainWindow::showHistory()
{
    QDialog dlg(this);
    dlg.setWindowTitle("转换历史");
    dlg.resize(880, 520);

    auto *layout = new QVBoxLayout(&dlg);
    auto *tip = new QLabel(QString("最近 %1 条记录，保存在：%2")
                               .arg(m_history.size())
                               .arg(QDir::toNativeSeparators(historyFilePath())),
                           &dlg);
    tip->setStyleSheet("color: palette(mid);");
    layout->addWidget(tip);

    auto *table = new QTableWidget(&dlg);
    table->setColumnCount(4);
    table->setHorizontalHeaderLabels({"时间", "源文件", "结果", "输出"});
    table->horizontalHeader()->setStretchLastSection(true);
    table->setColumnWidth(0, 140);
    table->setColumnWidth(1, 260);
    table->setColumnWidth(2, 90);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->setVisible(false);

    table->setRowCount(m_history.size());
    for (int i = 0; i < m_history.size(); ++i) {
        const QJsonObject o = m_history.at(i).toObject();
        const QString status = o.value("status").toString();
        auto *timeItem = new QTableWidgetItem(
            QDateTime::fromString(o.value("time").toString(), Qt::ISODate)
                .toString("MM-dd HH:mm:ss"));
        auto *srcItem = new QTableWidgetItem(QFileInfo(o.value("source").toString()).fileName());
        srcItem->setToolTip(o.value("source").toString());
        auto *stItem = new QTableWidgetItem(
            status == "ok" ? "成功" : (status == "skipped" ? "跳过" : "失败"));
        if (status == "ok")
            stItem->setForeground(QColor(0x1a, 0x7f, 0x37));
        else if (status == "failed")
            stItem->setForeground(QColor(0xc0, 0x2b, 0x1f));
        auto *outItem = new QTableWidgetItem(o.value("output").toString());
        outItem->setToolTip(QString("%1\n%2")
                                .arg(o.value("output").toString(),
                                     o.value("message").toString()));

        timeItem->setData(Qt::UserRole, o.value("source").toString());
        timeItem->setData(Qt::UserRole + 1, o.value("output").toString());
        table->setItem(i, 0, timeItem);
        table->setItem(i, 1, srcItem);
        table->setItem(i, 2, stItem);
        table->setItem(i, 3, outItem);
    }
    layout->addWidget(table, 1);

    auto *btnRow = new QHBoxLayout();
    auto *openFileBtn = new QPushButton("打开输出文件", &dlg);
    auto *openDirBtn = new QPushButton("打开所在目录", &dlg);
    auto *requeueBtn = new QPushButton("重新加入列表", &dlg);
    auto *clearBtn = new QPushButton("清空历史", &dlg);
    auto *closeBtn = new QPushButton("关闭", &dlg);
    btnRow->addWidget(openFileBtn);
    btnRow->addWidget(openDirBtn);
    btnRow->addWidget(requeueBtn);
    btnRow->addWidget(clearBtn);
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);

    auto currentPaths = [table]() -> QPair<QString, QString> {
        const int row = table->currentRow();
        if (row < 0)
            return {};
        return {table->item(row, 0)->data(Qt::UserRole).toString(),
                table->item(row, 0)->data(Qt::UserRole + 1).toString()};
    };

    connect(openFileBtn, &QPushButton::clicked, &dlg, [&]() {
        const auto p = currentPaths();
        if (p.second.isEmpty() || !QFileInfo::exists(p.second)) {
            QMessageBox::information(&dlg, "提示", "输出文件已不存在（可能被移动或删除）。");
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(p.second));
    });
    connect(openDirBtn, &QPushButton::clicked, &dlg, [&]() {
        const auto p = currentPaths();
        const QString path = p.second.isEmpty() ? p.first : p.second;
        if (path.isEmpty())
            return;
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    });
    connect(requeueBtn, &QPushButton::clicked, &dlg, [&]() {
        const auto p = currentPaths();
        if (p.first.isEmpty())
            return;
        addFilesToList({p.first});
        dlg.accept();
    });
    connect(clearBtn, &QPushButton::clicked, &dlg, [&]() {
        if (QMessageBox::question(&dlg, "清空历史", "确定清空全部转换历史？")
            != QMessageBox::Yes)
            return;
        m_history = QJsonArray();
        saveHistory();
        table->setRowCount(0);
    });
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(table, &QTableWidget::itemDoubleClicked, &dlg, [&](QTableWidgetItem *) {
        const auto p = currentPaths();
        if (!p.second.isEmpty() && QFileInfo::exists(p.second))
            QDesktopServices::openUrl(QUrl::fromLocalFile(p.second));
    });

    dlg.exec();
}

// ===========================================================================
// 预览
// ===========================================================================
void MainWindow::onPreviewSelected()
{
    QListWidgetItem *item = m_fileList->currentItem();
    if (!item) {
        if (m_fileList->count() > 0)
            item = m_fileList->item(0);
        else
            return;
    }
    const QString path = item->data(Qt::UserRole).toString();
    PreviewDialog dlg(path, this);
    dlg.exec();
}

void MainWindow::onListContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_fileList->itemAt(pos);
    if (!item)
        return;

    const QString path = item->data(Qt::UserRole).toString();

    QMenu menu(this);
    menu.addAction("预览", this, &MainWindow::onPreviewSelected);
    menu.addAction("用默认程序打开", this, [path]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    menu.addAction("在资源管理器中显示", this, [path]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    });
    menu.addSeparator();
    menu.addAction("复制完整路径", this, [path]() {
        QGuiApplication::clipboard()->setText(path);
    });
    menu.addSeparator();
    menu.addAction("从列表移除", this, &MainWindow::onRemoveSelected);
    menu.exec(m_fileList->mapToGlobal(pos));
}

// ===========================================================================
// 指定引擎路径
// ===========================================================================
void MainWindow::onChooseEnginePath(const QString &engine)
{
    const bool isPandoc = (engine == "pandoc");
    const QString title = isPandoc ? "选择 pandoc.exe" : "选择 soffice.exe";
    const QString filter = isPandoc ? "Pandoc (pandoc.exe)" : "LibreOffice (soffice.exe)";

    const QString current = isPandoc ? Converter::pandocPath() : Converter::sofficePath();
    const QString file = QFileDialog::getOpenFileName(
        this, title, QFileInfo(current).absolutePath(), filter);
    if (file.isEmpty())
        return;

    Converter::setEngineOverride(engine, file);
    appendLog(QString("%1 已指定为：%2")
                  .arg(isPandoc ? "Pandoc" : "LibreOffice", QDir::toNativeSeparators(file)));
    startEngineCheck();
}

// ===========================================================================
// 引擎检测（后台线程，避免启动时卡住界面）
// ===========================================================================
void MainWindow::startEngineCheck()
{
    m_engineStatusLabel->setText("正在检测转换引擎…");
    m_installBtn->setEnabled(false);

    QPointer<MainWindow> self(this);
    if (m_engineThread && m_engineThread->isRunning())
        m_engineThread->wait(5000);

    m_engineThread = QThread::create([self]() {
        // 这段在后台线程执行：python 冷启动 + import pdf2docx 可能要好几秒
        const QString pandoc = Converter::pandocPath();
        const QString soffice = Converter::sofficePath();
        const bool pdf2docx = Converter::pdf2docxAvailable();

        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, pandoc, soffice, pdf2docx]() {
                if (!self)
                    return;
                self->updateEngineLabel(pandoc, soffice, pdf2docx);
            },
            Qt::QueuedConnection);
    });
    m_engineThread->start();
}

void MainWindow::updateEngineLabel(const QString &pandoc, const QString &soffice, bool pdf2docx)
{
    m_pandocReady = !pandoc.isEmpty();
    m_sofficeReady = !soffice.isEmpty();
    m_pdf2docxReady = pdf2docx;
    m_enginesChecked = true;

    QString status;
    status += m_pandocReady ? "✅ Pandoc" : "❌ Pandoc";
    status += "　";
    status += m_sofficeReady ? "✅ LibreOffice" : "❌ LibreOffice";
    status += "　";
    status += m_pdf2docxReady ? "✅ pdf2docx（PDF 转 Word 高精度）"
                              : "❌ pdf2docx（可选，PDF 转 Word 高精度）";
    m_engineStatusLabel->setText(status);

    QString tip;
    tip += "Pandoc: " + (m_pandocReady ? pandoc : QString("未找到")) + "\n";
    tip += "LibreOffice: " + (m_sofficeReady ? soffice : QString("未找到")) + "\n";
    tip += QString("pdf2docx: ") + (m_pdf2docxReady ? "已就绪" : "未安装");
    m_engineStatusLabel->setToolTip(tip);

    const bool allReady = m_pandocReady && m_sofficeReady && m_pdf2docxReady;
    m_installBtn->setText(allReady ? "组件已就绪" : "一键安装缺失组件");
    updateControls();

    if (!m_pandocReady && !m_sofficeReady) {
        appendLog("⚠️ 未检测到 Pandoc 和 LibreOffice，暂时无法转换。"
                  "请点击右上角「一键安装缺失组件」。");
    } else if (!m_pandocReady) {
        appendLog("ℹ️ 未检测到 Pandoc：txt/md/html/epub 等格式的转换能力会受限。");
    } else if (!m_sofficeReady) {
        appendLog("ℹ️ 未检测到 LibreOffice：涉及 PDF 的转换将无法进行。");
    }
}

bool MainWindow::ensureEnginesOrWarn()
{
    if (!m_enginesChecked) {
        QMessageBox::information(this, "请稍候",
            "正在检测转换引擎，请稍后再试。");
        return false;
    }
    if (!m_pandocReady && !m_sofficeReady) {
        QMessageBox::warning(this, "缺少转换引擎",
            "没有检测到 Pandoc，也没有检测到 LibreOffice，无法进行任何转换。\n\n"
            "请点击窗口右上角的「一键安装缺失组件」，或手动安装后重试。");
        return false;
    }
    return true;
}

// ===========================================================================
// 文件列表
// ===========================================================================
void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            paths << url.toLocalFile();
    }
    addFilesToList(paths);
    event->acceptProposedAction();
}

void MainWindow::addFilesToList(const QStringList &paths)
{
    // 先把列表里已有的路径收进集合，避免 O(n²) 的重复判断
    QSet<QString> existing;
    for (int i = 0; i < m_fileList->count(); ++i) {
        existing.insert(m_fileList->item(i)->data(Qt::UserRole).toString());
    }

    int added = 0;
    int duplicated = 0;
    QStringList unsupported;

    for (const QString &raw : paths) {
        QFileInfo fi(raw);
        QStringList batch;

        if (fi.isDir()) {
            // 支持整个文件夹拖入：递归收集支持的格式。
            // 后缀清单统一由 Converter 提供，新增格式时不用改这里。
            QStringList patterns;
            for (const QString &ext : Converter::supportedSourceExtensions())
                patterns << ("*." + ext);
            QDirIterator it(raw, patterns,
                            QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext())
                batch << it.next();
        } else if (fi.isFile()) {
            batch << fi.absoluteFilePath();
        } else {
            continue;
        }

        for (const QString &p : batch) {
            const QString abs = QFileInfo(p).absoluteFilePath();
            if (!Converter::isSupportedSource(abs)) {
                unsupported << QFileInfo(abs).fileName();
                continue;
            }
            if (existing.contains(abs)) {
                ++duplicated;
                continue;
            }
            existing.insert(abs);

            const QFileInfo info(abs);
            auto *item = new QListWidgetItem(info.fileName(), m_fileList);
            item->setData(Qt::UserRole, abs);
            item->setData(Qt::UserRole + 1, StatePending);
            const QString baseTip = QString("%1\n大小：%2").arg(abs, humanSize(info.size()));
            item->setData(Qt::UserRole + 2, baseTip);   // 原始提示，重置状态时用
            item->setToolTip(baseTip);
            ++added;
        }
    }

    if (added > 0) {
        appendLog(QString("已添加 %1 个文件（列表共 %2 个）")
                      .arg(added).arg(m_fileList->count()));
    }
    if (duplicated > 0)
        appendLog(QString("忽略 %1 个重复文件").arg(duplicated));
    if (!unsupported.isEmpty()) {
        const QString names = unsupported.mid(0, 5).join("、");
        appendLog(QString("⚠️ 跳过不支持的格式：%1%2")
                      .arg(names)
                      .arg(unsupported.size() > 5
                               ? QString(" 等 %1 个").arg(unsupported.size())
                               : QString()));
    }
}

void MainWindow::onAddFiles()
{
    QStringList patterns;
    for (const QString &ext : Converter::supportedSourceExtensions())
        patterns << ("*." + ext);
    const QString filter = QString("文档 (%1);;所有文件 (*.*)").arg(patterns.join(' '));

    const QStringList files = QFileDialog::getOpenFileNames(
        this, "选择要转换的文件", QString(), filter);
    addFilesToList(files);
}

void MainWindow::onAddFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(this, "选择包含待转换文档的文件夹");
    if (!dir.isEmpty())
        addFilesToList({dir});
}

void MainWindow::onRemoveSelected()
{
    const auto items = m_fileList->selectedItems();
    for (QListWidgetItem *item : items)
        delete item;
}

void MainWindow::onClearList()
{
    m_fileList->clear();
    m_progressBar->setValue(0);
    m_progressLabel->clear();
}

QStringList MainWindow::selectedFilePaths() const
{
    QStringList paths;
    for (int i = 0; i < m_fileList->count(); ++i)
        paths << m_fileList->item(i)->data(Qt::UserRole).toString();
    return paths;
}

void MainWindow::setItemStatus(int index, int state, const QString &tooltip)
{
    QListWidgetItem *item = m_fileList->item(index);
    if (!item)
        return;

    item->setData(Qt::UserRole + 1, state);

    QIcon icon;
    QColor color;
    switch (state) {
    case StateRunning:
        icon = style()->standardIcon(QStyle::SP_MediaPlay);
        break;
    case StateOk:
        icon = style()->standardIcon(QStyle::SP_DialogApplyButton);
        color = QColor(0x1a, 0x7f, 0x37);
        break;
    case StateFailed:
        icon = style()->standardIcon(QStyle::SP_MessageBoxCritical);
        color = QColor(0xc0, 0x2b, 0x1f);
        break;
    case StateSkipped:
        icon = style()->standardIcon(QStyle::SP_MessageBoxWarning);
        color = QColor(0x8a, 0x6d, 0x00);
        break;
    default:
        break;
    }

    item->setIcon(icon);
    if (color.isValid())
        item->setForeground(color);
    else
        item->setForeground(QBrush());    // 传空画刷才会恢复默认颜色

    if (!tooltip.isEmpty())
        item->setToolTip(tooltip);
}

// ===========================================================================
// 输出选项
// ===========================================================================
void MainWindow::onChooseOutputDir()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, "选择输出目录", m_outputDirEdit->text());
    if (!dir.isEmpty())
        m_outputDirEdit->setText(QDir::toNativeSeparators(dir));
}

void MainWindow::onOpenOutputDir()
{
    QString dir;
    if (m_outputToSourceCheck->isChecked()) {
        if (m_fileList->count() > 0)
            dir = QFileInfo(m_fileList->item(0)->data(Qt::UserRole).toString()).absolutePath();
    } else {
        dir = m_outputDirEdit->text();
    }

    if (dir.isEmpty() || !QDir(dir).exists()) {
        QMessageBox::information(this, "提示", "输出目录还不存在，转换完成后会自动创建。");
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

// ===========================================================================
// 转换
// ===========================================================================
void MainWindow::onConvert()
{
    if (isBusy())
        return;

    const int count = m_fileList->count();
    if (count == 0) {
        QMessageBox::information(this, "提示", "请先添加要转换的文件。");
        return;
    }
    if (!ensureEnginesOrWarn())
        return;

    const bool toSource = m_outputToSourceCheck->isChecked();
    const QString outputDir = m_outputDirEdit->text().trimmed();

    if (!toSource) {
        if (outputDir.isEmpty()) {
            QMessageBox::warning(this, "提示", "请先选择输出目录。");
            return;
        }
        QDir dir(outputDir);
        if (!dir.exists() && !dir.mkpath(".")) {
            QMessageBox::warning(this, "错误",
                QString("输出目录无效，且无法创建：\n%1").arg(outputDir));
            return;
        }
    }

    const QString target = m_formatCombo->currentData().toString();
    m_currentTarget = target;
    const Converter::Options options = collectOptions();

    // 重置所有状态
    m_cancelToken.reset();
    m_successCount = m_failedCount = m_skippedCount = 0;
    m_totalCount = count;
    m_progressBar->setMaximum(count);
    m_progressBar->setValue(0);
    m_progressLabel->setText(QString("0 / %1").arg(count));
    for (int i = 0; i < count; ++i) {
        m_fileList->item(i)->setToolTip(m_fileList->item(i)->data(Qt::UserRole + 2).toString());
        setItemStatus(i, StatePending, m_fileList->item(i)->toolTip());
    }

    appendLog(QString("──────── 开始转换 %1 个文件 → %2 ────────")
                  .arg(count).arg(target.toUpper()));

    // 高级选项生效时在日志里留一行，出问题时便于回溯用了什么参数
    QStringList active;
    if (options.toc)
        active << "生成目录";
    if (options.numberSections)
        active << "章节编号";
    if (!options.referenceDoc.isEmpty())
        active << ("模板：" + QFileInfo(options.referenceDoc).fileName());
    if (!options.markdownFlavor.isEmpty())
        active << ("Markdown 方言：" + options.markdownFlavor);
    if (options.dpi > 0)
        active << QString("DPI：%1").arg(options.dpi);
    if (!options.author.isEmpty())
        active << ("作者：" + options.author);
    if (!active.isEmpty())
        appendLog("   高级选项：" + active.join("，"));

    setBusy(true);

    m_worker = new ConverterWorker(selectedFilePaths(), target, outputDir, toSource,
                                   options, &m_cancelToken);
    auto *thread = new QThread;

    m_worker->moveToThread(thread);
    connect(thread, &QThread::started, m_worker, &ConverterWorker::run);
    connect(m_worker, &ConverterWorker::fileStarted, this, &MainWindow::onFileStarted);
    connect(m_worker, &ConverterWorker::fileSkipped, this, &MainWindow::onFileSkipped);
    connect(m_worker, &ConverterWorker::fileSucceeded, this, &MainWindow::onFileSucceeded);
    connect(m_worker, &ConverterWorker::fileFailed, this, &MainWindow::onFileFailed);
    connect(m_worker, &ConverterWorker::finished, this, &MainWindow::onWorkerFinished);
    connect(m_worker, &ConverterWorker::finished, thread, &QThread::quit);
    connect(thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    m_workerThread = thread;
    thread->start();
}

void MainWindow::onCancel()
{
    if (!m_busy)
        return;
    m_cancelToken.cancel();
    m_cancelBtn->setEnabled(false);
    m_cancelBtn->setText("正在取消…");
    appendLog("正在取消，等待当前文件处理结束…");
}

void MainWindow::onFileStarted(int index, const QString &path)
{
    setItemStatus(index, StateRunning,
                  QString("%1\n正在转换…").arg(path));
    m_progressLabel->setText(QString("%1 / %2　%3")
                                 .arg(index)
                                 .arg(m_totalCount)
                                 .arg(QFileInfo(path).fileName()));
    appendLog(QString("▶ %1").arg(QFileInfo(path).fileName()));
    m_fileList->scrollToItem(m_fileList->item(index));
}

void MainWindow::onFileSkipped(int index, const QString &reason)
{
    const QString path = m_fileList->item(index)->data(Qt::UserRole).toString();
    setItemStatus(index, StateSkipped, QString("%1\n%2").arg(path, reason));
    m_progressBar->setValue(index + 1);
    appendLog("   ⏭ " + reason);
    addHistory(path, m_currentTarget, QString(), "skipped", reason);
}

void MainWindow::onFileSucceeded(int index, const QString &outputPath, const QString &note)
{
    const QString path = m_fileList->item(index)->data(Qt::UserRole).toString();
    setItemStatus(index, StateOk, QString("%1\n→ %2").arg(path, outputPath));
    m_progressBar->setValue(index + 1);
    appendLog("   ✅ " + QFileInfo(outputPath).fileName());
    if (!note.isEmpty())
        appendLog("   ℹ️ " + note);
    addHistory(path, m_currentTarget, outputPath, "ok", note);
}

void MainWindow::onFileFailed(int index, const QString &reason)
{
    const QString path = m_fileList->item(index)->data(Qt::UserRole).toString();
    setItemStatus(index, StateFailed, QString("%1\n失败：%2").arg(path, reason));
    m_progressBar->setValue(index + 1);
    appendLog("   ❌ " + reason);
    addHistory(path, m_currentTarget, QString(), "failed", reason);
}

void MainWindow::onWorkerFinished(int success, int failed, int skipped, bool canceled)
{
    m_successCount = success;
    m_failedCount = failed;
    m_skippedCount = skipped;

    setBusy(false);
    m_worker = nullptr;

    const QString summary = QString("成功 %1　跳过 %2　失败 %3")
                                .arg(success).arg(skipped).arg(failed);
    m_progressLabel->setText(canceled ? "已取消" : summary);
    appendLog(QString("──────── %1：%2 ────────")
                  .arg(canceled ? "已取消" : "转换结束", summary));
    saveHistory();

    if (canceled) {
        QMessageBox::information(this, "已取消",
            QString("转换已取消。\n\n已完成：%1").arg(summary));
        return;
    }

    QMessageBox box(QMessageBox::Information, "完成",
                    QString("全部处理完毕。\n\n%1").arg(summary),
                    QMessageBox::NoButton, this);
    QPushButton *openBtn = box.addButton("打开输出目录", QMessageBox::AcceptRole);
    box.addButton("关闭", QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == openBtn)
        onOpenOutputDir();
}

void MainWindow::setBusy(bool busy)
{
    m_busy = busy;
    updateControls();
}

// 所有「当前能不能点」的判断集中在这里：
// m_busy 表示正在转换（可取消），m_installing 表示正在装组件（不可取消）
void MainWindow::updateControls()
{
    const bool busy = isBusy();

    m_convertBtn->setEnabled(!busy);
    m_cancelBtn->setVisible(m_busy);        // 只有转换可以取消
    m_cancelBtn->setEnabled(m_busy);
    m_cancelBtn->setText("取消");

    m_addFilesBtn->setEnabled(!busy);
    m_addFolderBtn->setEnabled(!busy);
    m_removeBtn->setEnabled(!busy);
    m_clearBtn->setEnabled(!busy);
    m_formatCombo->setEnabled(!busy);
    m_fileList->setEnabled(!busy);
    m_installBtn->setEnabled(!busy && !(m_pandocReady && m_sofficeReady && m_pdf2docxReady));

    const bool manual = !m_outputToSourceCheck->isChecked();
    m_outputDirEdit->setEnabled(!busy && manual);
    m_browseBtn->setEnabled(!busy && manual);
    m_outputToSourceCheck->setEnabled(!busy);

    // 高级选项：转换过程中不允许改，避免「改了一半」的参数混进同一批
    m_advancedToggle->setEnabled(!busy);
    m_advancedPanel->setEnabled(!busy);
    m_profileCombo->setEnabled(!busy);
    if (!busy)
        updateFormatDependentOptions();
}

// ===========================================================================
// 组件安装
// ===========================================================================
void MainWindow::onInstallDependencies()
{
    if (isBusy())
        return;

    if (!Installer::isWingetAvailable()) {
        QMessageBox::warning(this, "无法自动安装",
            "未检测到 winget（Windows 包管理器）。\n\n"
            "解决办法：打开「Microsoft Store」，搜索并安装「应用安装程序」（App Installer），"
            "装好后重启电脑再点这个按钮。\n\n"
            "或者手动下载安装：\n"
            "Pandoc：https://pandoc.org/installing.html\n"
            "LibreOffice：https://www.libreoffice.org/download/download/\n"
            "Python：https://www.python.org/downloads/（装完执行 pip install pdf2docx）");
        return;
    }

    const QStringList missing = [&]() {
        QStringList list;
        if (!m_pandocReady)
            list << "Pandoc";
        if (!m_sofficeReady)
            list << "LibreOffice";
        if (!m_pdf2docxReady)
            list << "Python + pdf2docx";
        return list;
    }();

    if (missing.isEmpty()) {
        QMessageBox::information(this, "提示", "所需组件都已安装，无需重复安装。");
        return;
    }

    const auto reply = QMessageBox::question(
        this, "确认安装",
        QString("即将自动下载安装：%1\n\n"
                "LibreOffice 约 300MB，整个过程可能需要几分钟，"
                "期间系统可能弹出安全确认框，请点「是」放行。\n\n是否继续？")
            .arg(missing.join("、")));
    if (reply != QMessageBox::Yes)
        return;

    // 注意：安装只置 m_installing，不置 m_busy——
    // m_busy 专指转换，因为安装过程无法取消，不能显示「取消」按钮
    m_installing = true;
    updateControls();
    m_progressLabel->setText("正在安装组件…");
    m_progressBar->setMaximum(0);      // 忙碌条：安装过程拿不到百分比
    m_installBtn->setEnabled(false);

    m_installWorker = new InstallerWorker(!m_pandocReady, !m_sofficeReady, !m_pdf2docxReady);
    auto *thread = new QThread;
    m_installWorker->moveToThread(thread);

    connect(thread, &QThread::started, m_installWorker, &InstallerWorker::run);
    connect(m_installWorker, &InstallerWorker::logMessage, this, &MainWindow::onInstallLog);
    connect(m_installWorker, &InstallerWorker::stepResult, this, &MainWindow::onInstallStepResult);
    connect(m_installWorker, &InstallerWorker::finished, this, &MainWindow::onInstallFinished);
    connect(m_installWorker, &InstallerWorker::finished, thread, &QThread::quit);
    connect(thread, &QThread::finished, m_installWorker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    m_installThread = thread;
    thread->start();
}

void MainWindow::onInstallLog(const QString &text)
{
    appendLog(text);
}

void MainWindow::onInstallStepResult(bool ok, const QString &message)
{
    appendLog((ok ? QString("✅ ") : QString("❌ ")) + message);
}

void MainWindow::onInstallFinished(int succeeded, int failed)
{
    m_installWorker = nullptr;
    m_installing = false;

    // 恢复进度条（安装期间用的是不确定进度的忙碌条）
    m_progressBar->setMaximum(m_totalCount > 0 ? m_totalCount : 1);
    m_progressBar->setValue(0);
    m_progressLabel->clear();

    updateControls();
    // worker 里已经 refreshEngines() 过，这里重新探测状态即可
    QMetaObject::invokeMethod(this, &MainWindow::startEngineCheck, Qt::QueuedConnection);

    QMessageBox::information(this, "安装结束",
        QString("安装流程已结束：成功 %1 项，失败 %2 项。\n\n"
                "若状态仍显示 ❌，可能需要重启本程序让环境变量完全生效。")
            .arg(succeeded).arg(failed));
}

void MainWindow::onSaveLog()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (dir.isEmpty())
        dir = QDir::homePath();

    const QString file = QFileDialog::getSaveFileName(
        this, "保存日志", QDir(dir).filePath("DocConverter_log.txt"),
        "文本文件 (*.txt);;所有文件 (*.*)");
    if (file.isEmpty())
        return;

    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, "错误", "无法写入文件：\n" + file);
        return;
    }
    f.write(m_logEdit->toPlainText().toUtf8());
    f.close();
    appendLog("日志已保存到：" + file);
}

// ===========================================================================
// 杂项
// ===========================================================================
void MainWindow::appendLog(const QString &text)
{
    m_logEdit->appendPlainText(
        QString("[%1] %2").arg(QTime::currentTime().toString("HH:mm:ss"), text));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // winget 安装不可中途取消，强退可能留下装了一半的 LibreOffice / Python
    if (m_installing) {
        QMessageBox::information(this, "正在安装组件",
            "组件正在安装中，请等待安装完成后再退出。\n\n"
            "强制退出可能导致 LibreOffice / Python 安装不完整。");
        event->ignore();
        return;
    }

    if (m_busy) {
        const auto reply = QMessageBox::question(
            this, "正在转换",
            "还有文件正在转换，确定要退出吗？\n退出会中断当前转换。");
        if (reply != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        m_cancelToken.cancel();
        if (m_workerThread)
            m_workerThread->wait(15000);
    }

    saveSettings();
    event->accept();
}
