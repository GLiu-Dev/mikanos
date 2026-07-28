/**
 * @file font.hpp
 *
 * フォント描画に関する宣言をまとめたヘッダファイル.
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本头文件是 MikanOS 字体渲染子系统的对外接口声明，提供：
 *     ① GetFont(char c)    — 根据字符码获取 hankaku.bin 位图数据指针
 *     ② WriteAscii(...)    — 渲染单个 ASCII 字符（8×16 像素）
 *     ③ WriteString(...)   — 渲染以 '\0' 结尾的 ASCII 字符串
 *
 *   实现位于 font.cpp。
 *
 * ── 依赖关系 ─────────────────────────────────────────────────────────────────
 *
 *   font.hpp  →  graphics.hpp（PixelWriter, PixelColor）
 *             →  frame_buffer_config.hpp（间接，通过 graphics.hpp）
 *
 * ── 字体渲染调用链 ───────────────────────────────────────────────────────────
 *
 *   main.cpp: WriteString → WriteAscii → GetFont（查找位图）
 *                                      → PixelWriter::Write（写像素）
 */
 
#pragma once
 
#include <cstdint>        // uint8_t
#include "graphics.hpp"   // PixelWriter, PixelColor
 
/**
 * GetFont — 根据 ASCII 字符码返回对应 16 字节位图数据的指针
 *
 * @param c  ASCII 字符码（signed char）
 *           内部转为 unsigned int 后乘以 16 计算在 hankaku.bin 中的字节偏移。
 *           示例：'A'=65 → 偏移 16×65=1040，即 hankaku.bin[1040..1055]。
 *
 * @return   指向字符 c 的 16 字节位图数据的常量指针（可作为 uint8_t[16] 使用）；
 *           字符码超出字体文件范围时返回 nullptr。
 *
 * 返回指针格式（每字节对应一行，共 16 行，8 像素宽）：
 *   font[0]  第 0 行（顶行），bit7=最左像素，bit0=最右像素
 *   ...
 *   font[15] 第 15 行（底行）
 *
 * 详细实现原理见 font.cpp。
 */
const uint8_t* GetFont(char c);
 
/**
 * WriteAscii — 在屏幕指定坐标渲染单个 ASCII 字符（8×16 像素位图）
 *
 * @param writer  像素写入器引用，Write() 虚函数自动适配 RGB/BGR 帧缓冲格式
 * @param x       字符左上角屏幕 X 坐标（0=最左）；字符占 x~x+7 列
 * @param y       字符左上角屏幕 Y 坐标（0=最顶）；字符占 y~y+15 行
 * @param c       要渲染的字符码，GetFont(c) 超出范围时静默跳过
 * @param color   字符前景色（背景色不由本函数控制）
 *
 * 渲染算法：(font[dy] << dx) & 0x80u 逐位提取像素，详见 font.cpp。
 */
void WriteAscii(PixelWriter& writer, int x, int y, char c, const PixelColor& color);
 
/**
 * WriteString — 渲染以 '\0' 结尾的 ASCII 字符串
 *
 * @param writer  像素写入器引用（同 WriteAscii）
 * @param x       字符串第一个字符左上角的屏幕 X 坐标
 * @param y       字符串所在行的屏幕 Y 坐标（所有字符共用同一 Y）
 * @param s       以 '\0' 结尾的 ASCII 字符串指针
 *                可以是字符串字面量或 sprintf/snprintf 输出的字符数组
 * @param color   字符串整体前景色
 *
 * 实现：逐字符调用 WriteAscii(writer, x + 8*i, y, s[i], color)。
 * 每字符宽 8 像素，水平方向紧密排列。
 *
 * 使用示例（来自 main.cpp）：
 *   WriteString(*pixel_writer, 0, 66, "Hello, world!", {0, 0, 255});
 *
 *   char buf[128];
 *   sprintf(buf, "1 + 2 = %d", 1 + 2);
 *   WriteString(*pixel_writer, 0, 82, buf, {0, 0, 0});
 */
void WriteString(PixelWriter& writer, int x, int y, const char* s, const PixelColor& color);
 
 
 
/**
 * @file graphics.cpp
 *
 * 画像描画関連のプログラムを集めたファイル．
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本文件实现 graphics.hpp 中声明的两个具体像素写入器的 Write() 方法：
 *     ① RGBResv8BitPerColorPixelWriter::Write — RGB 字节序像素写入
 *     ② BGRResv8BitPerColorPixelWriter::Write — BGR 字节序像素写入
 *
 *   两个函数的唯一区别：红色与蓝色字节的存储顺序相反。
 *
 * ── 为何实现在 .cpp 而非 .hpp（ODR 原理）─────────────────────────────────────
 *
 *   若将 Write() 函数体直接写在头文件中（非 inline），
 *   每个 #include "graphics.hpp" 的翻译单元都会生成一份函数定义。
 *   链接时多份同名函数定义触发 ODR（One Definition Rule）违规，
 *   链接器报 "multiple definition of RGBResv8BitPerColorPixelWriter::Write"。
 *   将实现放在此文件中，整个项目只有一份定义，ODR 满足。
 *
 * ── 虚函数调用路径 ───────────────────────────────────────────────────────────
 *
 *   main.cpp 通过基类指针调用 Write()：
 *     pixel_writer->Write(x, y, color);
 *
 *   运行时 vtable 查找步骤：
 *     1. 从对象头部读取 vptr（虚函数表指针，编译器自动插入）
 *     2. 通过 vptr 找到该类的 vtable
 *     3. 从 vtable 读取 Write() 的实际地址，跳转执行
 *        → RGBResv8Bit... 对象：跳转到本文件第一个函数
 *        → BGRResv8Bit... 对象：跳转到本文件第二个函数
 */
 
// #@@range_begin(pixel_writer_impl)
/**
 * #include "graphics.hpp"
 *   引入：PixelColor 结构体、PixelWriter 基类（含受保护的 PixelAt() 方法）、
 *          两个子类的声明（本文件提供其 Write() 实现）。
 */
#include "graphics.hpp"
 
/**
 * RGBResv8BitPerColorPixelWriter::Write
 *   — 向 RGB 字节序帧缓冲区写入单个像素
 *
 * 适用场景：GOP PixelFormat == kPixelRGBResv8BitPerColor
 *
 * @param x  像素列号（0=最左）
 * @param y  像素行号（0=最顶）
 * @param c  逻辑颜色 {r, g, b}，各 8 位
 *
 * 实现步骤：
 *   1. PixelAt(x, y)：计算像素 (x,y) 在帧缓冲区的字节地址
 *      p = frame_buffer + 4 × (pixels_per_scan_line × y + x)
 *   2. p[0]=c.r, p[1]=c.g, p[2]=c.b：按 RGB 顺序写入三色字节
 *      p[3]（保留字节）不写入（保持原值，通常为 0）
 *
 * 物理字节序（内存低→高地址）：[R][G][B][_]
 * 与 PixelColor 逻辑字段顺序 {r, g, b} 一致，直接映射，无需转换。
 *
 * auto p = PixelAt(x, y)：
 *   PixelAt 返回 uint8_t*，auto 推导 p 为 uint8_t*。
 *   p[0..2] 为指针下标运算，访问同一像素的三个连续颜色字节。
 */
void RGBResv8BitPerColorPixelWriter::Write(int x, int y, const PixelColor& c) {
  auto p = PixelAt(x, y);  // 计算像素 (x, y) 在帧缓冲区中的字节地址
  p[0] = c.r;              // 偏移 0：红色（R）
  p[1] = c.g;              // 偏移 1：绿色（G）
  p[2] = c.b;              // 偏移 2：蓝色（B）
}
// #@@range_end(pixel_writer_impl)
 
/**
 * BGRResv8BitPerColorPixelWriter::Write
 *   — 向 BGR 字节序帧缓冲区写入单个像素
 *
 * 适用场景：GOP PixelFormat == kPixelBGRResv8BitPerColor（部分 VGA/QEMU 默认）
 *
 * @param x  像素列号（0=最左）
 * @param y  像素行号（0=最顶）
 * @param c  逻辑颜色 {r, g, b}，各 8 位
 *
 * 与 RGBResv8BitPerColorPixelWriter::Write 的唯一差异：
 *   RGB 版：p[0]=c.r, p[1]=c.g, p[2]=c.b  → 内存字节序 [R][G][B][_]
 *   BGR 版：p[0]=c.b, p[1]=c.g, p[2]=c.r  → 内存字节序 [B][G][R][_]
 *   仅红（R）和蓝（B）的存储位置互换，绿（G）始终在偏移 1。
 *
 * 字节序判断方法：渲染纯红色 {255, 0, 0}，
 *   若屏幕显示红色 → RGB 写入器正确；
 *   若屏幕显示蓝色 → 应换用 BGR 写入器（或反之）。
 */
void BGRResv8BitPerColorPixelWriter::Write(int x, int y, const PixelColor& c) {
  auto p = PixelAt(x, y);  // 计算像素 (x, y) 在帧缓冲区中的字节地址
  p[0] = c.b;              // 偏移 0：蓝色（B）← RGB 版此处写红色
  p[1] = c.g;              // 偏移 1：绿色（G）← 两版相同
  p[2] = c.r;              // 偏移 2：红色（R）← RGB 版此处写蓝色
}
 