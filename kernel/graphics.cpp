/**
 * @file graphics.cpp
 *
 * 画像描画関連のプログラムを集めたファイル．
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本文件实现两个像素写入器子类的 Write() 虚函数：
 *     ① RGBResv8BitPerColorPixelWriter::Write — RGB 字节序（r→p[0], g→p[1], b→p[2]）
 *     ② BGRResv8BitPerColorPixelWriter::Write — BGR 字节序（b→p[0], g→p[1], r→p[2]）
 *
 * ── 为何实现放在 .cpp 而非 .hpp ──────────────────────────────────────────────
 *
 *   One Definition Rule (ODR)：函数定义在整个程序中只能出现一次。
 *   若 Write() 实现写在 graphics.hpp，每个包含该头文件的 .cpp 编译单元各有一份定义，
 *   链接时报"多重定义"错误。将实现移到 graphics.cpp 确保只有一份定义。
 *
 * ── 帧缓冲区内存布局 ─────────────────────────────────────────────────────────
 *
 *   UEFI GOP 帧缓冲区：线性字节数组，每像素 4 字节（32 位颜色）。
 *   PixelAt(x, y) 计算 (x, y) 处像素的起始字节地址：
 *     frame_buffer + 4 * (pixels_per_scan_line * y + x)
 *     - pixels_per_scan_line：每行总像素数（含行末硬件填充，通常 >= horizontal_resolution）
 *     - 4 字节/像素：[byte0][byte1][byte2][byte3]（byte3 = Reserved，固件忽略）
 *
 * ── 两种字节序的必要性 ───────────────────────────────────────────────────────
 *
 *   RGB 格式：物理内存 [R][G][B][_]，c.r 写到 p[0]，c.b 写到 p[2]
 *   BGR 格式：物理内存 [B][G][R][_]，c.b 写到 p[0]，c.r 写到 p[2]
 *   若格式选错，红蓝颜色互换（如本应显示红色的像素显示为蓝色）。
 *   引导加载程序通过 gop->Mode->Info->PixelFormat 查询实际格式，
 *   KernelMain 据此选择对应写入器。
 */
 
// #@@range_begin(pixel_writer_impl)
#include "graphics.hpp"
 
/**
 * RGBResv8BitPerColorPixelWriter::Write — RGB 字节序像素写入
 *
 * @param x, y   目标像素的坐标（左上角为原点）
 * @param c      要写入的像素颜色（PixelColor{r, g, b}）
 *
 * ── 实现 ─────────────────────────────────────────────────────────────────────
 *
 *   auto p = PixelAt(x, y)：
 *     调用受保护的辅助函数，返回 (x, y) 处像素的字节指针（uint8_t*）。
 *
 *   p[0] = c.r：写红色分量到 byte0（内存低字节）
 *   p[1] = c.g：写绿色分量到 byte1
 *   p[2] = c.b：写蓝色分量到 byte2
 *   p[3]（byte3 = Reserved）：不写，保持固件初始值（通常 0 或 0xFF，GPU 忽略）
 *
 * 对应 UEFI PixelFormat：PixelRedGreenBlueReserved8BitPerColor
 * 物理内存：[R][G][B][_]（_ = Reserved/保留字节）
 */
void RGBResv8BitPerColorPixelWriter::Write(int x, int y, const PixelColor& c) {
  auto p = PixelAt(x, y);
  p[0] = c.r;
  p[1] = c.g;
  p[2] = c.b;
}
// #@@range_end(pixel_writer_impl)
 
/**
 * BGRResv8BitPerColorPixelWriter::Write — BGR 字节序像素写入
 *
 * @param x, y   目标像素的坐标
 * @param c      要写入的像素颜色
 *
 * ── 实现 ─────────────────────────────────────────────────────────────────────
 *
 *   auto p = PixelAt(x, y)：同上，获取像素字节指针。
 *
 *   p[0] = c.b：写蓝色分量到 byte0（注意：与 RGB 版本对调）
 *   p[1] = c.g：写绿色分量到 byte1（与 RGB 版本相同）
 *   p[2] = c.r：写红色分量到 byte2（注意：与 RGB 版本对调）
 *
 * 对应 UEFI PixelFormat：PixelBlueGreenRedReserved8BitPerColor
 * 物理内存：[B][G][R][_]
 *
 * QEMU 默认虚拟 VGA（-vga std）通常使用此格式（BGR），
 * 实体机则因显卡固件而异。
 */
void BGRResv8BitPerColorPixelWriter::Write(int x, int y, const PixelColor& c) {
  auto p = PixelAt(x, y);
  p[0] = c.b;
  p[1] = c.g;
  p[2] = c.r;
}