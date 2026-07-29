/**
 * @file console.hpp
 *
 * コンソール描画に関する宣言をまとめたヘッダファイル.
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本头文件声明 Console 类——MikanOS 内核的文本控制台。
 *   Console 在帧缓冲区上模拟传统终端的行为：
 *     ① 将字符串逐字符渲染到屏幕上（通过 WriteAscii）
 *     ② 当输出超过最后一行时，自动向上滚动（scroll）
 *     ③ 使用内部字符缓冲区（buffer_）记录当前屏幕内容，滚动时重绘
 *
 * ── 设计动机 ─────────────────────────────────────────────────────────────────
 *
 *   之前 main.cpp 直接调用 WriteAscii / WriteString，每次只能渲染单行内容，
 *   无法跨行输出，也无法滚动。Console 将"多行文本输出"封装为独立类：
 *     - 调用方只需 console.PutString(str)，无需关心光标位置和滚动逻辑
 *     - 支持 '\n' 换行，支持超出屏幕高度时自动上滚
 *
 * ── 终端尺寸约定 ─────────────────────────────────────────────────────────────
 *
 *   kRows = 25：25 行（经典 VT100 终端的行数）
 *   kColumns = 80：80 列（经典终端的列数）
 *   每字符宽 8 像素、高 16 像素（hankaku.bin 字体固定尺寸），
 *   因此控制台占用屏幕区域为 80×8=640 像素宽、25×16=400 像素高。
 *
 * ── 依赖关系 ─────────────────────────────────────────────────────────────────
 *
 *   console.hpp → graphics.hpp（PixelWriter, PixelColor）
 *   console.cpp → font.hpp（WriteAscii, WriteString）+ <cstring>（memcpy, memset）
 */
 
#pragma once
 
#include "graphics.hpp"  // PixelWriter（抽象基类）, PixelColor（颜色结构体）
 
/**
 * Console — 支持文本滚动的屏幕控制台
 *
 * ── 成员布局与职责 ───────────────────────────────────────────────────────────
 *
 *   writer_       ：像素写入器引用（由构造函数绑定），负责实际像素渲染
 *   fg_color_     ：前景色（文字颜色），const，构造后不可修改
 *   bg_color_     ：背景色，const，构造后不可修改；滚动时用于清屏
 *   buffer_       ：二维字符数组，存储当前屏幕上每个位置的字符
 *                   buffer_[row][col]：第 row 行第 col 列的字符
 *                   buffer_[row][kColumns] 始终为 '\0'（字符串终止符，供 WriteString 使用）
 *   cursor_row_   ：下一个字符将要写入的行号（0 = 最顶行）
 *   cursor_column_：下一个字符将要写入的列号（0 = 最左列）
 *
 * ── 滚动机制概述 ─────────────────────────────────────────────────────────────
 *
 *   当光标到达最后一行（cursor_row_ == kRows - 1）并再次换行时：
 *     1. 用 bg_color_ 清空整个控制台区域
 *     2. 将 buffer_[1..kRows-1] 逐行复制到 buffer_[0..kRows-2]（内容上移一行）
 *     3. 用 WriteString 重新渲染每一行的 buffer_ 内容
 *     4. 用 memset 清空最后一行的 buffer_（最后一行准备接受新内容）
 *     光标列重置为 0，光标行保持在 kRows - 1（最后一行）
 */
class Console {
 public:
  /**
   * kRows — 控制台最大行数（25 行，经典 VT100 终端行数）
   * kColumns — 控制台最大列数（80 列，经典终端列数）
   *
   * static const int：整型类常量，编译期常量，所有 Console 实例共享同一值。
   * 用于：
   *   - buffer_ 的静态数组大小（须为编译期常量）
   *   - PutString 中检查列溢出（cursor_column_ < kColumns - 1）
   *   - Newline 中检查行溢出（cursor_row_ < kRows - 1）
   *   - 屏幕渲染范围（8 × kColumns 像素宽，16 × kRows 像素高）
   */
  static const int kRows = 25, kColumns = 80;
 
  /**
   * 构造函数 — 初始化控制台的像素写入器、颜色和内部状态
   *
   * @param writer    像素写入器引用（PutString 通过它渲染字符）
   *                  引用语义：Console 生命周期内 writer 必须保持有效
   * @param fg_color  前景色（文字颜色），按值拷贝存入 fg_color_
   * @param bg_color  背景色（清屏颜色），按值拷贝存入 bg_color_
   *
   * 成员初始化列表：
   *   writer_{writer}        ：绑定像素写入器引用
   *   fg_color_{fg_color}    ：拷贝前景色
   *   bg_color_{bg_color}    ：拷贝背景色
   *   buffer_{}              ：零初始化二维字符数组（所有字节为 '\0'）
   *   cursor_row_{0}         ：光标初始位于第 0 行
   *   cursor_column_{0}      ：光标初始位于第 0 列
   */
  Console(PixelWriter& writer,
      const PixelColor& fg_color, const PixelColor& bg_color);
 
  /**
   * PutString — 向控制台输出以 '\0' 结尾的字符串
   *
   * @param s  要输出的字符串，可含 '\n' 换行符
   *
   * 处理逻辑：
   *   '\n' 字符    → 调用 Newline()（光标移到下一行首列，必要时滚动）
   *   其他可见字符 → 渲染到 (cursor_column_, cursor_row_) 位置，同时写入 buffer_，
   *                  cursor_column_ 递增
   *   若 cursor_column_ >= kColumns - 1（行已满）→ 跳过当前字符（不渲染溢出）
   *     保留最后一列为 '\0'（buffer_ 字符串终止符），避免字符串操作越界
   *
   * 像素坐标计算：
   *   x = 8  × cursor_column_（每字符 8 像素宽）
   *   y = 16 × cursor_row_  （每字符 16 像素高）
   */
  void PutString(const char* s);
 
 private:
  /**
   * Newline — 私有辅助函数：处理换行（光标移到下一行，必要时滚动屏幕）
   *
   * 调用时机：PutString 遇到 '\n' 字符时调用。
   *
   * 两种情况：
   *   cursor_row_ < kRows - 1：屏幕未满，直接 ++cursor_row_，无需滚动
   *   cursor_row_ == kRows - 1：屏幕已满，执行滚动：
   *     ① 清屏：用 bg_color_ 填充整个控制台像素区域
   *     ② 上移 buffer_：memcpy buffer_[row] ← buffer_[row+1]（row = 0..kRows-2）
   *     ③ 重绘：用 WriteString 将 buffer_[0..kRows-2] 渲染到屏幕
   *     ④ 清空最后一行：memset buffer_[kRows-1] 为 0
   *
   * cursor_column_ 始终重置为 0（新行从最左列开始）。
   */
  void Newline();
 
  /* ── 私有成员 ────────────────────────────────────────────────────────────── */
 
  /**
   * writer_ — 像素写入器引用
   * 通过虚函数 Write() / WriteAscii / WriteString 渲染字符到帧缓冲区。
   * 引用（非指针）：保证非 null，Console 生命周期内 writer 必须有效。
   */
  PixelWriter& writer_;
 
  /**
   * fg_color_ — 前景色（文字颜色），bg_color_ — 背景色
   * const 成员：构造后不可修改（颜色与控制台绑定，不支持运行时更换）。
   * 按值存储（非引用）：PixelColor 是 3 字节 POD，拷贝代价极低。
   */
  const PixelColor fg_color_, bg_color_;
 
  /**
   * buffer_[kRows][kColumns + 1] — 屏幕字符缓冲区
   *
   * 存储控制台当前显示的文本内容。
   * 每行 kColumns + 1 个字节：kColumns 个字符 + 1 个 '\0' 终止符。
   * '+1' 的必要性：
   *   滚动时调用 WriteString(buffer_[row], ...)，WriteString 直到 '\0' 终止遍历，
   *   因此每行最后一字节必须为 '\0'（由构造函数 buffer_{} 零初始化保证）。
   *   PutString 在 cursor_column_ < kColumns - 1 时才写字符，
   *   保留最后一位（kColumns - 1 处）不写可见字符，确保 buffer_[row][kColumns] = '\0'。
   *
   * 注意：buffer_ 仅记录字符内容，不记录颜色（所有字符统一使用 fg_color_）。
   */
  char buffer_[kRows][kColumns + 1];
 
  /**
   * cursor_row_   — 光标当前行号（0 = 最顶行，kRows-1 = 最底行）
   * cursor_column_— 光标当前列号（0 = 最左列，kColumns-1 = 最右列）
   *
   * 下一次 PutString 写字符时，字符渲染到坐标 (8*cursor_column_, 16*cursor_row_)，
   * 并存入 buffer_[cursor_row_][cursor_column_]。
   */
  int cursor_row_, cursor_column_;
};
 