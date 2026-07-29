#!/usr/bin/python3
"""
makefont.py — 半角字体文本位图编译器
 
将 hankaku.txt 中的 ASCII 位图描述转换为 hankaku.bin 二进制字体文件。
 
── 字体文件格式 ─────────────────────────────────────────────────────────────
 
  输入（hankaku.txt）：
    每个字符由一个注释头（如 "0x41 'A'"）和紧随其后的若干"位图行"组成。
    位图行：由 '.' 和 '@'（有时也用 '*'）组成的字符串，各 8 个字符宽。
      '.' = 背景像素（bit = 0）
      '@' 或 '*' = 前景像素（bit = 1）
    每个字符恰好 16 行（对应 16 像素高）。
 
  输出（hankaku.bin）：
    平坦二进制文件：每字符 16 字节，字节序为小端（bit7=最左列）。
    字符 c 的数据位于偏移 16 * c（ASCII 编码直接作为索引）。
    被 Makefile 通过 objcopy 嵌入内核 ELF，供 font.cpp 的 GetFont() 函数使用。
 
── 处理流程 ────────────────────────────────────────────────────────────────
 
  hankaku.txt
      │
      ▼  compile(src: str) → bytes
      │  1. lstrip()：去掉文件开头空白
      │  2. splitlines()：按行分割
      │  3. BITMAP_PATTERN.match(line)：过滤非位图行（注释/空行）
      │  4. 逐行：'.' → 0, '@'/'*' → 1，构造位列表
      │  5. functools.reduce(lambda a,b: 2*a+b, bits)：位列表 → 整数
      │  6. int.to_bytes(1, 'little')：整数 → 单字节
      │
      ▼  b''.join(result)
    hankaku.bin（字节串）
 
── 调用方式 ────────────────────────────────────────────────────────────────
 
  由 Makefile 调用：
    ../tools/makefont.py -o hankaku.bin hankaku.txt
 
  命令行参数：
    font（位置参数）：输入字体文本文件路径
    -o              ：输出二进制文件路径（默认 font.out）
"""
 
import argparse       # 命令行参数解析
import collections    # 导入但未直接使用（保留以备将来扩展）
import functools      # functools.reduce（位列表转整数）
import re             # 正则表达式（匹配位图行）
import sys            # 导入但未直接使用（保留标准做法）
 
# BITMAP_PATTERN — 匹配"位图行"的正则表达式
#
# r'([.*@]+)'：匹配由 '.', '*', '@' 组成的连续字符串（至少 1 个字符）。
#   '.'：背景像素（0 bit）
#   '*' 或 '@'：前景像素（1 bit）
#   字符集 [.*@] 中 '.' 为字面量（在字符集内 '.' 不是通配符）
#   '+' 要求至少 1 个字符，() 分组用于 m.group(1) 提取匹配内容
#
# match 方法要求从行首匹配：非位图行（注释行、空行）首字符不在 [.*@] 内，
# match 返回 None，由 compile() 中 if not m: continue 跳过。
BITMAP_PATTERN = re.compile(r'([.*@]+)')
 
def compile(src: str) -> bytes:
    """
    将字体文本源码编译为二进制字节串
 
    参数：
      src (str)：hankaku.txt 的完整文本内容
 
    返回：
      bytes：hankaku.bin 二进制数据，每字符 16 字节，共 256 字符 × 16 = 4096 字节
 
    处理流程：
      1. src.lstrip()：去掉文件开头的空白行/空格，确保从有效内容开始处理。
         使用 lstrip 而非 strip，保留末尾内容（末尾注释不影响逐行处理）。
 
      2. result = []：收集每行编译得到的 bytes 对象。
 
      3. for line in src.splitlines()：
           splitlines() 按 '\\n', '\\r\\n', '\\r' 等行结束符分割，
           返回各行字符串（不含行结束符）。
 
      4. m = BITMAP_PATTERN.match(line)：
           尝试从行首匹配位图模式。
           非位图行（如 "0x41 'A'"、空行）返回 None。
 
      5. if not m: continue：
           跳过所有非位图行（注释、字符标识行、空行）。
 
      6. bits = [(0 if x == '.' else 1) for x in m.group(1)]：
           将位图字符串转换为整数列表：'.' → 0，任何非 '.' 字符（@ 或 *）→ 1。
           列表长度 = 字符宽度（通常 8，对应 8 列像素）。
 
      7. bits_int = functools.reduce(lambda a, b: 2*a + b, bits)：
           将位列表转换为整数（大端 bit 顺序）。
           reduce 过程：从左到右，当前值 × 2 + 下一位（等价于左移 1 位后加新位）。
           示例：[1, 0, 1, 1, 0, 0, 0, 0]
             → ((((((( 1)*2+0)*2+1)*2+1)*2+0)*2+0)*2+0)*2+0
             = 0b10110000 = 0xB0
           结果 bit7 = bits[0]（最左列），bit0 = bits[7]（最右列）。
           这与 font.cpp 的提取算法 (font[dy] << dx) & 0x80u 对应：
             dx=0 检测 bit7（最左列），dx=7 检测 bit0（最右列）。
 
      8. result.append(bits_int.to_bytes(1, byteorder='little'))：
           将整数转为 1 字节的 bytes 对象，追加到 result。
           to_bytes(1, 'little')：1 字节，小端字节序（对单字节无影响）。
 
      9. return b''.join(result)：
           将所有字节串拼接为一个完整的 bytes 对象。
           顺序 = hankaku.txt 中位图行的出现顺序 = 字符 ASCII 升序排列。
    """
    src = src.lstrip()  # 去掉文件头部空白
    result = []
 
    for line in src.splitlines():
        m = BITMAP_PATTERN.match(line)
        if not m:
            # 非位图行（注释、字符标识、空行）：跳过
            continue
 
        # 将位图字符转换为 0/1 整数列表：'.' → 0，'@'/'*' → 1
        bits = [(0 if x == '.' else 1) for x in m.group(1)]
 
        # 位列表 → 8 位整数（大端 bit，bit7 = 最左列）
        bits_int = functools.reduce(lambda a, b: 2*a + b, bits)
 
        # 整数 → 1 字节，追加到结果
        result.append(bits_int.to_bytes(1, byteorder='little'))
 
    return b''.join(result)
 
def main():
    """
    命令行入口：解析参数，读取字体文本，编译并写出二进制文件。
 
    参数（命令行）：
      font      ：输入字体文本文件路径（如 hankaku.txt）
      -o <path> ：输出二进制文件路径（默认 font.out）
 
    Makefile 调用方式：
      ../tools/makefont.py -o hankaku.bin hankaku.txt
 
    文件打开模式：
      open(ns.o, 'wb')    ：二进制写模式（overwrite），确保写出原始字节
      open(ns.font)       ：文本读模式（默认 UTF-8），读取 hankaku.txt 字符位图
    """
    parser = argparse.ArgumentParser(
        description='半角字体文本位图 → 二进制字体文件编译器'
    )
    parser.add_argument('font', help='输入字体文本文件路径（如 hankaku.txt）')
    parser.add_argument('-o',
                        help='输出二进制文件路径（默认 font.out）',
                        default='font.out')
    ns = parser.parse_args()
 
    with open(ns.o, 'wb') as out, open(ns.font) as font:
        src = font.read()     # 读取字体文本全文
        out.write(compile(src))  # 编译为二进制并写出
 
if __name__ == '__main__':
    main()
 
 