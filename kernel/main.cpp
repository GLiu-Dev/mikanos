/**
 * @file main.cpp
 *
 * MikanOS カーネル本体のプログラムを書いたファイル．
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本文件是 MikanOS 内核的入口与像素/文字绘图子系统，提供：
 *     ① kFontA               — 字母 'A' 的 8×16 位图字体数据
 *     ② PixelColor           — RGB 颜色结构体（与格式无关的逻辑颜色）
 *     ③ PixelWriter          — 抽象基类，封装"向帧缓冲区某坐标写像素"的接口
 *     ④ RGBResv8BitPerColorPixelWriter — RGB 格式帧缓冲区的具体实现
 *     ⑤ BGRResv8BitPerColorPixelWriter — BGR 格式帧缓冲区的具体实现
 *     ⑥ WriteAscii           — 使用位图字体向屏幕渲染单个 ASCII 字符
 *     ⑦ Placement new/delete — 在静态缓冲区中原地构造 C++ 对象（无需堆）
 *     ⑧ KernelMain           — 内核入口函数（由 UEFI 引导程序跳转到此处）
 *
 * ── 本版新增功能 ─────────────────────────────────────────────────────────────
 *
 *   相对于上一版本，新增了位图字体渲染能力：
 *     • kFontA：手工设计的 'A' 字形，每行 1 字节（8 位 = 8 列），共 16 行
 *     • WriteAscii：利用位操作从字体位图中提取每个像素是否点亮
 *     • KernelMain 在绿色矩形上叠加绘制两个黑色 'A' 字符（列：50 和 58，行：50）
 *
 * ── 调用约定说明 ─────────────────────────────────────────────────────────────
 *
 *   UEFI 引导程序使用 Microsoft x64 ABI；内核使用 System V AMD64 ABI。
 *   KernelMain 通过 extern "C" 抑制 C++ 名称修饰，与 ELF e_entry 符号名匹配。
 */
 
#include <cstdint>   /* uint8_t / uint32_t 等固定宽度整数类型 */
#include <cstddef>   /* size_t（placement new 参数类型必须包含） */
 
#include "frame_buffer_config.hpp"  /* FrameBufferConfig + PixelFormat，与引导程序共用 */
 
/* ============================================================================
 * 一、kFontA — 字母 'A' 的 8×16 位图字体数据
 * ============================================================================
 *
 * 位图字体（Bitmap Font）是最简单的文字渲染方式：
 *   每个字符用一个二维位数组表示，1 = 该像素点亮，0 = 该像素熄灭。
 *   渲染时只需按位读取，无需复杂的矢量运算，适合早期内核实现。
 *
 * 本字体规格：
 *   宽度   8 像素（= 1 字节 / 行，bit7 = 最左列，bit0 = 最右列）
 *   高度  16 像素（= 16 字节 / 字符，索引 0 = 最顶行，索引 15 = 最底行）
 *   颜色   单色（1 位 / 像素），实际渲染颜色由调用方指定
 *
 * 位与列的对应关系（以第 1 行 0b00011000 为例）：
 *
 *   字节位：  bit7 bit6 bit5 bit4 bit3 bit2 bit1 bit0
 *   列索引：  dx=0 dx=1 dx=2 dx=3 dx=4 dx=5 dx=6 dx=7
 *   像素值：    0    0    0    1    1    0    0    0
 *   视觉效果：  .    .    .    *    *    .    .    .   （* = 亮，. = 暗）
 *
 * 注释中的字符图示（右侧）：
 *   每行右侧注释用空格和 * 展示该行的视觉效果，方便直接核对字形。
 *   字符 '/' 和 ' ' 分别代表点亮（有些实现用 * 或 #）和熄灭。
 *   本代码使用 * 表示点亮像素。
 *
 * 为什么只有 'A' 而没有完整字符集？
 *   这是教学阶段的最小可行实现：先验证渲染管线（位图→屏幕像素）正确，
 *   后续章节将引入完整 ASCII 字体数组（通常 128 或 256 个字符）。
 */
// #@@range_begin(font_a)
const uint8_t kFontA[16] = {
  0b00000000, //
  0b00011000, //    **       第 1 行：顶部小横杠（'A' 的尖顶）
  0b00011000, //    **       第 2 行：同上，加粗顶部
  0b00011000, //    **       第 3 行：顶部茎干
  0b00011000, //    **       第 4 行：顶部茎干
  0b00100100, //   *  *      第 5 行：左右两侧茎干开始分叉
  0b00100100, //   *  *      第 6 行：左右分叉
  0b00100100, //   *  *      第 7 行：左右分叉（中横线上方）
  0b00100100, //   *  *      第 8 行：左右分叉（中横线上方）
  0b01111110, //  ******     第 9 行：中横线（'A' 字的横撇，6 像素宽）
  0b01000010, //  *    *     第10 行：中横线下方，左右两侧腿部
  0b01000010, //  *    *     第11 行：腿部
  0b01000010, //  *    *     第12 行：腿部
  0b11100111, // ***  ***    第13 行：底部衬脚（serif 装饰）
  0b00000000, //             第14 行：行间距留白
  0b00000000, //             第15 行：行间距留白
};
// #@@range_end(font_a)
 
/* ============================================================================
 * 二、PixelColor — 与帧缓冲格式无关的逻辑颜色结构体
 * ============================================================================ */
 
/**
 * PixelColor — 以 R / G / B 三分量（各 8 位，0–255）表示的颜色
 *
 * 这是"逻辑颜色"——与帧缓冲区的物理字节序无关。
 * 无论底层格式是 RGB 还是 BGR，调用方统一用此结构体描述颜色，
 * 由 PixelWriter 的具体子类负责转换为正确的物理字节序写入内存。
 *
 * sizeof(PixelColor) == 3（三个 uint8_t 连续排列，无填充）。
 * 常用颜色字面量（聚合初始化）：
 *   {255, 255, 255}  白色
 *   {  0, 255,   0}  绿色
 *   {  0,   0,   0}  黑色
 *   {255,   0,   0}  红色
 */
struct PixelColor {
  uint8_t r;  /* 红色分量（Red），0 = 无红，255 = 最亮红 */
  uint8_t g;  /* 绿色分量（Green） */
  uint8_t b;  /* 蓝色分量（Blue） */
};
 
/* ============================================================================
 * 三、PixelWriter — 像素写入抽象基类
 * ============================================================================
 *
 * 职责：封装"将 PixelColor 写入帧缓冲区指定坐标"的操作。
 * 通过纯虚函数 Write() 定义接口，由子类实现具体格式的字节序转换。
 *
 * 继承关系：
 *   PixelWriter（抽象基类）
 *   ├── RGBResv8BitPerColorPixelWriter  字节序 R→G→B→保留
 *   └── BGRResv8BitPerColorPixelWriter  字节序 B→G→R→保留
 *
 * 对象通过 placement new 构造在全局静态缓冲区中，永不析构。
 */
class PixelWriter {
 public:
  /**
   * 构造函数 — 接受帧缓冲区配置的常量引用
   *
   * 初始化列表将引用绑定到私有成员 config_；
   * 子类通过 "using PixelWriter::PixelWriter" 直接继承此构造函数，
   * 无需重复声明。
   *
   * 使用引用（const FrameBufferConfig&）而非指针的理由：
   *   ① 引用不能为 null，省去空指针检查
   *   ② const 保证 PixelWriter 不会意外修改配置内容
   *   ③ 语义明确：PixelWriter 借用配置，不拥有配置
   */
  PixelWriter(const FrameBufferConfig& config) : config_{config} {
  }
 
  /**
   * 虚析构函数（= default）
   *
   * 多态基类必须声明虚析构函数：通过基类指针 PixelWriter* 删除对象时，
   * 才能正确调用派生类析构函数，避免资源泄漏。
   * = default：编译器生成空实现，同时保留 vtable 中的析构条目。
   * 本项目虽然不实际 delete 对象，声明虚析构是最佳实践。
   */
  virtual ~PixelWriter() = default;
 
  /**
   * Write() — 纯虚函数：向 (x, y) 坐标写入颜色 c（子类必须实现）
   *
   * 参数：
   *   x — 横坐标，0 = 最左列，向右增大
   *   y — 纵坐标，0 = 最顶行，向下增大
   *   c — 逻辑颜色（RGB 分量），子类负责转换为物理字节序
   *
   * 纯虚（= 0）使 PixelWriter 成为抽象类，强制所有子类实现此方法。
   */
  virtual void Write(int x, int y, const PixelColor& c) = 0;
 
 protected:
  /**
   * PixelAt() — 计算 (x, y) 坐标对应的帧缓冲区字节地址（受保护辅助方法）
   *
   * 地址计算公式：
   *   frame_buffer + 4 × (pixels_per_scan_line × y + x)
   *
   * ┌─── 关键：pixels_per_scan_line 而非 horizontal_resolution ──────────┐
   * │                                                                     │
   * │  帧缓冲区每行物理布局：                                              │
   * │  [像素0]..[像素 HR-1][填充0]..[填充 P-HR-1]                         │
   * │  |←── horizontal_resolution (HR) ──→||←── 行末对齐填充 ──────────→| │
   * │  |←───────── pixels_per_scan_line (P) ────────────────────────────→| │
   * │                                                                     │
   * │  GPU 硬件要求每行字节数对齐到特定边界（如 256 字节），               │
   * │  因此 P ≥ HR，误用 HR 作步长会导致画面从第二行起向左错位（斜切）。   │
   * └─────────────────────────────────────────────────────────────────────┘
   *
   * × 4：每像素 4 字节（R / G / B / 保留各 1 字节，RGB 和 BGR 格式均适用）
   *
   * protected（受保护）：子类可调用，外部代码不可直接访问字节地址。
   */
  uint8_t* PixelAt(int x, int y) {
    return config_.frame_buffer + 4 * (config_.pixels_per_scan_line * y + x);
  }
 
 private:
  /**
   * config_ — 帧缓冲区配置的常量引用（私有成员）
   *
   * private：子类无需直接访问 config_，所有地址计算已由 PixelAt() 封装，
   * 防止子类绕过封装直接操作内存导致格式错误。
   */
  const FrameBufferConfig& config_;
};
 
/* ============================================================================
 * 四、具体像素写入器：RGB 和 BGR 两种格式实现
 * ============================================================================ */
 
/**
 * RGBResv8BitPerColorPixelWriter — RGB 格式帧缓冲区的像素写入器
 *
 * 适用格式：PixelRedGreenBlueReserved8BitPerColor（GOP 枚举）
 *            对应 kPixelRGBResv8BitPerColor（内核枚举）
 *
 * 物理内存字节序（低地址→高地址）：[R][G][B][保留]
 * 典型硬件：Intel 核显、大多数现代 PC 独立显卡（UEFI 模式）
 *
 * 若将本类误用于 BGR 格式帧缓冲区：
 *   红色和蓝色分量互换，纯绿色（G=255）不受影响（G 字节位置不变），
 *   混合颜色（如黄色、青色）会变色。
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
   * 本类无额外成员，继承构造函数完全足够。
   */
  using PixelWriter::PixelWriter;
 
  /**
   * Write() — 以 RGB 字节序向 (x, y) 写入颜色 c
   *
   * p[0] ← c.r（红，低地址）
   * p[1] ← c.g（绿）
   * p[2] ← c.b（蓝，高地址）
   * p[3]    不修改（保留字节，对显示无影响）
   */
  virtual void Write(int x, int y, const PixelColor& c) override {
    auto p = PixelAt(x, y);
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
  }
};
 
/**
 * BGRResv8BitPerColorPixelWriter — BGR 格式帧缓冲区的像素写入器
 *
 * 适用格式：PixelBlueGreenRedReserved8BitPerColor（GOP 枚举）
 *            对应 kPixelBGRResv8BitPerColor（内核枚举）
 *
 * 物理内存字节序（低地址→高地址）：[B][G][R][保留]
 * 典型硬件：部分 AMD 显卡、某些 QEMU/VirtualBox 虚拟 GOP 实现
 *
 * 与 RGB 版的唯一区别：p[0] 写蓝（c.b），p[2] 写红（c.r）——R/B 互换，G 不变。
 */
class BGRResv8BitPerColorPixelWriter : public PixelWriter {
 public:
  using PixelWriter::PixelWriter;
 
  /**
   * Write() — 以 BGR 字节序向 (x, y) 写入颜色 c
   *
   * p[0] ← c.b（蓝，低地址；BGR 格式蓝在前）
   * p[1] ← c.g（绿，同 RGB 格式）
   * p[2] ← c.r（红，高地址；BGR 格式红在后）
   */
  virtual void Write(int x, int y, const PixelColor& c) override {
    auto p = PixelAt(x, y);
    p[0] = c.b;
    p[1] = c.g;
    p[2] = c.r;
  }
};
 
/* ============================================================================
 * 五、WriteAscii — 位图字体渲染函数
 * ============================================================================ */
 
/**
 * WriteAscii — 在屏幕指定坐标渲染单个 ASCII 字符（当前仅支持 'A'）
 *
 * ── 参数说明 ────────────────────────────────────────────────────────────────
 *
 *   writer（PixelWriter&）
 *     像素写入器的引用；调用 writer.Write() 通过虚函数自动适配 RGB/BGR 格式。
 *     用引用而非指针：语义上 writer 必须存在（不能为 null），省去空检查。
 *
 *   x, y（int）
 *     字符左上角在屏幕上的像素坐标（x=列，y=行）。
 *     字符向右延伸 8 像素（dx = 0..7），向下延伸 16 像素（dy = 0..15）。
 *
 *   c（char）
 *     要渲染的 ASCII 字符码。当前版本仅处理 'A'，其他字符直接返回。
 *     此设计为"最小可行实现"——先验证位图渲染管线正确，后续扩展字体数组。
 *
 *   color（const PixelColor&）
 *     字符前景色（笔画颜色）；背景色保持不变（调用前已绘制背景）。
 *     通过引用传递避免拷贝三字节结构体（虽然代价极小，引用更显意图）。
 *
 * ── 位图渲染算法详解 ─────────────────────────────────────────────────────────
 *
 *   外层循环 dy（0..15）：遍历字符的每一行（对应 kFontA[dy] 的各字节）
 *   内层循环 dx（0..7） ：遍历该行的每一列（对应字节中的各个位）
 *
 *   核心判断式：(kFontA[dy] << dx) & 0x80u
 *
 *     kFontA[dy] 的位排列（以 0b00011000 为例）：
 *       bit7 bit6 bit5 bit4 bit3 bit2 bit1 bit0
 *        0    0    0    1    1    0    0    0
 *       ↑                                    ↑
 *     最左列(dx=0)                       最右列(dx=7)
 *
 *     原理：将字节左移 dx 位，使"第 dx 列对应的位"移到 bit7（最高位）位置，
 *           再与 0x80u（= 1000 0000）按位与，检查 bit7 是否为 1。
 *
 *     逐步验证（以 0b00011000 为例）：
 *       dx=0: 0b00011000 << 0 = 0b00011000, & 0x80 = 0    → 不亮
 *       dx=1: 0b00011000 << 1 = 0b00110000, & 0x80 = 0    → 不亮
 *       dx=2: 0b00011000 << 2 = 0b01100000, & 0x80 = 0    → 不亮
 *       dx=3: 0b00011000 << 3 = 0b11000000, & 0x80 = 0x80 → 点亮(column 3)
 *       dx=4: 0b00011000 << 4 = 0x180,      & 0x80 = 0x80 → 点亮(column 4)
 *             ↑ uint8_t 提升为 int 后可左移超过 8 位，0x180 的 bit7 仍为 1
 *       dx=5: 0b00011000 << 5 = 0x300,      & 0x80 = 0    → 不亮
 *       dx=6: 0b00011000 << 6 = 0x600,      & 0x80 = 0    → 不亮
 *       dx=7: 0b00011000 << 7 = 0xC00,      & 0x80 = 0    → 不亮
 *     结果：在 dx=3 和 dx=4 处点亮，与注释 "    **" 完全吻合。
 *
 *     等价写法（更直观但稍慢）：
 *       if (kFontA[dy] & (0x80u >> dx))  // 将掩码右移对齐到目标位
 *     两种写法结果完全相同；左移字节版更常见（字体渲染惯例）。
 *
 * ── 实际屏幕坐标映射 ─────────────────────────────────────────────────────────
 *
 *   渲染第 dy 行第 dx 列的像素时，调用：
 *     writer.Write(x + dx, y + dy, color)
 *   即字符左上角 (x, y) + 局部偏移 (dx, dy) = 屏幕绝对坐标。
 */
// #@@range_begin(write_ascii)
void WriteAscii(PixelWriter& writer, int x, int y, char c, const PixelColor& color) {
  // 当前只有 'A' 的字体数据；其他字符码直接返回（静默跳过）。
  // 这是有意的"最小实现"：后续将扩展为完整 ASCII 字体数组后删除此守卫。
  if (c != 'A') {
    return;
  }
 
  // 遍历字符的 16 行（dy = 字符内行偏移，0 = 顶行，15 = 底行）
  for (int dy = 0; dy < 16; ++dy) {
    // 遍历该行的 8 列（dx = 字符内列偏移，0 = 最左列，7 = 最右列）
    for (int dx = 0; dx < 8; ++dx) {
      // 位图提取：将 kFontA[dy] 左移 dx 位，检查最高位（bit7）是否为 1。
      // 若为 1，表示字符在 (dx, dy) 处有笔画，向屏幕 (x+dx, y+dy) 写入前景色。
      // 若为 0，不写入（保留背景色）。
      if ((kFontA[dy] << dx) & 0x80u) {
        writer.Write(x + dx, y + dy, color);
      }
    }
  }
}
// #@@range_end(write_ascii)
 
/* ============================================================================
 * 六、Placement New / Delete 操作符重载
 * ============================================================================
 *
 * 内核此时没有堆（无 malloc / operator new 的默认实现），
 * 直接写 new T{} 会导致链接错误（找不到 operator new）。
 *
 * Placement new（new(buf) T{args}）在调用方提供的地址上原地构造对象：
 *   不分配内存，仅执行构造函数，适合内核静态缓冲区场景。
 *
 * operator delete 必须与 operator new 配对声明，否则当构造函数抛出异常时
 * 编译器找不到析构路径，产生编译/链接错误。此处实现为空（不释放任何内存）。
 */
 
/**
 * operator new(size_t, void*) — Placement new 操作符
 *
 * 标准 placement new 签名，直接返回 buf（不分配内存）。
 * 编译器随后在返回地址处调用对象的构造函数。
 *
 * 调用方责任：buf 必须指向足够大且对齐正确的内存
 *   （sizeof(T) 字节，alignof(T) 对齐）。
 */
void* operator new(size_t size, void* buf) {
  return buf;
}
 
/**
 * operator delete(void*) noexcept — 配对的释放操作符（空实现）
 *
 * 此函数实际上永远不会被调用（内核不退出、对象不析构）。
 * 保留声明仅为满足 C++ 规范要求：有 new 则必须有匹配的 delete。
 */
void operator delete(void* obj) noexcept {
}
 
/* ============================================================================
 * 七、全局像素写入器存储
 * ============================================================================ */
 
/**
 * pixel_writer_buf — 存放 PixelWriter 派生对象的静态字节缓冲区
 *
 * 大小取 sizeof(RGBResv8BitPerColorPixelWriter)。
 * 两个派生类大小相同（均只有 vtable 指针 + 继承的 config_ 引用），
 * 因此该缓冲区足以容纳任意一种格式的写入器对象。
 *
 * 全局（而非栈上）分配的原因：
 *   KernelMain 永不返回，但若将来中断处理程序调用写入器，
 *   全局变量生命周期贯穿内核全程，不存在悬空指针风险。
 *
 * 潜在改进：应加 alignas(RGBResv8BitPerColorPixelWriter) 以确保
 * vtable 指针所需的 8 字节对齐（当前依赖编译器对全局变量的默认对齐行为）。
 */
char pixel_writer_buf[sizeof(RGBResv8BitPerColorPixelWriter)];
 
/**
 * pixel_writer — 指向当前像素写入器对象的全局指针
 *
 * 声明为抽象基类指针（PixelWriter*），通过 vtable 虚函数分发：
 *   pixel_writer->Write(x, y, c)
 *   → RGBResv8BitPerColorPixelWriter::Write()（RGB 硬件）
 *   或 BGRResv8BitPerColorPixelWriter::Write()（BGR 硬件）
 *
 * 初始值为 nullptr（BSS 段全零），KernelMain 中 switch 必须在
 * 任何 Write 调用之前通过 placement new 赋予有效对象地址。
 * WriteAscii 通过引用接受 *pixel_writer，因此也依赖此指针有效。
 */
PixelWriter* pixel_writer;
 
/* ============================================================================
 * 八、KernelMain — 内核入口函数
 * ============================================================================ */
 
/**
 * KernelMain — MikanOS 内核主入口点（由 UEFI 引导程序跳转至此）
 *
 * ── extern "C" 说明 ──────────────────────────────────────────────────────
 *
 *   抑制 C++ 名称修饰（name mangling），确保链接符号为 "KernelMain"，
 *   与引导程序从 ELF e_entry 读取的入口地址对应的函数符号匹配。
 *
 * ── 参数说明 ─────────────────────────────────────────────────────────────
 *
 *   frame_buffer_config（const FrameBufferConfig&）
 *     由引导程序填充的帧缓冲区配置，通过 System V AMD64 ABI 的 RDI 寄存器传入。
 *     C++ 引用（&）与 C 指针（*）在该 ABI 中传参方式相同（均传地址）。
 *
 * ── 运行流程（四个阶段）──────────────────────────────────────────────────
 *
 *   阶段 1  根据像素格式构造正确的 PixelWriter 子类（placement new）
 *   阶段 2  全屏填充白色（清屏，验证显示系统可用）
 *   阶段 3  在左上角绘制 200×100 绿色矩形（第一个有意义的图形输出）
 *   阶段 4  在绿色矩形上叠加绘制两个黑色 'A'（第一个文字渲染输出）
 *   阶段 5  进入 HLT 无限循环（等待后续内核功能实现）
 *
 * ── 返回值 ───────────────────────────────────────────────────────────────
 *
 *   声明为 void，实际上永远不会返回（末尾为 while(1) HLT 无限循环）。
 */
extern "C" void KernelMain(const FrameBufferConfig& frame_buffer_config) {
 
  /* ── 阶段 1：根据像素格式构造 PixelWriter ────────────────────────────── */
 
  // 像素格式（RGB vs BGR）由显卡硬件决定，只有运行时查询 GOP 才能得知，
  // 因此必须在运行时判断，通过多态（虚函数）将格式差异封装在子类中。
  //
  // Placement new 语法：new(地址) 类型{构造参数}
  //   ① 调用上方 operator new(size, buf)，返回 pixel_writer_buf
  //   ② 在 pixel_writer_buf 处调用子类构造函数，初始化 vtable + config_ 引用
  //   ③ 将对象地址赋给全局 pixel_writer，后续所有 Write 调用通过此指针分发
  switch (frame_buffer_config.pixel_format) {
    case kPixelRGBResv8BitPerColor:
      // 帧缓冲字节序：[R][G][B][保留]
      pixel_writer = new(pixel_writer_buf)
        RGBResv8BitPerColorPixelWriter{frame_buffer_config};
      break;
    case kPixelBGRResv8BitPerColor:
      // 帧缓冲字节序：[B][G][R][保留]
      pixel_writer = new(pixel_writer_buf)
        BGRResv8BitPerColorPixelWriter{frame_buffer_config};
      break;
  }
 
  /* ── 阶段 2：全屏填充白色（清屏） ───────────────────────────────────── */
 
  // 遍历屏幕每个坐标 (x, y)，写入纯白 {255, 255, 255}。
  // 外层 x（列）、内层 y（行）的顺序：所有坐标均会访问，正确性无影响；
  // 若交换为外 y 内 x，帧缓冲缓存局部性更好（按行扫描），当前可接受。
  for (int x = 0; x < frame_buffer_config.horizontal_resolution; ++x) {
    for (int y = 0; y < frame_buffer_config.vertical_resolution; ++y) {
      pixel_writer->Write(x, y, {255, 255, 255});
    }
  }
 
  /* ── 阶段 3：绘制 200×100 绿色矩形 ──────────────────────────────────── */
 
  // 从 (0, 0)（左上角）开始，绘制宽 200、高 100 像素的纯绿色矩形。
  // {0, 255, 0}：R=0、G=255、B=0 → 纯绿。
  // 此矩形是内核的"Hello, World"图形版，用于验证像素格式选择正确。
  // （纯绿在 RGB/BGR 两种格式下视觉效果相同，因为 G 字节位置在两种格式中一致；
  //  更严格的验证应用不对称颜色，如纯红 {255,0,0}。）
  for (int x = 0; x < 200; ++x) {
    for (int y = 0; y < 100; ++y) {
      pixel_writer->Write(x, y, {0, 255, 0});
    }
  }
 
  /* ── 阶段 4：在绿色矩形上叠加绘制两个黑色 'A' ───────────────────────── */
 
  // #@@range_begin(write_aa)
  // 在 (50, 50) 处绘制第一个 'A'（宽 8px，高 16px）
  //   前景色 {0, 0, 0} = 纯黑，与绿色背景形成高对比度，字形清晰可辨。
  //   WriteAscii 内部仅在位图为 1 的位置写像素，背景绿色保留。
  WriteAscii(*pixel_writer, 50, 50, 'A', {0, 0, 0});
 
  // 在 (58, 50) 处绘制第二个 'A'（与第一个相距 8 像素 = 恰好一个字符宽度）
  //   两个字符紧密排列，演示字符间无额外间距时的效果（类似等宽字体列布局）。
  //   若要增加字间距，改为 x = 50 + 8 + 间距值（如 2 像素：x = 60）。
  WriteAscii(*pixel_writer, 58, 50, 'A', {0, 0, 0});
  // #@@range_end(write_aa)
 
  /* ── 阶段 5：HLT 无限停机循环 ───────────────────────────────────────── */
 
  // while (1) __asm__("hlt")：CPU 无限停机循环
  //
  // HLT 指令使 CPU 进入低功耗停止状态，直到下一个外部中断唤醒；
  // 唤醒后再次执行 HLT，形成"唤醒—再停机"的低功耗等待循环。
  //
  // 为何不用空循环 while(1)？
  //   ① 空循环以全速运行（100% CPU 占用），浪费电能
  //   ② HLT 将 CPU 置于 C1 或更深的功耗状态，符合操作系统最佳实践
  //
  // IDT（中断描述符表）尚未配置，外部中断唤醒 CPU 后若无处理程序，
  // 将触发三重故障（Triple Fault）使 CPU 自动重置——早期内核可接受，
  // 后续实现 IDT 后将正确处理中断。
  while (1) __asm__("hlt");
}