/**
 * @file x86_descriptor.hpp
 *
 * セグメントと割り込みディスクリプタのための共通定義を集めたファイル．
 * 存放GDT段描述符、IDT门描述符共用类型常量定义
 * x86/x86_64 描述符中「Type字段」枚举
 */
#pragma once

/**
 * @enum DescriptorType
 * 对应段/门描述符内 TYPE 4bit 字段
 * 分为两大类：
 * 1. System segment / Gate 系统段、门描述符（用于TSS、LDT、中断门、调用门等）
 * 2. Code/Data segment 代码段、数据段（普通程序段）
 */
enum class DescriptorType {
  // ======================
  // System segment & gate descriptor types（系统段 / 门描述符）
  // ======================
  kUpper8Bytes   = 0,    // 保留：描述符第2部分（16字节描述符高8字节类型标识）
  kLDT           = 2,    // LDT 局部描述符表段
  kTSSAvailable  = 9,    // 可用状态TSS（任务状态段，32/64位）
  kTSSBusy       = 11,   // 繁忙状态TSS
  kCallGate      = 12,   // 调用门（权限切换、跨任务调用）
  kInterruptGate = 14,   // 中断门（IDT使用，中断触发时清除IF，关闭可屏蔽中断）
  kTrapGate      = 15,   // 陷阱门（异常使用，不清除IF，保持中断开启）

  // ======================
  // code & data segment types（普通代码/数据段描述符）
  // 这类描述符S位=1（区别于系统段S=0）
  // ======================
  kReadWrite     = 2,    // 数据段：可读可写
  kExecuteRead   = 10,   // 代码段：可执行+可读
                         // bit1=1(Execute), bit0=1(Read)；代码段永远不可写入
};