/**
 * frame_buffer_config.hpp - 引导程序与内核之间共享的帧缓冲区配置接口
 *
 * ── 设计背景 ────────────────────────────────────────────────────────────────
 *
 * UEFI 引导程序通过 GOP（EFI_GRAPHICS_OUTPUT_PROTOCOL）从固件获取显示参数，
 * 但 UEFI 数据结构（EFI_GRAPHICS_OUTPUT_MODE_INFORMATION 等）定义在 EDK II
 * 头文件中，内核不应也无法依赖这些头文件（内核是独立的裸机程序，不链接 UEFI 库）。
 *
 * 解决方案：
 *   在引导程序与内核之间定义一个最小化的"共享数据结构"头文件，
 *   引导程序负责将 UEFI 的 GOP 信息转换并填入此结构体，
 *   内核通过函数参数接收该结构体指针，从启动的第一刻起即可访问显示输出。
 *
 * ── 文件使用方式 ────────────────────────────────────────────────────────────
 *
 *   引导程序侧（MikanLoaderPkg/Main.c）：
 *     #include "frame_buffer_config.hpp"
 *     struct FrameBufferConfig config = { ... };   // 从 GOP 填充
 *     entry_point(&config);                        // 传给内核入口点
 *
 *   内核侧（kernel/main.cpp）：
 *     #include "frame_buffer_config.hpp"
 *     extern "C" void KernelMain(const FrameBufferConfig& config) { ... }
 *
 * ── 跨语言兼容性 ────────────────────────────────────────────────────────────
 *
 *   本文件同时被 C 代码（引导程序，.c 文件）和 C++ 代码（内核，.cpp 文件）包含。
 *   因此必须遵守 C/C++ 公共子集的限制：
 *     ✓ 使用 enum（不使用 enum class，C 不支持）
 *     ✓ 使用 struct FrameBufferConfig（C++ 中 struct 名自动成为类型名，C 中需加 struct 关键字）
 *     ✓ 使用 <stdint.h>（而非 <cstdint>，后者是 C++ 专属头文件）
 *     ✗ 不使用命名空间、模板、构造函数、默认参数等 C++ 专有特性
 */
 
/**
 * #pragma once - 头文件包含保护
 *
 * 等价于传统的 #ifndef / #define / #endif 保护宏，但更简洁，由编译器直接支持。
 * 防止同一编译单元中多次包含本头文件导致的重定义错误（重复 enum/struct 定义）。
 * GCC、Clang、MSVC 均支持此指令，是现代 C/C++ 项目的主流做法。
 */
#pragma once
 
/**
 * <stdint.h> - 固定宽度整数类型
 *
 * 提供 uint8_t、uint32_t、uint64_t 等类型，确保整数位宽在所有平台上一致。
 *
 * 为何不直接使用 int、long 等内置类型？
 *   在 x86-64 Linux 上 long 是 64 位，但在 Windows/MSVC 上 long 是 32 位；
 *   内核与引导程序对结构体字段大小的理解必须完全一致，否则字段偏移错位
 *   会导致内核读取到错误的帧缓冲区地址或分辨率，产生难以定位的显示异常。
 *   使用 uint32_t 明确声明"此字段恰好 32 位"，消除平台差异。
 *
 * 选用 <stdint.h> 而非 <cstdint> 的原因：
 *   <cstdint> 是 C++ 标准库头文件，将类型名放入 std:: 命名空间并提供
 *   全局别名（uint32_t 等）；在裸机内核的 freestanding 环境中，
 *   C++ 标准库可能不完整，而 <stdint.h> 作为 C 语言标准头文件，
 *   仅依赖编译器内置定义（__INT8_TYPE__ 等），在 freestanding 环境中
 *   始终可用。此外，C 源文件（引导程序）只能包含 <stdint.h>。
 */
#include <stdint.h>
 
/**
 * PixelFormat - 帧缓冲区的像素颜色分量排列格式
 *
 * 帧缓冲区的每个像素通常占 32 位（4 字节），其中 3 字节分别存储
 * R（红）、G（绿）、B（蓝）分量，第 4 字节为保留位（显示控制器忽略）。
 * 关键问题在于：R、G、B 三个分量在内存中的字节顺序因显示控制器而异。
 *
 * 内核在写入像素时必须知道正确的字节顺序，否则颜色显示会错误
 * （例如将 RGB 格式的数据写入 BGR 格式的帧缓冲区，红色会显示为蓝色）。
 *
 * 此枚举由引导程序从 UEFI GOP 的 EFI_GRAPHICS_PIXEL_FORMAT 转换而来：
 *   EFI: PixelRedGreenBlueReserved8BitPerColor → kPixelRGBResv8BitPerColor
 *   EFI: PixelBlueGreenRedReserved8BitPerColor → kPixelBGRResv8BitPerColor
 *
 * 为何不直接复用 EFI_GRAPHICS_PIXEL_FORMAT？
 *   ① 内核不依赖 UEFI 头文件（彻底解耦）
 *   ② EFI 枚举包含内核无法处理的格式（PixelBitMask、PixelBltOnly），
 *      使用独立枚举可在编译期约束合法输入范围
 *   ③ 两套枚举数值当前碰巧一致（均为 0/1），但通过引导程序的显式 switch
 *      映射，未来任意一方调整枚举顺序都不会影响另一方
 */
enum PixelFormat {
  /**
   * kPixelRGBResv8BitPerColor - 红绿蓝保留，每分量 8 位
   *
   * 内存中每像素 4 字节的排列（低地址 → 高地址）：
   *   byte 0: Red   (0x00~0xFF)
   *   byte 1: Green (0x00~0xFF)
   *   byte 2: Blue  (0x00~0xFF)
   *   byte 3: Reserved（保留，写入任何值均可，显示控制器忽略此字节）
   *
   * 在小端（little-endian）x86-64 系统中，将一个像素以 uint32_t 表示时：
   *   uint32_t pixel = (Blue << 16) | (Green << 8) | Red;
   *   // 低字节 = Red，高字节序为 Blue → 符合 RGB 内存排列
   *
   * 示例（写入纯红色像素到地址 p）：
   *   p[0] = 0xFF;  // Red   = 255
   *   p[1] = 0x00;  // Green = 0
   *   p[2] = 0x00;  // Blue  = 0
   *   p[3] = 0x00;  // Reserved
   *
   * 这是 UEFI GOP 中最常见的格式，大多数现代 PC 显卡默认使用此格式。
   */
  kPixelRGBResv8BitPerColor,
 
  /**
   * kPixelBGRResv8BitPerColor - 蓝绿红保留，每分量 8 位
   *
   * 内存中每像素 4 字节的排列（低地址 → 高地址）：
   *   byte 0: Blue  (0x00~0xFF)
   *   byte 1: Green (0x00~0xFF)
   *   byte 2: Red   (0x00~0xFF)
   *   byte 3: Reserved（保留，显示控制器忽略）
   *
   * 在小端 x86-64 系统中，以 uint32_t 表示时：
   *   uint32_t pixel = (Red << 16) | (Green << 8) | Blue;
   *   // 低字节 = Blue，高字节序为 Red → 符合 BGR 内存排列
   *
   * 示例（写入纯红色像素到地址 p）：
   *   p[0] = 0x00;  // Blue  = 0
   *   p[1] = 0x00;  // Green = 0
   *   p[2] = 0xFF;  // Red   = 255
   *   p[3] = 0x00;  // Reserved
   *
   * 注意：BGR 与 RGB 的 R 和 B 字节位置完全互换。
   * 若将 RGB 格式的像素数据写入 BGR 帧缓冲区，红色像素会显示为蓝色，
   * 蓝色像素会显示为红色——这是帧缓冲区编程中最常见的颜色错误来源。
   *
   * 此格式等同于 Windows GDI 中的 BGRX32 / DIB_BGR_COLORS，
   * 以及 Linux framebuffer 驱动中常见的 FB_VISUAL_TRUECOLOR BGR 模式。
   */
  kPixelBGRResv8BitPerColor,
};
 
/**
 * FrameBufferConfig - 帧缓冲区的完整配置参数集
 *
 * 此结构体是引导程序向内核传递显示信息的唯一渠道，封装了内核
 * 正确操作帧缓冲区所需的全部参数。
 *
 * 数据流：
 *   UEFI GOP（固件） → 引导程序（Main.c）填充此结构体 → 内核（KernelMain）接收并使用
 *
 * 内存布局（x86-64，均为 4 字节或 8 字节对齐）：
 *   偏移  0 (8B): frame_buffer          ← 指针，64 位地址
 *   偏移  8 (4B): pixels_per_scan_line  ← uint32_t
 *   偏移 12 (4B): horizontal_resolution ← uint32_t
 *   偏移 16 (4B): vertical_resolution   ← uint32_t
 *   偏移 20 (4B): pixel_format          ← enum（底层为 int，4 字节）
 *   总大小：24 字节（含末尾无填充）
 *
 * 引导程序（C 代码）使用方式：
 *   struct FrameBufferConfig config = {
 *     (uint8_t*)gop->Mode->FrameBufferBase,
 *     gop->Mode->Info->PixelsPerScanLine,
 *     gop->Mode->Info->HorizontalResolution,
 *     gop->Mode->Info->VerticalResolution,
 *     kPixelRGBResv8BitPerColor   // 由 switch 映射后填入
 *   };
 *   entry_point(&config);         // 传指针给内核
 *
 * 内核（C++ 代码）使用方式：
 *   extern "C" void KernelMain(const FrameBufferConfig& config) {
 *     // config.frame_buffer 直接可用，无需再查询任何 UEFI 协议
 *   }
 */
struct FrameBufferConfig {
  /**
   * frame_buffer - 帧缓冲区的物理起始地址（以字节指针形式表示）
   *
   * 指向 MMIO（Memory-Mapped I/O）区域，直接写入此地址的数据
   * 会实时反映到显示器上（无需系统调用或驱动程序中转）。
   *
   * 地址来源：gop->Mode->FrameBufferBase（EFI_PHYSICAL_ADDRESS，即 UINT64）
   * 转换方式：(uint8_t*)gop->Mode->FrameBufferBase
   *
   * 重要特性：
   *   ① 此地址在 UEFI ExitBootServices 调用后依然有效。
   *      帧缓冲区是硬件 MMIO，不依赖 UEFI Boot Services；
   *      内核可在整个运行期间持续使用，无需重新初始化。
   *   ② 使用 uint8_t* 而非 void* 是为了方便按字节寻址像素分量。
   *      像素的第 n 行、第 m 列的起始字节地址为：
   *        frame_buffer + (n * pixels_per_scan_line + m) * 4
   *      （每像素 4 字节，即 sizeof(uint32_t)）
   *   ③ 对此内存区域的写入是非缓存的（Uncacheable 或 Write-Combining），
   *      CPU 不会对其进行缓存优化，写操作直接发往显示控制器。
   */
  uint8_t* frame_buffer;
 
  /**
   * pixels_per_scan_line - 每条水平扫描线的像素步长（含行末对齐填充）
   *
   * 这是帧缓冲区行与行之间的物理间距，单位为像素数（每像素 4 字节）。
   *
   * 关键区别：pixels_per_scan_line >= horizontal_resolution
   *
   * 为何两者可能不相等？
   *   显示控制器出于性能优化（内存访问对齐、缓存行对齐等），
   *   可能在每行有效像素之后追加若干填充像素（不显示在屏幕上）。
   *   例如：水平分辨率为 1920，但 pixels_per_scan_line 可能为 2048
   *   （对齐到 2 的幂），即每行末尾有 128 个填充像素。
   *
   * 错误用法（使用 horizontal_resolution 计算行首偏移）：
   *   // 错误：在有行末填充的显示器上，画面会向左或向右倾斜（shear 效果）
   *   uint8_t* row = frame_buffer + y * horizontal_resolution * 4;
   *
   * 正确用法（使用 pixels_per_scan_line 计算行首偏移）：
   *   // 正确：始终使用此字段跨行寻址
   *   uint8_t* row = frame_buffer + y * pixels_per_scan_line * 4;
   *   uint8_t* pixel = row + x * 4;  // 列偏移仍用 x（无列方向填充）
   *
   * 地址来源：gop->Mode->Info->PixelsPerScanLine（UINT32）
   */
  uint32_t pixels_per_scan_line;
 
  /**
   * horizontal_resolution - 屏幕水平方向的有效像素数（显示宽度）
   *
   * 屏幕上实际可见的每行像素数，即逻辑宽度（不含行末填充）。
   * 用途：
   *   ① 绘图边界检查（x 坐标必须满足 0 <= x < horizontal_resolution）
   *   ② 计算屏幕中心（horizontal_resolution / 2）
   *   ③ 传递给图形子系统作为"画布宽度"
   *
   * 注意：行首地址计算不使用此字段，必须使用 pixels_per_scan_line（见上）。
   *
   * 地址来源：gop->Mode->Info->HorizontalResolution（UINT32）
   * 典型值：1920（1080p）、2560（1440p）、3840（4K）
   */
  uint32_t horizontal_resolution;
 
  /**
   * vertical_resolution - 屏幕垂直方向的有效像素数（显示高度）
   *
   * 屏幕上实际可见的总行数，即逻辑高度。
   * 用途：
   *   ① 绘图边界检查（y 坐标必须满足 0 <= y < vertical_resolution）
   *   ② 计算屏幕中心（vertical_resolution / 2）
   *   ③ 帧缓冲区总像素数 = pixels_per_scan_line * vertical_resolution
   *      （注意：用 pixels_per_scan_line 而非 horizontal_resolution）
   *
   * 地址来源：gop->Mode->Info->VerticalResolution（UINT32）
   * 典型值：1080（1080p）、1440（1440p）、2160（4K）
   */
  uint32_t vertical_resolution;
 
  /**
   * pixel_format - 帧缓冲区的像素颜色分量排列格式
   *
   * 指定每个像素中 R、G、B 三个颜色分量在内存中的字节顺序，
   * 内核在写入像素时必须根据此字段决定字节排列方式。
   *
   * 赋值流程（引导程序中的 switch 语句）：
   *   EFI PixelRedGreenBlueReserved8BitPerColor → kPixelRGBResv8BitPerColor
   *   EFI PixelBlueGreenRedReserved8BitPerColor → kPixelBGRResv8BitPerColor
   *   其他格式（PixelBitMask / PixelBltOnly）  → 引导程序调用 Halt() 停机
   *
   * 内核使用示例（写入颜色 (r, g, b) 到像素 (x, y)）：
   *   uint8_t* p = config.frame_buffer
   *              + (y * config.pixels_per_scan_line + x) * 4;
   *   if (config.pixel_format == kPixelRGBResv8BitPerColor) {
   *     p[0] = r; p[1] = g; p[2] = b;
   *   } else if (config.pixel_format == kPixelBGRResv8BitPerColor) {
   *     p[0] = b; p[1] = g; p[2] = r;
   *   }
   *   p[3] = 0;  // Reserved 字节，值任意
   *
   * 底层存储：enum 在 C/C++ 中默认以 int（通常 4 字节）存储，
   * 与前三个 uint32_t 字段大小一致，结构体末尾无需额外填充。
   */
  enum PixelFormat pixel_format;
};
 