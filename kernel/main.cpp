/**
 * @file main.cpp
 *
 * MikanOS カーネル本体のプログラムを書いたファイル．
 *
 * ── 文件职责概述 ─────────────────────────────────────────────────────────────
 *
 *   本文件是 MikanOS 内核的入口与像素绘图子系统，提供：
 *     ① PixelColor          — RGB 颜色结构体（与格式无关的逻辑颜色）
 *     ② PixelWriter         — 抽象基类，封装"向帧缓冲区某坐标写像素"的接口
 *     ③ RGBResv8BitPerColorPixelWriter — 针对 RGB 帧缓冲格式的具体实现
 *     ④ BGRResv8BitPerColorPixelWriter — 针对 BGR 帧缓冲格式的具体实现
 *     ⑤ Placement new / delete 操作符重载 — 在静态缓冲区中原地构造 C++ 对象
 *     ⑥ KernelMain          — 内核入口函数（由 UEFI 引导程序跳转到此处）
 *
 * ── 设计关键点：为什么用多态而非 if-else？ ──────────────────────────────────
 *
 *   帧缓冲区的像素字节序（RGB vs BGR）在运行时才能确定（取决于 GOP 硬件），
 *   但绘图代码（填充白色、画绿色矩形）是与格式无关的。
 *   通过抽象基类 PixelWriter + 虚函数 Write()，绘图代码只需调用
 *     pixel_writer->Write(x, y, color)
 *   无需在每次绘图时都判断像素格式，实现"格式判断一次、绘图多次"。
 *
 * ── 调用约定说明 ─────────────────────────────────────────────────────────────
 *
 *   UEFI 引导程序使用 Microsoft x64 ABI（EFIAPI），
 *   内核使用 System V AMD64 ABI（Linux 标准）；
 *   两者参数寄存器不同（RCX/RDX vs RDI/RSI）。
 *   KernelMain 使用 System V AMD64 ABI（C++ 默认），
 *   引导程序通过函数指针调用时必须显式声明 FrameBufferConfig* 参数，
 *   由 System V ABI 的 RDI 寄存器传递。
 */
 
#include <cstdint>   /* uint8_t / uint32_t 等固定宽度整数类型（C++ 标准头） */
#include <cstddef>   /* size_t（operator new 参数类型，placement new 必须包含） */
 
#include "frame_buffer_config.hpp"  /* FrameBufferConfig 结构体 + PixelFormat 枚举，
                                     * 与引导程序共用同一份头文件，保证布局完全兼容 */
 
/* ============================================================================
 * 一、PixelColor — 与帧缓冲格式无关的逻辑颜色结构体
 * ============================================================================ */
 
/**
 * PixelColor — 以 RGB 分量表示的颜色（各 8 位，0–255）
 *
 * 这是"逻辑颜色"——与帧缓冲区的物理字节序无关。
 * 无论底层格式是 RGB 还是 BGR，调用方统一用此结构体描述颜色，
 * 由 PixelWriter 的具体子类负责将逻辑颜色转换为正确的物理字节序写入内存。
 *
 * 字段对齐与大小：
 *   r / g / b 均为 uint8_t（1 字节），连续排列，无填充，sizeof(PixelColor) == 3。
 *   在函数调用时以值传递（3 字节），编译器通常通过寄存器高效传递。
 *
 * 使用示例：
 *   PixelColor white{255, 255, 255};  // 纯白
 *   PixelColor green{0,   255,   0};  // 纯绿
 *   PixelColor black{0,     0,   0};  // 纯黑
 */
struct PixelColor {
  uint8_t r;  /* 红色分量（Red），0 = 无红，255 = 最亮红 */
  uint8_t g;  /* 绿色分量（Green），0 = 无绿，255 = 最亮绿 */
  uint8_t b;  /* 蓝色分量（Blue），0 = 无蓝，255 = 最亮蓝 */
};
 
/* ============================================================================
 * 二、PixelWriter — 像素写入抽象基类
 * ============================================================================
 *
 * 职责：
 *   封装"将 PixelColor 写入帧缓冲区指定坐标"的操作，
 *   通过纯虚函数 Write() 定义接口，由子类实现具体格式的字节序转换。
 *
 * 继承关系：
 *   PixelWriter（抽象基类）
 *   ├── RGBResv8BitPerColorPixelWriter  帧缓冲字节序 R→G→B→保留
 *   └── BGRResv8BitPerColorPixelWriter  帧缓冲字节序 B→G→R→保留
 *
 * 生命周期管理：
 *   对象通过 placement new 构造在全局静态缓冲区 pixel_writer_buf 中，
 *   永远不会被析构（内核不退出），因此不依赖堆内存分配器。
 */
// #@@range_begin(pixel_writer)
class PixelWriter {
 public:
  /**
   * 构造函数 — 接受帧缓冲区配置的常量引用
   *
   * 参数 config 是对 KernelMain 局部变量的引用，其生命周期覆盖内核整个运行期，
   * 因此引用持有是安全的（内核不会退出）。
   *
   * 初始化列表 config_{config} 将引用绑定到私有成员 config_，
   * 子类无需再接收 FrameBufferConfig，只需通过继承构造函数即可访问配置。
   *
   * 设计说明：使用引用（const FrameBufferConfig&）而非指针（const FrameBufferConfig*）：
   *   ① 引用不能为 null，省去空指针检查
   *   ② 语义上更清晰：PixelWriter 依赖配置但不拥有配置
   *   ③ const 保证 PixelWriter 不会意外修改配置内容
   */
  PixelWriter(const FrameBufferConfig& config) : config_{config} {
  }
 
  /**
   * 虚析构函数（= default）
   *
   * 虚析构函数是多态基类的必要元素：当通过基类指针 PixelWriter* 删除对象时，
   * 确保调用正确的派生类析构函数，避免资源泄漏。
   *
   * = default：让编译器生成默认实现（什么都不做的析构函数体），
   * 同时保留"虚"特性（vtable 中有此条目）。
   *
   * 在本项目中虽然不会实际 delete 对象（使用静态缓冲区），
   * 但仍声明虚析构是最佳实践——防止将来用 delete 时出现未定义行为。
   */
  virtual ~PixelWriter() = default;
 
  /**
   * Write() — 纯虚函数：向 (x, y) 坐标写入颜色 c（子类必须实现）
   *
   * 参数：
   *   x — 横坐标，0 = 最左列，向右增大
   *   y — 纵坐标，0 = 最顶行，向下增大
   *   c — 要写入的逻辑颜色（RGB 分量），子类负责转换为物理字节序
   *
   * 纯虚函数（= 0）使 PixelWriter 成为抽象类，无法直接实例化。
   * 这是一种强制约束：所有子类必须实现 Write()，否则编译报错。
   *
   * override（子类用）：向编译器声明"此函数覆盖了基类虚函数"，
   * 若基类中无匹配签名的虚函数，编译器报错，防止拼写错误导致的静默 bug。
   */
  virtual void Write(int x, int y, const PixelColor& c) = 0;
 
 protected:
  /**
   * PixelAt() — 计算 (x, y) 坐标对应的帧缓冲区字节地址（受保护辅助方法）
   *
   * 返回值：指向帧缓冲区中 (x, y) 像素第一个字节的指针
   *
   * 地址计算公式：
   *   frame_buffer + 4 × (pixels_per_scan_line × y + x)
   *
   *   ┌─── 关键：为什么用 pixels_per_scan_line 而非 horizontal_resolution？ ───┐
   *   │                                                                          │
   *   │  帧缓冲区在物理内存中的布局（每行）：                                   │
   *   │                                                                          │
   *   │  [像素0][像素1]...[像素 HR-1][填充0]...[填充 P-HR-1]                    │
   *   │  |←── horizontal_resolution (HR) ──→||←── 硬件行末对齐填充 ───────→|   │
   *   │  |←──────── pixels_per_scan_line (P) ─────────────────────────────→|   │
   *   │                                                                          │
   *   │  pixels_per_scan_line (P) ≥ horizontal_resolution (HR)                 │
   *   │  行与行之间存在对齐填充像素（GPU 硬件缓存/DMA 要求），                   │
   *   │  若错误使用 HR 计算行首地址：                                            │
   *   │    第 2 行误认为从 HR×4 字节处开始                                       │
   *   │    实际应从 P×4 字节处开始                                               │
   *   │  结果：图像从第二行起产生水平方向的错位（画面整体向左"斜切"）            │
   *   │                                                                          │
   *   └──────────────────────────────────────────────────────────────────────────┘
   *
   *   × 4：每像素 4 字节（R / G / B / 保留各 1 字节，与 RGB/BGR 格式均适用）
   *
   * 访问权限（protected）：
   *   子类 RGBResv8BitPerColorPixelWriter / BGRResv8BitPerColorPixelWriter 通过
   *   调用此方法获取像素地址，再按各自格式填写 p[0] / p[1] / p[2] / p[3]。
   *   外部代码无需（也不应）直接操作字节地址，故不设为 public。
   */
  uint8_t* PixelAt(int x, int y) {
    return config_.frame_buffer + 4 * (config_.pixels_per_scan_line * y + x);
  }
 
 private:
  /**
   * config_ — 帧缓冲区配置的常量引用（私有成员）
   *
   * 持有引用而非副本：
   *   ① FrameBufferConfig 含指针 + 四个整数，复制没有实质问题，
   *     但引用语义明确表示"借用，不拥有"
   *   ② 若将来引导程序修改配置（如分辨率切换），引用自动反映最新值
   *      （当前版本不存在此场景，但引用设计更具扩展性）
   *
   * 访问权限（private）：
   *   子类无需直接访问 config_，所有地址计算已由 protected PixelAt() 封装。
   *   私有化防止子类绕过 PixelAt 直接操作内存导致格式错误。
   */
  const FrameBufferConfig& config_;
};
// #@@range_end(pixel_writer)
 
/* ============================================================================
 * 三、具体像素写入器：RGB 和 BGR 两种格式实现
 * ============================================================================ */
 
// #@@range_begin(derived_pixel_writer)
 
/**
 * RGBResv8BitPerColorPixelWriter — RGB 格式帧缓冲区的像素写入器
 *
 * 适用帧缓冲格式：PixelRedGreenBlueReserved8BitPerColor（GOP 枚举值）
 *                  对应 kPixelRGBResv8BitPerColor（内核枚举值）
 *
 * 物理内存字节序（从低地址到高地址）：
 *   [R][G][B][保留]
 *   p[0] = 红  p[1] = 绿  p[2] = 蓝  p[3] = 不使用（保持原值即可）
 *
 * 此格式是大多数现代 PC 独立显卡的默认格式，
 * 也与 HTML/CSS 中 #RRGGBB 的字节顺序直觉一致。
 *
 * 典型硬件：Intel 核显、NVIDIA GeForce（UEFI 模式下）
 */
class RGBResv8BitPerColorPixelWriter : public PixelWriter {
 public:
  /**
   * using PixelWriter::PixelWriter — 继承构造函数（C++11 特性）
   *
   * 使基类 PixelWriter(const FrameBufferConfig&) 构造函数对本类可见，
   * 等价于手写：
   *   RGBResv8BitPerColorPixelWriter(const FrameBufferConfig& config)
   *       : PixelWriter(config) {}
   *
   * 使用 using 继承避免重复代码，尤其是当有多个子类时效果显著。
   * 子类自身无额外成员变量需要初始化，因此继承构造函数完全足够。
   */
  using PixelWriter::PixelWriter;
 
  /**
   * Write() — 以 RGB 字节序向帧缓冲区 (x, y) 写入颜色 c
   *
   * 步骤：
   *   1. 调用基类 PixelAt(x, y) 计算目标像素的字节指针 p
   *   2. 按 [R][G][B] 顺序填写 p[0] / p[1] / p[2]
   *   3. p[3]（保留字节）不写入，保持显卡/固件原有值（对显示无影响）
   *
   * override：向编译器声明覆盖基类纯虚函数，编译器验证签名匹配。
   */
  virtual void Write(int x, int y, const PixelColor& c) override {
    auto p = PixelAt(x, y);
    p[0] = c.r;  /* 低地址：红色分量 */
    p[1] = c.g;  /* 中地址：绿色分量 */
    p[2] = c.b;  /* 高地址：蓝色分量 */
    /* p[3]（保留/Alpha）不修改 */
  }
};
 
/**
 * BGRResv8BitPerColorPixelWriter — BGR 格式帧缓冲区的像素写入器
 *
 * 适用帧缓冲格式：PixelBlueGreenRedReserved8BitPerColor（GOP 枚举值）
 *                  对应 kPixelBGRResv8BitPerColor（内核枚举值）
 *
 * 物理内存字节序（从低地址到高地址）：
 *   [B][G][R][保留]
 *   p[0] = 蓝  p[1] = 绿  p[2] = 红  p[3] = 不使用
 *
 * R 和 B 字节相对于 RGB 格式互换，G 字节位置不变。
 * 若误用 RGBResv8BitPerColorPixelWriter 写入此格式的帧缓冲区，
 * 红色和蓝色会互换（红色画面变蓝、蓝色画面变红）。
 *
 * 典型硬件：部分 AMD 显卡、某些 QEMU/VirtualBox 虚拟 GOP 实现
 */
class BGRResv8BitPerColorPixelWriter : public PixelWriter {
 public:
  using PixelWriter::PixelWriter;  /* 同 RGB 版，继承基类构造函数 */
 
  /**
   * Write() — 以 BGR 字节序向帧缓冲区 (x, y) 写入颜色 c
   *
   * 与 RGBResv8BitPerColorPixelWriter::Write 的唯一区别：
   *   p[0] 写蓝（c.b），p[2] 写红（c.r）—— R/B 互换，G 不变。
   */
  virtual void Write(int x, int y, const PixelColor& c) override {
    auto p = PixelAt(x, y);
    p[0] = c.b;  /* 低地址：蓝色分量（BGR 格式蓝在前） */
    p[1] = c.g;  /* 中地址：绿色分量（与 RGB 相同） */
    p[2] = c.r;  /* 高地址：红色分量（BGR 格式红在后） */
    /* p[3]（保留）不修改 */
  }
};
// #@@range_end(derived_pixel_writer)
 
/* ============================================================================
 * 四、Placement New / Delete 操作符重载
 * ============================================================================
 *
 * 背景——为什么需要 placement new？
 *
 *   标准 C++ 的 new 表达式（operator new）从堆（自由存储区）分配内存，
 *   但内核此时没有堆：没有 malloc、没有 brk/sbrk 系统调用实现。
 *   若直接写 pixel_writer = new RGBResv8BitPerColorPixelWriter{...}，
 *   链接时会报错找不到 operator new 的定义。
 *
 *   Placement new 是 C++ 标准的一种特殊 new 形式：
 *     new(buffer_ptr) Type{args}
 *   它在调用者提供的内存地址 buffer_ptr 上原地构造 Type 对象，
 *   不进行任何内存分配，仅执行构造函数。
 *
 * 两个重载的作用分工：
 *   operator new(size, buf)   → placement new 调用路径，返回 buf 本身
 *   operator delete(obj)      → 匹配的释放操作符（空实现，不释放任何内存）
 */
 
// #@@range_begin(placement_new)
 
/**
 * operator new(size_t size, void* buf) — Placement new 内存"分配"操作符
 *
 * 这是全局 placement new 操作符的标准签名，定义在 <new> 头文件中。
 * 因内核不链接标准库，需手动提供此重载；否则 new(buf) T{} 语法无法编译。
 *
 * 参数：
 *   size — 对象所需字节数（由编译器自动传入，sizeof(T)）；此处不使用，
 *           仅保留以匹配标准 placement new 签名
 *   buf  — 目标内存地址（由调用方保证足够大且对齐正确）
 *
 * 返回值：buf 本身（编译器随后在返回的地址上调用构造函数）
 *
 * 关键前提（调用方责任）：
 *   buf 指向的内存必须满足：
 *     ① 大小 ≥ sizeof(T)
 *     ② 对齐 ≥ alignof(T)
 *   违反任一条件均为未定义行为（UB），可能导致对象字段写入错误地址。
 *
 *   本项目中 pixel_writer_buf 被声明为 char 数组，大小 = sizeof(RGBResv8BitPerColorPixelWriter)，
 *   对齐默认为 char（1 字节），可能不满足 PixelWriter（含 vtable 指针，需 8 字节对齐）。
 *   实践中编译器通常将全局 char 数组对齐到足够边界，但严格正确的做法是
 *   使用 alignas(RGBResv8BitPerColorPixelWriter) char pixel_writer_buf[...] 显式指定对齐。
 */
void* operator new(size_t size, void* buf) {
  return buf;  /* 直接返回调用方提供的缓冲区地址，不分配任何新内存 */
}
 
/**
 * operator delete(void* obj) noexcept — 与 placement new 配对的释放操作符（空实现）
 *
 * C++ 规范要求：每个 operator new 重载必须有对应的 operator delete 重载，
 * 否则当构造函数抛出异常时，编译器找不到匹配的释放函数，导致编译错误或
 * 链接错误（"undefined reference to operator delete"）。
 *
 * noexcept：声明此操作符不会抛出任何异常（标准 delete 的要求）。
 *
 * 实现为空的原因：
 *   Placement new 在已有内存上构造对象，"释放"时不应 free 该内存
 *   （该内存不是由 operator new 分配的，释放会损坏 BSS 或其他数据）。
 *   内核中对象生命周期与内核本身相同，永不需要析构，因此此函数实际上
 *   永远不会被调用。
 */
void operator delete(void* obj) noexcept {
  /* 意图为空：placement new 的配对 delete 不做任何操作 */
}
// #@@range_end(placement_new)
 
/* ============================================================================
 * 五、全局像素写入器存储
 * ============================================================================ */
 
/**
 * pixel_writer_buf — 用于原地存放 PixelWriter 派生对象的静态字节缓冲区
 *
 * 大小选为 sizeof(RGBResv8BitPerColorPixelWriter)（两个派生类大小相同，
 * 均只有 vtable 指针 + 继承自基类的 config_ 引用，sizeof 值一致）。
 *
 * 选用静态（全局）字节数组而非栈内存的原因：
 *   ① KernelMain 永不返回（末尾是 while(1) HLT），但若返回，栈帧销毁
 *      后 pixel_writer 将成为悬空指针，指向已失效内存 —— 极难调试。
 *   ② 全局变量位于 BSS 段（未初始化）或 DATA 段，生命周期贯穿内核全程，
 *      与 pixel_writer 指针的预期使用周期完全匹配。
 *
 * 潜在改进（生产级内核）：
 *   应加 alignas(RGBResv8BitPerColorPixelWriter) 确保 8 字节对齐，
 *   以满足 vtable 指针对齐要求（当前依赖编译器的全局变量默认对齐行为）。
 */
char pixel_writer_buf[sizeof(RGBResv8BitPerColorPixelWriter)];
 
/**
 * pixel_writer — 指向当前像素写入器对象的全局指针
 *
 * 声明为抽象基类指针（PixelWriter*），通过虚函数分发机制调用正确的 Write()：
 *   pixel_writer->Write(x, y, c)
 *   ↓ 运行时 vtable 查找
 *   → RGBResv8BitPerColorPixelWriter::Write()（RGB 硬件）
 *   或 BGRResv8BitPerColorPixelWriter::Write()（BGR 硬件）
 *
 * 初始值为 nullptr（BSS 段全零初始化），KernelMain 中 switch 语句
 * 在任何 Write 调用之前必须通过 placement new 赋予有效对象地址。
 *
 * 为什么用全局指针而非局部变量？
 *   KernelMain 末尾是无限循环，将来可能在中断处理程序（ISR）或
 *   其他内核子系统中调用 pixel_writer->Write()；
 *   全局指针使整个内核都能访问唯一的写入器实例，无需传递指针参数。
 */
PixelWriter* pixel_writer;
 
/* ============================================================================
 * 六、KernelMain — 内核入口函数
 * ============================================================================ */
 
// #@@range_begin(call_pixel_writer)
 
/**
 * KernelMain — MikanOS 内核主入口点（由 UEFI 引导程序跳转至此）
 *
 * ── extern "C" 说明 ────────────────────────────────────────────────────────
 *
 *   C++ 编译器默认对函数名进行"名称修饰"（name mangling），将参数类型
 *   编码进符号名（如 _ZN9KernelMainERK17FrameBufferConfig），使函数重载成为可能。
 *   但引导程序（C 语言编写）通过 ELF e_entry 跳转到此函数，
 *   它按字节序读取入口地址，无法知道修饰后的符号名。
 *
 *   extern "C" 抑制名称修饰，确保链接符号为 "KernelMain"，
 *   与链接脚本中 ENTRY(KernelMain) 或默认的 e_entry 解析结果匹配。
 *
 * ── 参数说明 ───────────────────────────────────────────────────────────────
 *
 *   frame_buffer_config（const FrameBufferConfig&）
 *     由引导程序在调用前填充的帧缓冲区配置：
 *       frame_buffer         — 帧缓冲区字节指针（MMIO 物理地址，ExitBootServices 后依然有效）
 *       pixels_per_scan_line — 每行物理步长（含行末对齐填充）
 *       horizontal_resolution — 屏幕逻辑宽度（像素）
 *       vertical_resolution   — 屏幕逻辑高度（像素）
 *       pixel_format          — kPixelRGBResv8BitPerColor 或 kPixelBGRResv8BitPerColor
 *
 *   C++ 引用（&）与 C 指针（*）在 System V AMD64 ABI 中传参方式相同：
 *   均通过 RDI 寄存器传递对象地址；引导程序中将指针传入 RDI 即可。
 *
 * ── 返回值 ─────────────────────────────────────────────────────────────────
 *
 *   声明为 void，但实际上永远不会返回：
 *   函数末尾有 while(1) __asm__("hlt") 无限停机循环。
 *   若 CPU 意外执行到 HLT 后续指令（外部中断唤醒），再次进入 HLT。
 *
 * ── 运行流程（三个阶段）──────────────────────────────────────────────────
 *
 *   阶段 1：根据像素格式选择并构造正确的 PixelWriter 子类对象
 *   阶段 2：全屏填充白色（验证显示系统可用）
 *   阶段 3：在左上角 (0,0) 绘制 200×100 绿色矩形（"Hello, World"的图形版本）
 *   阶段 4：进入 HLT 无限循环（等待后续内核功能实现）
 */
extern "C" void KernelMain(const FrameBufferConfig& frame_buffer_config) {
 
  /* ── 阶段 1：根据帧缓冲区像素格式，构造对应的 PixelWriter 子类 ──────── */
 
  /**
   * switch 在运行时判断像素格式，选择正确的写入器子类。
   *
   * 为何必须在运行时判断而非编译时选择？
   *   像素格式（RGB vs BGR）由显卡硬件决定，只有通过 GOP
   *   查询 gop->Mode->Info->PixelFormat 才能在运行时得知。
   *   无法在编译时将内核与特定格式绑定（内核需跨硬件通用）。
   *
   * Placement new 语法：new(地址) 类型{构造参数}
   *   ① 调用上方定义的 operator new(size, buf)，buf = pixel_writer_buf，返回 buf
   *   ② 在 buf 处调用 RGBResv8BitPerColorPixelWriter 构造函数，
   *      初始化 vtable 指针和 config_ 引用
   *   ③ 返回构造好的对象指针，赋给全局 pixel_writer
   *
   * 构造完成后，pixel_writer 指向 pixel_writer_buf 起始处的合法对象，
   * 调用 pixel_writer->Write() 将通过 vtable 分发到正确的子类实现。
   */
  switch (frame_buffer_config.pixel_format) {
    case kPixelRGBResv8BitPerColor:
      /* 帧缓冲字节序为 R→G→B→保留，在静态缓冲区中构造 RGB 写入器 */
      pixel_writer = new(pixel_writer_buf)
        RGBResv8BitPerColorPixelWriter{frame_buffer_config};
      break;
    case kPixelBGRResv8BitPerColor:
      /* 帧缓冲字节序为 B→G→R→保留，在静态缓冲区中构造 BGR 写入器 */
      pixel_writer = new(pixel_writer_buf)
        BGRResv8BitPerColorPixelWriter{frame_buffer_config};
      break;
  }
 
  /* ── 阶段 2：全屏填充白色（清屏） ───────────────────────────────────── */
 
  /**
   * 遍历屏幕上每一个像素坐标 (x, y)，写入纯白色 {255, 255, 255}。
   *
   * 循环范围：
   *   x ∈ [0, horizontal_resolution)  — 横轴，左到右
   *   y ∈ [0, vertical_resolution)    — 纵轴，上到下
   *
   * 为何外层循环是 x、内层是 y？
   *   并不影响正确性（所有坐标都会被访问），但若交换为外 y 内 x，
   *   内层循环按列扫描，CPU 缓存局部性更好（帧缓冲按行排列）。
   *   此处外 x 内 y 写法稍有缓存不友好，对于验证性清屏代码可接受。
   *
   * 此操作是引导程序全字节清白之后的"精确清屏"：
   *   引导程序：frame_buffer[i] = 255  逐字节写，覆盖所有字节（含行末填充）
   *   内核此处：按像素坐标写，仅覆盖逻辑可见区域，不触碰行末对齐填充字节
   * 两者结果在视觉上相同，但语义更严格。
   */
  for (int x = 0; x < frame_buffer_config.horizontal_resolution; ++x) {
    for (int y = 0; y < frame_buffer_config.vertical_resolution; ++y) {
      pixel_writer->Write(x, y, {255, 255, 255});  /* 纯白：R=255, G=255, B=255 */
    }
  }
 
  /* ── 阶段 3：在屏幕左上角绘制 200×100 绿色矩形 ─────────────────────── */
 
  /**
   * 从坐标 (0, 0)（左上角）开始，绘制宽 200 像素、高 100 像素的绿色矩形。
   *
   * 颜色 {0, 255, 0}：R=0（无红）、G=255（最亮绿）、B=0（无蓝）→ 纯绿
   *
   * 坐标范围：
   *   x ∈ [0, 200)  → 水平方向 200 个像素列
   *   y ∈ [0, 100)  → 垂直方向 100 个像素行
   *
   * 此矩形是内核的"Hello, World"——第一个有意义的图形输出，
   * 用于验证像素格式选择正确（若误选 BGR 写入 RGB 格式，绿色不变，
   * 但红/蓝混合颜色会反转；纯绿 G=255 在两种格式下视觉效果相同，
   * 因此更严格的验证应使用非对称颜色，如 {255, 0, 0} 红色）。
   */
  for (int x = 0; x < 200; ++x) {
    for (int y = 0; y < 100; ++y) {
      pixel_writer->Write(x, y, {0, 255, 0});  /* 纯绿矩形 */
    }
  }
 
  /* ── 阶段 4：HLT 无限停机循环 ───────────────────────────────────────── */
 
  /**
   * while (1) __asm__("hlt") — CPU 无限停机循环
   *
   * HLT（Halt）指令使 CPU 进入低功耗停止状态，直到下一个外部中断唤醒。
   * 被唤醒后（即使 IDT 尚未配置，某些中断如 NMI 仍会发生）再次执行 HLT，
   * 形成"唤醒—再停机"的低功耗等待循环。
   *
   * 为什么不直接 while(1)（空循环）？
   *   ① 空循环使 CPU 以全速运行（100% 占用），浪费电能、产生热量
   *   ② 空循环不给其他硬件线程（SMT/超线程）让出执行资源
   *   ③ HLT 将 CPU 置于 C1 或更深的功耗状态，符合操作系统的最佳实践
   *
   * 为什么不直接调用 _mm_pause() 或 PAUSE 指令？
   *   PAUSE 用于自旋锁的忙等待（缩短锁检查周期，避免内存排序违例惩罚），
   *   HLT 用于真正的"无事可做"状态；语义不同，此处 HLT 正确。
   *
   * 在 IDT（中断描述符表）配置前，外部中断唤醒 CPU 后若没有处理程序，
   * 将触发三重故障（Triple Fault）使 CPU 自动重置；
   * 当前阶段（早期内核）尚可接受，后续会配置 IDT 正确处理中断。
   */
  while (1) __asm__("hlt");
}
// #@@range_end(call_pixel_writer)
 