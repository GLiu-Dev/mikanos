#!/usr/bin/python3
"""
makefont.py — 半角字体文本转二进制工具
 
用途
────
将 hankaku.txt（人类可读的 8×16 位图字体描述文件）转换为
hankaku.bin（平坦二进制字体文件），供 objcopy 嵌入内核 ELF。
 
调用方式（由 kernel/Makefile 自动调用）
────────────────────────────────────────
    makefont.py -o hankaku.bin hankaku.txt
 
转换流程
────────
    hankaku.txt（文本，每字符 16 行 '@'/'.' 位图）
        ↓  本脚本
    hankaku.bin（二进制，每字符 16 字节，共 256 字符 × 16 = 4096 字节）
        ↓  objcopy -I binary -O elf64-x86-64
    hankaku.o  （ELF 目标文件，含 _binary_hankaku_bin_* 链接符号）
        ↓  ld.lld -lc
    kernel.elf （字体数据嵌入内核，运行时由 GetFont() 访问）
 
hankaku.txt 格式说明
─────────────────────
    文件由 256 个字符的位图块顺序排列（按 ASCII 码 0x00 → 0xFF）。
    每个字符块示例（字符 'A' = 0x41）：
 
        char 0x41
        ........    ← 第 0 行：'@'=有像素，'.'=无像素（8 列）
        ...@@...    ← 第 1 行
        ..@..@..
        ...（共 16 行）
 
    非位图行（空行、"char 0xXX" 注释行等）由 compile() 自动跳过。
 
hankaku.bin 格式说明
─────────────────────
    平坦二进制，无头部/元数据：
        偏移 16*c ~ 16*c+15  字符 c 的 16 字节位图
 
    每字节对应字符某一行的 8 像素：
        bit7（最高位）= 最左列（dx=0）
        bit0（最低位）= 最右列（dx=7）
 
    内核 font.cpp GetFont(c) 查找逻辑：
        return &_binary_hankaku_bin_start + 16 * (unsigned int)c;
"""
 
import argparse      # 命令行参数解析标准库
import collections   # 有序字典等容器（保留供扩展）
import functools     # 高阶函数工具（用 functools.reduce 做位列表→整数转换）
import re            # 正则表达式（匹配位图行）
import sys           # 系统接口（保留供扩展）
 
# -----------------------------------------------------------------------------
# 正则表达式：识别位图行
# -----------------------------------------------------------------------------
 
# BITMAP_PATTERN — 匹配由 '.', '*', '@' 组成的位图行
#
# 模式：r'([.*@]+)'
#   [.*@] ：字符类，包含三个字面字符（在 [...] 内 '.' 是字面点号，不是通配符）：
#              '.'  → 像素关闭（0）
#              '@'  → 像素点亮（1）
#              '*'  → 同 '@'，部分字体文件使用 * 作为像素标记
#   +     ：一个或多个
#   (...)  ：捕获组，m.group(1) 提取匹配内容
#
# re.match() 从行首匹配（不要求匹配到行尾），因此：
#   "...@@..."  → 匹配成功（全为位图字符）
#   "char 0x41" → 首字符 'c' 不在字符类中，匹配失败 → 跳过
#   ""（空行）  → + 要求至少 1 个，匹配失败 → 跳过
BITMAP_PATTERN = re.compile(r'([.*@]+)')
 
# -----------------------------------------------------------------------------
# compile — 将字体文本转换为二进制字节序列
# -----------------------------------------------------------------------------
 
def compile(src: str) -> bytes:
    """
    将 hankaku.txt 全文内容转换为平坦二进制字体数据。
 
    参数
    ────
    src : str
        hankaku.txt 的完整文本内容。
 
    返回值
    ──────
    bytes
        平坦二进制，每字节对应一行位图，长度 = 匹配到的位图行数
        （理想情况 256字符×16行 = 4096 字节）。
 
    处理流程
    ────────
    1. lstrip()：去除文件开头空白（避免首行被误判）
    2. splitlines()：按行分割
    3. BITMAP_PATTERN.match()：过滤非位图行
    4. 位列表 → 整数（Horner 法）→ 单字节
    5. b''.join() 拼接所有字节
    """
 
    # lstrip()：去除开头空白/BOM，避免首字符位图被错误偏移
    src = src.lstrip()
 
    # result：收集每行转换结果（bytes 对象），最后 join 拼接
    result = []
 
    for line in src.splitlines():
 
        # 尝试在行首匹配位图模式
        m = BITMAP_PATTERN.match(line)
 
        # 非位图行（空行、注释行等）跳过
        if not m:
            continue
 
        # ── 步骤 A：位图字符串 → 位列表 ────────────────────────────────────
        #
        # m.group(1)：匹配到的位图字符串，如 "...@@..."
        # '.' → 0（像素关闭），其他（'@'/'*'）→ 1（像素点亮）
        # 示例："...@@..." → [0, 0, 0, 1, 1, 0, 0, 0]
        #        MSB↑（最左列=bit7）              LSB↑（最右列=bit0）
        bits = [(0 if x == '.' else 1) for x in m.group(1)]
 
        # ── 步骤 B：位列表 → 整数（Horner 法）────────────────────────────────
        #
        # functools.reduce(lambda a, b: 2*a + b, bits)：
        #   每次将累积值左移 1 位（×2）再加新位，等价于：
        #     result = b7×128 + b6×64 + b5×32 + b4×16 + b3×8 + b2×4 + b1×2 + b0
        #   示例：[0,0,0,1,1,0,0,0] → 0×128+...+1×16+1×8+... = 0b00011000 = 24
        #
        # 位排列语义：
        #   bits[0]（最左）→ bit7（最高位），与 font.cpp 中
        #   (font[dy] << dx) & 0x80u 的位提取算法对应。
        bits_int = functools.reduce(lambda a, b: 2*a + b, bits)
 
        # ── 步骤 C：整数 → 单字节 bytes ────────────────────────────────────
        #
        # to_bytes(1, 'little')：生成 1 字节（位图行宽固定 8 像素，结果 0~255）
        # 单字节时 byteorder 无影响（'big'/'little' 结果相同）
        result.append(bits_int.to_bytes(1, byteorder='little'))
 
    # b''.join()：高效拼接所有单字节对象为完整二进制序列
    return b''.join(result)
 
# -----------------------------------------------------------------------------
# main — 命令行入口
# -----------------------------------------------------------------------------
 
def main():
    """
    命令行入口：解析参数，读取 hankaku.txt，写入 hankaku.bin。
 
    参数：
        font（位置参数）：hankaku.txt 路径
        -o（可选）      ：输出路径，默认 'font.out'
 
    示例（由 kernel/Makefile 生成）：
        makefont.py -o hankaku.bin hankaku.txt
    """
 
    # argparse.ArgumentParser()：创建解析器（自动生成 --help 信息）
    parser = argparse.ArgumentParser(
        description='半角字体文本（hankaku.txt）→ 二进制（hankaku.bin）转换工具'
    )
 
    # 位置参数 'font'：必须提供的字体文本文件路径
    parser.add_argument('font', help='path to a font file')
 
    # 可选参数 '-o'：输出文件路径，默认 'font.out'
    parser.add_argument('-o', help='path to an output file', default='font.out')
 
    # 解析 sys.argv[1:]，若参数不合法则自动报错退出
    ns = parser.parse_args()
 
    # 同时打开输出文件（二进制写）和字体文本文件（文本读）
    #   open(ns.o, 'wb')：'w'=写（存在则覆盖），'b'=二进制（不做换行转换）
    #   open(ns.font)   ：默认文本读（Python 3 UTF-8 解码，纯 ASCII 字体文件无影响）
    with open(ns.o, 'wb') as out, open(ns.font) as font:
        src = font.read()           # 一次性读入全部文本（< 100 KB，无内存压力）
        out.write(compile(src))     # 转换并写入二进制输出
 
# -----------------------------------------------------------------------------
# 脚本入口守卫
# -----------------------------------------------------------------------------
 
# if __name__ == '__main__'：
#   直接运行时 __name__ == '__main__' → 调用 main()
#   被 import 时 __name__ == 'makefont' → 不自动执行
#   允许其他脚本 import makefont 复用 compile() 函数
if __name__ == '__main__':
    main()
 