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
 *     UEFI GOP（Graphics Output Protocol）帧缓冲区的像素字节序因固件/硬件不同而异。
 *     将"写像素"操作抽象为虚函数，main.cpp 可以在运行时根据 pixel_format 动态选择
 *     正确的写入器，而渲染逻辑（WriteAscii、填充循环等）无需感知字节序差异。
 *
 *   依赖关系：
 *     graphics.hpp  →  frame_buffer_config.hpp（FrameBufferConfig, PixelFormat 枚举）
 *     graphics.cpp  →  graphics.hpp（实现 Write() 虚函数）
 *     font.hpp      →  graphics.hpp（WriteAscii 参数类型）
 *     main.cpp      →  graphics.hpp（构造写入器、调用 Write()）
 *
 * ── ODR（One Definition Rule）与头文件/源文件分工 ────────────────────────────
 *
 *   Write() 虚函数只在本头文件中"声明"，实现在 graphics.cpp 中。
 *   若将实现写在头文件中，则每个 #include "graphics.hpp" 的翻译单元都会生成
 *   一份函数实体，链接时触发"多重定义"错误（ODR 违规）。
 *   例外：PixelAt() 是 protected 内联函数，标注为 inline 语义，
 *         编译器在每个翻译单元内展开，不产生独立符号，故可放在头文件中。
 */
 
#pragma once
 
// #@@range_begin(pixel_color_def)
/**
 * frame_buffer_config.hpp — 提供 FrameBufferConfig 结构体和 PixelFormat 枚举。
 *
 * FrameBufferConfig 内容（由引导加载程序 Main.c 填充并传入内核）：
 *   frame_buffer        : uint8_t*  — 帧缓冲区起始地址（物理内存，直接映射）
 *   pixels_per_scan_line: uint32_t  — 每扫描行的像素数（含行末填充，≥ horizontal_resolution）
 *   horizontal_resolution: uint32_t — 屏幕水平分辨率（实际显示像素列数）
 *   vertical_resolution  : uint32_t — 屏幕垂直分辨率（实际显示像素行数）
 *   pixel_format         : enum     — kPixelRGBResv8BitPerColor 或 kPixelBGRResv8BitPerColor
 */
#include "frame_buffer_config.hpp"
 
/**
 * PixelColor — 表示一个像素颜色的 POD 结构体（红、绿、蓝各 8 位）
 *
 * 字段：
 *   r（uint8_t）— 红色分量，范围 0（无红）~ 255（全红）
 *   g（uint8_t）— 绿色分量，范围 0（无绿）~ 255（全绿）
 *   b（uint8_t）— 蓝色分量，范围 0（无蓝）~ 255（全蓝）
 *
 * 使用示例：
 *   PixelColor white = {255, 255, 255};  // 白色
 *   PixelColor black = {0,   0,   0  };  // 黑色
 *   PixelColor green = {0,   255, 0  };  // 纯绿
 *
 * 注意：
 *   本结构体描述"逻辑颜色"，与帧缓冲区中的"物理字节序"无关。
 *   RGBPixelWriter 将 {r,g,b} 写为 [r][g][b]（物理顺序 = 逻辑顺序）；
 *   BGRPixelWriter 将 {r,g,b} 写为 [b][g][r]（物理顺序反转）。
 *   调用方只需传入 PixelColor，字节序适配由写入器内部处理。
 */
struct PixelColor {
  uint8_t r, g, b;
};
// #@@range_end(pixel_color_def)
 
/**
 * PixelWriter — 像素写入器抽象基类
 *
 * 通过 C++ 虚函数机制，将"向帧缓冲区写入一个像素"这一操作抽象为统一接口。
 * 具体子类（RGBResv8BitPerColorPixelWriter / BGRResv8BitPerColorPixelWriter）
 * 根据帧缓冲区的实际字节序覆盖 Write() 方法，调用方对字节序完全透明。
 *
 * ── 对象生命周期与构造方式 ────────────────────────────────────────────────────
 *
 *   main.cpp 中使用"放置 new（placement new）"在静态缓冲区上构造写入器：
 *     char pixel_writer_buf[sizeof(RGBResv8BitPerColorPixelWriter)];
 *     PixelWriter* pixel_writer;
 *     pixel_writer = new(pixel_writer_buf) RGBResv8BitPerColorPixelWriter{frame_buffer_config};
 *
 *   原因：ExitBootServices 调用后 UEFI 内存管理失效，内核尚未建立堆管理器，
 *   无法使用 operator new（堆分配）。放置 new 在已存在的静态缓冲区上构造对象，
 *   无需堆分配，规避了这一限制。
 *
 * ── 继承关系 ─────────────────────────────────────────────────────────────────
 *
 *                PixelWriter（抽象基类）
 *               /                      \
 *   RGBResv8Bit...PixelWriter      BGRResv8Bit...PixelWriter
 *   （RGB 字节序，DisplayPort/HDMI 常见） （BGR 字节序，部分 VGA/VESA 设备）
 */
class PixelWriter {
 public:
  /**
   * 构造函数 — 初始化帧缓冲区配置引用
   *
   * @param config  FrameBufferConfig 的常量引用（由引导加载程序传入内核的共享结构体）
   *
   * 使用成员初始化列表 config_{config} 直接绑定引用，避免拷贝整个结构体。
   * const FrameBufferConfig& 确保写入器不会修改帧缓冲区配置（只读访问）。
   * 引用绑定后，写入器的生命周期内 config 必须保持有效（由 KernelMain 的栈变量保证）。
   */
  PixelWriter(const FrameBufferConfig& config) : config_{config} {
  }
 
  /**
   * 虚析构函数 — 确保通过基类指针删除子类对象时正确调用子类析构函数
   *
   * = default：让编译器生成默认实现（本类无手动管理的资源，空函数体即可）。
   * virtual   ：若用 PixelWriter* 调用 delete，先执行子类析构，再执行基类析构。
   *             缺少 virtual 则只调用基类析构，导致子类资源泄漏（undefined behavior）。
   *
   * 注：本项目使用放置 new，不调用 delete，但声明虚析构是良好的 OOP 实践。
   */
  virtual ~PixelWriter() = default;
 
  /**
   * Write — 纯虚函数：向帧缓冲区的 (x, y) 坐标写入颜色 c
   *
   * @param x  像素列号（0 = 最左列）
   * @param y  像素行号（0 = 最顶行）
   * @param c  要写入的颜色（逻辑 RGB，字节序由子类实现决定）
   *
   * = 0：纯虚函数，PixelWriter 本身不提供实现，不可直接实例化。
   *       必须由子类覆盖（override），否则子类也是抽象类，同样不可实例化。
   *
   * virtual：启用虚函数分派（vtable 查找）。
   *   通过 PixelWriter* 调用 Write() 时，运行时根据对象的实际类型
   *   （RGBResv8Bit... 或 BGRResv8Bit...）跳转到正确的 Write() 实现，
   *   无需调用方知道具体类型，实现运行时多态。
   */
  virtual void Write(int x, int y, const PixelColor& c) = 0;
 
 protected:
  /**
   * PixelAt — 内联辅助函数：计算屏幕坐标 (x, y) 对应的帧缓冲区字节指针
   *
   * @param x  像素列号（0 = 最左列）
   * @param y  像素行号（0 = 最顶行）
   *
   * @return   指向该像素的第一个字节（共 4 字节：3 色 + 1 保留位）的指针
   *
   * ── 计算公式详解 ───────────────────────────────────────────────────────────
   *
   *   帧缓冲区在内存中以"行优先"（row-major）方式存储：
   *     第 0 行：像素[0][0], [0][1], ..., [0][pixels_per_scan_line-1]
   *     第 1 行：像素[1][0], [1][1], ..., [1][pixels_per_scan_line-1]
   *     ...
   *
   *   坐标 (x, y) 的像素在内存中的偏移（以像素为单位）：
   *     像素偏移 = pixels_per_scan_line × y + x
   *
   *   转换为字节偏移（每像素 4 字节）：
   *     字节偏移 = 4 × (pixels_per_scan_line × y + x)
   *
   *   绝对地址：
   *     frame_buffer + 4 × (pixels_per_scan_line × y + x)
   *
   * ── 关键：pixels_per_scan_line 而非 horizontal_resolution ────────────────
   *
   *   UEFI GOP 规范允许每行末尾有填充像素（对齐目的），因此：
   *     pixels_per_scan_line ≥ horizontal_resolution
   *     pixels_per_scan_line 是内存中实际每行跨越的像素数（含填充）
   *     horizontal_resolution 是屏幕上实际显示的像素列数
   *
   *   若使用 horizontal_resolution 作为行步长（stride），
   *   从第 2 行开始每行地址会偏移，导致画面出现斜纹（shearing），
   *   在大多数硬件上这是难以察觉的 bug（两值恰好相等时不出现）。
   *   正确做法是始终使用 pixels_per_scan_line。
   *
   * ── 为何放在 protected 内联而非 private ──────────────────────────────────
   *
   *   protected：子类（RGBResv8Bit... / BGRResv8Bit...）的 Write() 实现
   *              需要调用 PixelAt() 获取像素地址，private 会阻止访问。
   *   内联（在类定义体内实现）：PixelAt 是写像素的热路径，
   *              内联消除函数调用开销，编译器可将其展开进 Write() 中。
   *
   * ── 4 字节像素格式 ────────────────────────────────────────────────────────
   *
   *   GOP PixelFormat kPixelRGBResv8BitPerColor / kPixelBGRResv8BitPerColor：
   *     每像素固定 4 字节：3 字节颜色（字节序不同）+ 1 字节保留（通常为 0）
   *     即使颜色只需 24 位，物理存储按 32 位对齐，便于 SIMD 和 DMA 操作。
   */
  uint8_t* PixelAt(int x, int y) {
    return config_.frame_buffer + 4 * (config_.pixels_per_scan_line * y + x);
  }
 
 private:
  /**
   * config_ — 帧缓冲区配置的常量引用（由构造函数绑定）
   *
   * 存储为引用而非拷贝：
   *   ① 避免拷贝整个 FrameBufferConfig 结构体（含 frame_buffer 指针等字段）
   *   ② 若 config 在外部被修改（理论上不应发生），写入器自动反映最新值
   *
   * private：封装实现细节，子类通过 PixelAt() 间接访问配置，不直接操作 config_。
   */
  const FrameBufferConfig& config_;
};
 
// #@@range_begin(pixel_writer_def)
/**
 * RGBResv8BitPerColorPixelWriter — RGB 字节序像素写入器
 *
 * 适用于 GOP PixelFormat = kPixelRGBResv8BitPerColor 的帧缓冲区。
 *
 * 字节序（内存中的实际存储顺序）：
 *   偏移 0：红色（R）
 *   偏移 1：绿色（G）
 *   偏移 2：蓝色（B）
 *   偏移 3：保留（Reserved，通常为 0）
 *
 * ── using PixelWriter::PixelWriter ──────────────────────────────────────────
 *
 *   C++11 继承构造函数语法：将基类 PixelWriter 的所有构造函数引入派生类的作用域，
 *   使 RGBResv8BitPerColorPixelWriter 可以直接用 FrameBufferConfig 参数构造，
 *   无需重复编写相同签名的构造函数。
 *   效果等价于：
 *     RGBResv8BitPerColorPixelWriter(const FrameBufferConfig& config)
 *       : PixelWriter(config) {}
 *
 * ── virtual void Write() override ───────────────────────────────────────────
 *
 *   覆盖基类纯虚函数，提供具体的 RGB 字节序写入实现。
 *   virtual  ：保持可被更深层子类再次覆盖（本项目未使用但是良好实践）。
 *   override ：让编译器验证此函数确实覆盖了基类的虚函数，
 *              若签名不匹配（如拼写错误），编译器报错而非静默创建新函数。
 *
 *   实现位于 graphics.cpp（ODR：避免头文件多包含导致多重定义）。
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
 *
 * 字节序（内存中的实际存储顺序）：
 *   偏移 0：蓝色（B）
 *   偏移 1：绿色（G）
 *   偏移 2：红色（R）
 *   偏移 3：保留（Reserved，通常为 0）
 *
 * 与 RGBResv8BitPerColorPixelWriter 的唯一差异：
 *   Write() 实现中 p[0]=c.b, p[2]=c.r（红蓝互换），绿色 p[1]=c.g 不变。
 *
 * 继承构造函数和 override 语义与 RGBResv8BitPerColorPixelWriter 完全相同，
 * 见上方注释。
 */
class BGRResv8BitPerColorPixelWriter : public PixelWriter {
 public:
  using PixelWriter::PixelWriter;
  virtual void Write(int x, int y, const PixelColor& c) override;
};
// #@@range_end(pixel_writer_def)
 
 