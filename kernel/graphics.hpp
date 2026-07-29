/**
 * @file graphics.hpp
 *
 * 画像描画に関するプログラムをまとめたヘッダファイル.
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本头文件声明像素级渲染所需的全部类型和类：
 *     ① PixelColor                       — 表示 RGB 颜色的 3 字节 POD 结构体
 *     ② PixelWriter（抽象基类）           — 定义写像素接口 Write() 及辅助方法 PixelAt()
 *     ③ RGBResv8BitPerColorPixelWriter   — RGB 字节序写入器（r→p[0], g→p[1], b→p[2]）
 *     ④ BGRResv8BitPerColorPixelWriter   — BGR 字节序写入器（b→p[0], g→p[1], r→p[2]）
 *
 * ── 为何需要两个写入器 ───────────────────────────────────────────────────────
 *
 *   UEFI GOP（Graphics Output Protocol）支持不同的像素字节序：
 *   - PixelRedGreenBlueReserved8BitPerColor：帧缓冲每像素 4 字节，顺序 [R][G][B][_]
 *   - PixelBlueGreenRedReserved8BitPerColor：顺序 [B][G][R][_]
 *   实际字节序由固件/硬件决定，运行时通过 gop->Mode->Info->PixelFormat 查询。
 *   两个子类实现各自正确的字节写入顺序，避免颜色错乱（红蓝互换）。
 *
 * ── 设计模式：面向对象多态 ───────────────────────────────────────────────────
 *
 *   PixelWriter* pixel_writer（全局指针）在 KernelMain 中以 placement new 初始化，
 *   指向栈上缓冲区中实际构造的 RGB 或 BGR 对象。
 *   所有渲染路径通过 pixel_writer->Write(x, y, c) 调用，
 *   vtable 在运行时分派到正确的子类实现，无需 if/switch 散布于渲染代码。
 *
 * ── 依赖关系 ─────────────────────────────────────────────────────────────────
 *
 *   graphics.hpp → frame_buffer_config.hpp（FrameBufferConfig 结构体）
 *   PixelWriter 构造函数接受 FrameBufferConfig& 引用，存储帧缓冲区信息。
 */
 
#pragma once
 
// #@@range_begin(pixel_color_def)
#include "frame_buffer_config.hpp"  // FrameBufferConfig（帧缓冲区配置），包含 uint8_t 等
 
/**
 * PixelColor — 表示一个像素颜色的 3 字节 POD 结构体
 *
 * 字段：
 *   r (uint8_t)：红色分量，0（无红）~ 255（全红）
 *   g (uint8_t)：绿色分量
 *   b (uint8_t)：蓝色分量
 *
 * 无 Alpha 通道：帧缓冲区每像素 4 字节，第 4 字节为保留位（ReservedBit），固件忽略。
 *
 * POD（Plain Old Data）：无构造函数、无析构函数，可用聚合初始化：
 *   PixelColor c = {255, 0, 0};   // 红色
 *   PixelColor c{0, 255, 0};      // 绿色（C++11 统一初始化）
 *   writer.Write(x, y, {0, 0, 255}); // 蓝色（隐式聚合构造）
 *
 * 字段按 r, g, b 顺序声明，但写入帧缓冲区时的内存字节序由 PixelWriter 子类决定：
 *   RGBResv8BitPerColorPixelWriter：按 r, g, b 顺序写入
 *   BGRResv8BitPerColorPixelWriter：按 b, g, r 顺序写入
 */
struct PixelColor {
  uint8_t r, g, b;
};
// #@@range_end(pixel_color_def)
 
/**
 * PixelWriter — 像素写入器抽象基类
 *
 * 定义向帧缓冲区写入单个像素的统一接口。
 * 通过虚函数（virtual）实现多态：
 *   基类指针 pixel_writer->Write(x, y, c) 在运行时分派到子类的 Write() 实现。
 *
 * ── 构造函数 ─────────────────────────────────────────────────────────────────
 *
 *   PixelWriter(const FrameBufferConfig& config)：
 *     接受 FrameBufferConfig 引用，存储到私有成员 config_。
 *     子类通过 using PixelWriter::PixelWriter（继承构造函数）复用此构造函数。
 *
 * ── 虚析构函数 ───────────────────────────────────────────────────────────────
 *
 *   virtual ~PixelWriter() = default：
 *     声明虚析构函数（即使本类不需要清理资源），
 *     确保通过基类指针 delete 时正确调用子类析构函数（C++ 多态规范）。
 *     = default：使用编译器生成的默认实现（空体，无需手动编写）。
 *
 * ── 纯虚函数 Write() ─────────────────────────────────────────────────────────
 *
 *   virtual void Write(int x, int y, const PixelColor& c) = 0：
 *     = 0：纯虚函数，PixelWriter 本身不可实例化（抽象类）。
 *     每个子类必须提供 Write() 实现，否则子类也是抽象类（无法实例化）。
 *     参数：
 *       x, y   — 像素坐标（0,0 为左上角）
 *       c      — 像素颜色（PixelColor{r, g, b}）
 *
 * ── 受保护辅助函数 PixelAt() ─────────────────────────────────────────────────
 *
 *   uint8_t* PixelAt(int x, int y)：
 *     计算帧缓冲区中 (x, y) 像素的字节地址。
 *     地址 = frame_buffer + 4 * (pixels_per_scan_line * y + x)
 *     - pixels_per_scan_line：每行总像素数（含行末填充，可能 > horizontal_resolution）
 *       使用 pixels_per_scan_line 而非 horizontal_resolution，避免行地址偏移错误。
 *     - 4：每像素 4 字节（R/G/B/Reserved 各 1 字节）
 *     protected：仅供 Write() 的子类实现调用，外部不可直接访问。
 */
class PixelWriter {
 public:
  PixelWriter(const FrameBufferConfig& config) : config_{config} {
  }
  virtual ~PixelWriter() = default;
  virtual void Write(int x, int y, const PixelColor& c) = 0;
 
 protected:
  uint8_t* PixelAt(int x, int y) {
    return config_.frame_buffer + 4 * (config_.pixels_per_scan_line * y + x);
  }
 
 private:
  /**
   * config_ — 帧缓冲区配置引用（私有，仅 PixelAt() 访问）
   * 存储 FrameBufferConfig 值（非引用）：PixelWriter 的生命周期内需要始终有效的配置副本。
   * 实际上以 const FrameBufferConfig& 形式存储（引用到外部 FrameBufferConfig 对象）。
   */
  const FrameBufferConfig& config_;
};
 
// #@@range_begin(pixel_writer_def)
/**
 * RGBResv8BitPerColorPixelWriter — RGB 字节序像素写入器
 *
 * 对应 UEFI PixelFormat：PixelRedGreenBlueReserved8BitPerColor
 * 帧缓冲内存布局（每像素 4 字节）：[p+0]=R, [p+1]=G, [p+2]=B, [p+3]=Reserved
 *
 * using PixelWriter::PixelWriter：
 *   继承基类构造函数。C++11 特性，等价于：
 *     RGBResv8BitPerColorPixelWriter(const FrameBufferConfig& c) : PixelWriter(c) {}
 *   避免子类重复声明构造函数。
 *
 * virtual void Write(...) override：
 *   override：编译器检查签名与基类虚函数完全匹配（防止拼写错误导致隐藏而非覆盖）。
 *   Write() 的实现在 graphics.cpp（ODR 要求实现仅出现一次）。
 */
class RGBResv8BitPerColorPixelWriter : public PixelWriter {
 public:
  using PixelWriter::PixelWriter;
  virtual void Write(int x, int y, const PixelColor& c) override;
};
 
/**
 * BGRResv8BitPerColorPixelWriter — BGR 字节序像素写入器
 *
 * 对应 UEFI PixelFormat：PixelBlueGreenRedReserved8BitPerColor
 * 帧缓冲内存布局（每像素 4 字节）：[p+0]=B, [p+1]=G, [p+2]=R, [p+3]=Reserved
 *
 * 结构与 RGBResv8BitPerColorPixelWriter 完全相同，Write() 内部 r/b 写入位置对调。
 * 使用 QEMU 虚拟机时通常为此格式（-vga std 默认 BGR）。
 */
class BGRResv8BitPerColorPixelWriter : public PixelWriter {
 public:
  using PixelWriter::PixelWriter;
  virtual void Write(int x, int y, const PixelColor& c) override;
};
// #@@range_end(pixel_writer_def)
 