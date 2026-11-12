"""pdf2docx 的薄封装：把 PDF 转成 docx，并检测「转出来是空的」这种扫描件情况。

用法: python -X utf8 -u pdf2docx_convert.py <输入.pdf> <输出.docx>

退出码:
    0  成功（即使检测到扫描件，文件仍然正常产出）
    2  参数错误
    其它  由 pdf2docx 抛出的异常决定

扫描件（图片型 PDF）转出来几乎没有文字，用户看到空文档会以为工具坏了。
这里检测到就在 stderr 打一个 ASCII 标记 SCANNED_PDF，由 C++ 侧转成中文提示——
脚本本身不输出中文，避免 Windows 控制台编码问题。
"""

import re
import sys
import zipfile

from pdf2docx import Converter

# 结果里有效文字少于此值，就认为是扫描件
MIN_TEXT_CHARS = 20


def looks_scanned(docx_path):
    """读 docx（本质是 zip）里的 document.xml，统计有效文字量。"""
    try:
        with zipfile.ZipFile(docx_path) as z:
            xml = z.read("word/document.xml").decode("utf-8", "ignore")
    except Exception:
        return False

    text = re.sub(r"<[^>]+>", "", xml)
    text = re.sub(r"\s+", "", text)
    return len(text) < MIN_TEXT_CHARS


def main():
    if len(sys.argv) != 3:
        print("usage: pdf2docx_convert.py <input.pdf> <output.docx>", file=sys.stderr)
        sys.exit(2)

    input_path, output_path = sys.argv[1], sys.argv[2]

    cv = Converter(input_path)
    try:
        cv.convert(output_path)
    finally:
        cv.close()

    if looks_scanned(output_path):
        print("SCANNED_PDF: converted docx contains almost no text", file=sys.stderr)


if __name__ == "__main__":
    main()
