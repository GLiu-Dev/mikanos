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
 *   这两个函数是唯一的区别：红色与蓝色字节的存储顺序相反。
 *
 * ── 为何实现在 .cpp 而非 .hpp（ODR 原理） ───────────────────────────────────
 *
 *   若将 Write() 函数体直接写在头文件中（非 inline），则每个 #include "graphics.hpp"
 *   的翻译单元（main.cpp、font.cpp、graphics.cpp 等）都会各自生成一份函数定义。
 *   链接时多份同名函数定义触发 ODR（One Definition Rule）违规，链接器报
 *   "multiple definition of `RGBResv8BitPerColorPixelWriter::Write`" 错误。
 *
 *   将实现放在 graphics.cpp 中，整个项目只有一份函数定义，ODR 满足。
 *   其他翻译单元通过包含 graphics.hpp 获得函数声明，链接时解析到本文件提供的定义。
 *
 * ── 虚函数调用路径 ───────────────────────────────────────────────────────────
 *
 *   main.cpp 通过基类指针调用 Write()：
 *     pixel_writer->Write(x, y, {255, 255, 255});
 *
 *   运行时查找步骤：
 *     1. 从对象头部读取 vptr（虚函数表指针，编译器自动插入）
 *     2. 通过 vptr 找到该类的 vtable
 *     3. 从 vtable 读取 Write() 的实际地址
 *        → RGBResv8Bit...PixelWriter 对象：跳转到本文件第一个函数
 *        → BGRResv8Bit...PixelWriter 对象：跳转到本文件第二个函数
 *
 *   此机制使 font.cpp 中的 WriteAscii、main.cpp 中的填充循环等渲染代码
 *   无需感知字节序差异，实现"对接口编程"的设计原则。
 */
 
// #@@range_begin(pixel_writer_impl)
/**
 * graphics.hpp — 提供：
 *   PixelColor 结构体（r, g, b 各 uint8_t）
 *   PixelWriter 基类（含 PixelAt() 受保护方法）
 *   RGBResv8BitPerColorPixelWriter / BGRResv8BitPerColorPixelWriter 声明
 *   frame_buffer_config.hpp（间接包含，提供 FrameBufferConfig）
 */
#include "graphics.hpp"
 
/**
 * RGBResv8BitPerColorPixelWriter::Write
 *   — 向 RGB 字节序帧缓冲区写入单个像素
 *
 * 适用场景：
 *   GOP PixelFormat == kPixelRGBResv8BitPerColor（DisplayPort、HDMI 常见格式）
 *
 * @param x  像素列号（0 = 最左）
 * @param y  像素行号（0 = 最顶）
 * @param c  逻辑颜色（{r, g, b}，各 8 位）
 *
 * ── 实现步骤详解 ───────────────────────────────────────────────────────────
 *
 *   1. PixelAt(x, y)
 *      调用基类受保护方法，计算 (x, y) 像素在帧缓冲区中的字节地址：
 *        p = config_.frame_buffer + 4 * (config_.pixels_per_scan_line * y + x)
 *      返回 uint8_t*，指向该像素的 4 字节区域的起始字节。
 *
 *   2. p[0] = c.r — 偏移 0：写入红色分量
 *      p[1] = c.g — 偏移 1：写入绿色分量
 *      p[2] = c.b — 偏移 2：写入蓝色分量
 *      p[3]（保留字节）未写入，保持原值（通常已为 0）。
 *
 *   物理字节顺序（内存低地址→高地址）：[R][G][B][_]
 *   与 PixelColor 的逻辑字段顺序（r, g, b）完全一致，直接映射，无需转换。
 *
 * ── auto p = PixelAt(x, y) 类型推导 ─────────────────────────────────────
 *
 *   PixelAt() 返回 uint8_t*，auto 推导 p 为 uint8_t*。
 *   p[0]、p[1]、p[2] 为指针下标运算，等价于 *(p+0)、*(p+1)、*(p+2)，
 *   访问连续的 3 个字节，对应同一像素的三个颜色通道。
 */
void RGBResv8BitPerColorPixelWriter::Write(int x, int y, const PixelColor& c) {
  auto p = PixelAt(x, y);  // 计算像素 (x, y) 的帧缓冲区字节地址
  p[0] = c.r;              // 偏移 0：红色（R）
  p[1] = c.g;              // 偏移 1：绿色（G）
  p[2] = c.b;              // 偏移 2：蓝色（B）
}
// #@@range_end(pixel_writer_impl)
 
/**
 * BGRResv8BitPerColorPixelWriter::Write
 *   — 向 BGR 字节序帧缓冲区写入单个像素
 *
 * 适用场景：
 *   GOP PixelFormat == kPixelBGRResv8BitPerColor（部分 VGA/VESA 兼容设备）
 *
 * @param x  像素列号（0 = 最左）
 * @param y  像素行号（0 = 最顶）
 * @param c  逻辑颜色（{r, g, b}，各 8 位）
 *
 * ── 与 RGBResv8BitPerColorPixelWriter::Write 的唯一差异 ───────────────────
 *
 *   RGB 版：p[0]=c.r, p[1]=c.g, p[2]=c.b  → 字节序 [R][G][B][_]
 *   BGR 版：p[0]=c.b, p[1]=c.g, p[2]=c.r  → 字节序 [B][G][R][_]
 *
 *   仅红色（R）和蓝色（B）的存储位置互换，绿色（G）始终在偏移 1。
 *
 *   若将 BGR 写入器用于 RGB 帧缓冲区（或反之），屏幕上红色会显示为蓝色，
 *   蓝色会显示为红色；绿色不受影响。这是判断字节序是否正确的简单测试：
 *   渲染纯红色 {255, 0, 0} 观察是否确实显示红色。
 *
 * ── 实现细节（与 RGB 版相同部分，不再重复） ────────────────────────────────
 *
 *   PixelAt(x, y) 计算像素地址（见 graphics.hpp 中的详细注释）。
 *   p[0..2] 为同一像素 4 字节中的前 3 字节（第 4 字节为保留位，不写入）。
 */
void BGRResv8BitPerColorPixelWriter::Write(int x, int y, const PixelColor& c) {
  auto p = PixelAt(x, y);  // 计算像素 (x, y) 的帧缓冲区字节地址
  p[0] = c.b;              // 偏移 0：蓝色（B）← RGB 版此处写红色
  p[1] = c.g;              // 偏移 1：绿色（G）← 两版相同
  p[2] = c.r;              // 偏移 2：红色（R）← RGB 版此处写蓝色
}