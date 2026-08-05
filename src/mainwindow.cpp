#include "mainwindow.h"
#include "converter.h"
#include "installer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QUrl>
#include <QCoreApplication>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle("文档格式转换工具");
    resize(760, 560);
    setAcceptDrops(true);

    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    QVBoxLayout *mainLayout = new QVBoxLayout(central);

    QHBoxLayout *engineLayout = new QHBoxLayout();
    m_engineStatusLabel = new QLabel(this);
    engineLayout->addWidget(m_engineStatusLabel, 1);
    m_installBtn = new QPushButton("一键安装缺失组件", this);
    engineLayout->addWidget(m_installBtn);
    mainLayout->addLayout(engineLayout);

    QLabel *hint = new QLabel(
        "将文件拖拽到下方列表，或点击「添加文件」选择要转换的文档\n"
        "（支持 docx / doc / odt / rtf / txt / html / md / epub / pdf）",
        this);
    hint->setWordWrap(true);
    mainLayout->addWidget(hint);

    m_fileList = new QListWidget(this);
    mainLayout->addWidget(m_fileList, 1);

    QHBoxLayout *fileBtnLayout = new QHBoxLayout();
    QPushButton *addBtn = new QPushButton("添加文件", this);
    QPushButton *removeBtn = new QPushButton("移除所选", this);
    QPushButton *clearBtn = new QPushButton("清空列表", this);
    fileBtnLayout->addWidget(addBtn);
    fileBtnLayout->addWidget(removeBtn);
    fileBtnLayout->addWidget(clearBtn);
    fileBtnLayout->addStretch();
    mainLayout->addLayout(fileBtnLayout);

    QHBoxLayout *optionLayout = new QHBoxLayout();
    optionLayout->addWidget(new QLabel("目标格式：", this));
    m_formatCombo = new QComboBox(this);
    for (const QString &fmt : Converter::supportedTargetFormats()) {
        m_formatCombo->addItem(fmt.toUpper(), fmt);
    }
    optionLayout->addWidget(m_formatCombo);

    optionLayout->addWidget(new QLabel("输出目录：", this));
    m_outputDirEdit = new QLineEdit(this);
    m_outputDirEdit->setText(QDir::homePath() + "/Desktop");
    optionLayout->addWidget(m_outputDirEdit, 1);
    QPushButton *browseBtn = new QPushButton("浏览...", this);
    optionLayout->addWidget(browseBtn);
    mainLayout->addLayout(optionLayout);

    m_convertBtn = new QPushButton("开始转换", this);
    m_convertBtn->setMinimumHeight(36);
    mainLayout->addWidget(m_convertBtn);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setValue(0);
    mainLayout->addWidget(m_progressBar);

    m_logEdit = new QPlainTextEdit(this);
    m_logEdit->setReadOnly(true);
    m_logEdit->setMaximumBlockCount(2000);
    mainLayout->addWidget(m_logEdit, 1);

    connect(addBtn, &QPushButton::clicked, this, &MainWindow::onAddFiles);
    connect(removeBtn, &QPushButton::clicked, this, &MainWindow::onRemoveSelected);
    connect(clearBtn, &QPushButton::clicked, this, &MainWindow::onClearList);
    connect(browseBtn, &QPushButton::clicked, this, &MainWindow::onChooseOutputDir);
    connect(m_convertBtn, &QPushButton::clicked, this, &MainWindow::onConvert);
    connect(m_installBtn, &QPushButton::clicked, this, &MainWindow::onInstallDependencies);

    checkEngines();
}

void MainWindow::checkEngines()
{
    const QString pandoc = Converter::findPandoc();
    const QString soffice = Converter::findLibreOffice();
    const bool pdf2docxReady = Converter::hasPdf2docx();

    QString status;
    status += pandoc.isEmpty() ? "❌ Pandoc　" : "✅ Pandoc　";
    status += soffice.isEmpty() ? "❌ LibreOffice　" : "✅ LibreOffice　";
    status += pdf2docxReady ? "✅ pdf2docx(高精度PDF转Word)" : "❌ pdf2docx(高精度PDF转Word，可选)";
    m_engineStatusLabel->setText(status);

    if (pandoc.isEmpty() && soffice.isEmpty()) {
        appendLog("警告：未检测到 Pandoc 和 LibreOffice，转换功能将无法使用。"
                   "请先安装其中至少一个（建议两者都装，以获得最好的格式覆盖率）。");
    }
}

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
}

void MainWindow::addFilesToList(const QStringList &paths)
{
    for (const QString &p : paths) {
        QFileInfo fi(p);
        if (!fi.isFile())
            continue;

        bool exists = false;
        for (int i = 0; i < m_fileList->count(); ++i) {
            if (m_fileList->item(i)->data(Qt::UserRole).toString() == p) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            QListWidgetItem *item = new QListWidgetItem(fi.fileName());
            item->setData(Qt::UserRole, p);
            item->setToolTip(p);
            m_fileList->addItem(item);
        }
    }
}

void MainWindow::onAddFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, "选择要转换的文件", QString(),
        "文档 (*.docx *.doc *.odt *.rtf *.txt *.html *.htm *.md *.epub *.pdf);;所有文件 (*.*)");
    addFilesToList(files);
}

void MainWindow::onRemoveSelected()
{
    qDeleteAll(m_fileList->selectedItems());
}

void MainWindow::onClearList()
{
    m_fileList->clear();
}

void MainWindow::onChooseOutputDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, "选择输出目录", m_outputDirEdit->text());
    if (!dir.isEmpty())
        m_outputDirEdit->setText(dir);
}

void MainWindow::appendLog(const QString &text)
{
    m_logEdit->appendPlainText(text);
}

void MainWindow::onConvert()
{
    const int count = m_fileList->count();
    if (count == 0) {
        QMessageBox::information(this, "提示", "请先添加要转换的文件");
        return;
    }

    const QString outputDir = m_outputDirEdit->text();
    QDir dir(outputDir);
    if (!dir.exists() && !dir.mkpath(".")) {
        QMessageBox::warning(this, "错误", "输出目录无效，且无法创建");
        return;
    }

    const QString target = m_formatCombo->currentData().toString();

    m_convertBtn->setEnabled(false);
    m_progressBar->setMaximum(count);
    m_progressBar->setValue(0);

    int successCount = 0;
    for (int i = 0; i < count; ++i) {
        const QString path = m_fileList->item(i)->data(Qt::UserRole).toString();
        appendLog(QString("正在转换: %1").arg(path));
        QCoreApplication::processEvents();

        const Converter::Result r = Converter::convertFile(path, target, outputDir);
        if (r.success) {
            successCount++;
            appendLog(QString("  ✅ 成功 -> %1").arg(r.outputPath));
        } else {
            appendLog(QString("  ❌ 失败: %1").arg(r.message));
        }

        m_progressBar->setValue(i + 1);
        QCoreApplication::processEvents();
    }

    appendLog(QString("转换完成：成功 %1 / %2").arg(successCount).arg(count));
    m_convertBtn->setEnabled(true);
    QMessageBox::information(this, "完成",
                              QString("转换完成，成功 %1 / %2 个文件").arg(successCount).arg(count));
}

void MainWindow::onInstallDependencies()
{
    const QString pandoc = Converter::findPandoc();
    const QString soffice = Converter::findLibreOffice();
    const bool pdf2docxReady = Converter::hasPdf2docx();

    if (!pandoc.isEmpty() && !soffice.isEmpty() && pdf2docxReady) {
        QMessageBox::information(this, "提示", "所需组件都已安装，无需重复安装。");
        return;
    }

    if (!Installer::isWingetAvailable()) {
        QMessageBox::warning(this, "无法自动安装",
            "未检测到 winget（Windows 包管理器）。\n\n"
            "解决办法：打开「Microsoft Store」，搜索并安装「应用安装程序」（App Installer），"
            "装好后重启电脑再点这个按钮试试。\n\n"
            "或者手动下载安装：\n"
            "Pandoc: https://pandoc.org/installing.html\n"
            "LibreOffice: https://www.libreoffice.org/download/download/\n"
            "Python: https://www.python.org/downloads/（装完后执行 pip install pdf2docx）");
        return;
    }

    const auto reply = QMessageBox::question(this, "确认安装",
        "即将自动下载安装缺失的组件（Pandoc / LibreOffice / Python+pdf2docx），"
        "可能需要几分钟，过程中系统可能会弹出安全确认框，请点「是」放行。\n\n是否继续？");
    if (reply != QMessageBox::Yes)
        return;

    m_installBtn->setEnabled(false);
    m_convertBtn->setEnabled(false);

    if (pandoc.isEmpty()) {
        appendLog("开始安装 Pandoc...");
        QCoreApplication::processEvents();
        const Installer::Result r = Installer::installPandoc();
        appendLog((r.success ? QString("✅ ") : QString("❌ ")) + r.message);
    }

    if (soffice.isEmpty()) {
        appendLog("开始安装 LibreOffice（体积较大，请耐心等待）...");
        QCoreApplication::processEvents();
        const Installer::Result r = Installer::installLibreOffice();
        appendLog((r.success ? QString("✅ ") : QString("❌ ")) + r.message);
    }

    if (!pdf2docxReady) {
        appendLog("开始安装 pdf2docx（用于提升PDF转Word精度，需要Python环境，可能较慢）...");
        QCoreApplication::processEvents();
        const Installer::Result r = Installer::installPdf2docx();
        appendLog((r.success ? QString("✅ ") : QString("❌ ")) + r.message);
    }

    appendLog("安装流程结束，重新检测组件状态...");
    checkEngines();

    m_installBtn->setEnabled(true);
    m_convertBtn->setEnabled(true);

    QMessageBox::information(this, "完成",
        "安装流程已结束，请查看上方状态和日志。如果显示仍未检测到，"
        "可能需要重启本程序、甚至重启电脑让 PATH 生效。");
}
