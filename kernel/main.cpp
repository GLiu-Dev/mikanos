/**
 * @file main.cpp
 *
 * カーネル本体のプログラムを書いたファイル．
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本文件是 MikanOS 内核的入口文件，包含：
 *     ① placement new / operator delete — 无堆内存的对象构造机制
 *     ② pixel_writer_buf / pixel_writer  — 像素写入器的静态存储与全局访问点
 *     ③ KernelMain()                    — 内核主函数（由引导加载程序跳转进入）
 *
 *   KernelMain 执行顺序：
 *     1. 根据 PixelFormat 构造对应像素写入器（运行时多态）
 *     2. 将全屏涂白（背景色）
 *     3. 构造 Console 对象（黑色文字 / 白色背景，25 行 × 80 列）
 *     4. 循环输出 27 行 "line N\n"（超过 25 行，触发 2 次屏幕滚动）
 *     5. 进入无限 HLT 循环
 *
 * ── 与上一版本的核心变化 ─────────────────────────────────────────────────────
 *
 *   旧版：直接调用 WriteAscii/WriteString 渲染固定位置的字符/字符串
 *   新版：引入 Console 类，通过 PutString 进行多行文本输出和自动滚动
 *
 *   具体替换：
 *     旧：绿色矩形 + 94 个 ASCII 字符 + "Hello, world!" + sprintf 格式化行
 *     新：全屏白色 + Console 输出 27 行（演示滚动功能）
 *
 *   新增头文件：#include "console.hpp"
 *
 * ── 内核被调用的方式 ─────────────────────────────────────────────────────────
 *
 *   引导加载程序读取 ELF e_entry（偏移 +24），以函数指针方式调用 KernelMain(&config)。
 *   extern "C" 禁用 C++ name mangling，使链接器能将 KernelMain 写入 e_entry。
 */
 
#include <cstdint>   // uint8_t 等固定宽度整数类型
#include <cstddef>   // size_t（operator new 参数类型）
#include <cstdio>    // sprintf（由 newlib libc -lc 提供，需要 sbrk 存根）
 
// #@@range_begin(includes)
#include "frame_buffer_config.hpp"  // FrameBufferConfig, PixelFormat 枚举
#include "graphics.hpp"             // PixelColor, PixelWriter 及两个子类
#include "font.hpp"                 // WriteAscii, WriteString（Console 内部使用）
#include "console.hpp"              // Console 类（多行文本控制台）
// #@@range_end(includes)
 
/* ============================================================================
 * 一、placement new / operator delete
 * ============================================================================ */
 
/**
 * operator new（placement new）— 在已存在的缓冲区上构造对象，不做堆分配
 *
 * ExitBootServices 后堆不可用，用 placement new 在静态缓冲区上构造写入器。
 * 语法：new(buf) T{args} → 编译器调用此函数，然后在返回地址上调用构造函数。
 */
void* operator new(size_t size, void* buf) {
  return buf;
}
 
/**
 * operator delete（空实现）— 满足 C++ ABI 要求，静态缓冲区无需释放
 */
void operator delete(void* obj) noexcept {
}
 
/* ============================================================================
 * 二、全局变量
 * ============================================================================ */
 
/**
 * pixel_writer_buf — 像素写入器对象的静态存储缓冲区
 *
 * 大小 = sizeof(RGBResv8BitPerColorPixelWriter)（RGB/BGR 两个子类 sizeof 相同）。
 * char 数组：C++ 标准允许将任意对象放置在 char 数组中（无别名/对齐问题）。
 */
char pixel_writer_buf[sizeof(RGBResv8BitPerColorPixelWriter)];
 
/**
 * pixel_writer — 指向当前激活的像素写入器的全局指针（初始 nullptr）
 *
 * 基类指针，通过虚函数 Write() 实现运行时多态：
 * vtable 根据实际对象类型（RGB/BGR）分派到正确的 Write() 实现。
 */
PixelWriter* pixel_writer;
 
/* ============================================================================
 * 三、KernelMain
 * ============================================================================ */
 
/**
 * KernelMain — MikanOS 内核入口函数
 *
 * @param frame_buffer_config  帧缓冲区配置（引导加载程序构造并传入）
 *
 * extern "C"：禁用 name mangling，使符号名保持 "KernelMain"，
 *   链接器将其地址写入 ELF e_entry，引导加载程序以此跳转入内核。
 * 函数永不返回：末尾为无限 HLT 循环。
 */
extern "C" void KernelMain(const FrameBufferConfig& frame_buffer_config) {
 
  /* ── 步骤 1：根据像素格式构造对应写入器 ─────────────────────────────────── */
 
  /**
   * switch(pixel_format)：运行时检查 GOP 报告的字节序，选择写入器实现。
   *   kPixelRGBResv8BitPerColor：内存字节序 [R][G][B][_]（现代显卡常见）
   *   kPixelBGRResv8BitPerColor：内存字节序 [B][G][R][_]（部分 VGA/QEMU）
   *
   * new(pixel_writer_buf) T{config}：placement new，在静态缓冲区上原地构造，无堆分配。
   */
  switch (frame_buffer_config.pixel_format) {
    case kPixelRGBResv8BitPerColor:
      pixel_writer = new(pixel_writer_buf)
        RGBResv8BitPerColorPixelWriter{frame_buffer_config};
      break;
    case kPixelBGRResv8BitPerColor:
      pixel_writer = new(pixel_writer_buf)
        BGRResv8BitPerColorPixelWriter{frame_buffer_config};
      break;
  }
 
  /* ── 步骤 2：将整个屏幕涂白（背景色）─────────────────────────────────────── */
 
  /**
   * 遍历 x ∈ [0, horizontal_resolution)，y ∈ [0, vertical_resolution)。
   * {255, 255, 255} = 白色。
   *
   * 为何用 horizontal/vertical_resolution 而非 pixels_per_scan_line：
   *   resolution 决定"画到哪里"（实际可见像素范围），
   *   pixels_per_scan_line 由 PixelAt() 内部用于字节地址计算（含行末填充步长）。
   */
  for (int x = 0; x < frame_buffer_config.horizontal_resolution; ++x) {
    for (int y = 0; y < frame_buffer_config.vertical_resolution; ++y) {
      pixel_writer->Write(x, y, {255, 255, 255});
    }
  }
 
  /* ── 步骤 3：构造控制台对象 ──────────────────────────────────────────────── */
 
  /**
   * Console console{*pixel_writer, {0, 0, 0}, {255, 255, 255}}：
   *   在栈上构造 Console 对象（Console 不用 placement new，因为它是函数局部变量）。
   *   *pixel_writer：解引用全局写入器指针，传入引用（Console 存储写入器引用）。
   *   {0, 0, 0}    ：前景色 = 黑色（文字颜色）
   *   {255,255,255}：背景色 = 白色（与步骤 2 的填充色一致，滚动清屏时使用）
   *
   * Console 初始状态：光标位于 (row=0, col=0)，buffer_ 全零，屏幕已由步骤 2 填白。
   */
  Console console{*pixel_writer, {0, 0, 0}, {255, 255, 255}};
 
  /* ── 步骤 4：循环输出 27 行文字（演示控制台滚动功能）────────────────────── */
 
  /**
   * 输出 27 行 "line N\n"（N = 0..26）：
   *   kRows = 25，输出超过 25 行时触发滚动：
   *     第 0~24 行（25 行）：正常逐行显示，cursor_row_ 从 0 增加到 24
   *     第 25 行（"line 25\n"）：触发第 1 次滚动，内容上移一行，显示 "line 1"~"line 25"
   *     第 26 行（"line 26\n"）：触发第 2 次滚动，显示 "line 2"~"line 26"
   *   最终屏幕显示 25 行："line 2" ~ "line 26"。
   *
   * sprintf(buf, "line %d\n", i)：
   *   使用 newlib libc（-lc）的 sprintf 格式化字符串。
   *   %d：将整数 i 格式化为十进制，\n 在 PutString 中触发换行。
   *
   * console.PutString(buf)：
   *   逐字符渲染 buf，遇到 '\n' 调用 Newline()。
   *   字符同时记录到 buffer_，供滚动时重绘使用。
   */
  char buf[128];
  for (int i = 0; i < 27; ++i) {
    sprintf(buf, "line %d\n", i);
    console.PutString(buf);
  }
 
  /* ── 步骤 5：无限 HLT 循环 ───────────────────────────────────────────────── */
 
  /**
   * HLT 使 CPU 进入低功耗等待状态，防止 KernelMain 返回后执行无效指令。
   * __asm__("hlt")：GCC/Clang 内联汇编，嵌入单条 x86 HLT 指令。
   */
  while (1) __asm__("hlt");
}
 
 