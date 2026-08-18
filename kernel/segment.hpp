/**
 * @file segment.hpp
 *
 * セグメンテーション用のプログラムを集めたファイル．
 * 存放分段机制相关定义与函数声明；
 * 对应x86_64 GDT（全局描述符表）段描述符结构定义
 */
#pragma once
#include <array>
#include <cstdint>
#include "x86_descriptor.hpp"

// #@@range_begin(segment_desc_definition)
/**
 * @union SegmentDescriptor
 * @brief x86/x86_64 段描述符结构体（GDT表项格式）
 *
 * union 设计：
 *  - data：完整64位原始数值，方便整体拷贝/调试查看原始bit
 *  - bits：位域结构体，方便按字段配置各个属性
 * __attribute__((packed))：取消编译器对齐填充，严格按照硬件规定64bit紧凑排列
 *
 * 硬件标准：一个段描述符固定占用 8 Byte = 64 bit
 */
union SegmentDescriptor {
  /// 完整64位原始数据
  uint64_t data;

  /**
   * @struct bits
   * 段描述符内部位域定义，严格遵循Intel CPU手册布局
   */
  struct {
    /// limit_low: 段界限 低16bit
    uint64_t limit_low : 16;
    /// base_low: 段基地址 低16bit
    uint64_t base_low : 16;
    /// base_middle: 段基地址 中间8bit
    uint64_t base_middle : 8;

    /// type：描述符类型（代码段/数据段/系统段等，来自枚举 DescriptorType）
    DescriptorType type : 4;
    /// system_segment: 0=系统段，1=代码/数据段（存储段）
    uint64_t system_segment : 1;
    /// descriptor_privilege_level(DPL): 特权级 0~3，内核一般使用0
    uint64_t descriptor_privilege_level : 2;
    /// present(P位): 1=该描述符有效存在；0=无效，CPU访问会触发异常
    uint64_t present : 1;

    /// limit_high: 段界限 高4bit
    uint64_t limit_high : 4;
    /// available(AVL): 操作系统自定义可用位，CPU硬件不使用
    uint64_t available : 1;
    /// long_mode(L位): 1=启用x86_64长模式代码段；数据段此位必须置0
    uint64_t long_mode : 1;
    /// default_operation_size(D/B位):
    /// 代码段：0=16位模式，1=32位模式；长模式下此位无效
    uint64_t default_operation_size : 1;
    /// granularity(G位): 粒度。0=界限单位1Byte；1=单位4KB
    uint64_t granularity : 1;

    /// base_high: 段基地址 高8bit
    uint64_t base_high : 8;
  } __attribute__((packed)) bits;
} __attribute__((packed));
// #@@range_end(segment_desc_definition)

/**
 * @brief 填充配置【代码段】描述符
 * @param desc    待配置的段描述符对象引用
 * @param type    段类型（可读/可执行等属性）
 * @param descriptor_privilege_level DPL特权等级 0~3
 * @param base    段基地址（32位；长模式下分段基址一般设0）
 * @param limit   段界限（地址上限）
 */
void SetCodeSegment(SegmentDescriptor& desc,
                    DescriptorType type,
                    unsigned int descriptor_privilege_level,
                    uint32_t base,
                    uint32_t limit);

/**
 * @brief 填充配置【数据段】描述符
 * @param desc    待配置的段描述符对象引用
 * @param type    段类型（可读/可写属性）
 * @param descriptor_privilege_level DPL特权等级 0~3
 * @param base    段基地址
 * @param limit   段界限
 */
void SetDataSegment(SegmentDescriptor& desc,
                    DescriptorType type,
                    unsigned int descriptor_privilege_level,
                    uint32_t base,
                    uint32_t limit);

/**
 * @brief 分段整体初始化入口函数
 * 1. 构造GDT全局描述符表（空描述符、代码段、数据段）
 * 2. 使用 lgdt 指令加载GDT到CPU
 * 3. 远跳转刷新CS寄存器
 * 4. 初始化 DS ES FS GS SS 段寄存器
 * day08b 将所有GDT初始化逻辑封装在此函数内
 */
void SetupSegments();

/*
补充重点知识（配合 mikanos day08 理解）
1. 基地址拼接规则
base_low(16) + base_middle(8) + base_high(8) 组成完整 32 位基地址
⚠️ x86_64 长模式：段基址强制设为 0，内存寻址全权交给分页机制，分段仅保留形式。
2. 段界限拼接规则
limit_low(16) + limit_high(4) 共 20 位界限值
G=0：最大寻址 2^20 Bytes = 1MB
G=1：界限 ×4KB，最大寻址 2^20 × 4KB = 4GB
3. #@@range_begin / range_end
只是书本配套工具用的注释标记，编译器完全忽略，作用是自动提取书中展示的代码片段，不属于 C++ 语法。
4. __attribute__((packed))
至关重要！
如果不加 packed，GCC 会为结构体插入对齐填充，生成的 8 字节描述符格式不符合 CPU 硬件要求，加载 GDT 后系统直接异常崩溃。

*/