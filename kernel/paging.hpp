/**
 * @file paging.hpp
 *
 * メモリページング用のプログラムを集めたファイル．
 * 存放x86_64分页机制相关声明；
 * day08b 引入，实现【恒等映射（Identity Mapping）】页表
 */
#pragma once
#include <cstddef>

/** @brief 静态分配的页目录(PDPT)数量
 *
 * 该常量用于 SetupIdentityPageTable 恒等映射初始化函数。
 * x86_64 4级分页结构，此处采用 **2MiB大页（HugePage）**：
 * 1 个 PDPT（Page Directory Pointer Table）条目可以管理 512 个 2MiB 页面
 * 512 × 2MiB = 1GiB
 *
 * kPageDirectoryCount = 64
 * → 总共映射：64 × 1GiB = 64GiB 地址空间
 * 虚拟地址 == 物理地址（恒等映射）
 */
const size_t kPageDirectoryCount = 64;

/**
 * @brief 构建恒等映射页表：虚拟地址 = 物理地址
 * 执行效果：
 * 1. 在静态全局内存中构建 PML4 → PDPT → PDT 三级页表结构（使用2MiB大页，无PT层）
 * 2. 填充页表条目，实现 VA=PA
 * 3. 将 CR3 寄存器设置为 PML4 物理地址
 * ⚠️ 调用该函数后，**尚未开启分页（CR0.PG位仍为0）**；
 * 真正开启分页的汇编指令在后续流程，MikanOS中配合GDT初始化完成后启用分页。
 */
void SetupIdentityPageTable();


/*

1、x86_64 四级分页简要回顾
地址拆分（48 位虚拟地址，2MiB 大页模式）
plaintext
[47:39] PML4 索引
[38:30] PDPT 索引
[29:21] PDT 索引
[20:0]  2MiB页内偏移
启用 2MiB HugePage 时，不使用 PT（Page Table）层级，到 PDT 层直接指向物理页。
2、kPageDirectoryCount 含义澄清（日文注释翻译拆解）
原文「ページディレクトリ」在注释里指代 PDPT entry：
每一个 PDPT 项对应 1 个 PDT；
一个 PDT 管理 512 个 2MiB 大页 = 1GiB；
kPageDirectoryCount = 64 → 覆盖 0～64GiB 物理内存。
64GiB 足够覆盖绝大多数 QEMU 虚拟机内存配置。
3、什么是 Identity PageTable（恒等映射）
虚拟地址 = 物理地址。
VA 0x1000 → PA 0x1000
VA 0x800000 → PA 0x800000
为什么内核早期必须恒等映射？
内核代码、数据、GDT、帧缓冲、MMIO 都运行在物理地址；
开启分页瞬间 CPU 仍然按照当前地址继续执行指令。
如果不做恒等映射，开启分页后地址翻译错位，CPU 立刻访问非法内存直接崩溃。
典型启动时序（day08b main.cpp）
cpp
运行
SetupSegments();        // 初始化GDT
SetDSAll / SetCSSS;     // 刷新段寄存器
SetupIdentityPageTable(); // 构造恒等页表（设置CR3）
// 之后汇编代码设置CR0.PG=1，正式开启分页
4、重要误区区分
SetupIdentityPageTable() 只构造页表 + 填写 CR3
≠ 开启分页！
开启分页需要修改 CR0 寄存器的 PG 位，在 asmfunc 汇编中完成。
本阶段只用静态全局数组存放页表
❌ 还没有动态内存分配器，不能new/malloc创建页表。
5、和 day08a /day08b 的版本关联
day08a：还没有分页相关代码，只完成 GDT；
day08b：新增分页模块 paging.hpp/cpp，
在 SetupSegments() 之后调用 SetupIdentityPageTable()，
完成从「不分段裸地址运行」向「分页内存模型」过渡。

*/