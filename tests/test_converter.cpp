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
    void testFormatLists();
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
    void testMdToTex();
    void testMdToPptx();
    void testDocxToDoc();
    void testPdfToMarkdown();
    void testGbkTxtToMd();
    void testUtf16TxtToMd();
    void testLongParagraphNotWrapped();
    void testPptxStructureToMd();
    void testOdtHasNoInjectedTitle();
    void testOptionsHtmlMetadataAndToc();
    void testMarkdownFlavorGfm();

private:
    void requireEngines();
    QString writeSample(const QString &name, const QByteArray &content);

    QTemporaryDir m_dir;
    QString m_outDir;
    QString m_mdPath;
    QString m_docxPath;     // testMdToDocx 生成，后续用例复用
    QString m_pdfPath;      // testMdToPdf 生成
    QString m_pptxPath;     // testMdToPptx 生成
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

void TestConverter::testFormatLists()
{
    const QStringList targets = Converter::supportedTargetFormats();
    for (const QString &expected : {"pdf", "docx", "doc", "odt", "rtf", "txt",
                                    "html", "md", "epub", "pptx", "tex",
                                    "rst", "org", "adoc"}) {
        QVERIFY2(targets.contains(expected), qPrintable("缺少目标格式 " + expected));
        // 每个目标格式都应该能当源格式用，否则会出现「能转出却拖不进来」
        QVERIFY2(Converter::isSupportedSource("a." + expected),
                 qPrintable("目标格式没有对应的源格式支持：" + expected));
    }

    // 源后缀清单要覆盖目标格式（多出来的 htm/markdown/asciidoc 是别名，正常）
    const QStringList sources = Converter::supportedSourceExtensions();
    for (const QString &t : targets) {
        bool covered = false;
        for (const QString &s : sources) {
            if (QFileInfo("a." + s).suffix().compare(t, Qt::CaseInsensitive) == 0)
                covered = true;
        }
        QVERIFY2(covered, qPrintable("源后缀清单缺少：" + t));
    }
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

void TestConverter::testMdToTex()
{
    requireEngines();
    if (Converter::pandocPath().isEmpty())
        QSKIP("未安装 Pandoc");

    const Converter::Result r = Converter::convertFile(m_mdPath, "tex", m_outDir);
    QVERIFY2(r.success, qPrintable(r.message));

    QFile f(r.outputPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString tex = QString::fromUtf8(f.readAll());
    QVERIFY(tex.contains("\\documentclass"));
    QVERIFY(tex.contains("标题一"));
}

void TestConverter::testMdToPptx()
{
    requireEngines();
    if (Converter::pandocPath().isEmpty())
        QSKIP("未安装 Pandoc");

    const Converter::Result r = Converter::convertFile(m_mdPath, "pptx", m_outDir);
    QVERIFY2(r.success, qPrintable(r.message));
    QVERIFY(QFileInfo(r.outputPath).size() > 1000);

    // pptx 是 zip 容器（PK 头），顺便确认不是空壳
    QFile f(r.outputPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.read(2), QByteArray("PK"));
    m_pptxPath = r.outputPath;
}

void TestConverter::testDocxToDoc()
{
    requireEngines();
    if (m_docxPath.isEmpty())
        QSKIP("依赖 testMdToDocx 的产物");
    if (Converter::sofficePath().isEmpty())
        QSKIP("doc 格式只有 LibreOffice 能写，未安装则跳过");

    const Converter::Result r = Converter::convertFile(m_docxPath, "doc", m_outDir);
    QVERIFY2(r.success, qPrintable(r.message));
    QVERIFY(QFileInfo(r.outputPath).size() > 0);
    QCOMPARE(QFileInfo(r.outputPath).suffix(), QString("doc"));
}

void TestConverter::testPdfToMarkdown()
{
    requireEngines();
    if (m_pdfPath.isEmpty())
        QSKIP("依赖 testMdToPdf 的产物");
    // LibreOffice 的 PDF 导入会把正文塞进文本框，Pandoc 读不到，
    // 只有装了 pdf2docx 才拿得到完整正文，所以这条用例在无 pdf2docx 时跳过
    if (!Converter::pdf2docxAvailable())
        QSKIP("未安装 pdf2docx，PDF 正文提取质量无法保证");

    const Converter::Result r = Converter::convertFile(m_pdfPath, "md", m_outDir);
    QVERIFY2(r.success, qPrintable(r.message));

    QFile f(r.outputPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString md = QString::fromUtf8(f.readAll());
    QVERIFY2(md.size() > 100, qPrintable("markdown 太小，正文可能没提取到：" + md));
    QVERIFY(md.contains("中文正文"));

    // 图片链接必须是相对路径，否则文件搬走后全断
    QVERIFY2(!md.contains(":/") || !md.contains("_media/"),
             qPrintable("markdown 里的图片链接是绝对路径：" + md));
}

// GBK 编码的 txt 是最容易出乱码的场景：Pandoc 只认 UTF-8，会退化成 latin1，
// 转出来就是「ÖÐÎÄ」这种。转换前必须先识别编码并转成 UTF-8。
void TestConverter::testGbkTxtToMd()
{
    requireEngines();
    if (Converter::pandocPath().isEmpty())
        QSKIP("未安装 Pandoc");

    // “中文标题\n\n这是中文正文。\n” 的 GBK 字节
    const QByteArray gbk = QByteArray::fromHex(
        "d6d0cec4b1eacce20a0ad5e2cac7d6d0cec4d5fdcec4a1a30a");
    const QString txtPath = writeSample("enc/gbk.txt", gbk);
    QVERIFY(!txtPath.isEmpty());

    const Converter::Result r = Converter::convertFile(txtPath, "md", m_outDir);
    QVERIFY2(r.success, qPrintable(r.message));

    QFile f(r.outputPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString md = QString::fromUtf8(f.readAll());

    QVERIFY2(md.contains("中文标题"), qPrintable("GBK 没被正确识别：" + md));
    QVERIFY2(md.contains("这是中文正文"), qPrintable("GBK 正文丢失：" + md));
    QVERIFY2(!md.contains(QChar(0xFFFD)), qPrintable("输出里出现替换字符：" + md));
    // 应当提示编码被转换过，用户才知道为什么文件被「动过」
    QVERIFY2(r.message.contains("编码"), qPrintable("没有报告编码转换：" + r.message));
}

// 记事本另存为「Unicode」得到的 UTF-16LE 文件：每个 ASCII 后面跟一个 0x00，
// 不按 UTF-16 读的话整篇都是乱码和空字符
void TestConverter::testUtf16TxtToMd()
{
    requireEngines();
    if (Converter::pandocPath().isEmpty())
        QSKIP("未安装 Pandoc");

    const QString text = QStringLiteral("UTF16 编码的中文正文。\n");
    QByteArray raw;
    raw.append("\xFF\xFE", 2);
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        raw.append(char(u & 0xFF));
        raw.append(char((u >> 8) & 0xFF));
    }
    const QString txtPath = writeSample("enc/u16.txt", raw);
    QVERIFY(!txtPath.isEmpty());

    const Converter::Result r = Converter::convertFile(txtPath, "md", m_outDir);
    QVERIFY2(r.success, qPrintable(r.message));

    QFile f(r.outputPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString md = QString::fromUtf8(f.readAll());
    QVERIFY2(md.contains("UTF16 编码的中文正文"), qPrintable("UTF-16 没被正确识别：" + md));
}

// Pandoc 的 markdown / plain 写出器默认按 72 列折行，中文段落会被从句子
// 中间硬切开；导出的 txt/md 看起来像断句错了，实际是排版副作用。
void TestConverter::testLongParagraphNotWrapped()
{
    requireEngines();
    if (Converter::pandocPath().isEmpty())
        QSKIP("未安装 Pandoc");

    const QString sentence =
        QStringLiteral("这是一个刻意写得很长的中文段落用来验证导出的文本不会被按列宽"
                       "硬折断因为一旦折断读者会以为句子就在这里结束了而实际上并没有"
                       "任何标点所以必须保持完整的一行才算正确。");
    const QString txtPath = writeSample("wrap/long.txt", sentence.toUtf8() + "\n");
    QVERIFY(!txtPath.isEmpty());

    const Converter::Result r = Converter::convertFile(txtPath, "md", m_outDir);
    QVERIFY2(r.success, qPrintable(r.message));

    QFile f(r.outputPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString md = QString::fromUtf8(f.readAll()).trimmed();
    QVERIFY2(md.contains(sentence), qPrintable("长段落被折行了：\n" + md));

    // txt 输出同理
    const Converter::Result r2 = Converter::convertFile(txtPath, "html", m_outDir);
    QVERIFY2(r2.success, qPrintable(r2.message));
}

// pptx 走 Pandoc 的读取器时是「按幻灯片结构抽取文本」，
// 比先拍平成 PDF 再提取要保真得多（标题层级、正文顺序都在）
void TestConverter::testPptxStructureToMd()
{
    requireEngines();
    if (m_pptxPath.isEmpty())
        QSKIP("依赖 testMdToPptx 的产物");
    if (Converter::pandocPath().isEmpty())
        QSKIP("未安装 Pandoc");

    const Converter::Result r = Converter::convertFile(m_pptxPath, "md", m_outDir);
    QVERIFY2(r.success, qPrintable(r.message));

    QFile f(r.outputPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString md = QString::fromUtf8(f.readAll());
    QVERIFY2(md.contains("标题一"), qPrintable("幻灯片文字没抽出来：" + md));
    QVERIFY2(md.contains("中文正文"), qPrintable("幻灯片正文丢失：" + md));
    // 走的是结构化抽取而不是 PDF 拍平
    QVERIFY2(r.message.contains("幻灯片结构"), qPrintable("没有走结构化抽取：" + r.message));
}

// ODT 的写出器会把 title 元数据渲染成正文第一段。我们只拿文件名当标题写
// 进文档属性，落到 ODT 里就成了一个莫名其妙的标题，必须避免。
void TestConverter::testOdtHasNoInjectedTitle()
{
    requireEngines();
    if (Converter::pandocPath().isEmpty())
        QSKIP("未安装 Pandoc");

    const QString srcPath = writeSample(
        "odt/titlecheck.txt",
        QStringLiteral("正文第一段，文件名不应该变成标题。\n").toUtf8());
    QVERIFY(!srcPath.isEmpty());

    const Converter::Result odt = Converter::convertFile(srcPath, "odt", m_outDir);
    QVERIFY2(odt.success, qPrintable(odt.message));

    const Converter::Result back = Converter::convertFile(odt.outputPath, "md", m_outDir);
    QVERIFY2(back.success, qPrintable(back.message));

    QFile f(back.outputPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString md = QString::fromUtf8(f.readAll());
    QVERIFY2(!md.contains("titlecheck"),
             qPrintable("ODT 里被插入了文件名当作标题：\n" + md));
    QVERIFY2(md.contains("正文第一段"), qPrintable("正文丢失：" + md));
}


// 高级选项应真实传给 Pandoc：自定义标题 / 作者进元数据，TOC 生成目录
void TestConverter::testOptionsHtmlMetadataAndToc()
{
    requireEngines();
    if (Converter::pandocPath().isEmpty())
        QSKIP("未安装 Pandoc");

    Converter::Options o;
    o.toc = true;
    o.title = QStringLiteral("自定义标题ABC");
    o.author = QStringLiteral("张三");
    o.dpi = 150;

    const Converter::Result r = Converter::convertFile(m_mdPath, "html", m_outDir,
                                                       nullptr, o);
    QVERIFY2(r.success, qPrintable(r.message));

    QFile f(r.outputPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString html = QString::fromUtf8(f.readAll());
    QVERIFY2(html.contains("自定义标题ABC"),
             "用户填写的标题没有进入 HTML 元数据");
    QVERIFY2(html.contains("张三"), "作者没有进入 HTML 元数据");
    QVERIFY2(html.contains("TOC"), "开启生成目录后 HTML 里没有 TOC");
}

// Markdown 方言选项：gfm 表达不了脚注，应摊平进正文而不是丢内容
void TestConverter::testMarkdownFlavorGfm()
{
    requireEngines();
    if (Converter::pandocPath().isEmpty())
        QSKIP("未安装 Pandoc");

    // md -> md 会被「同格式跳过」拦下，所以用带脚注的 HTML 当源
    const QString src = writeSample("flavor.html",
        "<html><head><meta charset=\"utf-8\"></head><body>"
        "<h1>标题</h1>"
        "<p>正文带脚注<sup class=\"footnote-ref\"><a href=\"#fn1\" id=\"fnref1\">[1]</a></sup>。</p>"
        "<section class=\"footnotes footnotes-end-of-document\">"
        "<ol><li id=\"fn1\"><p>这是脚注内容。"
        "<a href=\"#fnref1\" class=\"footnote-back\">\u21a9</a></p></li></ol></section>"
        "</body></html>");
    QVERIFY(!src.isEmpty());

    Converter::Options o;
    o.markdownFlavor = "gfm";
    const Converter::Result r = Converter::convertFile(src, "md", m_outDir, nullptr, o);
    QVERIFY2(r.success, qPrintable(r.message));

    QFile f(r.outputPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString out = QString::fromUtf8(f.readAll());
    QVERIFY2(out.contains("这是脚注内容"), "脚注内容丢失");
    QVERIFY2(!out.contains("[^1]:"),
             qPrintable("gfm 输出里不应保留脚注语法：\n" + out));
}

QTEST_GUILESS_MAIN(TestConverter)
#include "test_converter.moc"
