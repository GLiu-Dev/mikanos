/**
 * @file graphics.hpp
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本头文件是 MikanOS 内核图形子系统的公共接口，定义：
 *     ① PixelColor           — 与帧缓冲格式无关的逻辑颜色结构体（RGB 三分量）
 *     ② PixelWriter          — 像素写入抽象基类（虚函数接口）
 *     ③ RGBResv8BitPerColorPixelWriter — RGB 格式帧缓冲区的具体写入器（声明）
 *     ④ BGRResv8BitPerColorPixelWriter — BGR 格式帧缓冲区的具体写入器（声明）
 *
 * ── 重构背景：从 main.cpp 独立为 graphics.hpp / graphics.cpp ────────────
 *
 *   早期版本将 PixelColor、PixelWriter 及其子类全部写在 main.cpp 中。
 *   随着内核功能扩展（加入 font.cpp、未来的窗口管理等），
 *   像素写入逻辑被抽取到独立的图形子系统：
 *
 *   main.cpp   →  负责内核入口与子系统组装
 *   graphics.hpp/cpp  →  像素写入器接口与实现
 *   font.hpp/cpp →  字体数据与字符渲染
 *
 *   这种分离的好处：
 *     • graphics.hpp 可被 font.cpp、future window manager 等多处 include，
 *       无需将写入器代码复制到各处
 *     • 修改 Write() 的具体实现（如增加 alpha 混合）只需重新编译 graphics.cpp，
 *       不会触发 main.cpp / font.cpp 的重编译（减少编译时间）
 *     • 头文件只暴露接口（声明），实现细节隐藏在 graphics.cpp 中
 *
 * ── 头文件 vs 源文件的分工 ─────────────────────────────────────────────
 *
 *   graphics.hpp（本文件）：
 *     • PixelColor 结构体定义（全部）
 *     • PixelWriter 类定义（含 PixelAt 实现，inline 供子类直接使用）
 *     • RGBResv8BitPerColorPixelWriter / BGRResv8BitPerColorPixelWriter 类声明
 *       （Write() 仅声明，不在头文件中实现，避免多重定义）
 *
 *   graphics.cpp：
 *     • RGBResv8BitPerColorPixelWriter::Write() 的具体实现
 *     • BGRResv8BitPerColorPixelWriter::Write() 的具体实现
 *
 * ── 依赖关系 ────────────────────────────────────────────────────────────
 *
 *   graphics.hpp
 *     └─ #include "frame_buffer_config.hpp"  （FrameBufferConfig 结构体 + PixelFormat 枚举）
 */
 
#pragma once
 
// #@@range_begin(pixel_color_def)
#include "frame_buffer_config.hpp"  /* FrameBufferConfig、PixelFormat：与引导程序共用，
                                     * 保证帧缓冲区描述的内存布局两端完全一致 */
 
/**
 * PixelColor — 与帧缓冲格式无关的逻辑颜色结构体
 *
 * 这是"逻辑颜色"——与帧缓冲区的物理字节序无关。
 * 无论底层格式是 RGB 还是 BGR，调用方统一用此结构体描述颜色，
 * 由 PixelWriter 的具体子类负责将逻辑颜色转换为正确的物理字节序写入内存。
 *
 * 内存布局：
 *   r（1 字节）→ g（1 字节）→ b（1 字节），连续排列，无填充
 *   sizeof(PixelColor) == 3
 *
 * 常用颜色字面量（C++ 聚合初始化）：
 *   {255, 255, 255}  白色（全亮）
 *   {  0,   0,   0}  黑色（全暗）
 *   {255,   0,   0}  纯红
 *   {  0, 255,   0}  纯绿
 *   {  0,   0, 255}  纯蓝
 */
struct PixelColor {
  uint8_t r;  /* 红色分量（Red），  0 = 无红，255 = 最亮红 */
  uint8_t g;  /* 绿色分量（Green），0 = 无绿，255 = 最亮绿 */
  uint8_t b;  /* 蓝色分量（Blue），  0 = 无蓝，255 = 最亮蓝 */
};
// #@@range_end(pixel_color_def)
 
/**
 * PixelWriter — 像素写入抽象基类
 *
 * ── 设计意图 ────────────────────────────────────────────────────────────────
 *
 *   帧缓冲区的像素字节序（RGB vs BGR）在运行时才能确定（取决于 GOP 硬件），
 *   但绘图代码（清屏、矩形填充、字符渲染）是与格式无关的。
 *   通过抽象基类 PixelWriter + 虚函数 Write()，绘图代码只需调用：
 *     pixel_writer->Write(x, y, color)
 *   无需在每次绘图时判断像素格式，实现"格式判断一次，绘图多次"。
 *
 * ── 继承关系 ────────────────────────────────────────────────────────────────
 *
 *   PixelWriter（抽象基类，不可直接实例化）
 *   ├── RGBResv8BitPerColorPixelWriter  → 内存布局 [R][G][B][保留]
 *   └── BGRResv8BitPerColorPixelWriter  → 内存布局 [B][G][R][保留]
 *
 * ── 对象生命周期 ────────────────────────────────────────────────────────────
 *
 *   通过 placement new 构造在全局静态缓冲区中，生命周期与内核相同，永不析构。
 *   全局指针 PixelWriter* pixel_writer 指向当前写入器，供整个内核使用。
 */
class PixelWriter {
 public:
  /**
   * 构造函数 — 接受帧缓冲区配置的常量引用
   *
   * 初始化列表 config_{config} 将引用绑定到私有成员 config_。
   * 子类通过 "using PixelWriter::PixelWriter" 直接继承此构造函数，
   * 无需在子类中重复声明相同参数的构造函数。
   *
   * 使用引用（const FrameBufferConfig&）而非指针的理由：
   *   ① 引用不能为 null，省去空指针检查
   *   ② const 保证构造后 PixelWriter 不会意外修改帧缓冲区配置
   *   ③ 语义明确：PixelWriter 借用配置，不拥有配置
   */
  PixelWriter(const FrameBufferConfig& config) : config_{config} {
  }
 
  /**
   * 虚析构函数（= default，编译器生成空实现）
   *
   * 多态基类必须声明虚析构函数：通过 PixelWriter* 指针 delete 对象时，
   * 才能正确调用派生类析构函数，避免资源泄漏。
   * 本项目虽不实际 delete 对象，虚析构是防御性最佳实践。
   */
  virtual ~PixelWriter() = default;
 
  /**
   * Write() — 纯虚函数：向屏幕坐标 (x, y) 写入颜色 c（子类必须实现）
   *
   * 参数：
   *   x — 横坐标（列），0 = 最左，向右增大
   *   y — 纵坐标（行），0 = 最顶，向下增大
   *   c — 逻辑颜色（RGB 分量），子类负责转换为物理字节序
   *
   * = 0：使 PixelWriter 成为抽象类，强制子类提供具体实现，
   *       试图实例化 PixelWriter 本身会导致编译错误。
   *
   * 具体实现在 graphics.cpp 中（而非此头文件），原因：
   *   若在头文件中定义非 inline 函数体，每个 include 本头文件的
   *   编译单元都会产生一份实现，链接时出现"多重定义"错误。
   *   纯虚函数 = 0 本身没有函数体（在此头文件中），链接安全。
   */
  virtual void Write(int x, int y, const PixelColor& c) = 0;
 
 protected:
  /**
   * PixelAt() — 计算 (x, y) 坐标对应的帧缓冲区字节地址（受保护 inline 辅助方法）
   *
   * 地址计算公式：
   *   frame_buffer + 4 × (pixels_per_scan_line × y + x)
   *
   * ┌─── 关键：pixels_per_scan_line 而非 horizontal_resolution ─────────────┐
   * │                                                                        │
   * │  帧缓冲区每行物理布局：                                                 │
   * │  [像素0]..[像素 HR-1][填充0]..[填充 P-HR-1]                            │
   * │  |←── horizontal_resolution (HR) ──→||←── 行末对齐填充 ─────────────→| │
   * │  |←───────── pixels_per_scan_line (P) ──────────────────────────────→| │
   * │                                                                        │
   * │  GPU 硬件要求每行字节数对齐到特定边界（如 256 字节），                   │
   * │  P ≥ HR。若误用 HR 计算行首偏移，从第二行起图像向左斜切（错位）。       │
   * └────────────────────────────────────────────────────────────────────────┘
   *
   * × 4：每像素 4 字节（R / G / B / 保留，RGB 和 BGR 格式均适用）
   *
   * 为何在头文件中实现（inline）而非 graphics.cpp？
   *   ① PixelAt 是内层循环的热路径（每写一个像素调用一次），内联展开避免函数调用开销
   *   ② protected 成员只有子类可访问，不会被外部滥用
   *   ③ 实现极短（一行），放在头文件不会显著增加编译负担
   *
   * 访问权限（protected）：
   *   子类 RGBResv8BitPerColorPixelWriter / BGRResv8BitPerColorPixelWriter
   *   在 Write() 实现中调用 PixelAt() 获取像素地址；外部代码不可直接访问。
   */
  uint8_t* PixelAt(int x, int y) {
    return config_.frame_buffer + 4 * (config_.pixels_per_scan_line * y + x);
  }
 
 private:
  /**
   * config_ — 帧缓冲区配置的常量引用（私有成员）
   *
   * private：子类无需直接访问 config_，
   * 所有地址计算已由 protected PixelAt() 封装，防止子类绕过封装直接操作内存。
   * 若将来需要在子类中读取分辨率（如边界检查），应通过 getter 暴露，
   * 而非将 config_ 改为 protected。
   */
  const FrameBufferConfig& config_;
};
 
// #@@range_begin(pixel_writer_def)
/**
 * RGBResv8BitPerColorPixelWriter — RGB 格式帧缓冲区的像素写入器（类声明）
 *
 * 适用格式：PixelRedGreenBlueReserved8BitPerColor（GOP 枚举）
 *            对应 kPixelRGBResv8BitPerColor（内核枚举）
 *
 * 物理内存字节序（低地址→高地址）：[R][G][B][保留]
 * 典型硬件：Intel 核显、大多数现代 PC 独立显卡（UEFI 模式）
 *
 * 仅声明 Write()，具体实现在 graphics.cpp 中。
 * 这遵循 C++ 头文件/源文件分离惯例：
 *   头文件（.hpp）提供接口，让编译器知道"有这个函数"
 *   源文件（.cpp）提供实现，只编译一次，链接时解析引用
 *
 * 若在头文件中提供非 inline 的 Write() 函数体，
 * 每个 include 了 graphics.hpp 的 .cpp 文件都会生成一份 Write 符号，
 * 链接时会出现"multiple definition of Write"错误。
 */
class RGBResv8BitPerColorPixelWriter : public PixelWriter {
 public:
  /**
   * using PixelWriter::PixelWriter — 继承基类构造函数（C++11）
   *
   * 使 PixelWriter(const FrameBufferConfig&) 对本类可见，
   * 等价于手写：
   *   RGBResv8BitPerColorPixelWriter(const FrameBufferConfig& c)
   *       : PixelWriter(c) {}
   * 本类无额外成员，继承构造函数完全足够，不必重复声明。
   */
  using PixelWriter::PixelWriter;
 
  /**
   * Write() — RGB 格式像素写入（仅声明，实现在 graphics.cpp）
   *
   * 字节序：p[0]=R, p[1]=G, p[2]=B（p[3] 保留字节不修改）
   * override：向编译器声明覆盖基类虚函数，签名不匹配时编译报错。
   */
  virtual void Write(int x, int y, const PixelColor& c) override;
};
 
/**
 * BGRResv8BitPerColorPixelWriter — BGR 格式帧缓冲区的像素写入器（类声明）
 *
 * 适用格式：PixelBlueGreenRedReserved8BitPerColor（GOP 枚举）
 *            对应 kPixelBGRResv8BitPerColor（内核枚举）
 *
 * 物理内存字节序（低地址→高地址）：[B][G][R][保留]
 * 典型硬件：部分 AMD 显卡、某些 QEMU/VirtualBox 虚拟 GOP 实现
 *
 * 与 RGBResv8BitPerColorPixelWriter 的唯一区别：
 *   Write() 中 p[0] 写蓝（c.b），p[2] 写红（c.r）—— R/B 字节互换，G 不变。
 * 若将本类误用于 RGB 格式帧缓冲区，红色和蓝色分量会互换（颜色反转）。
 */
class BGRResv8BitPerColorPixelWriter : public PixelWriter {
 public:
  using PixelWriter::PixelWriter;  /* 同 RGB 版，继承基类构造函数 */
 
  /**
   * Write() — BGR 格式像素写入（仅声明，实现在 graphics.cpp）
   *
   * 字节序：p[0]=B, p[1]=G, p[2]=R（p[3] 保留字节不修改）
   */
  virtual void Write(int x, int y, const PixelColor& c) override;
};
// #@@range_end(pixel_writer_def)
 