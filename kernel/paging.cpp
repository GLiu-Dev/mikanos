#include "paging.hpp"
#include <array>
#include "asmfunc.h"

// #@@range_begin(setup_page)
namespace {
  /// 4KB标准页面大小
  const uint64_t kPageSize4K = 4096;
  /// 2MiB大页（Huge Page） = 512 × 4KB
  const uint64_t kPageSize2M = 512 * kPageSize4K;
  /// 1GiB巨页（Giant Page） = 512 × 2MiB
  const uint64_t kPageSize1G = 512 * kPageSize2M;

  /**
   * PML4 Table（Page Map Level 4）
   * x86_64四级分页根页表；共512项，每项8字节，整体4KB对齐
   * 每一项指向一张PDPT表，管理1GiB × 512 = 512GiB地址空间
   */
  alignas(kPageSize4K) std::array<uint64_t, 512> pml4_table;

  /**
   * PDP Table（Page Directory Pointer Table）
   * PML4下一级页表；512项，每项指向一张PDT
   */
  alignas(kPageSize4K) std::array<uint64_t, 512> pdp_table;

  /**
   * PDT Page Directory Table 数组
   * page_directory[i_pdpt]：第i_pdpt号PDT表
   * kPageDirectoryCount = 64，一共静态分配64张PDT
   * 每张PDT包含512项，每项映射1块2MiB物理内存
   * 单张PDT：512 × 2MiB = 1GiB
   * 全部64张PDT：64GiB 恒等映射范围
   */
  alignas(kPageSize4K)
    std::array<std::array<uint64_t, 512>, kPageDirectoryCount> page_directory;
}

/**
 * @brief 构建恒等映射页表 VA = PA，并设置CR3寄存器
 * 分页层级：PML4 → PDPT → PDT，使用 **2MiB HugePage**
 * 不使用PT层级，减少页表层级
 * 页表全部为全局静态变量：内核早期无堆内存分配器，不能new/malloc
 * 
 * 
 * 
 * 
  1. pml4_table[0] = &pdp_table | 0x003
    PML4 第 0 项指向唯一使用的 PDPT 表
  2. 外层循环 i_pdpt 0~63
    pdp_table[i_pdpt] = &page_directory[i_pdpt] | 0x003
    PDPT 每一项指向一张独立 PDT
  3. 内层循环 i_pd 0~511
    page_directory[i_pdpt][i_pd]
    = i_pdpt*1G + i_pd*2M | 0x083
    填写 2MiB 大页条目，实现 VA=PA
  4. SetCR3(&pml4_table[0])
    把根页表物理地址写入 CR3；此时分页还未开启
 */
void SetupIdentityPageTable() {
  /*
   * PML4[0] 指向 PDPT起始地址
   * 低12bit：页表项属性标志位
   * 0x003 = P | R/W
   * bit0(P)     = 1：Present，条目有效
   * bit1(RW)    = 1：可读可写
   */
  pml4_table[0] = reinterpret_cast<uint64_t>(&pdp_table[0]) | 0x003;

  // 遍历所有PDT（共64张）
  for (int i_pdpt = 0; i_pdpt < page_directory.size(); ++i_pdpt) {
    // PDPT[i_pdpt] 指向第 i_pdpt 张 PDT
    pdp_table[i_pdpt] = reinterpret_cast<uint64_t>(&page_directory[i_pdpt]) | 0x003;

    // 填充当前PDT内512个2MiB页表项
    for (int i_pd = 0; i_pd < 512; ++i_pd) {
      /*
       * 计算物理地址：
       * i_pdpt * 1GiB → 当前PDT基准地址
       * i_pd * 2MiB → 当前PDT内偏移
       *
       * 0x083 标志位：
       * bit0 P=1 存在
       * bit1 RW=1 读写
       * bit7 PS=1 Page Size：开启2MiB大页模式
       * PS=1 表示本条目直接指向2MiB物理页，不再寻址PT层
       */
      page_directory[i_pdpt][i_pd] = i_pdpt * kPageSize1G + i_pd * kPageSize2M | 0x083;
    }
  }

  // 将PML4物理地址写入CR3寄存器
  // CR3存放最高层级页表物理地址，CPU分页硬件通过CR3寻址页表
  SetCR3(reinterpret_cast<uint64_t>(&pml4_table[0]));
}
// #@@range_end(setup_page)



/*
虚拟地址 VA 分为四段：
 VA[47:39] │ VA[38:30] │ VA[29:21] │ VA[20:0]
  PML4索引    PDPT索引    PDT索引    2MiB页内偏移

代码只使用：
PML4 索引 = 0
PDPT 索引 = 0～63
更高索引全部置空，所以只映射 0～64GiB。

举例：
VA = 0x40000000（1GiB）
PML4[0]
PDPT[1]
PDT [0]
→ 命中 page_directory[1][0] = 1*1G + 0*2M
→ VA=1GiB → PA=1GiB



1、地址映射计算示例
i_pdpt = 0, i_pd = 0 → 0 * 1G + 0 * 2M = 0 → VA=0 → PA=0
i_pdpt = 0, i_pd = 1 → 0 * 1G + 1 * 2M = 0x200000 → VA=0x200000 → PA=0x200000
以此实现恒等映射 Identity Mapping
2、页表标志位详解
表格
值	名称	含义
bit0 (1)	P(Present)	有效位，必须置 1
bit1 (2)	R/W	1 = 允许读写；0 = 只读
bit7 (0x80)	PS(Page Size)	PDT 层置 1 = 启用 2MiB 大页
PML4、PDPT 项：0x003（不开启大页，只是下级页表指针）
PDT 最终页项：0x083（开启 2MiB 大页）
3、映射范围验算
每张 PDT：512 × 2MiB = 1GiB
kPageDirectoryCount = 64
总映射大小：64 × 1GiB = 64GiB
4、执行时序重点（main.cpp 调用顺序）
cpp
运行
SetupSegments();        // 初始化GDT
SetDSAll(0);
SetCSSS(kernel_cs, kernel_ss);
SetupIdentityPageTable(); // 1.构造页表 2.写入CR3
// 后续汇编代码设置CR0.PG = 1，正式开启分页！
⚠️ SetupIdentityPageTable 只设置 CR3，不会开启分页。
分页开启依靠修改 CR0 寄存器 PG 位，在 asmfunc 内后续完成。
5、为什么全部使用静态 std::array？
day08b 此时还没有物理内存分配器，不能动态分配页表。
所有 PML4/PDPT/PDT 都是全局静态变量，在内核加载时就占用固定内存。
6、层级结构简图
plaintext
CR3 → pml4_table[0] → pdp_table[i_pdpt] → page_directory[i_pdpt][i_pd] → 2MiB物理页
7、day08a ↔ day08b 对比
day08a：不存在 paging.hpp / paging.cpp，CPU 运行在不分页模式（物理地址直接寻址）
day08b：新增分页模块，构建恒等映射，为后续复杂虚拟内存管理打下基础


*/