#include "previewdialog.h"

#include "converter.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

PreviewDialog::PreviewDialog(const QString &path, QWidget *parent)
    : QDialog(parent), m_path(path)
{
    setWindowTitle(QString("预览 — %1").arg(QFileInfo(path).fileName()));
    resize(780, 580);

    auto *layout = new QVBoxLayout(this);

    m_status = new QLabel("正在提取文字…", this);
    m_status->setStyleSheet("color: palette(mid);");
    layout->addWidget(m_status);

    m_text = new QPlainTextEdit(this);
    m_text->setReadOnly(true);
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_text->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    layout->addWidget(m_text, 1);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    m_openBtn = new QPushButton("用默认程序打开", this);
    auto *closeBtn = new QPushButton("关闭", this);
    btnRow->addWidget(m_openBtn);
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);

    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_openBtn, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_path));
    });

    startLoading();
}

PreviewDialog::~PreviewDialog()
{
    // 关窗口时后台可能还在跑 Pandoc。线程对象由 deleteLater 回收，
    // 但析构函数不能让一个仍在运行的 QThread 被销毁，这里等一小会儿。
    if (m_thread && m_thread->isRunning()) {
        m_thread->requestInterruption();
        m_thread->wait(5000);
    }
}

void PreviewDialog::startLoading()
{
    QPointer<PreviewDialog> self(this);
    const QString path = m_path;

    m_thread = QThread::create([self, path]() {
        QString text;
        QString error;
        const bool ok = Converter::extractPreviewText(path, 20000, &text, &error);

        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, ok, text, error]() {
                if (!self)
                    return;
                if (ok) {
                    self->m_text->setPlainText(text);
                    self->m_status->setText(
                        QString("共读取 %1 个字符（只显示开头部分）").arg(text.size()));
                } else {
                    self->m_status->setText("无法预览：" + error);
                    self->m_text->setPlainText(
                        "这个文件没法在窗口里直接预览。\n\n"
                        "常见原因：\n"
                        "· 图片型扫描件（PDF 里没有文字层，需要 OCR）\n"
                        "· 文件是加密的，或正被其他程序占用\n"
                        "· 当前缺少对应的解析引擎\n\n"
                        "可以点下面的「用默认程序打开」查看原文件。");
                }
            },
            Qt::QueuedConnection);
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
}
