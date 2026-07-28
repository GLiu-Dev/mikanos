/**
 * @file graphics.hpp
 *
 * 画像描画関連の宣言をまとめたヘッダファイル.
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本头文件是 MikanOS 图形渲染子系统的核心接口声明，定义：
 *     ① PixelColor                         — 颜色结构体（RGB 三通道各 8 位）
 *     ② PixelWriter（抽象基类）             — 像素写入器的统一接口
 *     ③ RGBResv8BitPerColorPixelWriter      — RGB 字节序具体实现（声明）
 *     ④ BGRResv8BitPerColorPixelWriter      — BGR 字节序具体实现（声明）
 *
 *   设计动机：
 *     UEFI GOP 帧缓冲区的像素字节序因硬件/固件不同而异（RGB 或 BGR）。
 *     将"写像素"抽象为虚函数，渲染代码（WriteAscii/WriteString/填充循环）
 *     无需感知字节序差异，实现"对接口编程"。
 *
 *   依赖关系：
 *     graphics.hpp → frame_buffer_config.hpp（FrameBufferConfig, PixelFormat 枚举）
 *     font.hpp     → graphics.hpp（WriteAscii/WriteString 参数类型）
 *     main.cpp     → graphics.hpp（构造写入器、调用 Write()）
 *
 * ── ODR（One Definition Rule）与头文件/源文件分工 ────────────────────────────
 *
 *   Write() 虚函数只在本头文件中"声明"，实现在 graphics.cpp。
 *   若将实现写在头文件中，多个翻译单元 #include 时会触发多重定义错误。
 *   例外：PixelAt() 是 protected 内联函数（在类定义体内实现），
 *         编译器在每个翻译单元内展开，不产生独立符号，故可留在头文件。
 */
 
#pragma once
 
// #@@range_begin(pixel_color_def)
/**
 * frame_buffer_config.hpp — 提供 FrameBufferConfig 结构体和 PixelFormat 枚举
 *   frame_buffer         : uint8_t*  — 帧缓冲区物理地址（可直接写像素）
 *   pixels_per_scan_line : uint32_t  — 每扫描行像素数（含行末填充，PixelAt 用此）
 *   horizontal_resolution: uint32_t  — 水平分辨率（遍历像素列的上界）
 *   vertical_resolution  : uint32_t  — 垂直分辨率（遍历像素行的上界）
 *   pixel_format         : enum      — kPixelRGBResv8BitPerColor 或 BGR
 */
#include "frame_buffer_config.hpp"
 
/**
 * PixelColor — 表示一个像素颜色的 POD 结构体（红、绿、蓝各 8 位）
 *
 * 字段：
 *   r（uint8_t）— 红色分量，0（无红）~ 255（全红）
 *   g（uint8_t）— 绿色分量，0（无绿）~ 255（全绿）
 *   b（uint8_t）— 蓝色分量，0（无蓝）~ 255（全蓝）
 *
 * 使用示例：
 *   {255, 255, 255} = 白色，{0, 0, 0} = 黑色，{0, 255, 0} = 纯绿，{0, 0, 255} = 纯蓝
 *
 * 注意：
 *   本结构体描述"逻辑颜色"，与帧缓冲区字节序无关。
 *   RGBPixelWriter 将 {r,g,b} 写为 [r][g][b]；
 *   BGRPixelWriter 将 {r,g,b} 写为 [b][g][r]。
 */
struct PixelColor {
  uint8_t r, g, b;
};
// #@@range_end(pixel_color_def)
 
/**
 * PixelWriter — 像素写入器抽象基类
 *
 * 通过 C++ 虚函数机制，将"向帧缓冲区写入一个像素"抽象为统一接口。
 * 子类（RGB/BGR）根据帧缓冲区字节序覆盖 Write() 方法。
 *
 * ── 对象构造方式（placement new）────────────────────────────────────────────
 *
 *   main.cpp 使用 placement new 在静态缓冲区上构造写入器：
 *     new(pixel_writer_buf) RGBResv8BitPerColorPixelWriter{frame_buffer_config}
 *   原因：ExitBootServices 后无堆分配器，不能使用标准 operator new。
 */
class PixelWriter {
 public:
  /**
   * 构造函数 — 将 FrameBufferConfig 引用绑定到 config_ 成员
   *
   * @param config  帧缓冲区配置的常量引用（由引导加载程序传入内核，生命周期贯穿整个内核运行）
   * 使用成员初始化列表避免拷贝整个结构体；const 引用保证写入器不修改配置。
   */
  PixelWriter(const FrameBufferConfig& config) : config_{config} {
  }
 
  /**
   * 虚析构函数 — 确保通过基类指针析构时正确调用子类析构函数
   *
   * = default：让编译器生成默认实现（本类无手动管理的资源）。
   * virtual  ：通过 PixelWriter* delete 时先调用子类析构（本项目用 placement new，
   *            实际不调用 delete，但声明虚析构是良好 OOP 实践）。
   */
  virtual ~PixelWriter() = default;
 
  /**
   * Write — 纯虚函数：向帧缓冲区 (x, y) 坐标写入颜色 c
   *
   * @param x  像素列号（0=最左）
   * @param y  像素行号（0=最顶）
   * @param c  要写入的逻辑颜色（字节序由子类实现决定）
   *
   * = 0：纯虚函数，PixelWriter 不可直接实例化，必须由子类覆盖。
   * virtual：启用 vtable 分派，运行时根据对象实际类型跳转到正确实现。
   */
  virtual void Write(int x, int y, const PixelColor& c) = 0;
 
 protected:
  /**
   * PixelAt — 内联辅助函数：计算屏幕坐标 (x, y) 对应的帧缓冲区字节指针
   *
   * @return  指向像素 (x, y) 第一个字节（共 4 字节：3 色 + 1 保留）的指针
   *
   * ── 计算公式 ─────────────────────────────────────────────────────────────
   *
   *   帧缓冲区以行优先（row-major）存储：
   *     字节偏移 = 4 × (pixels_per_scan_line × y + x)
   *
   *   为何用 pixels_per_scan_line 而非 horizontal_resolution：
   *     UEFI GOP 允许每行末尾有填充像素（内存对齐），
   *     pixels_per_scan_line ≥ horizontal_resolution。
   *     若用 horizontal_resolution 作为步长，从第 2 行起地址偏移，
   *     屏幕出现斜纹（shearing），在大多数硬件上是难以察觉的 bug。
   *
   * ── 内联原因 ─────────────────────────────────────────────────────────────
   *
   *   PixelAt 是写像素的热路径，内联消除函数调用开销，
   *   编译器可将其展开进 Write() 中。
   *
   * protected 而非 private：
   *   子类 Write() 实现需要调用 PixelAt()，private 会阻止子类访问。
   */
  uint8_t* PixelAt(int x, int y) {
    return config_.frame_buffer + 4 * (config_.pixels_per_scan_line * y + x);
  }
 
 private:
  /**
   * config_ — 帧缓冲区配置的常量引用（由构造函数绑定）
   *
   * 存储为引用而非拷贝：避免拷贝结构体，且引用语义使配置变化自动反映。
   * private：封装实现细节，子类通过 PixelAt() 间接访问，不直接操作 config_。
   */
  const FrameBufferConfig& config_;
};
 
// #@@range_begin(pixel_writer_def)
/**
 * RGBResv8BitPerColorPixelWriter — RGB 字节序像素写入器
 *
 * 适用于 GOP PixelFormat = kPixelRGBResv8BitPerColor 的帧缓冲区。
 * 内存字节序：[R][G][B][_]（_=保留位，通常为 0）
 * 现代 DisplayPort / HDMI 输出常见此格式。
 *
 * using PixelWriter::PixelWriter（继承构造函数，C++11）：
 *   将基类所有构造函数引入子类作用域，无需重复编写相同签名的构造函数。
 *   效果等价于：RGBResv8Bit...(const FrameBufferConfig& c) : PixelWriter(c) {}
 *
 * virtual void Write() override：
 *   override：让编译器验证此函数确实覆盖了基类纯虚函数（拼写错误时编译报错）。
 *   实现在 graphics.cpp（ODR：避免头文件多次包含导致多重定义）。
 */
class RGBResv8BitPerColorPixelWriter : public PixelWriter {
 public:
  using PixelWriter::PixelWriter;
  virtual void Write(int x, int y, const PixelColor& c) override;
};
 
/**
 * BGRResv8BitPerColorPixelWriter — BGR 字节序像素写入器
 *
 * 适用于 GOP PixelFormat = kPixelBGRResv8BitPerColor 的帧缓冲区。
 * 内存字节序：[B][G][R][_]（红蓝互换，绿色仍在偏移 1）
 * 部分 VGA/VESA 兼容硬件、QEMU 默认使用此格式。
 *
 * 与 RGBResv8BitPerColorPixelWriter 唯一差异：Write() 中 p[0]=c.b, p[2]=c.r。
 * 若字节序选错，屏幕上红色显示为蓝色、蓝色显示为红色（绿色不受影响）。
 *
 * 继承构造函数和 override 语义与 RGB 版完全相同，见上方注释。
 */
class BGRResv8BitPerColorPixelWriter : public PixelWriter {
 public:
  using PixelWriter::PixelWriter;
  virtual void Write(int x, int y, const PixelColor& c) override;
};
// #@@range_end(pixel_writer_def)
 