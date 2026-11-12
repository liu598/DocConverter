"""pdf2docx 的薄封装：把 PDF 转成 docx，并报告文字层覆盖情况。

用法: python -X utf8 -u pdf2docx_convert.py <输入.pdf> <输出.docx>

退出码:
    0  成功（即使检测到扫描件，文件仍然正常产出）
    2  参数错误
    其它  由 pdf2docx 抛出的异常决定

两个作用：
1. 单页失败不会毁掉整份文档（ignore_page_error），坏页跳过、其余照常输出。
2. 统计有多少页根本没有文字层，把结果打成 ASCII 标记交给 C++ 侧转成中文提示。
   扫描件 PDF 转出来几乎是空的，用户看到空文档只会以为工具坏了。

标记格式（都在 stderr，脚本本身不输出中文，避免 Windows 控制台编码问题）:
    PDF_PAGES: total=12 empty=10
    SCANNED_PDF: converted docx contains almost no text
"""

import re
import sys
import zipfile

from pdf2docx import Converter

# 结果里有效文字少于此值，就认为整份是扫描件
MIN_TEXT_CHARS = 20
# 单页文字少于此值，认为该页没有文字层
MIN_PAGE_CHARS = 10


def page_text_stats(pdf_path):
    """返回 (总页数, 无文字层页数)，失败时返回 (0, 0)。"""
    try:
        import fitz  # pdf2docx 依赖 PyMuPDF，一定在
    except Exception:
        return 0, 0

    total = empty = 0
    try:
        doc = fitz.open(pdf_path)
        total = doc.page_count
        for page in doc:
            if len(page.get_text("text").strip()) < MIN_PAGE_CHARS:
                empty += 1
        doc.close()
    except Exception:
        return 0, 0
    return total, empty


def docx_text_chars(docx_path):
    """读 docx（本质是 zip）里的 document.xml，统计有效文字量。"""
    try:
        with zipfile.ZipFile(docx_path) as z:
            xml = z.read("word/document.xml").decode("utf-8", "ignore")
    except Exception:
        return -1

    text = re.sub(r"<[^>]+>", "", xml)
    text = re.sub(r"\s+", "", text)
    return len(text)


def main():
    if len(sys.argv) != 3:
        print("usage: pdf2docx_convert.py <input.pdf> <output.docx>", file=sys.stderr)
        sys.exit(2)

    input_path, output_path = sys.argv[1], sys.argv[2]

    cv = Converter(input_path)
    try:
        # 单页解析失败时跳过该页而不是整份放弃——复杂版面里个别页
        # 遇到不支持的图形对象很常见，没必要因此前功尽弃。
        try:
            cv.convert(output_path, ignore_page_error=True)
        except TypeError:
            # 老版本 pdf2docx 没有这个参数
            cv.convert(output_path)
    finally:
        cv.close()

    # 统计放在转换之后打印：C++ 侧只保留输出的尾部若干 KB，
    # 提前打印会被 pdf2docx 的逐页进度刷掉。
    total, empty = page_text_stats(input_path)
    if total:
        print(f"PDF_PAGES: total={total} empty={empty}", file=sys.stderr)

    chars = docx_text_chars(output_path)
    if 0 <= chars < MIN_TEXT_CHARS:
        print("SCANNED_PDF: converted docx contains almost no text", file=sys.stderr)


if __name__ == "__main__":
    main()
