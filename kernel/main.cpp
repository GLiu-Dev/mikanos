/**
 * main.cpp - MikanOS 内核入口与早期图形输出
 *
 * ── 本文件的职责 ────────────────────────────────────────────────────────────
 *
 * 本文件是内核的起点，包含两个核心部分：
 *
 *   1. WritePixel：像素级绘图原语
 *      将一个 RGB 颜色值写入帧缓冲区的指定坐标，并处理 RGB/BGR 两种字节顺序。
 *      这是内核图形子系统的最底层函数，所有更高级的绘图操作都建立在它之上。
 *
 *   2. KernelMain：内核 C++ 入口点
 *      由引导程序（MikanLoaderPkg/Main.c）在退出 UEFI Boot Services 后直接跳转。
 *      当前阶段完成两件事：
 *        ① 全屏填白（清除引导程序的白屏，改为由内核主动绘制）
 *        ② 在坐标 (100, 100) 处绘制一个 200×100 的绿色矩形，验证像素写入正确
 *      绘制完成后进入低功耗 HLT 循环。
 *
 * ── 与前一版本的变化 ────────────────────────────────────────────────────────
 *
 *   旧版 KernelMain(uint64_t base, uint64_t size)：
 *     仅接收帧缓冲区物理地址和大小两个裸整数，
 *     只能逐字节写入渐变色（i % 256），无法指定坐标或颜色。
 *
 *   本版 KernelMain(const FrameBufferConfig& config)：
 *     接收完整的显示配置结构体，包含：
 *       frame_buffer、pixels_per_scan_line、分辨率、pixel_format
 *     由此可实现坐标寻址（WritePixel）和格式无关的颜色写入。
 *     新增 PixelColor 结构体将颜色的三个分量统一封装，避免参数顺序混淆。
 */
 
/**
 * <cstdint> - 固定宽度整数类型（C++ 版本）
 *
 * 提供 uint8_t（8位无符号整数）等类型。
 * 在裸机 freestanding 环境中，<cstdint> 仅依赖编译器内置类型定义，
 * 不需要 C++ 运行时支持，可以安全使用。
 * uint8_t 用于表示颜色分量（0~255）和帧缓冲区的字节地址。
 */
#include <cstdint>
 
/**
 * <cstddef> - 基础类型与宏（C++ 版本）
 *
 * 提供 size_t、nullptr_t、NULL、offsetof 等。
 * 本文件当前主要依赖其提供的 size_t（用于内存大小计算），
 * 以及 NULL/nullptr（尽管本文件目前未直接使用，包含备用）。
 * 同样不依赖 C++ 运行时，在 freestanding 环境可用。
 */
#include <cstddef>
 
/**
 * frame_buffer_config.hpp - 引导程序与内核的共享显示配置接口
 *
 * 定义了两个类型：
 *   enum PixelFormat        → 像素格式（kPixelRGBResv8BitPerColor / kPixelBGRResv8BitPerColor）
 *   struct FrameBufferConfig → 完整显示参数集（帧缓冲区指针、分辨率、行步长、像素格式）
 *
 * 该头文件同时被引导程序（C 代码）和内核（C++ 代码）包含，
 * 是两者之间唯一的数据契约——引导程序填充，内核读取。
 */
#include "frame_buffer_config.hpp"
 
// #@@range_begin(write_pixel)
 
/**
 * PixelColor - 像素的 RGB 颜色表示
 *
 * 将红、绿、蓝三个颜色分量封装为一个具名结构体，
 * 避免在 WritePixel 调用处出现三个裸 uint8_t 参数导致顺序混淆的问题。
 *
 * 字段范围：
 *   r（Red）   : 0（无红色）~ 255（最大红色）
 *   g（Green） : 0（无绿色）~ 255（最大绿色）
 *   b（Blue）  : 0（无蓝色）~ 255（最大蓝色）
 *
 * 常用颜色速查：
 *   {255, 255, 255} → 白色（R=G=B=最大）
 *   {  0,   0,   0} → 黑色（R=G=B=0）
 *   {255,   0,   0} → 纯红
 *   {  0, 255,   0} → 纯绿
 *   {  0,   0, 255} → 纯蓝
 *   {255, 255,   0} → 黄色（红+绿）
 *   {255,   0, 255} → 品红（红+蓝）
 *   {  0, 255, 255} → 青色（绿+蓝）
 *
 * C++ 聚合初始化语法：
 *   PixelColor c = {255, 0, 0};  // 或
 *   WritePixel(config, x, y, {255, 0, 0});  // 直接传临时对象（C++11 起）
 *   字段按声明顺序 r, g, b 依次对应。
 *
 * 内存占用：3 字节（编译器可能补齐为 4 字节，但作为函数参数时不影响语义）
 */
struct PixelColor {
  uint8_t r, g, b;
};
 
/**
 * WritePixel - 向帧缓冲区的指定坐标写入一个像素颜色
 *
 * 这是内核图形子系统的最底层原语。所有更高级的绘图操作
 * （画线、填充矩形、渲染字符等）最终都归结为对此函数的调用。
 *
 * 坐标系：
 *   原点 (0, 0) 位于屏幕左上角。
 *   x 轴向右增大（范围：0 ~ horizontal_resolution - 1）。
 *   y 轴向下增大（范围：0 ~ vertical_resolution - 1）。
 *
 * 地址计算：
 *   像素 (x, y) 的帧缓冲区字节偏移：
 *     offset = (y * pixels_per_scan_line + x) * 4
 *
 *   为什么乘以 pixels_per_scan_line 而非 horizontal_resolution？
 *     hardware 可能在每行末尾追加对齐填充像素，
 *     pixels_per_scan_line 包含了填充，是行与行之间的真实物理步长。
 *     使用 horizontal_resolution 会在有行末填充的显卡上导致画面斜切（shear）。
 *
 *   每像素 4 字节：3 字节颜色分量（R/G/B）+ 1 字节保留（显示控制器忽略）。
 *
 * 像素格式处理：
 *   RGB 格式（kPixelRGBResv8BitPerColor）：
 *     p[0]=R, p[1]=G, p[2]=B, p[3]=忽略
 *   BGR 格式（kPixelBGRResv8BitPerColor）：
 *     p[0]=B, p[1]=G, p[2]=R, p[3]=忽略
 *   两种格式 G 分量位置相同（byte 1），仅 R 和 B 互换。
 *
 * 参数：
 *   config  帧缓冲区配置（由引导程序从 UEFI GOP 填充并传入 KernelMain）
 *   x       像素列坐标（屏幕水平位置，0 为最左列）
 *   y       像素行坐标（屏幕垂直位置，0 为最顶行）
 *   c       要写入的 RGB 颜色值
 *
 * 返回值：
 *    0  写入成功
 *   -1  像素格式不支持（既非 RGB 也非 BGR，理论上引导程序已过滤此情况）
 *
 * 注意：本函数不做坐标越界检查（x >= horizontal_resolution 或 y >= vertical_resolution），
 * 调用者有责任确保坐标在有效范围内，否则会写入帧缓冲区边界之外，
 * 产生未定义行为（可能破坏相邻内存或触发硬件保护）。
 */
int WritePixel(const FrameBufferConfig& config,
               int x, int y, const PixelColor& c) {
 
  // 计算目标像素相对于帧缓冲区起点的像素下标（以像素为单位，非字节）。
  // 公式：行首像素下标 = y * pixels_per_scan_line（含行末对齐填充）
  //       列偏移      = + x
  // pixel_position 即该像素在"像素数组"中的一维索引，
  // 乘以 4 后得到字节偏移（每像素 4 字节）。
  const int pixel_position = config.pixels_per_scan_line * y + x;
 
  if (config.pixel_format == kPixelRGBResv8BitPerColor) {
    // RGB 格式：内存低地址 → 高地址排列为 [R][G][B][Reserved]
    // 取该像素起始字节的地址：帧缓冲区起点 + 字节偏移（pixel_position * 4）
    uint8_t* p = &config.frame_buffer[4 * pixel_position];
    p[0] = c.r;  // byte 0 = Red   分量
    p[1] = c.g;  // byte 1 = Green 分量
    p[2] = c.b;  // byte 2 = Blue  分量
    // p[3]（Reserved）不写入，保持原值；显示控制器忽略该字节
 
  } else if (config.pixel_format == kPixelBGRResv8BitPerColor) {
    // BGR 格式：内存低地址 → 高地址排列为 [B][G][R][Reserved]
    // 与 RGB 格式相比，R 和 B 的字节位置完全互换；G 位置不变（byte 1）。
    // 若在此分支误用 RGB 的写法（p[0]=r），颜色会出现红蓝对调的错误。
    uint8_t* p = &config.frame_buffer[4 * pixel_position];
    p[0] = c.b;  // byte 0 = Blue  分量（注意：与 RGB 格式 byte 0=Red 不同）
    p[1] = c.g;  // byte 1 = Green 分量（两种格式相同）
    p[2] = c.r;  // byte 2 = Red   分量（注意：与 RGB 格式 byte 2=Blue 不同）
    // p[3]（Reserved）不写入
 
  } else {
    // 不支持的像素格式（PixelBitMask、PixelBltOnly 等）。
    // 正常情况下，引导程序在填充 FrameBufferConfig 时已通过 switch 语句
    // 过滤掉不支持的格式（并调用 Halt() 停机），此分支理论上不可达。
    // 返回 -1 而非直接 Halt()，保留了调用者自行处理错误的灵活性。
    return -1;
  }
  return 0;
}
// #@@range_end(write_pixel)
 
// #@@range_begin(call_write_pixel)
 
/**
 * KernelMain - 内核 C++ 入口函数
 *
 * ── 调用约定与链接 ───────────────────────────────────────────────────────────
 *
 * extern "C"：
 *   关闭 C++ 的名称修饰（name mangling）。若无此声明，编译器会将函数符号
 *   编码为 _ZN10KernelMainERK18FrameBufferConfig 之类的修饰名，
 *   引导程序从 ELF e_entry 字段读取到的入口地址无法对应到此函数。
 *   extern "C" 保证符号名保持为 KernelMain，与链接脚本和引导程序的期望一致。
 *
 * 参数传递（System V AMD64 ABI）：
 *   引导程序调用：entry_point(&config)
 *   C++ 参数声明：const FrameBufferConfig& frame_buffer_config
 *   C++ 引用（&）与 C 指针（*）在调用约定上等价：
 *   第一个参数通过 RDI 寄存器传递帧缓冲区配置结构体的地址。
 *   const 保证内核不会意外修改引导程序传入的配置数据。
 *
 * ── 当前功能 ─────────────────────────────────────────────────────────────────
 *
 * 步骤 1：全屏填白
 *   用白色（{255, 255, 255}）覆盖整个帧缓冲区，给内核一个干净的初始画面。
 *   虽然引导程序已经做过全屏清白（每字节写 255），但引导程序是逐字节写入，
 *   包含了行末对齐填充区域；本步骤通过 WritePixel 按坐标写入，
 *   只覆盖逻辑可见区域（horizontal × vertical），更语义化，
 *   也为后续统一走 WritePixel 路径建立习惯。
 *
 * 步骤 2：绘制绿色矩形
 *   在屏幕坐标 (100, 100) 到 (299, 199) 的区域（宽 200 × 高 100 像素）
 *   填充纯绿色（{0, 255, 0}），用于视觉验证：
 *     ① WritePixel 函数工作正常（坐标寻址、颜色写入）
 *     ② 像素格式检测正确（RGB/BGR 均应显示为绿色，因为 G 分量在两种格式中位置相同）
 *     ③ 内核与引导程序的 ABI 对接无误（FrameBufferConfig 字段正确传递）
 *   若矩形位置或大小异常（如画面斜切、颜色错误），可从此处开始排查。
 *
 * 步骤 3：进入低功耗 HLT 休眠循环（永不返回）
 *   当前内核没有更多任务可执行，通过 HLT 指令让 CPU 停止取指等待中断，
 *   避免 CPU 空转（100% 占用率）浪费电力/产生热量。
 *   外层 while(1) 确保 HLT 被中断唤醒后立即再次执行 HLT，
 *   因为当前尚未设置 IDT（中断描述符表），任何中断都会触发 Triple Fault 复位。
 *
 * 参数：
 *   frame_buffer_config  由引导程序从 UEFI GOP 填充的完整显示配置
 *                        （帧缓冲区地址、行步长、分辨率、像素格式）
 */
extern "C" void KernelMain(const FrameBufferConfig& frame_buffer_config) {
 
  /* ── 步骤 1：全屏填白 ─────────────────────────────────────────────────── */
 
  // 遍历所有可见像素坐标（x: 0~水平分辨率-1，y: 0~垂直分辨率-1），
  // 用白色覆盖每个像素。外层循环遍历列（x），内层遍历行（y）。
  //
  // 循环顺序说明（先 x 后 y 与先 y 后 x 的选择）：
  //   对内存访问模式而言，按行扫描（先固定 y 再遍历 x）更利于缓存：
  //   同一行的相邻像素在内存中连续，符合空间局部性原则。
  //   当前代码是先固定 x 再遍历 y（按列扫描），内存访问跨度大，
  //   缓存命中率较低，但在早期开发阶段功能正确性优先于性能。
  for (int x = 0; x < frame_buffer_config.horizontal_resolution; ++x) {
    for (int y = 0; y < frame_buffer_config.vertical_resolution; ++y) {
      // {255, 255, 255} 是 PixelColor 的聚合初始化，依次对应 r=255, g=255, b=255
      // R=G=B=255 在 RGB 和 BGR 两种格式下均表示白色（G 分量位置相同，R/B 均最大）
      WritePixel(frame_buffer_config, x, y, {255, 255, 255});
    }
  }
 
  /* ── 步骤 2：绘制绿色矩形（验证像素级绘图能力）────────────────────────── */
 
  // 在屏幕上绘制一个 200 像素宽、100 像素高的绿色实心矩形。
  // 矩形左上角坐标：(100, 100)
  // 矩形右下角坐标：(299, 199)（不含边界：x < 100+200=300，y < 100+100=200）
  //
  // 坐标分析：
  //   x 范围：[100, 299]（100 + 0 到 100 + 199）
  //   y 范围：[100, 199]（100 + 0 到 100 +  99）
  //
  // 选择绿色 {0, 255, 0} 的原因：
  //   Green 分量（g）在 RGB 和 BGR 两种像素格式中均位于 byte 1，
  //   因此无论显卡实际使用哪种格式，纯绿色都能正确显示。
  //   若此处选用红色 {255, 0, 0}，而格式判断有误（如将 BGR 误判为 RGB），
  //   则显示结果将是蓝色而非红色，无法第一时间判断是格式错误还是坐标错误。
  //   绿色矩形因此是最安全的早期测试用例。
  for (int x = 0; x < 200; ++x) {
    for (int y = 0; y < 100; ++y) {
      // 100 + x 和 100 + y 将矩形相对坐标平移到屏幕 (100, 100) 位置
      WritePixel(frame_buffer_config, 100 + x, 100 + y, {0, 255, 0});
    }
  }
 
  /* ── 步骤 3：低功耗 HLT 休眠循环（永不返回）────────────────────────────── */
 
  // HLT 指令（x86 机器码 0xF4）：
  //   使当前逻辑 CPU 停止取指，进入低功耗等待状态，
  //   直到收到外部中断（IRQ/NMI）或 RESET 信号后才唤醒继续执行。
  //
  // 为何用 HLT 而非空的 while(1) {} 忙等待？
  //   忙等待（Busy Loop）会让 CPU 持续执行空指令，占用 100% 计算资源，
  //   在 QEMU/KVM 上大量消耗宿主机 CPU，产生不必要的热量和功耗。
  //   HLT 将 CPU 时间片归还给调度器（在虚拟机场景下归还给宿主机），
  //   是更节能的等待方式。
  //
  // 外层 while(1) 的必要性：
  //   如果 HLT 被中断信号唤醒，CPU 会从 HLT 的下一条指令继续执行。
  //   若没有外层循环，CPU 会从 while 退出，执行不存在的后续代码，
  //   滑入随机内存区域引发崩溃。while(1) 保证每次唤醒后立即再次 HLT。
  //
  // 当前阶段的实际行为：
  //   内核尚未设置 IDT（Interrupt Descriptor Table，中断描述符表），
  //   任何中断（包括定时器、键盘等）到来时，CPU 会因缺少中断处理程序
  //   而连续触发 #GP（通用保护异常）→ #DF（双重故障）→ Triple Fault（三重故障），
  //   最终导致 CPU 复位（在 QEMU 中表现为虚拟机重启）。
  //   因此当前的 HLT 循环实际上是"永久停机"，除非被外部 RESET 打断。
  //   未来添加 IDT 和中断处理程序后，HLT 才会成为真正的低功耗等待机制。
  while (1) __asm__("hlt");
}
// #@@range_end(call_write_pixel)