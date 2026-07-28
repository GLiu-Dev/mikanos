/**
 * @file font.cpp
 *
 * フォント描画のプログラムを集めたファイル.
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本文件实现 MikanOS 内核的完整 ASCII 字符渲染子系统，提供：
 *     ① _binary_hankaku_bin_* 外部符号 — 链接器注入的字体二进制数据引用
 *     ② GetFont(char c)               — 根据字符码查找对应位图数据的指针
 *     ③ WriteAscii(...)               — 向帧缓冲区渲染单个 ASCII 字符
 *     ④ WriteString(...)              — 渲染以 '\0' 结尾的 ASCII 字符串
 *
 * ── 字体数据来源（hankaku.bin 链接器嵌入机制）────────────────────────────────
 *
 *   构建系统通过 objcopy 将 hankaku.bin 转换为目标文件并与内核链接：
 *     objcopy -I binary -O elf64-x86-64 -B i386:x86-64 hankaku.bin hankaku.o
 *
 *   objcopy 自动生成三个全局符号：
 *     _binary_hankaku_bin_start  → 字体数据第一个字节的地址
 *     _binary_hankaku_bin_end    → 字体数据结束地址（past-the-end）
 *     _binary_hankaku_bin_size   → ★特殊符号：其"地址"值 = 文件字节数★
 *
 *   hankaku.bin 格式：每字符 16 字节（8×16 像素位图），字符 c 从偏移 16*c 开始。
 *
 * ── 函数调用关系 ─────────────────────────────────────────────────────────────
 *
 *   WriteString → WriteAscii → GetFont → _binary_hankaku_bin_start
 *                            → PixelWriter::Write（虚函数分派）
 */
 
#include "font.hpp"
 
/* ============================================================================
 * 一、链接器注入的字体二进制数据符号
 * ============================================================================ */
 
/**
 * _binary_hankaku_bin_start — 字体数据第一个字节的地址
 *
 * 取地址 &_binary_hankaku_bin_start 得到字体数据的起始指针（uint8_t*）。
 * 不直接使用符号值本身（它是起始字节的值，不是我们需要的地址）。
 */
extern const uint8_t _binary_hankaku_bin_start;
 
/**
 * _binary_hankaku_bin_end — 字体数据结束位置（最后字节的下一地址）
 *
 * 遵循 C++ 的 past-the-end 惯例，本文件未直接使用，保留供将来扩展。
 */
extern const uint8_t _binary_hankaku_bin_end;
 
/**
 * _binary_hankaku_bin_size — ★ 字体文件大小（以非直觉的方式编码）★
 *
 * objcopy 生成的 size 符号与 start/end 不同：
 *   其"符号值"（链接器地址字段）= hankaku.bin 的字节数（而非内存地址）。
 *
 * 正确读取方式（取地址并转为整数）：
 *   uintptr_t size = reinterpret_cast<uintptr_t>(&_binary_hankaku_bin_size);
 *
 * 错误读取方式（不能直接读取值）：
 *   uint8_t wrong = _binary_hankaku_bin_size; // 读取的是某内存地址处的数据
 */
extern const uint8_t _binary_hankaku_bin_size;
 
/* ============================================================================
 * 二、GetFont — 根据字符码返回位图数据指针
 * ============================================================================ */
 
/**
 * GetFont — 在 hankaku.bin 中查找字符 c 的 16 字节位图数据指针
 *
 * @param c  ASCII 字符码（signed char）
 *           内部转为 unsigned int 后乘以 16 计算字节偏移，
 *           避免 signed char 负值字符码产生负索引。
 *
 * @return   指向字符 c 位图数据起始字节的指针（可作为 uint8_t[16] 使用）；
 *           字符码超出字体文件范围时返回 nullptr。
 *
 * ── 实现步骤 ─────────────────────────────────────────────────────────────────
 *
 *   步骤 1：index = 16 × (unsigned int)c
 *     hankaku.bin 中每字符固定 16 字节，字符 c 的位图从偏移 16*c 开始。
 *     static_cast<unsigned int>(c)：将 signed char 转为无符号，
 *     防止扩展 ASCII（如 (char)0xFF = -1）产生负索引。
 *
 *   步骤 2：边界检查
 *     reinterpret_cast<uintptr_t>(&_binary_hankaku_bin_size)：
 *       取 size 符号的地址值 = 文件字节数（如 256×16 = 4096）。
 *     若 index ≥ 文件大小，则字符超出范围，返回 nullptr。
 *
 *   步骤 3：返回 &_binary_hankaku_bin_start + index
 *     从字体数据起始地址偏移 index 字节，精确指向字符 c 的位图。
 */
const uint8_t* GetFont(char c) {
  // 计算字符 c 在 hankaku.bin 中的字节偏移（每字符 16 字节）
  // static_cast<unsigned int>：避免 signed char 负值产生负索引
  auto index = 16 * static_cast<unsigned int>(c);
 
  // 边界检查：index 必须小于字体文件总字节数
  // reinterpret_cast<uintptr_t>(&_binary_hankaku_bin_size)：
  //   取 size 符号的"地址"并解释为整数 = hankaku.bin 的字节数
  if (index >= reinterpret_cast<uintptr_t>(&_binary_hankaku_bin_size)) {
    return nullptr;  // 字符码超出字体覆盖范围
  }
 
  // 返回字体起始地址 + 偏移 = 字符 c 位图数据的第一个字节地址
  return &_binary_hankaku_bin_start + index;
}
 
/* ============================================================================
 * 三、WriteAscii — 单字符渲染
 * ============================================================================ */
 
/**
 * WriteAscii — 在屏幕指定坐标渲染单个 ASCII 字符（8×16 像素位图）
 *
 * @param writer  像素写入器引用，虚函数 Write() 自动适配 RGB/BGR 帧缓冲格式
 * @param x       字符左上角屏幕 X 坐标（像素列，0=最左）；字符占 x~x+7 列
 * @param y       字符左上角屏幕 Y 坐标（像素行，0=最顶）；字符占 y~y+15 行
 * @param c       要渲染的字符码，传给 GetFont(c) 查找位图
 * @param color   字符前景色；背景色不由本函数控制（调用方预先填充背景）
 *
 * ── 位图渲染算法 ─────────────────────────────────────────────────────────────
 *
 *   外层循环 dy（0..15）：遍历字符 16 行，读取 font[dy]（该行 8 像素的位图字节）
 *   内层循环 dx（0..7） ：遍历该行 8 列，提取对应位
 *
 *   核心判断式：(font[dy] << dx) & 0x80u
 *     将 font[dy] 左移 dx 位，使"第 dx 列的位"移到 bit7（最高位），
 *     再与 0x80u 按位与，判断该位是否为 1（有笔画）。
 *
 *   位排列：bit7=dx=0（最左列） ... bit0=dx=7（最右列）
 *   示例（font[dy]=0b01111110）：
 *     dx=0: 0b01111110 & 0x80 = 0   → 不亮
 *     dx=1: 0b11111100 & 0x80 = 0x80 → 点亮
 *     dx=2: 0b11111000 & 0x80 = 0x80 → 点亮（注：左移后超过8位，int 提升保留高位）
 *     ...
 *     dx=6: 0b10000000 & 0x80 = 0x80 → 点亮
 *     dx=7: 0b00000000 & 0x80 = 0   → 不亮
 */
void WriteAscii(PixelWriter& writer, int x, int y, char c, const PixelColor& color) {
  // 从 hankaku.bin 中查找字符 c 的 16 字节位图数据指针
  const uint8_t* font = GetFont(c);
 
  // GetFont 返回 nullptr 表示字符超出字体范围，静默跳过
  if (font == nullptr) {
    return;
  }
 
  // 遍历字符的 16 行（dy = 字符内行偏移）
  for (int dy = 0; dy < 16; ++dy) {
    // 遍历该行的 8 列（dx = 字符内列偏移，0=最左，7=最右）
    for (int dx = 0; dx < 8; ++dx) {
      // (font[dy] << dx) & 0x80u：检查 dx 列是否有笔画
      // uint8_t 左移时提升为 int，允许左移超过 8 位而不截断
      if ((font[dy] << dx) & 0x80u) {
        // 有笔画：以前景色写像素（背景色保持不变）
        writer.Write(x + dx, y + dy, color);
      }
    }
  }
}
 
/* ============================================================================
 * 四、WriteString — 字符串渲染
 * ============================================================================ */
 
// #@@range_begin(write_string)
/**
 * WriteString — 渲染以 '\0' 结尾的 ASCII 字符串
 *
 * @param writer  像素写入器引用（同 WriteAscii）
 * @param x       字符串第一个字符左上角的屏幕 X 坐标
 * @param y       字符串所在行的屏幕 Y 坐标（所有字符共用同一 Y 坐标）
 * @param s       以 '\0' 结尾的 ASCII 字符串指针
 *                可以是字符串字面量（"Hello"）或 char 数组（如 sprintf 的输出缓冲区）
 * @param color   字符串整体前景色（所有字符使用同一颜色）
 *
 * ── 实现原理 ─────────────────────────────────────────────────────────────────
 *
 *   遍历字符串中每个字符 s[i]（直到遇到 '\0' 终止符）：
 *     调用 WriteAscii(writer, x + 8*i, y, s[i], color)
 *     每字符宽 8 像素，水平方向紧密排列（无字符间距）。
 *
 *   字符位置：
 *     s[0] → 起始于 (x, y)
 *     s[1] → 起始于 (x+8, y)
 *     s[i] → 起始于 (x + 8*i, y)
 *
 * ── 使用示例（来自 main.cpp）────────────────────────────────────────────────
 *
 *   // 渲染蓝色 "Hello, world!" 字符串（位于 y=66 行）
 *   WriteString(*pixel_writer, 0, 66, "Hello, world!", {0, 0, 255});
 *
 *   // 渲染 sprintf 格式化后的字符串（位于 y=82 行）
 *   char buf[128];
 *   sprintf(buf, "1 + 2 = %d", 1 + 2);
 *   WriteString(*pixel_writer, 0, 82, buf, {0, 0, 0});
 *
 * ── 边界注意事项 ─────────────────────────────────────────────────────────────
 *
 *   本函数不检查字符串长度与屏幕宽度的关系。
 *   若字符串过长导致 x + 8*i ≥ horizontal_resolution，
 *   PixelAt() 计算出的地址会指向下一行或超出帧缓冲区，调用方需确保字符串不溢出。
 */
void WriteString(PixelWriter& writer, int x, int y, const char* s, const PixelColor& color) {
  // 逐字符遍历，遇到 '\0' 终止（标准 C 字符串约定）
  for (int i = 0; s[i] != '\0'; ++i) {
    // x + 8*i：每个字符向右偏移 8 像素（字符宽度固定为 8 像素）
    WriteAscii(writer, x + 8 * i, y, s[i], color);
  }
}
// #@@range_end(write_string)