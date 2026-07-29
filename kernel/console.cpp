/**
 * @file console.cpp
 *
 * コンソール描画のプログラムを集めたファイル．
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本文件实现 Console 类的三个成员函数：
 *     ① Console::Console   — 构造函数，初始化控制台状态
 *     ② Console::PutString — 输出字符串，处理换行，记录字符到缓冲区
 *     ③ Console::Newline   — 换行处理，达到底部时执行屏幕滚动
 *
 * ── 控制台模型概述 ───────────────────────────────────────────────────────────
 *
 *   Console 模拟传统 VT100/ANSI 终端的文本显示：
 *
 *   屏幕布局（kRows=25，kColumns=80）：
 *     第 0 行：像素 y=0..15
 *     第 1 行：像素 y=16..31
 *     ...
 *     第 24 行：像素 y=384..399
 *     第 0 列：像素 x=0..7
 *     ...
 *     第 79 列：像素 x=632..639
 *
 *   字符渲染：WriteAscii(writer_, 8*col, 16*row, c, fg_color_)
 *   字符记录：buffer_[row][col] = c（用于滚动时重绘）
 *
 *   滚动触发：cursor_row_ 已在最后一行（kRows-1=24），再次换行时执行滚动。
 *
 * ── 与上一版本的关系 ─────────────────────────────────────────────────────────
 *
 *   之前 main.cpp 直接调用 WriteString / WriteAscii，输出固定位置单行文本。
 *   Console 将多行输出和滚动逻辑封装起来：
 *     main.cpp 只需 console.PutString(buf)，无需管理坐标和滚动。
 *   验证场景（来自 main.cpp）：输出 27 行 "line N\n"（超过 kRows=25），触发 2 次滚动。
 */
 
#include "console.hpp"
 
#include <cstring>    // memcpy（复制缓冲区行数据）、memset（清空最后一行）
#include "font.hpp"   // WriteAscii（单字符渲染）、WriteString（整行重绘）
 
/* ============================================================================
 * 一、构造函数
 * ============================================================================ */
 
// #@@range_begin(constructor)
/**
 * Console::Console — 初始化控制台
 *
 * @param writer    像素写入器引用（渲染字符到帧缓冲区）
 * @param fg_color  前景色（文字颜色）
 * @param bg_color  背景色（清屏填充色）
 *
 * ── 成员初始化列表详解 ───────────────────────────────────────────────────────
 *
 *   writer_{writer}
 *     绑定写入器引用。Console 的所有渲染操作通过此引用调用 Write() 虚函数，
 *     运行时自动分派到 RGB 或 BGR 版本的写入器。
 *
 *   fg_color_{fg_color}, bg_color_{bg_color}
 *     拷贝颜色值。PixelColor 是 3 字节 POD，拷贝代价极低，无需引用传递。
 *     const 成员：控制台颜色在构造后不可修改（设计上与控制台实例绑定）。
 *
 *   buffer_{}
 *     对 char buffer_[kRows][kColumns+1] 执行聚合初始化（aggregate initialization），
 *     将所有字节初始化为 '\0'。
 *     效果等价于：memset(buffer_, 0, sizeof(buffer_))
 *     '\0' 初始化的重要性：
 *       WriteString 函数遇到 '\0' 终止，若 buffer_ 有未初始化的垃圾字节，
 *       滚动重绘时会渲染乱码。
 *
 *   cursor_row_{0}, cursor_column_{0}
 *     光标初始位于左上角（第 0 行第 0 列），即屏幕起始位置。
 */
Console::Console(PixelWriter& writer,
    const PixelColor& fg_color, const PixelColor& bg_color)
    : writer_{writer}, fg_color_{fg_color}, bg_color_{bg_color},
      buffer_{}, cursor_row_{0}, cursor_column_{0} {
}
// #@@range_end(constructor)
 
/* ============================================================================
 * 二、PutString — 字符串输出
 * ============================================================================ */
 
// #@@range_begin(put_string)
/**
 * Console::PutString — 向控制台输出字符串
 *
 * @param s  以 '\0' 结尾的字符串（可含 '\n' 换行符）
 *
 * ── 逐字符处理逻辑 ───────────────────────────────────────────────────────────
 *
 *   while (*s)：遍历字符串，遇到 '\0' 终止（不使用索引，直接移动指针）。
 *
 *   if (*s == '\n')：换行符处理
 *     调用 Newline()：光标移到下一行首列，若已到底部则滚动屏幕。
 *
 *   else if (cursor_column_ < kColumns - 1)：普通字符处理（行未满时）
 *     WriteAscii(writer_, 8*cursor_column_, 16*cursor_row_, *s, fg_color_)：
 *       渲染字符 *s 到屏幕坐标 (8*cursor_column_, 16*cursor_row_)。
 *       像素坐标：每字符宽 8 像素（hankaku.bin 固定宽度），高 16 像素。
 *     buffer_[cursor_row_][cursor_column_] = *s：
 *       将字符记录到字符缓冲区，用于滚动时重绘。
 *     ++cursor_column_：光标向右移动一列。
 *
 *   为何上界是 kColumns - 1 而非 kColumns：
 *     buffer_[row] 共 kColumns + 1 字节，最后一字节（索引 kColumns）必须保持为 '\0'
 *     （供 WriteString 用作字符串终止符）。
 *     若 cursor_column_ == kColumns - 1，写入后 cursor_column_ 变为 kColumns - 1，
 *     下一字符因条件不满足而被丢弃，buffer_[row][kColumns] = '\0' 得以保留。
 *     实际上最多存储 kColumns - 1 = 79 个字符（略少于显示宽度），这是设计取舍。
 *
 *   行已满时（cursor_column_ >= kColumns - 1）的处理：
 *     当前字符被静默跳过（不渲染，不写入缓冲区）。
 *     等待 '\n' 触发换行，否则光标停在原位。
 *
 *   ++s：每次循环末尾推进字符串指针，处理下一个字符。
 */
void Console::PutString(const char* s) {
  while (*s) {
    if (*s == '\n') {
      // 换行符：调用 Newline 处理光标移动和可能的滚动
      Newline();
    } else if (cursor_column_ < kColumns - 1) {
      // 普通字符且行未满：渲染到屏幕，记录到缓冲区，光标右移
      WriteAscii(writer_, 8 * cursor_column_, 16 * cursor_row_, *s, fg_color_);
      buffer_[cursor_row_][cursor_column_] = *s;
      ++cursor_column_;
    }
    ++s;  // 推进到下一个字符
  }
}
// #@@range_end(put_string)
 
/* ============================================================================
 * 三、Newline — 换行与滚动
 * ============================================================================ */
 
// #@@range_begin(newline)
/**
 * Console::Newline — 处理换行，必要时执行屏幕滚动
 *
 * 由 PutString 在遇到 '\n' 字符时调用。
 *
 * ── 情况 1：屏幕未满（cursor_row_ < kRows - 1）────────────────────────────────
 *
 *   cursor_column_ = 0：光标移到行首
 *   ++cursor_row_      ：光标移到下一行
 *   无需清屏或重绘，操作 O(1)。
 *
 * ── 情况 2：屏幕已满（cursor_row_ == kRows - 1）—— 执行滚动 ──────────────────
 *
 *   此时光标已在最后一行，再换行需要将所有内容上移一行。
 *
 *   步骤 A：清屏（用背景色填充整个控制台像素区域）
 *     双重循环遍历 y ∈ [0, 16*kRows) 和 x ∈ [0, 8*kColumns)：
 *       writer_.Write(x, y, bg_color_) 清除每个像素为背景色。
 *     清屏范围：控制台占用的全部像素（400 行 × 640 列 = 256000 像素）。
 *     为何要完整清屏：WriteString 重绘时不清除字符间隙，若不清屏，
 *       旧字符的像素残留会与新内容叠加显示乱码。
 *
 *   步骤 B：上移字符缓冲区（buffer_ 数据迁移）
 *     for row = 0 to kRows - 2：
 *       memcpy(buffer_[row], buffer_[row + 1], kColumns + 1)
 *       将下一行的字符数据复制到当前行（上移一行）。
 *       kColumns + 1：含 '\0' 终止符的完整行（buffer_ 列宽）。
 *     执行后：
 *       buffer_[0] ← 原 buffer_[1]（第 2 行内容）
 *       buffer_[1] ← 原 buffer_[2]（第 3 行内容）
 *       ...
 *       buffer_[kRows-2] ← 原 buffer_[kRows-1]（最后一行内容）
 *       buffer_[kRows-1]：数据仍是旧的最后一行（下一步清零）
 *
 *   步骤 C：重绘所有行（buffer_ 数据 → 屏幕像素）
 *     for row = 0 to kRows - 2：
 *       WriteString(writer_, 0, 16*row, buffer_[row], fg_color_)
 *       将 buffer_[row] 的字符串渲染到屏幕第 row 行的起始位置。
 *       只重绘 kRows - 1 行（不含最后一行，最后一行将被新内容填充）。
 *
 *   步骤 D：清空最后一行缓冲区
 *     memset(buffer_[kRows - 1], 0, kColumns + 1)
 *     将 buffer_ 最后一行全部置 '\0'，为接下来 PutString 写入新内容做准备。
 *     若不清零，上次内容残留在 buffer_ 中，下次滚动时会被重绘到错误位置。
 *
 *   cursor_column_ = 0（情况 1/2 均执行）：
 *     无论是否滚动，换行后光标都回到行首（列 0）。
 *   cursor_row_ 在情况 2 中保持为 kRows - 1：
 *     滚动后光标仍在最后一行（视觉上屏幕内容整体上移了一行）。
 *
 * ── 调用示例（来自 main.cpp）────────────────────────────────────────────────
 *
 *   for (int i = 0; i < 27; ++i) {
 *     sprintf(buf, "line %d\n", i);   // 每条消息含 '\n'
 *     console.PutString(buf);
 *   }
 *   kRows=25，输出 27 行：前 25 行正常，第 26 行触发第 1 次滚动，
 *   第 27 行触发第 2 次滚动。最终屏幕显示 "line 2" ~ "line 26"（25 行）。
 */
void Console::Newline() {
  cursor_column_ = 0;  // 光标回到行首（无论是否滚动）
  if (cursor_row_ < kRows - 1) {
    // 情况 1：屏幕未满，直接移到下一行
    ++cursor_row_;
  } else {
    // 情况 2：屏幕已满，执行滚动
 
    // 步骤 A：清屏（用背景色覆盖整个控制台区域的所有像素）
    for (int y = 0; y < 16 * kRows; ++y) {
      for (int x = 0; x < 8 * kColumns; ++x) {
        writer_.Write(x, y, bg_color_);
      }
    }
 
    // 步骤 B & C：上移缓冲区并逐行重绘
    for (int row = 0; row < kRows - 1; ++row) {
      // B：将 buffer_[row+1] 的内容上移到 buffer_[row]（含 '\0' 终止符）
      memcpy(buffer_[row], buffer_[row + 1], kColumns + 1);
      // C：将上移后的 buffer_[row] 渲染到屏幕第 row 行
      //    x=0（从最左列开始），y=16*row（第 row 行的像素起始纵坐标）
      WriteString(writer_, 0, 16 * row, buffer_[row], fg_color_);
    }
 
    // 步骤 D：清空最后一行的字符缓冲区，准备接收新内容
    memset(buffer_[kRows - 1], 0, kColumns + 1);
  }
}
// #@@range_end(newline)
 