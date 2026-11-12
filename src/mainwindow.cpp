#include "mainwindow.h"
#include "converterworker.h"
#include "installerworker.h"
#include "installer.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QBrush>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileInfoList>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QThread>
#include <QTime>
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

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle("文档格式转换工具");
    setAcceptDrops(true);
    resize(820, 640);

    buildUi();
    loadSettings();

    appendLog("就绪。把文件（或文件夹）拖进窗口，选择目标格式后点「开始转换」。");
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
        "把文件或整个文件夹拖到下面的列表，也可用「添加文件 / 添加文件夹」。\n"
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
}

// ===========================================================================
// 设置持久化
// ===========================================================================
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
}

void MainWindow::saveSettings()
{
    QSettings s;
    s.setValue(kSettingGeometry, saveGeometry());
    s.setValue(kSettingFormat, m_formatCombo->currentData().toString());
    s.setValue(kSettingOutputDir, m_outputDirEdit->text());
    s.setValue(kSettingOutputToSource, m_outputToSourceCheck->isChecked());
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

    setBusy(true);

    m_worker = new ConverterWorker(selectedFilePaths(), target, outputDir, toSource, &m_cancelToken);
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
}

void MainWindow::onFileSucceeded(int index, const QString &outputPath, const QString &note)
{
    const QString path = m_fileList->item(index)->data(Qt::UserRole).toString();
    setItemStatus(index, StateOk, QString("%1\n→ %2").arg(path, outputPath));
    m_progressBar->setValue(index + 1);
    appendLog("   ✅ " + QFileInfo(outputPath).fileName());
    if (!note.isEmpty())
        appendLog("   ℹ️ " + note);
}

void MainWindow::onFileFailed(int index, const QString &reason)
{
    const QString path = m_fileList->item(index)->data(Qt::UserRole).toString();
    setItemStatus(index, StateFailed, QString("%1\n失败：%2").arg(path, reason));
    m_progressBar->setValue(index + 1);
    appendLog("   ❌ " + reason);
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
