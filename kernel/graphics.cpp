/**
 * @file graphics.cpp
 *
 * 画像描画関連のプログラムを集めたファイル．
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本文件提供 graphics.hpp 中声明的像素写入器子类的具体实现：
 *     ① RGBResv8BitPerColorPixelWriter::Write() — RGB 格式帧缓冲区的像素写入
 *     ② BGRResv8BitPerColorPixelWriter::Write() — BGR 格式帧缓冲区的像素写入
 *
 * ── 为什么实现放在 .cpp 而非 .hpp？ ─────────────────────────────────────────
 *
 *   C++ 翻译单元规则（One Definition Rule, ODR）：
 *     一个非 inline 函数在整个程序中只能有一份定义。
 *     若将 Write() 的函数体写在 graphics.hpp 中（且不标记 inline），
 *     每一个 #include "graphics.hpp" 的 .cpp 文件编译后都会产生一份
 *     Write 的目标代码，链接器报"multiple definition"错误。
 *
 *   将实现放在 graphics.cpp 中：
 *     • 只有 graphics.cpp 这一个翻译单元包含 Write 的目标代码
 *     • 其他翻译单元（main.cpp、font.cpp）通过 graphics.hpp 中的声明
 *       获知函数签名，链接时由链接器解析实际地址，一切安全
 *
 *   例外：PixelAt() 辅助方法写在 graphics.hpp 中并标记为 inline，
 *   因为它是热路径（每写一个像素调用一次），内联展开可避免函数调用开销。
 *   inline 告知编译器/链接器"多个翻译单元中相同的 inline 定义是预期的，
 *   只保留一份即可"，不触发 ODR 错误。
 *
 * ── 依赖关系 ──────────────────────────────────────────────────────────────
 *
 *   graphics.cpp
 *     └─ #include "graphics.hpp"
 *           ├─ PixelWriter（抽象基类）
 *           ├─ RGBResv8BitPerColorPixelWriter（声明）
 *           ├─ BGRResv8BitPerColorPixelWriter（声明）
 *           └─ PixelColor、PixelAt()、FrameBufferConfig（传递依赖）
 */
 
// #@@range_begin(pixel_writer_impl)
#include "graphics.hpp"
 
/**
 * RGBResv8BitPerColorPixelWriter::Write() — RGB 格式像素写入实现
 *
 * ── 调用路径 ───────────────────────────────────────────────────────────────
 *
 *   pixel_writer->Write(x, y, color)
 *     │  ← pixel_writer 的静态类型是 PixelWriter*（抽象基类指针）
 *     │  ← 运行时 vtable 查找，分发到本函数
 *     ↓
 *   RGBResv8BitPerColorPixelWriter::Write(x, y, color)
 *     │
 *     ├─ PixelAt(x, y)  ← 继承自 PixelWriter，计算帧缓冲字节地址
 *     └─ p[0..2] 赋值   ← 按 RGB 字节序填写三个颜色分量
 *
 * ── 参数说明 ──────────────────────────────────────────────────────────────
 *
 *   x, y（int）
 *     屏幕像素坐标（x=列，y=行，0=左上角）。
 *     PixelAt(x, y) 内部使用 pixels_per_scan_line（而非 horizontal_resolution）
 *     计算行首偏移，正确处理帧缓冲区行末的硬件对齐填充。
 *
 *   c（const PixelColor&）
 *     逻辑颜色（RGB 分量），本函数按 [R][G][B] 顺序写入内存字节。
 *
 * ── 帧缓冲字节序（RGB 格式）─────────────────────────────────────────────
 *
 *   内存中该像素的 4 字节（低地址→高地址）：
 *     p[0] = c.r  (Red)      低地址
 *     p[1] = c.g  (Green)
 *     p[2] = c.b  (Blue)
 *     p[3]        (Reserved) 不修改，保持显卡/固件原有值（对显示无影响）
 *
 *   PixelRedGreenBlueReserved8BitPerColor 名称含义：
 *     Pixel = 像素单元
 *     RedGreenBlue = 字节排列顺序（R 在低地址）
 *     Reserved = 第 4 字节保留（不用于颜色，可能被 GPU 用作其他用途）
 *     8BitPerColor = 每个颜色分量 8 位（即 1 字节，0–255 范围）
 *
 * ── auto p 的类型推断 ────────────────────────────────────────────────────
 *
 *   auto p = PixelAt(x, y);
 *   PixelAt 返回 uint8_t*，因此 p 的类型被推断为 uint8_t*。
 *   使用 auto 避免重复写长类型名，且若将来 PixelAt 返回类型变更，
 *   此处自动适配（无需手动修改）。
 */
void RGBResv8BitPerColorPixelWriter::Write(int x, int y, const PixelColor& c) {
  auto p = PixelAt(x, y);  /* p 指向帧缓冲区中 (x,y) 像素的第一个字节 */
  p[0] = c.r;              /* 低地址：红色分量 */
  p[1] = c.g;              /* 中地址：绿色分量 */
  p[2] = c.b;              /* 高地址：蓝色分量 */
  /* p[3]（保留字节）不修改 */
}
// #@@range_end(pixel_writer_impl)
 
/**
 * BGRResv8BitPerColorPixelWriter::Write() — BGR 格式像素写入实现
 *
 * ── 帧缓冲字节序（BGR 格式）─────────────────────────────────────────────
 *
 *   内存中该像素的 4 字节（低地址→高地址）：
 *     p[0] = c.b  (Blue)     低地址 ← BGR 格式蓝在最低地址
 *     p[1] = c.g  (Green)           ← 绿色位置与 RGB 格式相同
 *     p[2] = c.r  (Red)      高地址 ← BGR 格式红在最高地址
 *     p[3]        (Reserved) 不修改
 *
 *   与 RGBResv8BitPerColorPixelWriter::Write() 的唯一区别：
 *     p[0] 写 c.b（蓝）而非 c.r（红）
 *     p[2] 写 c.r（红）而非 c.b（蓝）
 *     p[1] 写 c.g（绿）——两种格式中绿色位置相同
 *
 * ── 混用两种写入器的视觉后果 ─────────────────────────────────────────────
 *
 *   若将 RGBResv8BitPerColorPixelWriter 误用于 BGR 格式帧缓冲区（或反之）：
 *     • 绿色（G=255, R=B=0）：视觉不变（G 字节位置两种格式相同）
 *     • 红色（R=255, G=B=0）：显示为蓝色（R 字节写到了 BGR 的蓝色位置）
 *     • 蓝色（B=255, G=R=0）：显示为红色
 *     • 白色（R=G=B=255）：视觉不变（三分量全满，顺序无所谓）
 *   这正是为何 main.cpp 中必须通过 switch(pixel_format) 选择正确子类。
 *
 * ── PixelBlueGreenRedReserved8BitPerColor 名称含义 ────────────────────────
 *
 *   BlueGreenRed = 字节排列顺序（B 在低地址，R 在高地址）
 *   其余部分与 RGB 格式名称含义相同。
 */
void BGRResv8BitPerColorPixelWriter::Write(int x, int y, const PixelColor& c) {
  auto p = PixelAt(x, y);  /* p 指向帧缓冲区中 (x,y) 像素的第一个字节 */
  p[0] = c.b;              /* 低地址：蓝色分量（BGR 格式蓝在前） */
  p[1] = c.g;              /* 中地址：绿色分量（与 RGB 格式相同） */
  p[2] = c.r;              /* 高地址：红色分量（BGR 格式红在后） */
  /* p[3]（保留字节）不修改 */
}