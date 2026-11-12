// Converter 的单元测试 + 引擎集成测试。
//
// 纯逻辑用例（命名、格式判断、跳过、取消）不依赖任何外部程序；
// 集成用例需要本机装了 Pandoc / LibreOffice，没装会自动 QSKIP 而不是报错。
//
// 运行：ctest --test-dir build/windows-msvc --output-on-failure

#include <QtTest>

#include "converter.h"

class TestConverter : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // ---- 纯逻辑，不需要引擎 ----
    void testUniqueOutputPath();
    void testIsSupportedSource();
    void testSameFormatSkipped();
    void testMissingInput();
    void testCancelToken();

    // ---- 集成，需要引擎 ----
    void testMdToDocx();
    void testNoOverwrite();
    void testMdToPdf();
    void testDocxToHtml();
    void testDocxToMd();
    void testPdfToDocx();

private:
    void requireEngines();
    QString writeSample(const QString &name, const QByteArray &content);

    QTemporaryDir m_dir;
    QString m_outDir;
    QString m_mdPath;
    QString m_docxPath;     // testMdToDocx 生成，后续用例复用
    QString m_pdfPath;      // testMdToPdf 生成
};

void TestConverter::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_outDir = m_dir.filePath("out");
    QVERIFY(QDir().mkpath(m_outDir));

    m_mdPath = writeSample("sample.md",
                           "# 标题一\n\n这是一段中文正文，用来测试转换链路。\n\n"
                           "| 列A | 列B |\n|-----|-----|\n| 1 | 2 |\n\n"
                           "1. 第一项\n2. 第二项\n");
    QVERIFY(QFileInfo::exists(m_mdPath));
}

void TestConverter::requireEngines()
{
    if (Converter::pandocPath().isEmpty() && Converter::sofficePath().isEmpty())
        QSKIP("Pandoc / LibreOffice 均未安装，跳过引擎集成测试");
}

QString TestConverter::writeSample(const QString &name, const QByteArray &content)
{
    const QString path = m_dir.filePath(name);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    f.write(content);
    return path;
}

// ---------------------------------------------------------------------------
// 纯逻辑
// ---------------------------------------------------------------------------
void TestConverter::testUniqueOutputPath()
{
    // 不存在的路径原样返回
    const QString fresh = m_dir.filePath("unique/a.pdf");
    QCOMPARE(Converter::uniqueOutputPath(fresh), fresh);

    // 已存在则加序号，且不会覆盖原文件
    const QString existing = writeSample("unique/dup.pdf", "x");
    QVERIFY(!existing.isEmpty());
    const QString second = Converter::uniqueOutputPath(existing);
    QVERIFY(second != existing);
    QVERIFY(second.contains("(1)"));

    // 连续调用应递增
    writeSample("unique/dup (1).pdf", "x");
    const QString third = Converter::uniqueOutputPath(existing);
    QVERIFY(third.contains("(2)"));
}

void TestConverter::testIsSupportedSource()
{
    QVERIFY(Converter::isSupportedSource("a.pdf"));
    QVERIFY(Converter::isSupportedSource("a.DOCX"));
    QVERIFY(Converter::isSupportedSource("a.htm"));
    QVERIFY(Converter::isSupportedSource("a.markdown"));
    QVERIFY(!Converter::isSupportedSource("a.exe"));
    QVERIFY(!Converter::isSupportedSource("a.xlsx"));
    QVERIFY(!Converter::isSupportedSource("noext"));
}

void TestConverter::testSameFormatSkipped()
{
    const Converter::Result r = Converter::convertFile(m_mdPath, "md", m_outDir);
    QVERIFY(r.skipped);
    QVERIFY(!r.success);        // 跳过不算成功，也不算失败
    QVERIFY(!r.canceled);
}

void TestConverter::testMissingInput()
{
    const Converter::Result r =
        Converter::convertFile(m_dir.filePath("nope.md"), "pdf", m_outDir);
    QVERIFY(!r.success);
    QVERIFY(!r.skipped);
    QVERIFY(!r.message.isEmpty());
}

void TestConverter::testCancelToken()
{
    Converter::CancelToken token;
    QVERIFY(!token.isCanceled());
    token.cancel();
    QVERIFY(token.isCanceled());

    // 预先取消时，不应产生任何输出文件
    const Converter::Result r = Converter::convertFile(m_mdPath, "pdf", m_outDir, &token);
    QVERIFY(r.canceled);
    QVERIFY(!r.success);

    token.reset();
    QVERIFY(!token.isCanceled());
}

// ---------------------------------------------------------------------------
// 引擎集成
// ---------------------------------------------------------------------------
void TestConverter::testMdToDocx()
{
    requireEngines();
    if (Converter::pandocPath().isEmpty())
        QSKIP("未安装 Pandoc");

    const Converter::Result r = Converter::convertFile(m_mdPath, "docx", m_outDir);
    QVERIFY2(r.success, qPrintable(r.message));
    QVERIFY(QFileInfo(r.outputPath).size() > 0);
    m_docxPath = r.outputPath;
}

void TestConverter::testNoOverwrite()
{
    requireEngines();
    if (Converter::pandocPath().isEmpty())
        QSKIP("未安装 Pandoc");

    const Converter::Result first = Converter::convertFile(m_mdPath, "docx", m_outDir);
    QVERIFY2(first.success, qPrintable(first.message));

    const Converter::Result second = Converter::convertFile(m_mdPath, "docx", m_outDir);
    QVERIFY2(second.success, qPrintable(second.message));

    // 两次都得到文件，但路径不同，且第一次的产物没被覆盖
    QVERIFY(first.outputPath != second.outputPath);
    QVERIFY(QFileInfo::exists(first.outputPath));
    QVERIFY(QFileInfo::exists(second.outputPath));
}

void TestConverter::testMdToPdf()
{
    requireEngines();
    if (Converter::pandocPath().isEmpty() || Converter::sofficePath().isEmpty())
        QSKIP("md->pdf 需要 Pandoc 和 LibreOffice 同时在场");

    const Converter::Result r = Converter::convertFile(m_mdPath, "pdf", m_outDir);
    QVERIFY2(r.success, qPrintable(r.message));
    QVERIFY(QFileInfo(r.outputPath).size() > 1000);

    // 确认真的是 PDF
    QFile f(r.outputPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.read(4), QByteArray("%PDF"));
    m_pdfPath = r.outputPath;
}

void TestConverter::testDocxToHtml()
{
    requireEngines();
    if (m_docxPath.isEmpty())
        QSKIP("依赖 testMdToDocx 的产物");

    const Converter::Result r = Converter::convertFile(m_docxPath, "html", m_outDir);
    QVERIFY2(r.success, qPrintable(r.message));

    QFile f(r.outputPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString html = QString::fromUtf8(f.readAll());

    // 必须是完整文档，而不是裸片段
    QVERIFY(html.contains("<html", Qt::CaseInsensitive));
    QVERIFY(html.contains("<head", Qt::CaseInsensitive));
    QVERIFY(html.contains("标题一"));
}

void TestConverter::testDocxToMd()
{
    requireEngines();
    if (m_docxPath.isEmpty())
        QSKIP("依赖 testMdToDocx 的产物");

    const Converter::Result r = Converter::convertFile(m_docxPath, "md", m_outDir);
    QVERIFY2(r.success, qPrintable(r.message));

    QFile f(r.outputPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString md = QString::fromUtf8(f.readAll());
    QVERIFY(md.contains("标题一"));
}

void TestConverter::testPdfToDocx()
{
    requireEngines();
    if (m_pdfPath.isEmpty())
        QSKIP("依赖 testMdToPdf 的产物");
    if (Converter::sofficePath().isEmpty() && !Converter::pdf2docxAvailable())
        QSKIP("pdf->docx 需要 pdf2docx 或 LibreOffice");

    const Converter::Result r = Converter::convertFile(m_pdfPath, "docx", m_outDir);
    QVERIFY2(r.success, qPrintable(r.message));
    QVERIFY(QFileInfo(r.outputPath).size() > 0);

    // 这是文字版 PDF，不应触发扫描件提示
    QVERIFY(!r.message.contains("扫描件"));
}

QTEST_GUILESS_MAIN(TestConverter)
#include "test_converter.moc"
