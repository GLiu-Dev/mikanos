/**
 * @file font.hpp
 *
 * フォント描画に関する宣言をまとめたヘッダファイル.
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本头文件是 MikanOS 字体渲染子系统的对外接口声明，提供：
 *     ① GetFont(char c)    — 根据字符码获取 hankaku.bin 位图数据指针
 *     ② WriteAscii(...)    — 在帧缓冲区指定坐标渲染单个 ASCII 字符
 *
 *   实现位于 font.cpp，使用者只需包含本头文件即可调用这两个函数。
 *
 * ── 字体渲染架构说明 ─────────────────────────────────────────────────────────
 *
 *   字体数据来源：
 *     hankaku.bin（半角字体文件）由构建系统通过 objcopy 嵌入内核 ELF，
 *     运行时通过链接器生成的 _binary_hankaku_bin_* 符号访问，无需文件 I/O。
 *
 *   渲染依赖关系：
 *     font.hpp / font.cpp  →  graphics.hpp（PixelWriter, PixelColor）
 *     graphics.hpp         →  frame_buffer_config.hpp（FrameBufferConfig）
 *
 *   调用关系（main.cpp 中）：
 *     KernelMain → WriteAscii → GetFont（查找位图） → PixelWriter::Write（写像素）
 *
 * ── 字符集说明 ───────────────────────────────────────────────────────────────
 *
 *   hankaku.bin 通常涵盖 ASCII（0x00–0x7F）及半角假名（0xA1–0xDF）。
 *   GetFont 对越界字符返回 nullptr，WriteAscii 静默跳过不渲染。
 *
 *   main.cpp 示例（渲染全部可打印 ASCII '!' 到 '~'，共 94 字符）：
 *     int i = 0;
 *     for (char c = '!'; c <= '~'; ++c, ++i) {
 *       WriteAscii(*pixel_writer, 8 * i, 50, c, {0, 0, 0});
 *     }
 */
 
#pragma once
 
#include <cstdint>        // uint8_t
#include "graphics.hpp"   // PixelWriter, PixelColor
 
/**
 * GetFont — 根据 ASCII 字符码返回对应位图数据的指针
 *
 * @param c  ASCII 字符码（signed char）
 *           内部转为 unsigned int 后乘以 16 计算在 hankaku.bin 中的字节偏移。
 *           示例：'A'=65 → 偏移 16×65=1040，即 hankaku.bin[1040..1055]。
 *
 * @return   指向字符 c 的 16 字节位图数据的常量指针（uint8_t[16]）；
 *           若字符码超出字体文件范围（越界）则返回 nullptr。
 *
 * 返回指针的格式（每字节对应一行，共 16 行，8 像素宽）：
 *   font[0]  第 0 行（顶行），bit7=最左像素，bit0=最右像素
 *   font[1]  第 1 行
 *   ...
 *   font[15] 第 15 行（底行）
 *
 * 详细实现原理见 font.cpp 中的函数注释。
 */
const uint8_t* GetFont(char c);
 
/**
 * WriteAscii — 在屏幕指定坐标渲染单个 ASCII 字符（8×16 像素位图）
 *
 * @param writer  像素写入器引用，通过虚函数 Write() 适配 RGB/BGR 帧缓冲格式。
 *                RGBResv8BitPerColorPixelWriter 或 BGRResv8BitPerColorPixelWriter，
 *                由 main.cpp 根据 FrameBufferConfig.pixel_format 选择并构造。
 *
 * @param x       字符左上角的屏幕 X 坐标（像素列号，0=最左）。
 *                字符向右渲染 8 个像素（x, x+1, ..., x+7）。
 *
 * @param y       字符左上角的屏幕 Y 坐标（像素行号，0=最顶）。
 *                字符向下渲染 16 个像素（y, y+1, ..., y+15）。
 *
 * @param c       要渲染的字符，传给 GetFont(c) 查找位图。
 *                若 GetFont 返回 nullptr（字符超出范围），函数静默返回，不渲染任何像素。
 *
 * @param color   字符前景色（笔画颜色）。背景色不由本函数控制，由调用方预先填充。
 *                const 引用传递，避免拷贝 {r,g,b} 三字节结构体。
 *
 * 渲染算法要点：
 *   for dy in 0..15:          // 遍历 16 行
 *     for dx in 0..7:         // 遍历 8 列
 *       if (font[dy] << dx) & 0x80u:  // bit7 测试：该位置是否有笔画
 *         writer.Write(x+dx, y+dy, color)  // 有笔画 → 写前景色像素
 *
 * 详细实现及位操作说明见 font.cpp 中的函数注释。
 */
void WriteAscii(PixelWriter& writer, int x, int y, char c, const PixelColor& color);
 
 