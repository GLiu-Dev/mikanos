#include "segment.hpp"
#include "asmfunc.h"

// #@@range_begin(gdt_definition)
/**
 * @brief 匿名命名空间：限制作用域仅当前 .cpp 文件
 * 外部无法 extern 访问 gdt，实现信息隐藏（模块化封装）
 *
 * std::array<SegmentDescriptor,3> gdt
 * GDT(全局描述符表)，一共3个表项：
 * gdt[0] : NULL 空描述符，CPU强制要求第一项必须为空，禁止使用选择子0
 * gdt[1] : 代码段描述符
 * gdt[2] : 数据段描述符
 * 每个 SegmentDescriptor 占8字节，整张GDT大小 = 3 × 8 = 24 Byte
 */
namespace {
  std::array<SegmentDescriptor, 3> gdt;
}
// #@@range_end(gdt_definition)

// #@@range_begin(setup_segm_function)
/**
 * @brief 填充代码段描述符通用函数
 * @param desc 待填充的段描述符
 * @param type 段访问属性（执行/可读等，DescriptorType枚举）
 * @param descriptor_privilege_level DPL 特权级 0~3，内核使用0
 * @param base 段基地址(32bit)，x86_64长模式统一设为0
 * @param limit 段界限（20位有效数值）
 *
 * 重点硬件规则：
 * long_mode=1 代表64位代码段；此时 default_operation_size(D位) 必须置0
 * granularity=1：界限单位为4KB
 */
void SetCodeSegment(SegmentDescriptor& desc,
                    DescriptorType type,
                    unsigned int descriptor_privilege_level,
                    uint32_t base,
                    uint32_t limit) {
  // 先把整个64位描述符清零，防止残留垃圾bit
  desc.data = 0;

  // 拆分32bit基地址 base → base_low[16] | base_middle[8] | base_high[8]
  desc.bits.base_low = base & 0xffffu;
  desc.bits.base_middle = (base >> 16) & 0xffu;
  desc.bits.base_high = (base >> 24) & 0xffu;

  // 拆分20bit界限 limit → limit_low[16] | limit_high[4]
  desc.bits.limit_low = limit & 0xffffu;
  desc.bits.limit_high = (limit >> 16) & 0xfu;

  // 段类型（可读、可执行、可写等标志）
  desc.bits.type = type;
  // system_segment=1 → 存储段（代码/数据段）；0代表系统段(TSS、LDT等)
  desc.bits.system_segment = 1;
  // DPL 特权级别
  desc.bits.descriptor_privilege_level = descriptor_privilege_level;
  // P=Present 有效位：1=该描述符合法可用
  desc.bits.present = 1;

  // AVL位：操作系统自由使用，CPU硬件无视，置0
  desc.bits.available = 0;
  // L=Long Mode 标志：1=64位长模式代码段
  desc.bits.long_mode = 1;
  // D/B位：long_mode=1时，此位必须为0（硬件规范）
  desc.bits.default_operation_size = 0;
  // G=粒度位：1 → limit单位 = 4KB
  desc.bits.granularity = 1;
}

/**
 * @brief 填充数据段描述符
 * @note 复用 SetCodeSegment 完成公共字段初始化，再修改数据段专属标志位
 *
 * 关键区别：
 * 数据段 **不能开启 long_mode(L=0)**
 * default_operation_size = 1：代表32位栈段（长模式下栈操作默认64bit，但遵循教材标准配置）
 */
void SetDataSegment(SegmentDescriptor& desc,
                    DescriptorType type,
                    unsigned int descriptor_privilege_level,
                    uint32_t base,
                    uint32_t limit) {
  // 先借用代码段初始化逻辑填充通用字段
  SetCodeSegment(desc, type, descriptor_privilege_level, base, limit);
  // 数据段禁止长模式标记
  desc.bits.long_mode = 0;
  // D/B=1：32位数据/栈段标记
  desc.bits.default_operation_size = 1;
}

/**
 * @brief GDT完整初始化入口函数（day08b对外唯一接口 SetupSegments()）
 * 执行流程：
 * 1. GDT[0] 设置为空描述符（CPU强制要求）
 * 2. GDT[1] 创建代码段：可执行可读，特权级0，基址0，界限0xFFFFF
 * 3. GDT[2] 创建数据段：可读可写，特权级0，基址0，界限0xFFFFF
 * 4. 调用汇编函数 LoadGDT，执行 lgdt 指令加载全局描述符表到CPU GDTR寄存器
 *
 * limit = 0xFFFFF，granularity=1：
 * 有效寻址范围 = 0xFFFFF * 4KB = 4GB
 */
void SetupSegments() {
  // 第0项：NULL描述符，全部bit置0，CPU规定选择子0禁止使用
  gdt[0].data = 0;

  // 代码段：执行+可读，Ring0，基址0，界限0xFFFFF
  SetCodeSegment(gdt[1], DescriptorType::kExecuteRead, 0, 0, 0xfffff);
  // 数据段：读+可写，Ring0，基址0，界限0xFFFFF
  SetDataSegment(gdt[2], DescriptorType::kReadWrite, 0, 0, 0xfffff);

  /*
   LoadGDT limit, address
   参数1：GDT大小-1（GDTR寄存器要求界限 = 表字节数 - 1）
   参数2：GDT起始虚拟地址
   内部封装 lgdt 硬件指令，定义在 asmfunc.asm
  */
  LoadGDT(sizeof(gdt) - 1, reinterpret_cast<uintptr_t>(&gdt[0]));
}
// #@@range_end(setup_segm_function)



/*
1、GDT 选择子对应关系
gdt[0] → 选择子 0x00（空描述符，禁止使用）
gdt[1] → 选择子 0x08（代码段，RPL=0）
gdt[2] → 选择子 0x10（数据段，RPL=0）


对应 day08a/day08b 内汇编：
ljmp $0x8, $1f 刷新 CS；mov $0x10, %%ds 设置数据段寄存器
2、为什么 limit = 0xFFFFF
limit 是 20 位最大值 0xFFFFF，granularity=1，单位 4KB
\(0xFFFFF \times 4096 = 4\mathrm{GB}\)
整个 32 位地址空间全部覆盖。
3、x86_64 重要陷阱
代码段 L=1；数据段 L 必须 = 0，不能混淆；
L=1 的代码段，D 位（default_operation_size）硬件强制要求为 0；
长模式下段基地址虽然配置为 0，但 GDT 不能省略，CPU 进入长模式前提是加载合法 GDT。
4、匿名命名空间作用对比day08a
day08a：GDT 是 KernelMain 局部变量；
day08b：gdt 作为静态变量放在匿名命名空间，生命周期全程存在，并且外部文件不可访问，典型 OS 模块化封装思想。
5、LoadGDT 汇编小补充
LoadGDT(size_minus_1, addr)
nasm
LoadGDT:
    lgdt [rdi]
    ret

注意：lgdt 加载的是 GDTR 结构体：界限 (2byte) + 地址 (8byte)，这个封装在 asmfunc 内部。


*/