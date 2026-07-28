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
    hankaku.o  （ELF 目标文件，含 _binary_hankaku_bin_start 等链接符号）
        ↓  ld.lld
    kernel.elf （字体数据嵌入内核，运行时由 GetFont() 访问）
 
hankaku.txt 格式说明
─────────────────────
    文件由 256 个字符的位图块顺序排列（按 ASCII 码 0x00 → 0xFF）。
    每个字符块格式示例（字符 'A' = 0x41）：
 
        char 0x41
        ........    ← 第 0 行：8 个位置，'@'=有像素，'.'=无像素
        ...@@...    ← 第 1 行
        ..@..@..
        .@....@.
        .@@@@@@.
        .@....@.
        .@....@.
        ........
        ........
        ........
        ........
        ........
        ........
        ........
        ........
        ........    ← 第 15 行
 
    非位图行（空行、"char 0xXX" 注释行等）由 compile() 自动跳过。
 
hankaku.bin 格式说明
─────────────────────
    平坦二进制文件，无任何元数据/头部：
        偏移 0x000–0x00F   字符 0x00 的 16 字节位图
        偏移 0x010–0x01F   字符 0x01 的 16 字节位图
        ...
        偏移 16*c ~ 16*c+15  字符 c 的 16 字节位图
 
    每字节对应字符某一行的 8 个像素：
        bit 7（最高位） = 最左列（dx=0）
        bit 6           = dx=1
        ...
        bit 0（最低位） = 最右列（dx=7）
 
    内核 font.cpp 中 GetFont(c) 的查找逻辑：
        index = 16 * (unsigned int)c;
        return &_binary_hankaku_bin_start + index;
"""
 
import argparse      # 命令行参数解析标准库
import collections   # 有序字典等容器（本脚本导入但未直接使用，保留供扩展）
import functools     # 高阶函数工具（本脚本用 functools.reduce 做位列表→整数转换）
import re            # 正则表达式（用于匹配位图行）
import sys           # 系统接口（未直接使用，保留供错误退出扩展）
 
# -----------------------------------------------------------------------------
# 正则表达式：识别位图行
# -----------------------------------------------------------------------------
 
# BITMAP_PATTERN — 匹配由 '.', '*', '@' 组成的位图行的正则表达式
#
# 模式：r'([.*@]+)'
#   [.*@]  ：字符类，包含三个字面字符：
#              '.'  表示该像素位为 0（空白，不点亮）
#              '@'  表示该像素位为 1（有笔画，点亮）
#              '*'  同 '@'，部分字体文件用 * 代替 @（均视为 1）
#            注意：在字符类 [...] 内，'.' 是字面点号，不是正则通配符。
#   +      ：一个或多个上述字符（位图行至少含 1 个像素）
#   (...)  ：捕获组，m.group(1) 提取实际匹配内容（去除其他前缀字符影响）
#
# re.match() 从行首开始匹配（不要求匹配到行尾），因此：
#   "...@@..."  → 匹配成功，m.group(1) = "...@@..."
#   "char 0x41" → 以字母 'c' 开头，不在字符类中，匹配失败 → 跳过
#   ""（空行）  → 无字符，+ 要求至少 1 个，匹配失败 → 跳过
#   "@@@@@@@"   → 匹配成功（全 1 行）
#
# 此设计让 compile() 无需显式解析字符头部注释，自动忽略所有非位图行。
BITMAP_PATTERN = re.compile(r'([.*@]+)')
 
# -----------------------------------------------------------------------------
# compile — 将字体文本字符串转换为二进制字节序列
# -----------------------------------------------------------------------------
 
def compile(src: str) -> bytes:
    """
    将 hankaku.txt 全文内容转换为平坦二进制字体数据。
 
    参数
    ────
    src : str
        hankaku.txt 的完整文本内容（由 main() 通过 font.read() 传入）。
        包含所有 256 个字符的位图描述块及注释行。
 
    返回值
    ──────
    bytes
        平坦二进制数据，长度 = 匹配到的位图行数（理想情况下 256字符×16行=4096 字节）。
        每字节对应一个位图行，bit7=最左像素，bit0=最右像素。
 
    处理流程
    ────────
    1. lstrip()：去除文件开头的空白行/BOM，确保首行不被误判。
    2. splitlines()：按行分割，逐行处理。
    3. BITMAP_PATTERN.match(line)：过滤非位图行。
    4. 位列表 → 整数（Horner 法）→ 单字节。
    5. 拼接所有字节，返回。
    """
 
    # lstrip()：去除字符串开头的空白字符（空格、换行、制表符等）。
    # 若 hankaku.txt 以空行或 BOM（字节顺序标记）开头，lstrip 将其剥离，
    # 避免第一个字符的位图被错误偏移。
    src = src.lstrip()
 
    # result：收集每一行转换结果（bytes 对象，每个元素 1 字节）的列表。
    # 最后用 b''.join(result) 拼接为完整二进制序列，比逐步 b += ... 拼接更高效。
    result = []
 
    # 逐行处理字体文件内容
    for line in src.splitlines():
 
        # 尝试在行首匹配位图模式（由 '.', '@', '*' 组成的序列）
        m = BITMAP_PATTERN.match(line)
 
        # 若当前行不是位图行（空行、"char 0x41" 注释、其他文本），跳过
        if not m:
            continue
 
        # ── 步骤 A：将位图字符串转换为位列表（list of int） ────────────────
        #
        # m.group(1)：捕获组匹配到的位图字符串，如 "...@@..."
        # 列表推导式逐字符处理：
        #   '.' → 0（像素关闭）
        #   '@' 或 '*' → 1（像素点亮）
        #
        # 示例："...@@..." → [0, 0, 0, 1, 1, 0, 0, 0]
        #        MSB↑                              ↑LSB
        #       （最左列=bit7）              （最右列=bit0）
        bits = [(0 if x == '.' else 1) for x in m.group(1)]
 
        # ── 步骤 B：将位列表转换为整数（Horner 法，二进制展开） ──────────────
        #
        # functools.reduce(func, iterable)：
        #   以二元函数 func 从左到右归约列表，等价于：
        #     result = iterable[0]
        #     for x in iterable[1:]:
        #         result = func(result, x)
        #
        # lambda a, b: 2*a + b：
        #   每次将当前累积值左移 1 位（×2），再加新位。
        #   等价于 Horner 多项式展开（二进制→整数的标准算法）：
        #     bits = [b7, b6, b5, b4, b3, b2, b1, b0]
        #     result = b7
        #     result = 2*b7 + b6
        #     result = 2*(2*b7 + b6) + b5 = 4*b7 + 2*b6 + b5
        #     ...
        #     result = 128*b7 + 64*b6 + 32*b5 + 16*b4 + 8*b3 + 4*b2 + 2*b1 + b0
        #
        # 具体示例（"...@@..." → [0,0,0,1,1,0,0,0]）：
        #   初始 a = 0
        #   2*0+0 = 0   (b6)
        #   2*0+0 = 0   (b5)
        #   2*0+1 = 1   (b4)
        #   2*1+1 = 3   (b3)
        #   2*3+0 = 6   (b2)
        #   2*6+0 = 12  (b1)
        #   2*12+0= 24  (b0，但此示例只有 8 位)
        #   等等……实际结果为 0b00011000 = 0x18 = 24
        #
        # 位排列语义：
        #   最先处理的 bits[0]（列表最左元素）对应 bit7（最高位 = 最左列），
        #   因此屏幕上最左像素由字节的最高位控制，与 font.cpp 中的
        #   (font[dy] << dx) & 0x80u 位提取算法完全对应。
        bits_int = functools.reduce(lambda a, b: 2*a + b, bits)
 
        # ── 步骤 C：将整数转换为单字节 bytes 对象 ────────────────────────────
        #
        # int.to_bytes(length, byteorder)：
        #   length=1   ：生成 1 字节（字体行宽固定 8 像素，结果必在 0–255 范围内）
        #   byteorder='little'：字节序（对单字节无实际影响，'big'/'little' 结果相同）
        #
        # 返回 bytes 对象，如 bits_int=24 → b'\x18'
        result.append(bits_int.to_bytes(1, byteorder='little'))
 
    # b''.join(result)：将所有单字节 bytes 对象高效拼接为完整二进制序列。
    # 比在循环内反复做 result += byte 效率高（避免每次创建新 bytes 对象）。
    return b''.join(result)
 
# -----------------------------------------------------------------------------
# main — 命令行入口：解析参数、读取输入、写入输出
# -----------------------------------------------------------------------------
 
def main():
    """
    命令行入口函数。
 
    解析参数：
        font（位置参数）：hankaku.txt 的路径
        -o（可选参数）：输出文件路径，默认 'font.out'
 
    示例调用（由 kernel/Makefile 生成）：
        makefont.py -o hankaku.bin hankaku.txt
    """
 
    # ── 命令行参数解析 ────────────────────────────────────────────────────────
 
    # argparse.ArgumentParser()：创建参数解析器（自动生成 --help 信息）
    parser = argparse.ArgumentParser(
        description='半角字体文本（hankaku.txt）→ 二进制（hankaku.bin）转换工具'
    )
 
    # 位置参数 'font'：必须提供，字体文本文件路径
    #   用法：makefont.py hankaku.txt
    #   ns.font 将保存此路径字符串
    parser.add_argument('font', help='path to a font file')
 
    # 可选参数 '-o'：输出文件路径，默认 'font.out'
    #   用法：makefont.py -o hankaku.bin hankaku.txt
    #   ns.o 将保存此路径字符串（default 确保未指定时不为 None）
    parser.add_argument('-o', help='path to an output file', default='font.out')
 
    # parse_args()：解析 sys.argv[1:] 中的实际命令行参数
    # 若参数不符合规则，自动打印错误信息并以非零状态退出
    ns = parser.parse_args()
 
    # ── 读取字体文本，转换并写入二进制输出 ────────────────────────────────────
 
    # with 语句同时打开两个文件，任一异常时均自动关闭（RAII 风格）：
    #   open(ns.o, 'wb')   ：以二进制写入模式打开输出文件
    #                         'w' = 写（若存在则覆盖），'b' = 二进制（不做换行转换）
    #   open(ns.font)      ：以文本读取模式（默认 'r'）打开字体文本文件
    #                         Python 3 默认 UTF-8 解码（hankaku.txt 为纯 ASCII，无影响）
    with open(ns.o, 'wb') as out, open(ns.font) as font:
 
        # font.read()：一次性读取整个字体文本文件为字符串
        # 整个文件大小通常 < 100 KB，一次性读取无内存压力
        src = font.read()
 
        # compile(src)：将字体文本转换为二进制字节序列（见上方函数详解）
        # out.write(...)：将结果二进制数据写入输出文件
        # 输出文件内容即为 hankaku.bin，后续由 objcopy 嵌入内核 ELF
        out.write(compile(src))
 
# -----------------------------------------------------------------------------
# 脚本入口守卫
# -----------------------------------------------------------------------------
 
# if __name__ == '__main__'：
#   Python 约定：直接运行脚本时 __name__ == '__main__'，
#   被 import 时 __name__ == 'makefont'（模块名）。
#   此守卫确保 main() 只在直接运行时调用，import 时不自动执行，
#   方便其他脚本 import makefont 复用 compile() 函数。
if __name__ == '__main__':
    main()
 
 
 
 