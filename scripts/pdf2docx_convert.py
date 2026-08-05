import sys

from pdf2docx import Converter


def main():
    if len(sys.argv) != 3:
        print("用法: pdf2docx_convert.py <输入.pdf> <输出.docx>", file=sys.stderr)
        sys.exit(2)

    input_path, output_path = sys.argv[1], sys.argv[2]

    cv = Converter(input_path)
    try:
        cv.convert(output_path)
    finally:
        cv.close()


if __name__ == "__main__":
    main()
